/**
 * @file          BulkXfer.c
 * @brief         Engine of the BLE bulk transfer (BulkXfer) framework: TX and
 *                RX session state machines, flow control and GATT write hook.
 *
 *                Execution contexts:
 *                  - BLE RX thread : gt_BLK_RxWriteHook() only validates the
 *                                    frame, copies it into a slab block and
 *                                    queues it. It never blocks.
 *                  - BLE TX context: sv_NotifyComplete() returns a TX credit.
 *                  - Timer ISR     : timer expiry sets an event bit.
 *                  - Engine thread : everything else. All session state is
 *                                    owned by this thread under sst_lock.
 *                Every producer "kicks" the engine through sst_wakeSem; the
 *                engine drains all pending work on each wake, so coalesced
 *                kicks never lose work.
 *
 *                Flow control: sst_txCredits counts notifications handed to
 *                the host but not yet sent. A credit is taken before every
 *                bt_gatt_notify_cb() and returned in the completion callback,
 *                which keeps the controller queue full without ever blocking on
 *                host buffer allocation.
 *
 * @date          22/09/2026
 * @author        Shivam Chudasama
 * @copyright     Shivam Chudasama
 * @license       MIT
 */

/* SPDX-License-Identifier: MIT */

/******************************************************************************/
/*                                                                            */
/*                                  INCLUDES                                  */
/*                                                                            */
/******************************************************************************/
#include <string.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/util.h>
#include "BulkXfer.h"
#include "AppLog.h"

/******************************************************************************/
/*                                                                            */
/*                                  DEFINES                                   */
/*                                                                            */
/******************************************************************************/
/**
 * @def           BLK_CRC_READ_CHUNK
 * @brief         Chunk size used when computing the CRC of a source before
 *                sending.
 */
#define BLK_CRC_READ_CHUNK                   (64U)

/**
 * @def           BLK_SEQ_HALF_RANGE
 * @brief         Half of the 8-bit sequence space: separates "ahead" from
 *                "behind".
 */
#define BLK_SEQ_HALF_RANGE                   (128U)

/******************************************************************************/
/*                                                                            */
/*                                   ENUMS                                    */
/*                                                                            */
/******************************************************************************/
/**
 * @enum          BlkEvent_E
 * @brief         Bit positions in sat_events.
 */
typedef enum
{
   eBE_TX_TIMEOUT = 0,    /**< TX ACK timer expired.                   */
   eBE_TX_RETRY,          /**< Host was out of buffers, retry pumping. */
   eBE_RX_ACK_DUE,        /**< RX delayed-ACK timer expired.           */
   eBE_RX_IDLE,           /**< RX inactivity timer expired.            */
   eBE_RX_OVERFLOW,       /**< Write hook dropped a frame (no block).  */
   eBE_TX_ABORT_REQ,      /**< Application called gv_BLK_AbortTx().    */
   eBE_RX_ABORT_REQ,      /**< Application called gv_BLK_AbortRx().    */
} BlkEvent_E;

/**
 * @enum          BlkTxState_E
 * @brief         States of the outgoing transfer.
 */
typedef enum
{
   eBTS_IDLE = 0,           /**< No transfer.                            */
   eBTS_START_PENDING,      /**< gi_BLK_Send() accepted, START not sent. */
   eBTS_START_SENT,         /**< START sent, waiting for ACK(seq 0).     */
   eBTS_SENDING,            /**< DATA flowing, waiting for END.          */
} BlkTxState_E;

/******************************************************************************/
/*                                                                            */
/*                                 STRUCTURES                                 */
/*                                                                            */
/******************************************************************************/
/**
 * @struct        BlkTxSession_T
 * @brief         Outgoing transfer. Frame indices are 32-bit "absolute"
 *                counters; only their low 8 bits travel on the wire.
 */
typedef struct
{
   BlkTxState_E e_state;
   uint8_t u8_xferId;
   uint8_t u8_appType;
   uint8_t u8_chunkSize;
   uint8_t u8_window;
   uint8_t u8_retries;
   uint32_t u32_totalLen;
   uint32_t u32_crc32;
   uint32_t u32_totalFrames;
   uint32_t u32_nextAbs;      /**< Next frame to transmit.                 */
   uint32_t u32_ackedAbs;     /**< All frames below this are acknowledged. */
   BlkSource_T st_source;
} BlkTxSession_T;

/**
 * @struct        BlkRxSession_T
 * @brief         Incoming transfer.
 */
typedef struct
{
   bool b_active;
   bool b_nackSent;           /**< Suppress repeated NACKs for one gap.    */
   uint8_t u8_xferId;
   uint8_t u8_appType;
   uint8_t u8_chunkSize;
   uint8_t u8_window;
   uint8_t u8_sinceAck;       /**< In-order frames since the last ACK.     */
   uint32_t u32_totalLen;
   uint32_t u32_expectedCrc;
   uint32_t u32_crc32;
   uint32_t u32_totalFrames;
   uint32_t u32_nextAbs;      /**< Next expected frame.                    */
} BlkRxSession_T;

/**
 * @struct        BlkRxBlock_T
 * @brief         One received frame queued from the write hook to the engine.
 */
typedef struct
{
   void *vpt_fifoReserved;    /**< Required by k_fifo (first word).        */
   atomic_val_t t_connGen;    /**< Connection generation at reception.     */
   uint16_t u16_len;
   uint8_t u8ar_data[BLK_MAX_FRAME_LEN];
} BlkRxBlock_T;

/**
 * @struct        BlkPendingDone_T
 * @brief         Completion that must be reported from the engine thread
 *                because it was detected in BLE stack context (disconnect).
 */
typedef struct
{
   bool b_txPending;
   uint8_t u8_txAppType;
   bool b_rxPending;
   uint8_t u8_rxAppType;
   uint32_t u32_rxTotalLen;
} BlkPendingDone_T;

/******************************************************************************/
/*                                                                            */
/*                                   UNIONS                                   */
/*                                                                            */
/******************************************************************************/

/******************************************************************************/
/*                                                                            */
/*                       PRIVATE FUNCTION DECLARATIONS                        */
/*                                                                            */
/******************************************************************************/
static void sv_EngineRunOnce(void);
static void sv_EngineThread(void *vpt_p1, void *vpt_p2, void *vpt_p3);
static void sv_Kick(void);
static void sv_TimerExpiry(struct k_timer *stpt_timer);
static void sv_NotifyComplete(struct bt_conn *stpt_conn, void *vpt_userData);
static void sv_ResetCredits(void);
static int si_NotifyWithCredit(struct bt_conn *stpt_conn, const uint8_t *u8pt_buf,
   uint16_t u16_len);
static int si_SendCtrl(const uint8_t *u8pt_buf, uint16_t u16_len);
static uint16_t su16_FrameCapacity(struct bt_conn *stpt_conn);
static uint32_t su32_FrameCount(uint32_t u32_totalLen, uint8_t u8_chunkSize);
static uint32_t su32_SeqToAbs(uint8_t u8_seq, uint32_t u32_base);
static void sv_HandleEvents(void);
static void sv_DeliverPendingDone(void);
static void sv_DrainRxFifo(void);
static void sv_HandleFrame(const uint8_t *u8pt_buf, uint16_t u16_len);
static void sv_TxFinish(BlkStatus_E e_status, bool b_sendAbort);
static void sv_TxOnTimeout(void);
static void sv_TxOnAck(const BlkFrame_T *stpt_frame);
static void sv_TxOnNack(const BlkFrame_T *stpt_frame);
static void sv_TxPump(void);
static void sv_RxFinish(BlkStatus_E e_status);
static void sv_RxAbort(BlkStatus_E e_status);
static void sv_RxSendAck(void);
static void sv_RxComplete(void);
static void sv_RxOnStart(const BlkFrame_T *stpt_frame);
static void sv_RxOnData(const BlkFrame_T *stpt_frame);
static int si_RamSourceRead(void *vpt_ctx, uint32_t u32_offset, uint8_t *u8pt_buf,
   uint16_t u16_len);
#if defined(CONFIG_BT_GATT_CLIENT)
static void sv_MtuExchanged(struct bt_conn *stpt_conn, uint8_t u8_err,
   struct bt_gatt_exchange_params *stpt_params);
#endif

/******************************************************************************/
/*                                                                            */
/*                              EXTERN VARIABLES                              */
/*                                                                            */
/******************************************************************************/

/******************************************************************************/
/*                                                                            */
/*                              PUBLIC VARIABLES                              */
/*                                                                            */
/******************************************************************************/
/**
 * @var           gt_blkThread
 * @brief         Engine thread; started by gi_BLK_Init(). K_THREAD_DEFINE
 *                gives the thread ID external linkage.
 */
K_THREAD_DEFINE(gt_blkThread, BLK_THREAD_STACK_SIZE, sv_EngineThread, NULL, NULL,
   NULL, BLK_THREAD_PRIORITY, 0, SYS_FOREVER_MS);

/******************************************************************************/
/*                                                                            */
/*                             PRIVATE VARIABLES                              */
/*                                                                            */
/******************************************************************************/
/**
 * @var           sst_lock
 * @brief         Protects all session state, sst_cfg and sstpt_conn.
 *                Recursive, so application callbacks may call the public API.
 */
static K_MUTEX_DEFINE(sst_lock);

/**
 * @var           sst_wakeSem
 * @brief         Binary "work available" signal for the engine thread.
 */
static K_SEM_DEFINE(sst_wakeSem, 0, 1);

/**
 * @var           sst_txCredits
 * @brief         Notifications that may still be handed to the host.
 */
static K_SEM_DEFINE(sst_txCredits, BLK_TX_INFLIGHT_MAX, BLK_TX_INFLIGHT_MAX);

/**
 * @var           sst_rxFifo
 * @brief         Received frames, write hook -> engine.
 */
static K_FIFO_DEFINE(sst_rxFifo);

/**
 * @var           sst_rxSlab
 * @brief         Storage for received frames.
 */
K_MEM_SLAB_DEFINE_STATIC(sst_rxSlab, sizeof(BlkRxBlock_T), BLK_RX_POOL_DEPTH, 4);

/**
 * @var           sst_txAckTimer
 * @brief         Sender: ACK progress timeout (raises eBE_TX_TIMEOUT).
 */
static K_TIMER_DEFINE(sst_txAckTimer, sv_TimerExpiry, NULL);

/**
 * @var           sst_txRetryTimer
 * @brief         Sender: back-off after the host ran out of buffers (raises
 *                eBE_TX_RETRY).
 */
static K_TIMER_DEFINE(sst_txRetryTimer, sv_TimerExpiry, NULL);

/**
 * @var           sst_rxAckTimer
 * @brief         Receiver: delayed-ACK timer (raises eBE_RX_ACK_DUE).
 */
static K_TIMER_DEFINE(sst_rxAckTimer, sv_TimerExpiry, NULL);

/**
 * @var           sst_rxIdleTimer
 * @brief         Receiver: inactivity timeout (raises eBE_RX_IDLE).
 */
static K_TIMER_DEFINE(sst_rxIdleTimer, sv_TimerExpiry, NULL);

/**
 * @var           sst_cfg
 * @brief         Copy of the configuration passed to gi_BLK_Init().
 */
static BlkCfg_T sst_cfg;

/**
 * @var           sb_initialized
 * @brief         Set once gi_BLK_Init() has succeeded.
 */
static bool sb_initialized = false;

/**
 * @var           sstpt_conn
 * @brief         Current connection (referenced). Guarded by sst_lock.
 */
static struct bt_conn *sstpt_conn = NULL;

/**
 * @var           sapt_hookConn
 * @brief         Lock-free copy of sstpt_conn for the write hook (not
 *                referenced).
 */
static atomic_ptr_t sapt_hookConn = ATOMIC_PTR_INIT(NULL);

/**
 * @var           sat_connGen
 * @brief         Incremented on every connect / disconnect to discard stale
 *                frames.
 */
static atomic_t sat_connGen = ATOMIC_INIT(0);

/**
 * @var           sat_events
 * @brief         Pending BlkEvent_E bits.
 */
static atomic_t sat_events = ATOMIC_INIT(0);

/**
 * @var           sst_tx
 * @brief         State of the outgoing transfer.
 */
static BlkTxSession_T sst_tx;

/**
 * @var           sst_rx
 * @brief         State of the incoming transfer.
 */
static BlkRxSession_T sst_rx;

/**
 * @var           sst_pending
 * @brief         Completions recorded at disconnect, reported by the engine.
 */
static BlkPendingDone_T sst_pending;

/**
 * @var           su8_txXferCounter
 * @brief         Source of the xferId of the next outgoing transfer.
 */
static uint8_t su8_txXferCounter = 0U;

/**
 * @var           su8ar_txFrame
 * @brief         DATA frame build buffer (engine thread only).
 */
static uint8_t su8ar_txFrame[BLK_MAX_FRAME_LEN];

/******************************************************************************/
/*                                                                            */
/*                              EXTERN FUNCTIONS                              */
/*                                                                            */
/******************************************************************************/

/******************************************************************************/
/*                                                                            */
/*                        PRIVATE FUNCTION DEFINITIONS                        */
/*                                                                            */
/******************************************************************************/
/**
 * @private       sv_EngineRunOnce
 * @brief         One engine pass: process events, received frames and
 *                outgoing data under sst_lock. Separated from the thread loop
 *                so that host tests can drive the engine step by step.
 * @return        void
 */
static void sv_EngineRunOnce(void)
{
   (void)k_mutex_lock(&sst_lock, K_FOREVER);
   sv_DeliverPendingDone();
   sv_HandleEvents();
   sv_DrainRxFifo();

   // Check if the write hook had to drop a frame of the active transfer
   if (atomic_test_and_clear_bit(&sat_events, eBE_RX_OVERFLOW) && sst_rx.b_active)
   {
      uint8_t u8ar_frame[BLK_CTRL_FRAME_MAX_LEN];
      uint16_t u16_len = gu16_BLK_EncodeNack(u8ar_frame, sizeof(u8ar_frame),
         sst_rx.u8_xferId, (uint8_t)sst_rx.u32_nextAbs, eBS_NO_RESOURCES);

      (void)si_SendCtrl(u8ar_frame, u16_len);
      sst_rx.b_nackSent = true;
   }

   sv_TxPump();
   k_mutex_unlock(&sst_lock);
}

/**
 * @private       sv_EngineThread
 * @brief         Engine main loop: wait for a kick, then run one pass.
 * @param[in]     vpt_p1 Unused.
 * @param[in]     vpt_p2 Unused.
 * @param[in]     vpt_p3 Unused.
 * @return        void
 */
static void sv_EngineThread(void *vpt_p1, void *vpt_p2, void *vpt_p3)
{
   ARG_UNUSED(vpt_p1);
   ARG_UNUSED(vpt_p2);
   ARG_UNUSED(vpt_p3);

   for (;;)
   {
      (void)k_sem_take(&sst_wakeSem, K_FOREVER);
      sv_EngineRunOnce();
   }
}

/**
 * @private       sv_Kick
 * @brief         Wake the engine thread (coalescing).
 * @return        void
 */
static void sv_Kick(void)
{
   k_sem_give(&sst_wakeSem);
}

/**
 * @private       sv_TimerExpiry
 * @brief         Shared expiry handler of all BulkXfer timers (ISR context).
 * @param[in]     stpt_timer Expired timer.
 * @return        void
 */
static void sv_TimerExpiry(struct k_timer *stpt_timer)
{
   // Check which timer expired and raise the matching event
   if (stpt_timer == &sst_txAckTimer)
   {
      atomic_set_bit(&sat_events, eBE_TX_TIMEOUT);
   }
   else if (stpt_timer == &sst_txRetryTimer)
   {
      atomic_set_bit(&sat_events, eBE_TX_RETRY);
   }
   else if (stpt_timer == &sst_rxAckTimer)
   {
      atomic_set_bit(&sat_events, eBE_RX_ACK_DUE);
   }
   else
   {
      atomic_set_bit(&sat_events, eBE_RX_IDLE);
   }

   sv_Kick();
}

/**
 * @private       sv_NotifyComplete
 * @brief         Notification handed to the controller: return its credit.
 * @note          The host does not call this for notifications that fail
 *                because the link dropped; sv_ResetCredits() covers that case.
 * @param[in]     stpt_conn Connection (unused).
 * @param[in]     vpt_userData Unused.
 * @return        void
 */
static void sv_NotifyComplete(struct bt_conn *stpt_conn, void *vpt_userData)
{
   ARG_UNUSED(stpt_conn);
   ARG_UNUSED(vpt_userData);

   k_sem_give(&sst_txCredits);
   sv_Kick();
}

/**
 * @private       sv_ResetCredits
 * @brief         Restore the full TX credit budget (after a disconnect).
 * @return        void
 */
static void sv_ResetCredits(void)
{
   uint32_t u32_idx = 0U;

   k_sem_reset(&sst_txCredits);

   for (u32_idx = 0U; u32_idx < BLK_TX_INFLIGHT_MAX; u32_idx++)
   {
      k_sem_give(&sst_txCredits);
   }
}

/**
 * @private       si_NotifyWithCredit
 * @brief         Send one frame as a notification. The caller must already
 *                hold a TX credit; it is returned here if the send fails.
 * @param[in]     stpt_conn Connection.
 * @param[in]     u8pt_buf Frame (copied by the host before returning).
 * @param[in]     u16_len Frame length.
 * @return        0 on success or the negative errno of bt_gatt_notify_cb().
 */
static int si_NotifyWithCredit(struct bt_conn *stpt_conn, const uint8_t *u8pt_buf,
   uint16_t u16_len)
{
   struct bt_gatt_notify_params st_params = { 0 };
   int i_ret = 0;

   st_params.attr = sst_cfg.stpt_txAttr;
   st_params.data = u8pt_buf;
   st_params.len = u16_len;
   st_params.func = sv_NotifyComplete;
   st_params.user_data = NULL;

   i_ret = bt_gatt_notify_cb(stpt_conn, &st_params);

   // Check if the notification was rejected; its credit is then still ours
   if (i_ret != 0)
   {
      k_sem_give(&sst_txCredits);
   }

   return i_ret;
}

/**
 * @private       si_SendCtrl
 * @brief         Send a control frame on the current connection, waiting up
 *                to BLK_CTRL_TX_TIMEOUT_MS for a TX credit. Engine thread only.
 * @param[in]     u8pt_buf Frame.
 * @param[in]     u16_len Frame length (0 = encoding failed, nothing sent).
 * @return        0 on success, negative errno otherwise.
 */
static int si_SendCtrl(const uint8_t *u8pt_buf, uint16_t u16_len)
{
   int i_ret = 0;

   // Check if there is a link and a valid frame
   if ((sstpt_conn == NULL) || (u16_len == 0U))
   {
      return -ENOTCONN;
   }

   // Check if a TX credit becomes available in time
   if (k_sem_take(&sst_txCredits, K_MSEC(BLK_CTRL_TX_TIMEOUT_MS)) != 0)
   {
      APP_LOG_WRN("no TX credit for control frame 0x%02x", u8pt_buf[1]);
      return -EAGAIN;
   }

   i_ret = si_NotifyWithCredit(sstpt_conn, u8pt_buf, u16_len);

   // Check if the host refused the control frame
   if (i_ret != 0)
   {
      APP_LOG_WRN("control frame 0x%02x not sent (%d)", u8pt_buf[1], i_ret);
   }

   return i_ret;
}

/**
 * @private       su16_FrameCapacity
 * @brief         Largest frame usable on the link: min(ATT_MTU - 3, max).
 * @param[in]     stpt_conn Connection.
 * @return        Frame capacity in bytes.
 */
static uint16_t su16_FrameCapacity(struct bt_conn *stpt_conn)
{
   uint16_t u16_mtu = bt_gatt_get_mtu(stpt_conn);
   uint16_t u16_cap = (u16_mtu > 3U) ? (uint16_t)(u16_mtu - 3U) : 0U;

   return MIN(u16_cap, (uint16_t)BLK_MAX_FRAME_LEN);
}

/**
 * @private       su32_FrameCount
 * @brief         Number of DATA frames needed for u32_totalLen bytes.
 * @param[in]     u32_totalLen Object size.
 * @param[in]     u8_chunkSize Bytes per frame (non-zero).
 * @return        ceil(u32_totalLen / u8_chunkSize), overflow-safe.
 */
static uint32_t su32_FrameCount(uint32_t u32_totalLen, uint8_t u8_chunkSize)
{
   return (u32_totalLen / u8_chunkSize) + (((u32_totalLen % u8_chunkSize) != 0U) ? 1U : 0U);
}

/**
 * @private       su32_SeqToAbs
 * @brief         Expand an 8-bit wire sequence number to an absolute frame
 *                index at or after u32_base (window <= 128 keeps this unique).
 * @param[in]     u8_seq Wire sequence number.
 * @param[in]     u32_base Reference absolute index.
 * @return        Absolute frame index.
 */
static uint32_t su32_SeqToAbs(uint8_t u8_seq, uint32_t u32_base)
{
   return u32_base + (uint8_t)(u8_seq - (uint8_t)u32_base);
}

/**
 * @private       sv_DeliverPendingDone
 * @brief         Report completions recorded by gv_BLK_OnDisconnected().
 * @return        void
 */
static void sv_DeliverPendingDone(void)
{
   // Check if a TX completion is waiting to be reported
   if (sst_pending.b_txPending)
   {
      sst_pending.b_txPending = false;

      // Check if the application wants TX completions
      if (sst_cfg.fpt_onTxDone != NULL)
      {
         sst_cfg.fpt_onTxDone(sst_pending.u8_txAppType, eBS_DISCONNECTED);
      }
   }

   // Check if an RX completion is waiting to be reported
   if (sst_pending.b_rxPending)
   {
      sst_pending.b_rxPending = false;

      // Check if the application wants RX completions
      if (sst_cfg.fpt_onRxDone != NULL)
      {
         sst_cfg.fpt_onRxDone(sst_pending.u8_rxAppType, eBS_DISCONNECTED,
            sst_pending.u32_rxTotalLen);
      }
   }
}

/**
 * @private       sv_HandleEvents
 * @brief         Process timer and application-request events.
 * @return        void
 */
static void sv_HandleEvents(void)
{
   // Check if the application aborted the outgoing transfer
   if (atomic_test_and_clear_bit(&sat_events, eBE_TX_ABORT_REQ)
      && (sst_tx.e_state != eBTS_IDLE))
   {
      sv_TxFinish(eBS_ABORTED, sst_tx.e_state != eBTS_START_PENDING);
   }

   // Check if the application aborted the incoming transfer
   if (atomic_test_and_clear_bit(&sat_events, eBE_RX_ABORT_REQ) && sst_rx.b_active)
   {
      sv_RxAbort(eBS_ABORTED);
   }

   // Check if the TX ACK timer expired
   if (atomic_test_and_clear_bit(&sat_events, eBE_TX_TIMEOUT))
   {
      sv_TxOnTimeout();
   }

   // The retry event only needs to wake the engine; sv_TxPump() does the work
   (void)atomic_test_and_clear_bit(&sat_events, eBE_TX_RETRY);

   // Check if a delayed ACK is due
   if (atomic_test_and_clear_bit(&sat_events, eBE_RX_ACK_DUE) && sst_rx.b_active)
   {
      sv_RxSendAck();
   }

   // Check if the incoming transfer went silent
   if (atomic_test_and_clear_bit(&sat_events, eBE_RX_IDLE) && sst_rx.b_active)
   {
      APP_LOG_WRN("RX transfer %u timed out", sst_rx.u8_xferId);
      sv_RxAbort(eBS_TIMEOUT);
   }
}

/**
 * @private       sv_DrainRxFifo
 * @brief         Process every queued frame of the current connection.
 * @return        void
 */
static void sv_DrainRxFifo(void)
{
   BlkRxBlock_T *stpt_blk = NULL;

   while ((stpt_blk = k_fifo_get(&sst_rxFifo, K_NO_WAIT)) != NULL)
   {
      // Check if the frame belongs to the current connection
      if (stpt_blk->t_connGen == atomic_get(&sat_connGen))
      {
         sv_HandleFrame(stpt_blk->u8ar_data, stpt_blk->u16_len);
      }

      k_mem_slab_free(&sst_rxSlab, stpt_blk);
   }
}

/**
 * @private       sv_HandleFrame
 * @brief         Decode one received frame and dispatch it.
 * @param[in]     u8pt_buf Frame bytes.
 * @param[in]     u16_len Frame length.
 * @return        void
 */
static void sv_HandleFrame(const uint8_t *u8pt_buf, uint16_t u16_len)
{
   BlkFrame_T st_frame;
   int i_ret = gi_BLK_FrameParse(u8pt_buf, u16_len, &st_frame);

   // Check if the frame is well formed
   if (i_ret != 0)
   {
      APP_LOG_WRN("dropping malformed frame (len %u, err %d)", (unsigned)u16_len, i_ret);
      return;
   }

   // Check if this is a single-frame application message
   if (st_frame.u8_type <= BLK_APP_TYPE_MAX)
   {
      // Check if the application handles short messages
      if (sst_cfg.fpt_onRxShort != NULL)
      {
         sst_cfg.fpt_onRxShort(st_frame.u8_type, st_frame.u8pt_payload,
            st_frame.u8_payloadLen);
      }
      return;
   }

   switch (st_frame.u8_type)
   {
      case eBFT_START:
         sv_RxOnStart(&st_frame);
         break;

      case eBFT_DATA:
         sv_RxOnData(&st_frame);
         break;

      case eBFT_ACK:
         sv_TxOnAck(&st_frame);
         break;

      case eBFT_NACK:
         sv_TxOnNack(&st_frame);
         break;

      case eBFT_END:
         // Check if the END refers to the outgoing transfer
         if ((sst_tx.e_state >= eBTS_START_SENT)
            && (st_frame.u_body.st_end.u8_xferId == sst_tx.u8_xferId))
         {
            sv_TxFinish((BlkStatus_E)st_frame.u_body.st_end.u8_status, false);
         }
         break;

      case eBFT_ABORT:
         // Check if the receiver cancelled our outgoing transfer
         if ((st_frame.u_body.st_abort.u8_dir == eBAD_BY_RECEIVER)
            && (sst_tx.e_state >= eBTS_START_SENT)
            && (st_frame.u_body.st_abort.u8_xferId == sst_tx.u8_xferId))
         {
            sv_TxFinish((st_frame.u_body.st_abort.u8_reason == eBS_REJECTED)
               ? eBS_REJECTED : eBS_REMOTE_ABORTED, false);
         }
         // Check if the sender cancelled our incoming transfer
         else if ((st_frame.u_body.st_abort.u8_dir == eBAD_BY_SENDER)
            && sst_rx.b_active
            && (st_frame.u_body.st_abort.u8_xferId == sst_rx.u8_xferId))
         {
            sv_RxFinish(eBS_REMOTE_ABORTED);
         }
         break;

      default:
         break;
   }
}

/* ======================================================================== */
/*  TX (device -> central)                                                  */
/* ======================================================================== */

/**
 * @private       sv_TxFinish
 * @brief         End the outgoing transfer and report e_status.
 * @param[in]     e_status Result.
 * @param[in]     b_sendAbort true to tell the receiver with an ABORT frame.
 * @return        void
 */
static void sv_TxFinish(BlkStatus_E e_status, bool b_sendAbort)
{
   uint8_t u8_appType = sst_tx.u8_appType;

   // Check if the receiver must be told that the transfer is cancelled
   if (b_sendAbort)
   {
      uint8_t u8ar_frame[BLK_CTRL_FRAME_MAX_LEN];
      uint16_t u16_len = gu16_BLK_EncodeAbort(u8ar_frame, sizeof(u8ar_frame),
         sst_tx.u8_xferId, (uint8_t)e_status, eBAD_BY_SENDER);

      (void)si_SendCtrl(u8ar_frame, u16_len);
   }

   k_timer_stop(&sst_txAckTimer);
   k_timer_stop(&sst_txRetryTimer);
   sst_tx.e_state = eBTS_IDLE;

   APP_LOG_INF("TX transfer %u done, status %u", sst_tx.u8_xferId, (unsigned)e_status);

   // Check if the application wants TX completions
   if (sst_cfg.fpt_onTxDone != NULL)
   {
      sst_cfg.fpt_onTxDone(u8_appType, e_status);
   }
}

/**
 * @private       sv_TxOnTimeout
 * @brief         No ACK progress for BLK_TX_ACK_TIMEOUT_MS: resend START or
 *                go back to the last acknowledged frame.
 * @return        void
 */
static void sv_TxOnTimeout(void)
{
   // Check if there is a transfer waiting for the peer
   if ((sst_tx.e_state != eBTS_START_SENT) && (sst_tx.e_state != eBTS_SENDING))
   {
      return;
   }

   sst_tx.u8_retries++;

   // Check if the peer has been silent for too long
   if (sst_tx.u8_retries > BLK_TX_MAX_RETRIES)
   {
      sv_TxFinish(eBS_TIMEOUT, true);
      return;
   }

   APP_LOG_WRN("TX transfer %u: ACK timeout, retry %u", sst_tx.u8_xferId,
      sst_tx.u8_retries);

   // Check if START itself was never acknowledged
   if (sst_tx.e_state == eBTS_START_SENT)
   {
      sst_tx.e_state = eBTS_START_PENDING;
   }
   else
   {
      sst_tx.u32_nextAbs = sst_tx.u32_ackedAbs;
   }

   k_timer_start(&sst_txAckTimer, K_MSEC(BLK_TX_ACK_TIMEOUT_MS), K_NO_WAIT);
}

/**
 * @private       sv_TxOnAck
 * @brief         Cumulative ACK from the receiver.
 * @param[in]     stpt_frame Decoded ACK frame.
 * @return        void
 */
static void sv_TxOnAck(const BlkFrame_T *stpt_frame)
{
   uint32_t u32_abs = 0U;
   uint8_t u8_window = 0U;

   // Check if the ACK belongs to the outgoing transfer
   if ((stpt_frame->u_body.st_ack.u8_xferId != sst_tx.u8_xferId)
      || (sst_tx.e_state < eBTS_START_SENT))
   {
      return;
   }

   // Check if this is the receiver accepting START
   if (sst_tx.e_state == eBTS_START_SENT)
   {
      // Check if the ACK is the expected START acknowledgement (seq 0)
      if (stpt_frame->u_body.st_ack.u8_seq != 0U)
      {
         return;
      }

      u8_window = stpt_frame->u_body.st_ack.u8_window;
      u8_window = CLAMP(u8_window, 1U, BLK_SEQ_HALF_RANGE);
      sst_tx.u8_window = MIN(sst_tx.u8_window, u8_window);
      sst_tx.e_state = eBTS_SENDING;
      sst_tx.u8_retries = 0U;
      k_timer_start(&sst_txAckTimer, K_MSEC(BLK_TX_ACK_TIMEOUT_MS), K_NO_WAIT);
      return;
   }

   u32_abs = su32_SeqToAbs(stpt_frame->u_body.st_ack.u8_seq, sst_tx.u32_ackedAbs);

   // Check if the ACK acknowledges new frames that were actually sent
   if ((u32_abs > sst_tx.u32_ackedAbs) && (u32_abs <= sst_tx.u32_nextAbs))
   {
      sst_tx.u32_ackedAbs = u32_abs;
      sst_tx.u8_retries = 0U;
      k_timer_start(&sst_txAckTimer, K_MSEC(BLK_TX_ACK_TIMEOUT_MS), K_NO_WAIT);
   }
}

/**
 * @private       sv_TxOnNack
 * @brief         The receiver lost frames: go back to its expected frame.
 * @param[in]     stpt_frame Decoded NACK frame.
 * @return        void
 */
static void sv_TxOnNack(const BlkFrame_T *stpt_frame)
{
   uint32_t u32_abs = 0U;

   // Check if the NACK belongs to the outgoing transfer in data phase
   if ((stpt_frame->u_body.st_nack.u8_xferId != sst_tx.u8_xferId)
      || (sst_tx.e_state != eBTS_SENDING))
   {
      return;
   }

   u32_abs = su32_SeqToAbs(stpt_frame->u_body.st_nack.u8_seq, sst_tx.u32_ackedAbs);

   // Check if the requested frame lies within what was sent
   if (u32_abs <= sst_tx.u32_nextAbs)
   {
      APP_LOG_DBG("TX transfer %u: NACK, resend from %u (reason %u)", sst_tx.u8_xferId,
         u32_abs, stpt_frame->u_body.st_nack.u8_reason);
      sst_tx.u32_ackedAbs = u32_abs;
      sst_tx.u32_nextAbs = u32_abs;
      k_timer_start(&sst_txAckTimer, K_MSEC(BLK_TX_ACK_TIMEOUT_MS), K_NO_WAIT);
   }
}

/**
 * @private       sv_TxPump
 * @brief         Send START (if pending) and as many DATA frames as the
 *                window and the TX credits allow. Never blocks.
 * @return        void
 */
static void sv_TxPump(void)
{
   uint32_t u32_offset = 0U;
   uint16_t u16_dataLen = 0U;
   uint16_t u16_frameLen = 0U;
   int i_ret = 0;

   // Check if there is a link to send on
   if (sstpt_conn == NULL)
   {
      return;
   }

   // Check if START still has to be sent
   if (sst_tx.e_state == eBTS_START_PENDING)
   {
      // Check if a TX credit is free; otherwise the completion kick retries
      if (k_sem_take(&sst_txCredits, K_NO_WAIT) != 0)
      {
         return;
      }

      u16_frameLen = gu16_BLK_EncodeStart(su8ar_txFrame, sizeof(su8ar_txFrame),
         sst_tx.u8_xferId, sst_tx.u8_appType, sst_tx.u32_totalLen, sst_tx.u8_chunkSize,
         sst_tx.u8_window, sst_tx.u32_crc32);
      i_ret = si_NotifyWithCredit(sstpt_conn, su8ar_txFrame, u16_frameLen);

      // Check if the host is temporarily out of buffers
      if ((i_ret == -ENOMEM) || (i_ret == -EAGAIN))
      {
         k_timer_start(&sst_txRetryTimer, K_MSEC(BLK_NOTIFY_RETRY_MS), K_NO_WAIT);
         return;
      }

      // Check if START could not be sent at all
      if (i_ret != 0)
      {
         APP_LOG_ERR("START not sent (%d)", i_ret);
         sv_TxFinish((i_ret == -ENOTCONN) ? eBS_DISCONNECTED
            : eBS_PROTOCOL_ERROR, false);
         return;
      }

      sst_tx.e_state = eBTS_START_SENT;
      k_timer_start(&sst_txAckTimer, K_MSEC(BLK_TX_ACK_TIMEOUT_MS), K_NO_WAIT);
      return;
   }

   // Check if the transfer is in its data phase
   if (sst_tx.e_state != eBTS_SENDING)
   {
      return;
   }

   while ((sst_tx.u32_nextAbs < sst_tx.u32_totalFrames)
      && ((sst_tx.u32_nextAbs - sst_tx.u32_ackedAbs) < sst_tx.u8_window))
   {
      // Check if a TX credit is free; otherwise the completion kick resumes
      if (k_sem_take(&sst_txCredits, K_NO_WAIT) != 0)
      {
         break;
      }

      u32_offset = sst_tx.u32_nextAbs * sst_tx.u8_chunkSize;
      u16_dataLen = (uint16_t)MIN((uint32_t)sst_tx.u8_chunkSize,
         sst_tx.u32_totalLen - u32_offset);
      u16_frameLen = gu16_BLK_EncodeDataHeader(su8ar_txFrame, sizeof(su8ar_txFrame),
         sst_tx.u8_xferId, (uint8_t)sst_tx.u32_nextAbs, u16_dataLen);

      // Read the chunk straight into the frame, behind the header (no copy)
      i_ret = sst_tx.st_source.fpt_read(sst_tx.st_source.vpt_ctx, u32_offset,
         &su8ar_txFrame[BLK_DATA_HDR_LEN], u16_dataLen);

      // Check if the application's source failed
      if (i_ret != 0)
      {
         k_sem_give(&sst_txCredits);
         APP_LOG_ERR("source read failed at offset %u (%d)", u32_offset, i_ret);
         sv_TxFinish(eBS_SOURCE_ERROR, true);
         return;
      }

      i_ret = si_NotifyWithCredit(sstpt_conn, su8ar_txFrame, u16_frameLen);

      // Check if the host is temporarily out of buffers
      if ((i_ret == -ENOMEM) || (i_ret == -EAGAIN))
      {
         k_timer_start(&sst_txRetryTimer, K_MSEC(BLK_NOTIFY_RETRY_MS), K_NO_WAIT);
         break;
      }

      // Check if the notification failed for good
      if (i_ret != 0)
      {
         APP_LOG_ERR("DATA not sent (%d)", i_ret);
         sv_TxFinish((i_ret == -ENOTCONN) ? eBS_DISCONNECTED
            : eBS_PROTOCOL_ERROR, false);
         return;
      }

      sst_tx.u32_nextAbs++;
   }
}

/* ======================================================================== */
/*  RX (central -> device)                                                  */
/* ======================================================================== */

/**
 * @private       sv_RxFinish
 * @brief         End the incoming transfer and report e_status.
 * @param[in]     e_status Result.
 * @return        void
 */
static void sv_RxFinish(BlkStatus_E e_status)
{
   k_timer_stop(&sst_rxAckTimer);
   k_timer_stop(&sst_rxIdleTimer);
   sst_rx.b_active = false;

   APP_LOG_INF("RX transfer %u done, status %u", sst_rx.u8_xferId, (unsigned)e_status);

   // Check if the application wants RX completions
   if (sst_cfg.fpt_onRxDone != NULL)
   {
      sst_cfg.fpt_onRxDone(sst_rx.u8_appType, e_status, sst_rx.u32_totalLen);
   }
}

/**
 * @private       sv_RxAbort
 * @brief         Cancel the incoming transfer and tell the sender.
 * @param[in]     e_status Reason, also reported locally.
 * @return        void
 */
static void sv_RxAbort(BlkStatus_E e_status)
{
   uint8_t u8ar_frame[BLK_CTRL_FRAME_MAX_LEN];
   uint16_t u16_len = gu16_BLK_EncodeAbort(u8ar_frame, sizeof(u8ar_frame),
      sst_rx.u8_xferId, (uint8_t)e_status, eBAD_BY_RECEIVER);

   (void)si_SendCtrl(u8ar_frame, u16_len);
   sv_RxFinish(e_status);
}

/**
 * @private       sv_RxSendAck
 * @brief         Acknowledge every frame before the next expected one.
 * @return        void
 */
static void sv_RxSendAck(void)
{
   uint8_t u8ar_frame[BLK_CTRL_FRAME_MAX_LEN];
   uint16_t u16_len = gu16_BLK_EncodeAck(u8ar_frame, sizeof(u8ar_frame),
      sst_rx.u8_xferId, (uint8_t)sst_rx.u32_nextAbs, sst_rx.u8_window);

   k_timer_stop(&sst_rxAckTimer);
   sst_rx.u8_sinceAck = 0U;
   (void)si_SendCtrl(u8ar_frame, u16_len);
}

/**
 * @private       sv_RxComplete
 * @brief         All frames received: verify the CRC, send END, report.
 * @return        void
 */
static void sv_RxComplete(void)
{
   uint8_t u8ar_frame[BLK_CTRL_FRAME_MAX_LEN];
   uint16_t u16_len = 0U;
   BlkStatus_E e_status = (sst_rx.u32_crc32 == sst_rx.u32_expectedCrc)
      ? eBS_OK : eBS_CRC_ERROR;

   u16_len = gu16_BLK_EncodeEnd(u8ar_frame, sizeof(u8ar_frame), sst_rx.u8_xferId,
      (uint8_t)e_status);
   (void)si_SendCtrl(u8ar_frame, u16_len);
   sv_RxFinish(e_status);
}

/**
 * @private       sv_RxOnStart
 * @brief         A peer announces a transfer: validate, ask the application,
 *                and acknowledge with ACK(seq 0) or reject with ABORT.
 * @param[in]     stpt_frame Decoded START frame.
 * @return        void
 */
static void sv_RxOnStart(const BlkFrame_T *stpt_frame)
{
   uint8_t u8ar_frame[BLK_CTRL_FRAME_MAX_LEN];
   uint16_t u16_len = 0U;
   BlkStatus_E e_reject = eBS_OK;
   uint8_t u8_xferId = stpt_frame->u_body.st_start.u8_xferId;
   uint8_t u8_chunk = stpt_frame->u_body.st_start.u8_chunkSize;
   uint8_t u8_window = stpt_frame->u_body.st_start.u8_window;

   // Check if this is a repeated START of the transfer already accepted
   if (sst_rx.b_active && (sst_rx.u8_xferId == u8_xferId) && (sst_rx.u32_nextAbs == 0U))
   {
      sv_RxSendAck();
      return;
   }

   // Check if a new START supersedes an unfinished transfer
   if (sst_rx.b_active)
   {
      APP_LOG_WRN("RX transfer %u superseded by %u", sst_rx.u8_xferId, u8_xferId);
      sv_RxFinish(eBS_REMOTE_ABORTED);
   }

   // Check if the transfer parameters are acceptable
   if ((u8_chunk == 0U) || (u8_chunk > BLK_MAX_CHUNK_LEN) || (u8_window == 0U)
      || (u8_window > BLK_SEQ_HALF_RANGE))
   {
      e_reject = eBS_PROTOCOL_ERROR;
   }
   // Check if the application is able to receive transfers
   else if (sst_cfg.fpt_onRxData == NULL)
   {
      e_reject = eBS_REJECTED;
   }
   // Check if the application accepts this particular transfer
   else if ((sst_cfg.fpt_onRxStart != NULL)
      && (sst_cfg.fpt_onRxStart(stpt_frame->u_body.st_start.u8_appType,
         stpt_frame->u_body.st_start.u32_totalLen) != 0))
   {
      e_reject = eBS_REJECTED;
   }

   // Check if the transfer must be refused
   if (e_reject != eBS_OK)
   {
      u16_len = gu16_BLK_EncodeAbort(u8ar_frame, sizeof(u8ar_frame), u8_xferId,
         (uint8_t)e_reject, eBAD_BY_RECEIVER);
      (void)si_SendCtrl(u8ar_frame, u16_len);
      return;
   }

   (void)memset(&sst_rx, 0, sizeof(sst_rx));
   sst_rx.b_active = true;
   sst_rx.u8_xferId = u8_xferId;
   sst_rx.u8_appType = stpt_frame->u_body.st_start.u8_appType;
   sst_rx.u8_chunkSize = u8_chunk;
   sst_rx.u8_window = MIN(u8_window, (uint8_t)BLK_WINDOW_DEFAULT);
   sst_rx.u32_totalLen = stpt_frame->u_body.st_start.u32_totalLen;
   sst_rx.u32_expectedCrc = stpt_frame->u_body.st_start.u32_crc32;
   sst_rx.u32_totalFrames = su32_FrameCount(sst_rx.u32_totalLen, u8_chunk);

   APP_LOG_INF("RX transfer %u: type 0x%02x, %u bytes in %u frames", u8_xferId,
      sst_rx.u8_appType, sst_rx.u32_totalLen, sst_rx.u32_totalFrames);

   // Check if this is an empty object (complete immediately)
   if (sst_rx.u32_totalFrames == 0U)
   {
      sv_RxComplete();
      return;
   }

   sv_RxSendAck();
   k_timer_start(&sst_rxIdleTimer, K_MSEC(BLK_RX_IDLE_TIMEOUT_MS), K_NO_WAIT);
}

/**
 * @private       sv_RxOnData
 * @brief         DATA frame: deliver it if in order, NACK a gap, re-ACK a
 *                duplicate.
 * @param[in]     stpt_frame Decoded DATA frame.
 * @return        void
 */
static void sv_RxOnData(const BlkFrame_T *stpt_frame)
{
   uint8_t u8ar_frame[BLK_CTRL_FRAME_MAX_LEN];
   uint16_t u16_len = 0U;
   uint8_t u8_diff = 0U;
   uint32_t u32_offset = 0U;
   uint32_t u32_expLen = 0U;
   uint8_t u8_ackEvery = 0U;

   // Check if the frame belongs to the active incoming transfer
   if (!sst_rx.b_active || (stpt_frame->u_body.st_data.u8_xferId != sst_rx.u8_xferId))
   {
      return;
   }

   k_timer_start(&sst_rxIdleTimer, K_MSEC(BLK_RX_IDLE_TIMEOUT_MS), K_NO_WAIT);
   u8_diff = (uint8_t)(stpt_frame->u_body.st_data.u8_seq - (uint8_t)sst_rx.u32_nextAbs);

   // Check if the frame is behind (a retransmitted duplicate)
   if (u8_diff >= BLK_SEQ_HALF_RANGE)
   {
      // Re-ACK soon so that the sender moves forward again
      if (k_timer_remaining_get(&sst_rxAckTimer) == 0U)
      {
         k_timer_start(&sst_rxAckTimer, K_MSEC(BLK_RX_ACK_DELAY_MS), K_NO_WAIT);
      }
      return;
   }

   // Check if the frame is ahead of the expected one (a frame was lost)
   if (u8_diff != 0U)
   {
      // Check if the gap was already reported
      if (!sst_rx.b_nackSent)
      {
         u16_len = gu16_BLK_EncodeNack(u8ar_frame, sizeof(u8ar_frame), sst_rx.u8_xferId,
            (uint8_t)sst_rx.u32_nextAbs, eBS_OUT_OF_ORDER);
         (void)si_SendCtrl(u8ar_frame, u16_len);
         sst_rx.b_nackSent = true;
      }
      return;
   }

   u32_offset = sst_rx.u32_nextAbs * sst_rx.u8_chunkSize;
   u32_expLen = MIN((uint32_t)sst_rx.u8_chunkSize, sst_rx.u32_totalLen - u32_offset);

   // Check if the chunk has the size implied by START
   if (stpt_frame->u_body.st_data.u8_dataLen != u32_expLen)
   {
      APP_LOG_ERR("RX transfer %u: frame %u has %u bytes, expected %u", sst_rx.u8_xferId,
         sst_rx.u32_nextAbs, stpt_frame->u_body.st_data.u8_dataLen, u32_expLen);
      sv_RxAbort(eBS_PROTOCOL_ERROR);
      return;
   }

   // Check if the application's sink accepted the chunk
   if (sst_cfg.fpt_onRxData(sst_rx.u8_appType, u32_offset,
      stpt_frame->u_body.st_data.u8pt_data, stpt_frame->u_body.st_data.u8_dataLen) != 0)
   {
      sv_RxAbort(eBS_SINK_ERROR);
      return;
   }

   sst_rx.u32_crc32 = crc32_ieee_update(sst_rx.u32_crc32,
      stpt_frame->u_body.st_data.u8pt_data, stpt_frame->u_body.st_data.u8_dataLen);
   sst_rx.u32_nextAbs++;
   sst_rx.u8_sinceAck++;
   sst_rx.b_nackSent = false;

   // Check if this was the last frame (END replaces the final ACK)
   if (sst_rx.u32_nextAbs == sst_rx.u32_totalFrames)
   {
      sv_RxComplete();
      return;
   }

   u8_ackEvery = MAX(sst_rx.u8_window / 2U, 1U);

   // Check if half a window arrived since the last ACK
   if (sst_rx.u8_sinceAck >= u8_ackEvery)
   {
      sv_RxSendAck();
   }
   // Otherwise make sure a delayed ACK is scheduled
   else if (k_timer_remaining_get(&sst_rxAckTimer) == 0U)
   {
      k_timer_start(&sst_rxAckTimer, K_MSEC(BLK_RX_ACK_DELAY_MS), K_NO_WAIT);
   }
}

/**
 * @private       si_RamSourceRead
 * @brief         Built-in BlkSourceRead_F for objects held in RAM.
 * @param[in]     vpt_ctx Start of the object.
 * @param[in]     u32_offset Byte offset.
 * @param[out]    u8pt_buf Destination.
 * @param[in]     u16_len Number of bytes.
 * @return        0.
 */
static int si_RamSourceRead(void *vpt_ctx, uint32_t u32_offset, uint8_t *u8pt_buf,
   uint16_t u16_len)
{
   (void)memcpy(u8pt_buf, (const uint8_t *)vpt_ctx + u32_offset, u16_len);
   return 0;
}

#if defined(CONFIG_BT_GATT_CLIENT)
/**
 * @private       sv_MtuExchanged
 * @brief         Log the result of the ATT MTU exchange.
 * @param[in]     stpt_conn Connection.
 * @param[in]     u8_err ATT error (0 on success).
 * @param[in]     stpt_params Exchange parameters (unused).
 * @return        void
 */
static void sv_MtuExchanged(struct bt_conn *stpt_conn, uint8_t u8_err,
   struct bt_gatt_exchange_params *stpt_params)
{
   ARG_UNUSED(stpt_params);

   APP_LOG_INF("MTU exchange %s, ATT MTU %u", (u8_err == 0U) ? "done" : "failed",
      bt_gatt_get_mtu(stpt_conn));
}
#endif

/******************************************************************************/
/*                                                                            */
/*                        PUBLIC FUNCTION DEFINITIONS                         */
/*                                                                            */
/******************************************************************************/
/**
 * @public        gi_BLK_Init
 * @brief         Store the configuration and start the engine thread.
 * @param[in]     stpt_cfg Configuration (copied). stpt_txAttr is required.
 * @return        0 on success, -EINVAL on bad configuration, -EALREADY if
 *                already initialised.
 */
int gi_BLK_Init(const BlkCfg_T *stpt_cfg)
{
   // Check if the configuration names the TX characteristic
   if ((stpt_cfg == NULL) || (stpt_cfg->stpt_txAttr == NULL))
   {
      return -EINVAL;
   }

   (void)k_mutex_lock(&sst_lock, K_FOREVER);

   // Check if the framework is already running
   if (sb_initialized)
   {
      k_mutex_unlock(&sst_lock);
      return -EALREADY;
   }

   sst_cfg = *stpt_cfg;
   (void)memset(&sst_tx, 0, sizeof(sst_tx));
   (void)memset(&sst_rx, 0, sizeof(sst_rx));
   (void)memset(&sst_pending, 0, sizeof(sst_pending));
   sb_initialized = true;
   k_mutex_unlock(&sst_lock);

   k_thread_start(gt_blkThread);

   return 0;
}

/**
 * @public        gv_BLK_OnConnected
 * @brief         Bind the framework to a new connection. Call from the
 *                application's bt_conn_cb.connected callback (no error).
 * @param[in]     stpt_conn New connection.
 * @return        void
 */
void gv_BLK_OnConnected(struct bt_conn *stpt_conn)
{
   bool b_tune = false;

   (void)k_mutex_lock(&sst_lock, K_FOREVER);

   // Check if the framework is ready and not already serving a connection
   if (!sb_initialized || (sstpt_conn != NULL))
   {
      k_mutex_unlock(&sst_lock);
      APP_LOG_WRN("ignored (not initialised or a connection is already bound)");
      return;
   }

   sstpt_conn = bt_conn_ref(stpt_conn);
   (void)atomic_inc(&sat_connGen);
   (void)atomic_ptr_set(&sapt_hookConn, stpt_conn);
   b_tune = sst_cfg.b_autoTuneLink;
   k_mutex_unlock(&sst_lock);

   // Check if the framework should request throughput-oriented link settings
   if (b_tune)
   {
#if defined(CONFIG_BT_USER_PHY_UPDATE)
      (void)bt_conn_le_phy_update(stpt_conn, BT_CONN_LE_PHY_PARAM_2M);
#endif
#if defined(CONFIG_BT_USER_DATA_LEN_UPDATE)
      (void)bt_conn_le_data_len_update(stpt_conn, BT_LE_DATA_LEN_PARAM_MAX);
#endif
#if defined(CONFIG_BT_GATT_CLIENT)
      static struct bt_gatt_exchange_params slst_mtuParams = { .func = sv_MtuExchanged };

      (void)bt_gatt_exchange_mtu(stpt_conn, &slst_mtuParams);
#endif
   }
}

/**
 * @public        gv_BLK_OnDisconnected
 * @brief         Release the connection and fail any running transfer with
 *                eBS_DISCONNECTED (reported from the engine thread).
 *                Call from the application's bt_conn_cb.disconnected callback.
 * @param[in]     stpt_conn Connection that went down.
 * @return        void
 */
void gv_BLK_OnDisconnected(struct bt_conn *stpt_conn)
{
   (void)k_mutex_lock(&sst_lock, K_FOREVER);

   // Check if this is the connection the framework is bound to
   if (stpt_conn != sstpt_conn)
   {
      k_mutex_unlock(&sst_lock);
      return;
   }

   (void)atomic_ptr_set(&sapt_hookConn, NULL);
   (void)atomic_inc(&sat_connGen);
   atomic_clear(&sat_events);
   k_timer_stop(&sst_txAckTimer);
   k_timer_stop(&sst_txRetryTimer);
   k_timer_stop(&sst_rxAckTimer);
   k_timer_stop(&sst_rxIdleTimer);

   // Check if an outgoing transfer was running
   if (sst_tx.e_state != eBTS_IDLE)
   {
      sst_pending.b_txPending = true;
      sst_pending.u8_txAppType = sst_tx.u8_appType;
      sst_tx.e_state = eBTS_IDLE;
   }

   // Check if an incoming transfer was running
   if (sst_rx.b_active)
   {
      sst_pending.b_rxPending = true;
      sst_pending.u8_rxAppType = sst_rx.u8_appType;
      sst_pending.u32_rxTotalLen = sst_rx.u32_totalLen;
      sst_rx.b_active = false;
   }

   // Notifications lost with the link never complete: restore all credits
   sv_ResetCredits();

   bt_conn_unref(sstpt_conn);
   sstpt_conn = NULL;
   k_mutex_unlock(&sst_lock);

   sv_Kick();
}

/**
 * @public        gi_BLK_Send
 * @brief         Start an asynchronous multi-frame transfer to the central.
 *
 *                The CRC-32 of the whole object is computed here, in the
 *                caller's thread, by reading the source once. The source must
 *                then stay readable and unchanged until fpt_onTxDone.
 *
 * @param[in]     u8_appType Application type (0x00..BLK_APP_TYPE_MAX).
 * @param[in]     stpt_source Data source (copied).
 * @param[in]     u32_totalLen Object size in bytes (0 is allowed).
 * @return        0 if the transfer was queued, otherwise:
 *                -EPERM not initialised, -EINVAL bad argument, -EIO source
 *                failed, -ENOTCONN no link, -EBUSY a transfer is running,
 *                -EACCES central not subscribed to TX, -EMSGSIZE MTU too small.
 */
int gi_BLK_Send(uint8_t u8_appType, const BlkSource_T *stpt_source,
   uint32_t u32_totalLen)
{
   uint8_t u8ar_buf[BLK_CRC_READ_CHUNK];
   uint32_t u32_crc = 0U;
   uint32_t u32_offset = 0U;
   uint16_t u16_len = 0U;
   uint16_t u16_frameCap = 0U;
   int i_ret = 0;

   // Check if the framework is running and the arguments are valid
   if (!sb_initialized)
   {
      return -EPERM;
   }
   if ((stpt_source == NULL) || (stpt_source->fpt_read == NULL)
      || (u8_appType > BLK_APP_TYPE_MAX))
   {
      return -EINVAL;
   }

   /* ------------------------------------------------------------------ */
   /* 1. CRC pre-pass in the caller's thread (engine is not blocked).    */
   /* ------------------------------------------------------------------ */
   while (u32_offset < u32_totalLen)
   {
      u16_len = (uint16_t)MIN((uint32_t)sizeof(u8ar_buf), u32_totalLen - u32_offset);

      // Check if the source could provide the data
      if (stpt_source->fpt_read(stpt_source->vpt_ctx, u32_offset, u8ar_buf, u16_len) != 0)
      {
         return -EIO;
      }

      u32_crc = crc32_ieee_update(u32_crc, u8ar_buf, u16_len);
      u32_offset += u16_len;
   }

   /* ------------------------------------------------------------------ */
   /* 2. Set up the session and let the engine send START.               */
   /* ------------------------------------------------------------------ */
   (void)k_mutex_lock(&sst_lock, K_FOREVER);

   // Check if there is a connection
   if (sstpt_conn == NULL)
   {
      i_ret = -ENOTCONN;
   }
   // Check if the previous transfer has finished
   else if (sst_tx.e_state != eBTS_IDLE)
   {
      i_ret = -EBUSY;
   }
   // Check if the central listens to the TX characteristic
   else if (!bt_gatt_is_subscribed(sstpt_conn, sst_cfg.stpt_txAttr, BT_GATT_CCC_NOTIFY))
   {
      i_ret = -EACCES;
   }
   else
   {
      u16_frameCap = su16_FrameCapacity(sstpt_conn);

      // Check if the MTU leaves room for at least one data byte per frame
      if (u16_frameCap < BLK_MIN_FRAME_LEN)
      {
         i_ret = -EMSGSIZE;
      }
   }

   // Check if all preconditions were met
   if (i_ret == 0)
   {
      (void)memset(&sst_tx, 0, sizeof(sst_tx));
      sst_tx.u8_xferId = ++su8_txXferCounter;
      sst_tx.u8_appType = u8_appType;
      sst_tx.u8_chunkSize = (uint8_t)(u16_frameCap - BLK_DATA_HDR_LEN);
      sst_tx.u8_window = BLK_WINDOW_DEFAULT;
      sst_tx.u32_totalLen = u32_totalLen;
      sst_tx.u32_crc32 = u32_crc;
      sst_tx.u32_totalFrames = su32_FrameCount(u32_totalLen, sst_tx.u8_chunkSize);
      sst_tx.st_source = *stpt_source;
      sst_tx.e_state = eBTS_START_PENDING;

      APP_LOG_INF("TX transfer %u: type 0x%02x, %u bytes, %u B/frame", sst_tx.u8_xferId,
         u8_appType, u32_totalLen, sst_tx.u8_chunkSize);
   }

   k_mutex_unlock(&sst_lock);

   // Check if the engine has work to do
   if (i_ret == 0)
   {
      sv_Kick();
   }

   return i_ret;
}

/**
 * @public        gi_BLK_SendBuffer
 * @brief         gi_BLK_Send() for an object held in RAM. The buffer must stay
 *                valid and unchanged until fpt_onTxDone.
 * @param[in]     u8_appType Application type (0x00..BLK_APP_TYPE_MAX).
 * @param[in]     vpt_data Object.
 * @param[in]     u32_totalLen Object size in bytes.
 * @return        See gi_BLK_Send().
 */
int gi_BLK_SendBuffer(uint8_t u8_appType, const void *vpt_data, uint32_t u32_totalLen)
{
   BlkSource_T st_source;

   // Check if a buffer was given for a non-empty object
   if ((vpt_data == NULL) && (u32_totalLen != 0U))
   {
      return -EINVAL;
   }

   st_source.fpt_read = si_RamSourceRead;
   st_source.vpt_ctx = (void *)vpt_data;

   return gi_BLK_Send(u8_appType, &st_source, u32_totalLen);
}

/**
 * @public        gi_BLK_SendShort
 * @brief         Send a single-frame application message [len][type][data]
 *                immediately (no ACK, no retransmission). Can be used while a
 *                multi-frame transfer is running.
 * @param[in]     u8_appType Application type (0x00..BLK_APP_TYPE_MAX).
 * @param[in]     vpt_data Payload.
 * @param[in]     u8_len Payload length (<= gu16_BLK_GetMaxShortPayload()).
 * @param[in]     t_timeout Maximum wait for a free TX credit.
 * @return        0 on success, -EPERM, -EINVAL, -ENOTCONN, -EACCES, -EMSGSIZE,
 *                -EAGAIN (no credit in time) or a bt_gatt_notify_cb() error.
 */
int gi_BLK_SendShort(uint8_t u8_appType, const void *vpt_data, uint8_t u8_len,
   k_timeout_t t_timeout)
{
   uint8_t u8ar_frame[BLK_MAX_FRAME_LEN];
   struct bt_conn *stpt_conn = NULL;
   uint16_t u16_frameLen = 0U;
   int i_ret = 0;

   // Check if the framework is running and the arguments are valid
   if (!sb_initialized)
   {
      return -EPERM;
   }
   if ((u8_appType > BLK_APP_TYPE_MAX) || ((vpt_data == NULL) && (u8_len != 0U)))
   {
      return -EINVAL;
   }

   (void)k_mutex_lock(&sst_lock, K_FOREVER);

   // Check if there is a subscribed connection with a large enough MTU
   if (sstpt_conn == NULL)
   {
      i_ret = -ENOTCONN;
   }
   else if (!bt_gatt_is_subscribed(sstpt_conn, sst_cfg.stpt_txAttr, BT_GATT_CCC_NOTIFY))
   {
      i_ret = -EACCES;
   }
   else if (((uint16_t)u8_len + BLK_FRAME_HDR_LEN) > su16_FrameCapacity(sstpt_conn))
   {
      i_ret = -EMSGSIZE;
   }
   else
   {
      stpt_conn = bt_conn_ref(sstpt_conn);
   }

   k_mutex_unlock(&sst_lock);

   // Check if the preconditions failed
   if (i_ret != 0)
   {
      return i_ret;
   }

   u16_frameLen = gu16_BLK_EncodeShort(u8ar_frame, sizeof(u8ar_frame), u8_appType,
      (const uint8_t *)vpt_data, u8_len);

   // Wait for a TX credit outside the lock so the engine keeps running
   if (k_sem_take(&sst_txCredits, t_timeout) != 0)
   {
      i_ret = -EAGAIN;
   }
   else
   {
      i_ret = si_NotifyWithCredit(stpt_conn, u8ar_frame, u16_frameLen);
   }

   bt_conn_unref(stpt_conn);

   return i_ret;
}

/**
 * @public        gv_BLK_AbortTx
 * @brief         Request cancellation of the outgoing transfer. Completion is
 *                reported through fpt_onTxDone with eBS_ABORTED.
 * @return        void
 */
void gv_BLK_AbortTx(void)
{
   atomic_set_bit(&sat_events, eBE_TX_ABORT_REQ);
   sv_Kick();
}

/**
 * @public        gv_BLK_AbortRx
 * @brief         Request cancellation of the incoming transfer. Completion is
 *                reported through fpt_onRxDone with eBS_ABORTED.
 * @return        void
 */
void gv_BLK_AbortRx(void)
{
   atomic_set_bit(&sat_events, eBE_RX_ABORT_REQ);
   sv_Kick();
}

/**
 * @public        gb_BLK_IsTxBusy
 * @brief         Whether an outgoing transfer is in progress.
 * @return        true while gi_BLK_Send() would return -EBUSY.
 */
bool gb_BLK_IsTxBusy(void)
{
   bool b_busy = false;

   (void)k_mutex_lock(&sst_lock, K_FOREVER);
   b_busy = (sst_tx.e_state != eBTS_IDLE);
   k_mutex_unlock(&sst_lock);

   return b_busy;
}

/**
 * @public        gu16_BLK_GetMaxShortPayload
 * @brief         Largest payload gi_BLK_SendShort() accepts on the current
 *                link (depends on the negotiated ATT MTU).
 * @return        Payload size in bytes, or 0 without a connection.
 */
uint16_t gu16_BLK_GetMaxShortPayload(void)
{
   uint16_t u16_cap = 0U;

   (void)k_mutex_lock(&sst_lock, K_FOREVER);

   // Check if there is a connection to size against
   if (sstpt_conn != NULL)
   {
      u16_cap = su16_FrameCapacity(sstpt_conn);
      u16_cap = (u16_cap > BLK_FRAME_HDR_LEN) ? (uint16_t)(u16_cap - BLK_FRAME_HDR_LEN) : 0U;
   }

   k_mutex_unlock(&sst_lock);

   return u16_cap;
}

/**
 * @public        gv_BLK_GetCaps
 * @brief         Fill the capability record served by the optional Caps
 *                characteristic.
 * @param[out]    stpt_caps Destination.
 * @return        void
 */
void gv_BLK_GetCaps(BlkCaps_T *stpt_caps)
{
   __ASSERT(stpt_caps != NULL, "gv_BLK_GetCaps: stpt_caps is NULL");

   stpt_caps->u8_protocolVersion = BLK_PROTOCOL_VERSION;
   stpt_caps->u8_maxFrameLen = (uint8_t)MIN(BLK_MAX_FRAME_LEN, 255U);
   stpt_caps->u8_window = BLK_WINDOW_DEFAULT;
   stpt_caps->u8_reserved = 0U;
}

/**
 * @public        gt_BLK_RxWriteHook
 * @brief         Post-write hook for the RX characteristic. Matches
 *                GATTCustomWriteCb_F; forward to it from the hook named in the
 *                configurator. Runs in the BLE RX thread and never blocks.
 *
 *                gt_GATT_GenericWrite has already copied the frame into the
 *                descriptor's buffer; that copy is ignored here. The frame is
 *                taken from vpt_buf / u16_length, which always hold exactly
 *                the bytes of this write.
 *
 * @param[in]     stpt_connHandle Connection that wrote.
 * @param[in]     stpt_attr RX attribute (unused).
 * @param[in]     vpt_buf Written bytes (one frame).
 * @param[in]     u16_length Number of bytes.
 * @param[in]     u16_offset Must be 0 (long writes are not supported).
 * @param[in]     u8_flags Write flags; prepare writes are refused.
 * @return        0 to keep the generic return value, or BT_GATT_ERR().
 */
ssize_t gt_BLK_RxWriteHook(struct bt_conn *stpt_connHandle,
   const struct bt_gatt_attr *stpt_attr, const void *vpt_buf, uint16_t u16_length,
   uint16_t u16_offset, uint8_t u8_flags)
{
   BlkRxBlock_T *stpt_blk = NULL;
   const uint8_t *u8pt_buf = (const uint8_t *)vpt_buf;

   ARG_UNUSED(stpt_attr);

   // Check if this is a long (prepared / offset) write
   if ((u8_flags & BT_GATT_WRITE_FLAG_PREPARE) != 0U)
   {
      return BT_GATT_ERR(BT_ATT_ERR_WRITE_NOT_PERMITTED);
   }
   if (u16_offset != 0U)
   {
      return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
   }

   // Check if the write comes from the bound connection
   if (!sb_initialized || (stpt_connHandle != atomic_ptr_get(&sapt_hookConn)))
   {
      return 0;
   }

   // Check if the frame length is consistent with its len byte
   if ((u16_length < BLK_FRAME_HDR_LEN) || (u16_length > BLK_MAX_FRAME_LEN)
      || (((uint16_t)u8pt_buf[0] + BLK_FRAME_HDR_LEN) != u16_length))
   {
      return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
   }

   // Check if a queue block is free; otherwise let the engine NACK
   if (k_mem_slab_alloc(&sst_rxSlab, (void **)&stpt_blk, K_NO_WAIT) != 0)
   {
      atomic_set_bit(&sat_events, eBE_RX_OVERFLOW);
      sv_Kick();
      return BT_GATT_ERR(BT_ATT_ERR_INSUFFICIENT_RESOURCES);
   }

   stpt_blk->t_connGen = atomic_get(&sat_connGen);
   stpt_blk->u16_len = u16_length;
   (void)memcpy(stpt_blk->u8ar_data, u8pt_buf, u16_length);
   k_fifo_put(&sst_rxFifo, stpt_blk);
   sv_Kick();

   return 0;
}
