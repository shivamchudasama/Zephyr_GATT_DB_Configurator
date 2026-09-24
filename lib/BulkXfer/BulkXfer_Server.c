/**
 * @file          BulkXfer_Server.c
 * @brief         BulkXfer Server role: RX session state machine, DATA write
 *                hook and CTRL notification sender.
 *
 *                The remote client writes START / DATA / ABORT frames into
 *                the DATA characteristic (Write Without Response). This side
 *                answers with ACK / NACK / END / ABORT notifications on CTRL.
 *
 *                Flow control: sst_BLKS_credits counts CTRL notifications
 *                handed to the host but not yet sent. Only control frames and
 *                short messages use it, so a small budget suffices.
 *
 * @date          24/09/2026
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
#include "BulkXfer_Core_Priv.h"
#include "AppLog.h"

#if BLK_ENABLE_SERVER

/******************************************************************************/
/*                                                                            */
/*                                  DEFINES                                   */
/*                                                                            */
/******************************************************************************/

/******************************************************************************/
/*                                                                            */
/*                                   ENUMS                                    */
/*                                                                            */
/******************************************************************************/

/******************************************************************************/
/*                                                                            */
/*                                 STRUCTURES                                 */
/*                                                                            */
/******************************************************************************/
/**
 * @struct        BlkRxSession_T
 * @brief         Incoming transfer.
 */
typedef struct
{
   bool b_active;                            /**< A transfer is in progress.              */
   bool b_nackSent;                          /**< Suppress repeated NACKs for one gap.    */
   uint8_t u8_xferId;                        /**< Transfer ID from the START frame.       */
   uint8_t u8_appType;                       /**< Application type from the START frame.  */
   uint8_t u8_chunkSize;                     /**< Payload bytes per DATA frame.           */
   uint8_t u8_window;                        /**< Window granted to the sender.           */
   uint8_t u8_sinceAck;                      /**< In-order frames since the last ACK.     */
   uint32_t u32_totalLen;                    /**< Total object length in bytes.           */
   uint32_t u32_expectedCrc;                 /**< CRC-32 announced in the START frame.    */
   uint32_t u32_crc32;                       /**< Running CRC-32 of the data received.    */
   uint32_t u32_totalFrames;                 /**< DATA frames expected for the object.    */
   uint32_t u32_nextAbsFrame;                /**< Next expected frame.                    */
} BlkRxSession_T;

/**
 * @struct        BlkRxPendingDone_T
 * @brief         fpt_onRxDone owed for a transfer dropped at disconnect (BT
 *                context), reported from the engine thread.
 */
typedef struct
{
   bool b_pending;                           /**< A completion is owed.                   */
   uint8_t u8_appType;                       /**< Application type of the dropped RX.     */
   uint32_t u32_totalLen;                    /**< Announced length of the dropped RX.     */
} BlkRxPendingDone_T;

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
static void sv_SrvTimerExpiry(struct k_timer *stpt_timer);
static void sv_SrvNotifyComplete(struct bt_conn *stpt_conn, void *vpt_userData);
static void sv_SrvResetCredits(void);
static int si_SrvNotifyWithCredit(struct bt_conn *stpt_conn, const uint8_t *u8pt_buf,
   uint16_t u16_len);
static int si_SrvSendCtrl(const uint8_t *u8pt_buf, uint16_t u16_len);
static void sv_SrvHandleFrame(const uint8_t *u8pt_buf, uint16_t u16_len);
static void sv_RxFinish(BlkStatus_E e_status);
static void sv_RxAbort(BlkStatus_E e_status);
static void sv_RxSendAck(void);
static void sv_RxComplete(void);
static void sv_RxOnStart(const BlkFrame_T *stpt_frame);
static void sv_RxOnData(const BlkFrame_T *stpt_frame);
#if defined(CONFIG_BT_GATT_CLIENT)
static void sv_SrvMtuExchanged(struct bt_conn *stpt_conn, uint8_t u8_err,
   struct bt_gatt_exchange_params *stpt_params);
#endif // CONFIG_BT_GATT_CLIENT

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

/******************************************************************************/
/*                                                                            */
/*                             PRIVATE VARIABLES                              */
/*                                                                            */
/******************************************************************************/
/**
 * @var           sst_BLKS_credits
 * @brief         CTRL notifications that may still be handed to the host.
 */
static K_SEM_DEFINE(sst_BLKS_credits, BLK_SRV_NOTIFY_INFLIGHT_MAX, BLK_SRV_NOTIFY_INFLIGHT_MAX);

/**
 * @var           sst_BLKS_fifo
 * @brief         Frames written to DATA, write hook -> engine.
 */
static K_FIFO_DEFINE(sst_BLKS_fifo);

/**
 * @var           sst_BLKS_slab
 * @brief         Storage for queued DATA frames.
 */
K_MEM_SLAB_DEFINE_STATIC(sst_BLKS_slab, sizeof(BlkFrameBlock_T), BLK_RX_POOL_DEPTH, 4);

/**
 * @var           sst_BLKS_ackTimer
 * @brief         Delayed-ACK timer (raises eBE_SRV_ACK_DUE).
 */
static K_TIMER_DEFINE(sst_BLKS_ackTimer, sv_SrvTimerExpiry, NULL);

/**
 * @var           sst_BLKS_idleTimer
 * @brief         Inactivity timeout (raises eBE_SRV_IDLE).
 */
static K_TIMER_DEFINE(sst_BLKS_idleTimer, sv_SrvTimerExpiry, NULL);

/**
 * @var           sst_BLKS_cfg
 * @brief         Copy of the configuration passed to gi_BLKS_Init().
 */
static BlkSrvCfg_T sst_BLKS_cfg;

/**
 * @var           sb_BLKS_initialized
 * @brief         Set once gi_BLKS_Init() has succeeded.
 */
static bool sb_BLKS_initialized = false;

/**
 * @var           sstpt_BLKS_conn
 * @brief         Bound connection (referenced). Guarded by gst_BLK_lock.
 */
static struct bt_conn *sstpt_BLKS_conn = NULL;

/**
 * @var           st_BLKS_hookConn
 * @brief         Lock-free copy of sstpt_BLKS_conn for the write hook (not
 *                referenced).
 */
static atomic_ptr_t st_BLKS_hookConn = ATOMIC_PTR_INIT(NULL);

/**
 * @var           st_BLKS_connGen
 * @brief         Incremented on every connect / disconnect to discard stale
 *                frames.
 */
static atomic_t st_BLKS_connGen = ATOMIC_INIT(0);

/**
 * @var           sst_BLKS_session
 * @brief         State of the incoming transfer.
 */
static BlkRxSession_T sst_BLKS_session;

/**
 * @var           sst_BLKS_pendingDone
 * @brief         Completion recorded at disconnect, reported by the engine.
 */
static BlkRxPendingDone_T sst_BLKS_pendingDone;

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
 * @private       sv_SrvTimerExpiry
 * @brief         Expiry handler of the Server timers (ISR context).
 * @param[in]     stpt_timer Expired timer.
 * @return        void
 */
static void sv_SrvTimerExpiry(struct k_timer *stpt_timer)
{
   atomic_set_bit(&gt_BLK_events,
      (stpt_timer == &sst_BLKS_ackTimer) ? eBE_SRV_ACK_DUE : eBE_SRV_IDLE);
   gv_BLK_Kick();
}

/**
 * @private       sv_SrvNotifyComplete
 * @brief         CTRL notification handed to the controller: return its credit.
 * @note          Not called for notifications lost with the link;
 *                sv_SrvResetCredits() covers that case.
 * @param[in]     stpt_conn Connection (unused).
 * @param[in]     vpt_userData Unused.
 * @return        void
 */
static void sv_SrvNotifyComplete(struct bt_conn *stpt_conn, void *vpt_userData)
{
   ARG_UNUSED(stpt_conn);
   ARG_UNUSED(vpt_userData);

   k_sem_give(&sst_BLKS_credits);
   gv_BLK_Kick();
}

/**
 * @private       sv_SrvResetCredits
 * @brief         Restore the full credit budget (after a disconnect).
 * @return        void
 */
static void sv_SrvResetCredits(void)
{
   uint32_t u32_idx = 0U;

   k_sem_reset(&sst_BLKS_credits);

   for (u32_idx = 0U; u32_idx < BLK_SRV_NOTIFY_INFLIGHT_MAX; u32_idx++)
   {
      k_sem_give(&sst_BLKS_credits);
   }
}

/**
 * @private       si_SrvNotifyWithCredit
 * @brief         Send one frame as a CTRL notification. The caller must
 *                already hold a credit; it is returned here if the send fails.
 * @param[in]     stpt_conn Connection.
 * @param[in]     u8pt_buf Frame (copied by the host before returning).
 * @param[in]     u16_len Frame length.
 * @return        0 on success or the negative errno of bt_gatt_notify_cb().
 */
static int si_SrvNotifyWithCredit(struct bt_conn *stpt_conn, const uint8_t *u8pt_buf,
   uint16_t u16_len)
{
   struct bt_gatt_notify_params st_params = { 0 };
   int i_ret = 0;

   st_params.attr = sst_BLKS_cfg.stpt_ctrlAttr;
   st_params.data = u8pt_buf;
   st_params.len = u16_len;
   st_params.func = sv_SrvNotifyComplete;
   st_params.user_data = NULL;

   i_ret = bt_gatt_notify_cb(stpt_conn, &st_params);

   // Check if the notification was rejected; its credit is then still ours
   if (i_ret != 0)
   {
      k_sem_give(&sst_BLKS_credits);
   }

   return i_ret;
}

/**
 * @private       si_SrvSendCtrl
 * @brief         Notify a control frame on the bound connection, waiting up
 *                to BLK_CTRL_TX_TIMEOUT_MS for a credit. Engine thread only.
 * @param[in]     u8pt_buf Frame.
 * @param[in]     u16_len Frame length (0 = encoding failed, nothing sent).
 * @return        0 on success, negative errno otherwise.
 */
static int si_SrvSendCtrl(const uint8_t *u8pt_buf, uint16_t u16_len)
{
   int i_ret = 0;

   // Check if there is a link and a valid frame
   if ((sstpt_BLKS_conn == NULL) || (u16_len == 0U))
   {
      return -ENOTCONN;
   }

   // Check if a credit becomes available in time. A lost ACK / NACK stalls
   // the client until its own timeout, so control frames wait (bounded).
   if (k_sem_take(&sst_BLKS_credits, K_MSEC(BLK_CTRL_TX_TIMEOUT_MS)) != 0)
   {
      APP_LOG_WRN("no credit for control frame 0x%02x", u8pt_buf[1]);
      return -EAGAIN;
   }

   i_ret = si_SrvNotifyWithCredit(sstpt_BLKS_conn, u8pt_buf, u16_len);

   // Check if the host refused the control frame
   if (i_ret != 0)
   {
      APP_LOG_WRN("control frame 0x%02x not sent (%d)", u8pt_buf[1], i_ret);
   }

   return i_ret;
}

/**
 * @private       sv_SrvHandleFrame
 * @brief         Decode one frame written to DATA and dispatch it. Frames that
 *                belong on CTRL (ACK, NACK, END, receiver ABORT) are dropped.
 * @param[in]     u8pt_buf Frame bytes.
 * @param[in]     u16_len Frame length.
 * @return        void
 */
static void sv_SrvHandleFrame(const uint8_t *u8pt_buf, uint16_t u16_len)
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
      if (sst_BLKS_cfg.fpt_onRxShort != NULL)
      {
         sst_BLKS_cfg.fpt_onRxShort(st_frame.u8_type, st_frame.u8pt_payload,
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

      case eBFT_ABORT:
         // Check if the sender cancelled the incoming transfer
         if ((st_frame.u_body.st_abort.u8_dir == eBAD_BY_SENDER) && sst_BLKS_session.b_active
            && (st_frame.u_body.st_abort.u8_xferId == sst_BLKS_session.u8_xferId))
         {
            sv_RxFinish(eBS_REMOTE_ABORTED);
         }
         break;

      default:
         APP_LOG_WRN("frame 0x%02x is not valid on DATA", st_frame.u8_type);
         break;
   }
}

/**
 * @private       sv_RxFinish
 * @brief         End the incoming transfer and report e_status.
 * @param[in]     e_status Result.
 * @return        void
 */
static void sv_RxFinish(BlkStatus_E e_status)
{
   // Only b_active is cleared: the callback still needs the other fields
   k_timer_stop(&sst_BLKS_ackTimer);
   k_timer_stop(&sst_BLKS_idleTimer);
   sst_BLKS_session.b_active = false;

   APP_LOG_INF("RX transfer %u done, status %u", sst_BLKS_session.u8_xferId, (unsigned)e_status);

   // Check if the application wants RX completions
   if (sst_BLKS_cfg.fpt_onRxDone != NULL)
   {
      sst_BLKS_cfg.fpt_onRxDone(sst_BLKS_session.u8_appType, e_status,
         sst_BLKS_session.u32_totalLen);
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
      sst_BLKS_session.u8_xferId, (uint8_t)e_status, eBAD_BY_RECEIVER);

   (void)si_SrvSendCtrl(u8ar_frame, u16_len);
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
      sst_BLKS_session.u8_xferId, (uint8_t)sst_BLKS_session.u32_nextAbsFrame,
      sst_BLKS_session.u8_window);

   // Cumulative, so it supersedes any pending delayed ACK
   k_timer_stop(&sst_BLKS_ackTimer);
   sst_BLKS_session.u8_sinceAck = 0U;
   (void)si_SrvSendCtrl(u8ar_frame, u16_len);
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
   BlkStatus_E e_status = (sst_BLKS_session.u32_crc32 == sst_BLKS_session.u32_expectedCrc)
      ? eBS_OK : eBS_CRC_ERROR;

   // END replaces the final ACK. It is not retransmitted: if it is lost,
   // the sender ends with eBS_TIMEOUT although this side reports eBS_OK.
   u16_len = gu16_BLK_EncodeEnd(u8ar_frame, sizeof(u8ar_frame), sst_BLKS_session.u8_xferId,
      (uint8_t)e_status);
   (void)si_SrvSendCtrl(u8ar_frame, u16_len);
   sv_RxFinish(e_status);
}

/**
 * @private       sv_RxOnStart
 * @brief         A client announces a transfer: validate, ask the application,
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

   // Check if the client can hear the answer. Without a CTRL subscription
   // neither ACK nor ABORT reaches it; its ACK timeout ends the transfer.
   if (!bt_gatt_is_subscribed(sstpt_BLKS_conn, sst_BLKS_cfg.stpt_ctrlAttr, BT_GATT_CCC_NOTIFY))
   {
      APP_LOG_WRN("START %u ignored: client not subscribed to CTRL", u8_xferId);
      return;
   }

   // Check if this is a repeated START of the transfer already accepted
   // (our ACK(seq 0) was lost)
   if (sst_BLKS_session.b_active && (sst_BLKS_session.u8_xferId == u8_xferId)
      && (sst_BLKS_session.u32_nextAbsFrame == 0U))
   {
      sv_RxSendAck();
      return;
   }

   // Check if a new START supersedes an unfinished transfer
   if (sst_BLKS_session.b_active)
   {
      APP_LOG_WRN("RX transfer %u superseded by %u", sst_BLKS_session.u8_xferId, u8_xferId);
      sv_RxFinish(eBS_REMOTE_ABORTED);
   }

   // Check if the transfer parameters are acceptable
   if ((u8_chunk == 0U) || (u8_chunk > BLK_MAX_CHUNK_LEN) || (u8_window == 0U)
      || (u8_window > BLK_SEQ_HALF_RANGE))
   {
      e_reject = eBS_PROTOCOL_ERROR;
   }
   // Check if the application is able to receive transfers
   else if (sst_BLKS_cfg.fpt_onRxData == NULL)
   {
      e_reject = eBS_REJECTED;
   }
   // Check if the application accepts this particular transfer
   else if ((sst_BLKS_cfg.fpt_onRxStart != NULL)
      && (sst_BLKS_cfg.fpt_onRxStart(stpt_frame->u_body.st_start.u8_appType,
         stpt_frame->u_body.st_start.u32_totalLen) != 0))
   {
      e_reject = eBS_REJECTED;
   }

   // Check if the transfer must be refused
   if (e_reject != eBS_OK)
   {
      u16_len = gu16_BLK_EncodeAbort(u8ar_frame, sizeof(u8ar_frame), u8_xferId,
         (uint8_t)e_reject, eBAD_BY_RECEIVER);
      (void)si_SrvSendCtrl(u8ar_frame, u16_len);
      return;
   }

   (void)memset(&sst_BLKS_session, 0, sizeof(sst_BLKS_session));
   sst_BLKS_session.b_active = true;
   sst_BLKS_session.u8_xferId = u8_xferId;
   sst_BLKS_session.u8_appType = stpt_frame->u_body.st_start.u8_appType;
   sst_BLKS_session.u8_chunkSize = u8_chunk;
   sst_BLKS_session.u8_window = MIN(u8_window, (uint8_t)BLK_WINDOW_DEFAULT);
   sst_BLKS_session.u32_totalLen = stpt_frame->u_body.st_start.u32_totalLen;
   sst_BLKS_session.u32_expectedCrc = stpt_frame->u_body.st_start.u32_crc32;
   sst_BLKS_session.u32_totalFrames = gu32_BLK_FrameCount(sst_BLKS_session.u32_totalLen, u8_chunk);

   APP_LOG_INF("RX transfer %u: type 0x%02x, %u bytes in %u frames", u8_xferId,
      sst_BLKS_session.u8_appType, sst_BLKS_session.u32_totalLen,
      sst_BLKS_session.u32_totalFrames);

   // Check if this is an empty object (complete immediately)
   if (sst_BLKS_session.u32_totalFrames == 0U)
   {
      sv_RxComplete();
      return;
   }

   // ACK(seq 0) accepts the transfer
   sv_RxSendAck();
   k_timer_start(&sst_BLKS_idleTimer, K_MSEC(BLK_RX_IDLE_TIMEOUT_MS), K_NO_WAIT);
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
   if (!sst_BLKS_session.b_active
      || (stpt_frame->u_body.st_data.u8_xferId != sst_BLKS_session.u8_xferId))
   {
      return;
   }

   k_timer_start(&sst_BLKS_idleTimer, K_MSEC(BLK_RX_IDLE_TIMEOUT_MS), K_NO_WAIT);

   // Modulo-256 distance from the expected seq: 0 in order, 1..127 ahead
   // (gap), 128..255 behind (duplicate)
   u8_diff = (uint8_t)(stpt_frame->u_body.st_data.u8_seq
      - (uint8_t)sst_BLKS_session.u32_nextAbsFrame);

   // Check if the frame is behind (a retransmitted duplicate)
   if (u8_diff >= BLK_SEQ_HALF_RANGE)
   {
      // Re-ACK soon so that the sender moves forward again
      if (k_timer_remaining_get(&sst_BLKS_ackTimer) == 0U)
      {
         k_timer_start(&sst_BLKS_ackTimer, K_MSEC(BLK_RX_ACK_DELAY_MS), K_NO_WAIT);
      }
      return;
   }

   // Check if the frame is ahead of the expected one (a frame was lost).
   // Go-Back-N: out-of-order frames are dropped, not buffered.
   if (u8_diff != 0U)
   {
      // Check if the gap was already reported
      if (!sst_BLKS_session.b_nackSent)
      {
         u16_len = gu16_BLK_EncodeNack(u8ar_frame, sizeof(u8ar_frame), sst_BLKS_session.u8_xferId,
            (uint8_t)sst_BLKS_session.u32_nextAbsFrame, eBS_OUT_OF_ORDER);
         (void)si_SrvSendCtrl(u8ar_frame, u16_len);
         sst_BLKS_session.b_nackSent = true;
      }
      return;
   }

   u32_offset = sst_BLKS_session.u32_nextAbsFrame * sst_BLKS_session.u8_chunkSize;
   u32_expLen = MIN((uint32_t)sst_BLKS_session.u8_chunkSize,
      sst_BLKS_session.u32_totalLen - u32_offset);

   // Check if the chunk has the size implied by START
   if (stpt_frame->u_body.st_data.u8_dataLen != u32_expLen)
   {
      APP_LOG_ERR("RX transfer %u: frame %u has %u bytes, expected %u", sst_BLKS_session.u8_xferId,
         sst_BLKS_session.u32_nextAbsFrame, stpt_frame->u_body.st_data.u8_dataLen, u32_expLen);
      sv_RxAbort(eBS_PROTOCOL_ERROR);
      return;
   }

   // Check if the application's sink accepted the chunk
   if (sst_BLKS_cfg.fpt_onRxData(sst_BLKS_session.u8_appType, u32_offset,
      stpt_frame->u_body.st_data.u8pt_data, stpt_frame->u_body.st_data.u8_dataLen) != 0)
   {
      sv_RxAbort(eBS_SINK_ERROR);
      return;
   }

   sst_BLKS_session.u32_crc32 = crc32_ieee_update(sst_BLKS_session.u32_crc32,
      stpt_frame->u_body.st_data.u8pt_data, stpt_frame->u_body.st_data.u8_dataLen);
   sst_BLKS_session.u32_nextAbsFrame++;
   sst_BLKS_session.u8_sinceAck++;
   sst_BLKS_session.b_nackSent = false;

   // Check if this was the last frame (END replaces the final ACK)
   if (sst_BLKS_session.u32_nextAbsFrame == sst_BLKS_session.u32_totalFrames)
   {
      sv_RxComplete();
      return;
   }

   u8_ackEvery = MAX(sst_BLKS_session.u8_window / 2U, 1U);

   // Check if half a window arrived since the last ACK
   if (sst_BLKS_session.u8_sinceAck >= u8_ackEvery)
   {
      sv_RxSendAck();
   }
   // Otherwise make sure a delayed ACK is scheduled. A running timer is
   // not restarted, or a trickle of frames would postpone it indefinitely.
   else if (k_timer_remaining_get(&sst_BLKS_ackTimer) == 0U)
   {
      k_timer_start(&sst_BLKS_ackTimer, K_MSEC(BLK_RX_ACK_DELAY_MS), K_NO_WAIT);
   }
}

#if defined(CONFIG_BT_GATT_CLIENT)
/**
 * @private       sv_SrvMtuExchanged
 * @brief         Log the result of the ATT MTU exchange.
 * @param[in]     stpt_conn Connection.
 * @param[in]     u8_err ATT error (0 on success).
 * @param[in]     stpt_params Exchange parameters (unused).
 * @return        void
 */
static void sv_SrvMtuExchanged(struct bt_conn *stpt_conn, uint8_t u8_err,
   struct bt_gatt_exchange_params *stpt_params)
{
   ARG_UNUSED(stpt_params);

   APP_LOG_INF("MTU exchange %s, ATT MTU %u", (u8_err == 0U) ? "done" : "failed",
      bt_gatt_get_mtu(stpt_conn));
}
#endif // CONFIG_BT_GATT_CLIENT

/******************************************************************************/
/*                                                                            */
/*                        PUBLIC FUNCTION DEFINITIONS                         */
/*                                                                            */
/******************************************************************************/
/**
 * @public        gv_BLKS_EnginePre
 * @brief         Engine pass, first half. Called once per wake-up by the core
 *                engine thread, with gst_BLK_lock held, before the Client's
 *                EnginePre and before gv_BLKS_EnginePost().
 *
 *                Steps, in order:
 *                1. Owed completion: if gv_BLKS_OnDisconnected() dropped an
 *                   active transfer, report it now through fpt_onRxDone with
 *                   eBS_DISCONNECTED. The disconnect runs in BT context, where
 *                   application callbacks must not be called.
 *                2. Events: take and clear each Server event bit. The bit is
 *                   cleared even without an active session, which discards
 *                   events left over from a finished transfer.
 *                   - eBE_SRV_ABORT_REQ (gv_BLKS_AbortRx()): send ABORT and
 *                     report eBS_ABORTED. Checked first so that an abort wins
 *                     over an ACK raised in the same pass.
 *                   - eBE_SRV_ACK_DUE (delayed-ACK timer): send a cumulative
 *                     ACK.
 *                   - eBE_SRV_IDLE (inactivity timer): send ABORT and report
 *                     eBS_TIMEOUT.
 *                3. Queued frames: drain sst_BLKS_fifo without blocking. A
 *                   frame whose connection generation differs from
 *                   st_BLKS_connGen was queued before a disconnect or
 *                   reconnect; it is dropped. Every other frame is decoded and
 *                   dispatched (START / DATA / ABORT / short message).
 *                   Every block goes back to sst_BLKS_slab either way.
 *
 *                Application callbacks (fpt_onRxDone, fpt_onRxStart,
 *                fpt_onRxData, fpt_onRxShort) run from here, on the engine
 *                thread, with gst_BLK_lock held. Sending a control frame can
 *                block for up to BLK_CTRL_TX_TIMEOUT_MS while it waits for a
 *                credit.
 * @return        void
 */
void gv_BLKS_EnginePre(void)
{
   BlkFrameBlock_T *stpt_blk = NULL;

   // Recorded by gv_BLKS_OnDisconnected(), which runs in BT context where
   // callbacks must not be called
   if (sst_BLKS_pendingDone.b_pending)
   {
      sst_BLKS_pendingDone.b_pending = false;

      // Check if the application wants RX completions
      if (sst_BLKS_cfg.fpt_onRxDone != NULL)
      {
         sst_BLKS_cfg.fpt_onRxDone(sst_BLKS_pendingDone.u8_appType, eBS_DISCONNECTED,
            sst_BLKS_pendingDone.u32_totalLen);
      }
   }

   // Bits are cleared even when the session check fails, so stale events
   // are discarded. Abort first, so it wins over an ACK in the same pass.

   // Check if the application aborted the incoming transfer
   if (atomic_test_and_clear_bit(&gt_BLK_events, eBE_SRV_ABORT_REQ) && sst_BLKS_session.b_active)
   {
      sv_RxAbort(eBS_ABORTED);
   }

   // Check if a delayed ACK is due
   if (atomic_test_and_clear_bit(&gt_BLK_events, eBE_SRV_ACK_DUE) && sst_BLKS_session.b_active)
   {
      sv_RxSendAck();
   }

   // Check if the incoming transfer went silent
   if (atomic_test_and_clear_bit(&gt_BLK_events, eBE_SRV_IDLE) && sst_BLKS_session.b_active)
   {
      APP_LOG_WRN("RX transfer %u timed out", sst_BLKS_session.u8_xferId);
      sv_RxAbort(eBS_TIMEOUT);
   }

   while ((stpt_blk = k_fifo_get(&sst_BLKS_fifo, K_NO_WAIT)) != NULL)
   {
      // Check if the frame belongs to the current connection
      if (stpt_blk->t_connGen == atomic_get(&st_BLKS_connGen))
      {
         sv_SrvHandleFrame(stpt_blk->u8ar_data, stpt_blk->u16_len);
      }

      k_mem_slab_free(&sst_BLKS_slab, stpt_blk);
   }
}

/**
 * @public        gv_BLKS_EnginePost
 * @brief         Engine pass, second half. Called once per wake-up by the core
 *                engine thread, with gst_BLK_lock held, after both roles'
 *                EnginePre and before the Client's EnginePost.
 *
 *                Handles one event, eBE_SRV_OVERFLOW. gt_BLKS_DataWriteHook()
 *                raises it when sst_BLKS_slab has no free block, so a written
 *                frame had to be dropped.
 *                - The bit is always cleared. A NACK is sent only if a
 *                  transfer is still active; otherwise the event is stale.
 *                - The NACK asks the client to go back to u32_nextAbsFrame,
 *                  with reason eBS_NO_RESOURCES. The client never sees the
 *                  hook's ATT error (Write Without Response), so this NACK
 *                  is its only recovery signal.
 *                - It runs here, not in gv_BLKS_EnginePre(), because the
 *                  queue has been drained by then. u32_nextAbsFrame then
 *                  already counts every frame queued before the drop, so
 *                  the client does not resend frames that were delivered.
 *                - It is sent even if a gap NACK is already outstanding
 *                  (b_nackSent is not checked). Afterwards b_nackSent is set,
 *                  so that gaps caused by the same drop are not NACKed again.
 *
 *                Sending the NACK can block for up to BLK_CTRL_TX_TIMEOUT_MS
 *                while it waits for a credit.
 * @return        void
 */
void gv_BLKS_EnginePost(void)
{
   uint8_t u8ar_frame[BLK_CTRL_FRAME_MAX_LEN];
   uint16_t u16_len = 0U;

   // Check if the write hook had to drop a frame of the active transfer
   if (atomic_test_and_clear_bit(&gt_BLK_events, eBE_SRV_OVERFLOW) && sst_BLKS_session.b_active)
   {
      // With Write Without Response the client never sees the hook's ATT
      // error, so this NACK is its only recovery signal
      u16_len = gu16_BLK_EncodeNack(u8ar_frame, sizeof(u8ar_frame),
         sst_BLKS_session.u8_xferId, (uint8_t)sst_BLKS_session.u32_nextAbsFrame,
         eBS_NO_RESOURCES);
      (void)si_SrvSendCtrl(u8ar_frame, u16_len);
      sst_BLKS_session.b_nackSent = true;
   }
}

/**
 * @public        gi_BLKS_Init
 * @brief         Initialise the Server role. Call once, from thread context,
 *                before the first connection is reported through
 *                gv_BLKS_OnConnected().
 *
 *                - Copies *stpt_cfg into sst_BLKS_cfg; the caller's struct
 *                  need not outlive the call. Only stpt_ctrlAttr is
 *                  mandatory. The callbacks are optional:
 *                  - fpt_onRxData NULL: every START is refused (eBS_REJECTED).
 *                  - fpt_onRxStart NULL: every valid START is accepted.
 *                  - fpt_onRxDone / fpt_onRxShort NULL: not reported.
 *                - Clears the session and any owed completion.
 *                - Starts the shared engine thread. This is idempotent, so
 *                  the Client role may be initialised before or after.
 *
 *                The engine reads sst_BLKS_cfg without the lock. This is safe
 *                because the configuration is written once, before
 *                sb_BLKS_initialized is set, and there is no de-init.
 * @param[in]     stpt_cfg Configuration (copied). stpt_ctrlAttr is required.
 * @return        0 on success.
 *                -EINVAL if stpt_cfg or stpt_cfg->stpt_ctrlAttr is NULL.
 *                -EALREADY if the Server is already initialised (the stored
 *                configuration is left unchanged).
 */
int gi_BLKS_Init(const BlkSrvCfg_T *stpt_cfg)
{
   // Check if the configuration names the CTRL characteristic
   if ((stpt_cfg == NULL) || (stpt_cfg->stpt_ctrlAttr == NULL))
   {
      return -EINVAL;
   }

   (void)k_mutex_lock(&gst_BLK_lock, K_FOREVER);

   // Check if the Server is already running
   if (sb_BLKS_initialized)
   {
      k_mutex_unlock(&gst_BLK_lock);
      return -EALREADY;
   }

   // The engine reads sst_BLKS_cfg without further locking. That is safe
   // only because it is written once, before sb_BLKS_initialized is set.
   sst_BLKS_cfg = *stpt_cfg;
   (void)memset(&sst_BLKS_session, 0, sizeof(sst_BLKS_session));
   (void)memset(&sst_BLKS_pendingDone, 0, sizeof(sst_BLKS_pendingDone));
   sb_BLKS_initialized = true;

   k_mutex_unlock(&gst_BLK_lock);

   gv_BLK_EngineStart();

   return 0;
}

/**
 * @public        gv_BLKS_OnConnected
 * @brief         Bind the Server to a new connection. Call from the
 *                application's bt_conn_cb.connected callback, only when the
 *                connection succeeded (err == 0). Runs in the BT context.
 *
 *                - One connection at a time. The call is ignored (with a
 *                  warning) if gi_BLKS_Init() has not run yet or another
 *                  connection is already bound. An ignored connection is not
 *                  bound later: its writes to DATA are dropped silently.
 *                - Takes its own reference on stpt_conn, released in
 *                  gv_BLKS_OnDisconnected().
 *                - Increments st_BLKS_connGen, then publishes the connection
 *                  to the write hook (st_BLKS_hookConn). In this order, every
 *                  frame the hook accepts for the new link carries the new
 *                  generation.
 *                - If b_autoTuneLink is set, requests 2M PHY and maximum data
 *                  length (gv_BLK_TuneLink()). With CONFIG_BT_GATT_CLIENT, it
 *                  also starts an ATT MTU exchange; the result is only
 *                  logged. Both are done outside gst_BLK_lock because they
 *                  may block in the BT stack. Their results are not waited
 *                  for.
 *
 *                A transfer can start only after the client has subscribed to
 *                CTRL notifications. Until then, START frames are ignored.
 * @param[in]     stpt_conn New connection.
 * @return        void
 */
void gv_BLKS_OnConnected(struct bt_conn *stpt_conn)
{
   bool b_tune = false;

   (void)k_mutex_lock(&gst_BLK_lock, K_FOREVER);

   // Check if the Server is ready and not already serving a connection
   if (!sb_BLKS_initialized || (sstpt_BLKS_conn != NULL))
   {
      k_mutex_unlock(&gst_BLK_lock);
      APP_LOG_WRN("ignored (not initialised or a connection is already bound)");
      return;
   }

   // Own reference, released in gv_BLKS_OnDisconnected()
   sstpt_BLKS_conn = bt_conn_ref(stpt_conn);

   // Bumped before publishing st_BLKS_hookConn so the first frame accepted
   // for this link already carries the new generation
   (void)atomic_inc(&st_BLKS_connGen);

   // Lock-free copy for the write hook. Only compared, never dereferenced.
   (void)atomic_ptr_set(&st_BLKS_hookConn, stpt_conn);

   b_tune = sst_BLKS_cfg.b_autoTuneLink;
   k_mutex_unlock(&gst_BLK_lock);

   // Check if the Server should request throughput-oriented link settings
   if (b_tune)
   {
      gv_BLK_TuneLink(stpt_conn);
#if defined(CONFIG_BT_GATT_CLIENT)
      // Static: the stack keeps this pointer until the callback runs
      static struct bt_gatt_exchange_params slst_BLKS_mtuParams = { .func = sv_SrvMtuExchanged };

      (void)bt_gatt_exchange_mtu(stpt_conn, &slst_BLKS_mtuParams);
#endif // CONFIG_BT_GATT_CLIENT
   }
}

/**
 * @public        gv_BLKS_OnDisconnected
 * @brief         Release the connection and fail a running transfer with
 *                eBS_DISCONNECTED. Normally reached through
 *                gv_BLK_OnDisconnected(), from the application's
 *                bt_conn_cb.disconnected callback (BT context).
 *
 *                - Ignored if stpt_conn is NULL or is not the bound
 *                  connection. The caller need not filter.
 *                - Unpublishes the connection from the write hook and
 *                  increments st_BLKS_connGen. Frames still queued for the
 *                  old link are discarded (and freed) by the next
 *                  gv_BLKS_EnginePre().
 *                - Clears all Server events and stops both timers, so that
 *                  nothing left over acts on a later connection.
 *                - If a transfer was active, marks the session inactive and
 *                  records the completion in sst_BLKS_pendingDone. The engine
 *                  thread reports it through fpt_onRxDone(eBS_DISCONNECTED),
 *                  because callbacks must not run in BT context. No ABORT is
 *                  sent; the link is gone.
 *                - Restores the full notification credit budget.
 *                  sv_SrvNotifyComplete() is never called for notifications
 *                  lost with the link.
 *                - Drops the connection reference taken in
 *                  gv_BLKS_OnConnected() and wakes the engine.
 * @param[in]     stpt_conn Connection that went down.
 * @return        void
 */
void gv_BLKS_OnDisconnected(struct bt_conn *stpt_conn)
{
   (void)k_mutex_lock(&gst_BLK_lock, K_FOREVER);

   // Check if this is the connection the Server is bound to
   if ((stpt_conn == NULL) || (stpt_conn != sstpt_BLKS_conn))
   {
      k_mutex_unlock(&gst_BLK_lock);
      return;
   }

   // Stop queueing frames for this link and invalidate those already queued
   (void)atomic_ptr_set(&st_BLKS_hookConn, NULL);
   (void)atomic_inc(&st_BLKS_connGen);

   atomic_clear_bit(&gt_BLK_events, eBE_SRV_ACK_DUE);
   atomic_clear_bit(&gt_BLK_events, eBE_SRV_IDLE);
   atomic_clear_bit(&gt_BLK_events, eBE_SRV_OVERFLOW);
   atomic_clear_bit(&gt_BLK_events, eBE_SRV_ABORT_REQ);
   k_timer_stop(&sst_BLKS_ackTimer);
   k_timer_stop(&sst_BLKS_idleTimer);

   // Callbacks cannot run in BT context: record it for the engine
   if (sst_BLKS_session.b_active)
   {
      sst_BLKS_pendingDone.b_pending = true;
      sst_BLKS_pendingDone.u8_appType = sst_BLKS_session.u8_appType;
      sst_BLKS_pendingDone.u32_totalLen = sst_BLKS_session.u32_totalLen;
      sst_BLKS_session.b_active = false;
   }

   // Notifications lost with the link never complete: restore all credits
   sv_SrvResetCredits();

   bt_conn_unref(sstpt_BLKS_conn);
   sstpt_BLKS_conn = NULL;
   k_mutex_unlock(&gst_BLK_lock);

   gv_BLK_Kick();
}

/**
 * @public        gi_BLKS_SendShort
 * @brief         Notify a single-frame application message [len][type][data]
 *                on CTRL. Synchronous. Thread context only; not ISR-safe.
 *
 *                - Best effort: no ACK, no retransmission. 0 means that the
 *                  host accepted the notification, not that the client
 *                  received it.
 *                - Can be used while a transfer is running. It shares the
 *                  notification credits with the ACK / NACK / END / ABORT
 *                  frames. A burst of short messages can therefore delay
 *                  those frames.
 *                - The connection checks are made under gst_BLK_lock. The
 *                  credit wait is made without it, holding a connection
 *                  reference, so the engine keeps running meanwhile. From
 *                  inside a BulkXfer callback the engine already holds the
 *                  lock. The engine is then blocked for up to t_timeout, so
 *                  use K_NO_WAIT there.
 *                - vpt_data is copied before the call returns.
 * @param[in]     u8_appType Application type (0x00..BLK_APP_TYPE_MAX).
 * @param[in]     vpt_data Payload. May be NULL only if u8_len is 0.
 * @param[in]     u8_len Payload length (<= gu16_BLKS_GetMaxShortPayload()).
 * @param[in]     t_timeout Maximum wait for a free credit.
 * @return        0 on success (notification queued in the host).
 *                -EPERM if gi_BLKS_Init() has not run.
 *                -EINVAL if u8_appType > BLK_APP_TYPE_MAX, or vpt_data is
 *                NULL with u8_len > 0.
 *                -ENOTCONN if no connection is bound.
 *                -EACCES if the client is not subscribed to CTRL.
 *                -EMSGSIZE if the frame does not fit the current ATT MTU.
 *                -EAGAIN if no credit became free within t_timeout.
 *                Any other negative value is a bt_gatt_notify_cb() error.
 *                The credit is returned in that case.
 */
int gi_BLKS_SendShort(uint8_t u8_appType, const void *vpt_data, uint8_t u8_len,
   k_timeout_t t_timeout)
{
   uint8_t u8ar_frame[BLK_MAX_FRAME_LEN];
   struct bt_conn *stpt_conn = NULL;
   uint16_t u16_frameLen = 0U;
   int i_ret = 0;

   // Check if the Server is running and the arguments are valid
   if (!sb_BLKS_initialized)
   {
      return -EPERM;
   }
   if ((u8_appType > BLK_APP_TYPE_MAX) || ((vpt_data == NULL) && (u8_len != 0U)))
   {
      return -EINVAL;
   }

   (void)k_mutex_lock(&gst_BLK_lock, K_FOREVER);

   // Check if there is a subscribed connection with a large enough MTU
   if (sstpt_BLKS_conn == NULL)
   {
      i_ret = -ENOTCONN;
   }
   else if (!bt_gatt_is_subscribed(sstpt_BLKS_conn, sst_BLKS_cfg.stpt_ctrlAttr, BT_GATT_CCC_NOTIFY))
   {
      i_ret = -EACCES;
   }
   else if (((uint16_t)u8_len + BLK_FRAME_HDR_LEN) > gu16_BLK_FrameCapacity(sstpt_BLKS_conn))
   {
      i_ret = -EMSGSIZE;
   }
   else
   {
      // Own reference: the credit wait below runs without the lock
      stpt_conn = bt_conn_ref(sstpt_BLKS_conn);
   }

   k_mutex_unlock(&gst_BLK_lock);

   // Check if the preconditions failed
   if (i_ret != 0)
   {
      return i_ret;
   }

   u16_frameLen = gu16_BLK_EncodeShort(u8ar_frame, sizeof(u8ar_frame), u8_appType,
      (const uint8_t *)vpt_data, u8_len);

   // Wait outside the lock so the engine keeps running. Called from a
   // BulkXfer callback, this blocks the engine for up to t_timeout.
   if (k_sem_take(&sst_BLKS_credits, t_timeout) != 0)
   {
      i_ret = -EAGAIN;
   }
   else
   {
      i_ret = si_SrvNotifyWithCredit(stpt_conn, u8ar_frame, u16_frameLen);
   }

   bt_conn_unref(stpt_conn);

   return i_ret;
}

/**
 * @public        gv_BLKS_AbortRx
 * @brief         Request cancellation of the incoming transfer. Asynchronous;
 *                safe from any context, including ISRs and BulkXfer
 *                callbacks (it only sets an event bit and wakes the engine).
 *
 *                The engine acts on the request in its next
 *                gv_BLKS_EnginePre(). It sends ABORT(eBAD_BY_RECEIVER) to the
 *                client and reports fpt_onRxDone(eBS_ABORTED).
 *
 *                The request applies to whatever transfer is active when the
 *                engine handles it. It has no effect in these cases:
 *                - No transfer is active at that time.
 *                - The transfer ends first (completed, timed out or
 *                  disconnected). fpt_onRxDone then reports that result
 *                  instead.
 *                A request is never kept for a later transfer.
 * @return        void
 */
void gv_BLKS_AbortRx(void)
{
   atomic_set_bit(&gt_BLK_events, eBE_SRV_ABORT_REQ);
   gv_BLK_Kick();
}

/**
 * @public        gb_BLKS_IsRxBusy
 * @brief         Whether an incoming transfer is in progress. Takes
 *                gst_BLK_lock, so thread context only (not ISR-safe). May be
 *                called from BulkXfer callbacks (the lock is recursive).
 *
 *                The value is a snapshot and can change right after the
 *                call. The flag is cleared before fpt_onRxDone runs, so:
 *                - Inside fpt_onRxDone it already returns false.
 *                - After a disconnect it returns false immediately, even
 *                  though fpt_onRxDone(eBS_DISCONNECTED) is delivered later
 *                  by the engine.
 * @return        true from the accepted START until the transfer ends,
 *                false otherwise.
 */
bool gb_BLKS_IsRxBusy(void)
{
   bool b_busy = false;

   (void)k_mutex_lock(&gst_BLK_lock, K_FOREVER);
   b_busy = sst_BLKS_session.b_active;
   k_mutex_unlock(&gst_BLK_lock);

   return b_busy;
}

/**
 * @public        gu16_BLKS_GetMaxShortPayload
 * @brief         Largest payload gi_BLKS_SendShort() accepts on the current
 *                link. Takes gst_BLK_lock, so thread context only (not
 *                ISR-safe).
 *
 *                Computed as min(ATT_MTU - 3, BLK_MAX_FRAME_LEN) minus the
 *                2-byte frame header. The ATT MTU is often 23 until the MTU
 *                exchange completes. Query again after the exchange, and do
 *                not cache the value across connections.
 * @return        Payload size in bytes (at most BLK_MAX_SHORT_PAYLOAD).
 *                0 if no connection is bound.
 */
uint16_t gu16_BLKS_GetMaxShortPayload(void)
{
   uint16_t u16_cap = 0U;

   (void)k_mutex_lock(&gst_BLK_lock, K_FOREVER);

   // Check if there is a connection to size against
   if (sstpt_BLKS_conn != NULL)
   {
      u16_cap = gu16_BLK_FrameCapacity(sstpt_BLKS_conn);
      u16_cap = (u16_cap > BLK_FRAME_HDR_LEN) ? (uint16_t)(u16_cap - BLK_FRAME_HDR_LEN) : 0U;
   }

   k_mutex_unlock(&gst_BLK_lock);

   return u16_cap;
}

/**
 * @public        gv_BLKS_GetCaps
 * @brief         Fill the capability record served by the optional Caps
 *                characteristic. Call it from that characteristic's read
 *                handler. Uses compile-time values only, so it needs no
 *                lock and no gi_BLKS_Init(), and is safe from any context.
 *
 *                - u8_protocolVersion: BLK_PROTOCOL_VERSION, so that a client
 *                  can check wire compatibility before it sends START.
 *                - u8_maxFrameLen: BLK_MAX_FRAME_LEN, capped at 255 to fit
 *                  the field. The usable size on a link is further limited
 *                  by the ATT MTU.
 *                - u8_window: BLK_WINDOW_DEFAULT. This is the largest window
 *                  the Server grants; a larger START window is reduced to it.
 *                - u8_reserved: 0.
 * @param[out]    stpt_caps Destination. Must not be NULL (asserted).
 * @return        void
 */
void gv_BLKS_GetCaps(BlkCaps_T *stpt_caps)
{
   __ASSERT(stpt_caps != NULL, "gv_BLKS_GetCaps: stpt_caps is NULL");

   stpt_caps->u8_protocolVersion = BLK_PROTOCOL_VERSION;
   stpt_caps->u8_maxFrameLen = (uint8_t)MIN(BLK_MAX_FRAME_LEN, 255U);
   stpt_caps->u8_window = BLK_WINDOW_DEFAULT;
   stpt_caps->u8_reserved = 0U;
}

/**
 * @public        gt_BLKS_DataWriteHook
 * @brief         Post-write hook for the DATA characteristic. Matches
 *                GATTCustomWriteCb_F; forward to it from the hook named in the
 *                configurator. Runs in the BLE RX thread and never blocks.
 *
 *                gt_GATT_GenericWrite has already copied the frame into the
 *                descriptor's buffer; that copy is ignored here. The frame is
 *                taken from vpt_buf / u16_length, which always hold exactly
 *                the bytes of this write.
 *
 *                The hook only validates and queues the frame; it does not
 *                decode it. Checks, in order:
 *                1. Prepare (long) writes and non-zero offsets are refused.
 *                2. Writes from any connection other than the bound one,
 *                   or before gi_BLKS_Init(), are ignored silently. The
 *                   connection is compared lock-free with st_BLKS_hookConn
 *                   and is never dereferenced.
 *                3. The length must match the frame's len byte and must not
 *                   exceed BLK_MAX_FRAME_LEN (gb_BLK_FrameLenValid()).
 *                4. A block from sst_BLKS_slab is needed. If none is free, the
 *                   frame is dropped and eBE_SRV_OVERFLOW is raised, so that
 *                   gv_BLKS_EnginePost() NACKs it.
 *                A frame that passes is copied into the block, tagged with
 *                the current st_BLKS_connGen, put into sst_BLKS_fifo, and the
 *                engine is woken.
 *
 *                The client normally uses Write Without Response, so it never
 *                sees the ATT errors below. They only matter for Write
 *                Requests. For DATA frames, recovery relies on NACK and on
 *                the client's ACK timeout.
 *
 * @param[in]     stpt_connHandle Connection that wrote.
 * @param[in]     stpt_attr DATA attribute (unused).
 * @param[in]     vpt_buf Written bytes (one frame).
 * @param[in]     u16_length Number of bytes.
 * @param[in]     u16_offset Must be 0 (long writes are not supported).
 * @param[in]     u8_flags Write flags; prepare writes are refused.
 * @return        0 if the frame was queued, or ignored (not initialised /
 *                not the bound connection); the generic return value is
 *                kept.
 *                BT_GATT_ERR(BT_ATT_ERR_WRITE_NOT_PERMITTED) for a prepare
 *                write.
 *                BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET) if u16_offset != 0.
 *                BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN) if the length
 *                does not match the len byte.
 *                BT_GATT_ERR(BT_ATT_ERR_INSUFFICIENT_RESOURCES) if the RX
 *                queue is full.
 */
ssize_t gt_BLKS_DataWriteHook(struct bt_conn *stpt_connHandle,
   const struct bt_gatt_attr *stpt_attr, const void *vpt_buf, uint16_t u16_length,
   uint16_t u16_offset, uint8_t u8_flags)
{
   BlkFrameBlock_T *stpt_blk = NULL;

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
   if (!sb_BLKS_initialized || (stpt_connHandle != atomic_ptr_get(&st_BLKS_hookConn)))
   {
      return 0;
   }

   // Check if the frame length is consistent with its len byte
   if (!gb_BLK_FrameLenValid((const uint8_t *)vpt_buf, u16_length))
   {
      return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
   }

   // Check if a queue block is free; otherwise let the engine NACK
   if (k_mem_slab_alloc(&sst_BLKS_slab, (void **)&stpt_blk, K_NO_WAIT) != 0)
   {
      atomic_set_bit(&gt_BLK_events, eBE_SRV_OVERFLOW);
      gv_BLK_Kick();
      return BT_GATT_ERR(BT_ATT_ERR_INSUFFICIENT_RESOURCES);
   }

   stpt_blk->t_connGen = atomic_get(&st_BLKS_connGen);
   stpt_blk->u16_len = u16_length;
   (void)memcpy(stpt_blk->u8ar_data, vpt_buf, u16_length);
   k_fifo_put(&sst_BLKS_fifo, stpt_blk);
   gv_BLK_Kick();

   return 0;
}

#endif // BLK_ENABLE_SERVER
