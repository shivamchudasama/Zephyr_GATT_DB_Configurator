/**
 * @file          BulkXfer_Client.c
 * @brief         BulkXfer Client role: service discovery, CTRL subscription,
 *                TX session state machine and Write Without Response sender.
 *
 *                Attach sequence (BT context, one step per callback):
 *                  [MTU exchange] -> primary service -> characteristics
 *                  (DATA, CTRL) -> CCC of CTRL -> subscribe -> ready.
 *                The callbacks never take gst_BLK_lock: the result is posted
 *                through st_BLKC_attachStatus + eBE_CLI_ATTACH_DONE and applied
 *                by the engine, which then calls fpt_onReady.
 *
 *                Flow control: sst_BLKC_credits counts DATA writes handed to
 *                the host but not yet sent. A credit is taken before every
 *                bt_gatt_write_without_response_cb() and returned in its
 *                completion callback, which keeps the controller queue full
 *                without ever blocking on host buffer allocation.
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

#if BLK_ENABLE_CLIENT

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

/******************************************************************************/
/*                                                                            */
/*                                   ENUMS                                    */
/*                                                                            */
/******************************************************************************/
/**
 * @enum          BlkTxState_E
 * @brief         States of the outgoing transfer. __packed makes it one byte
 *                so it sits with the uint8_t members of BlkTxSession_T
 *                without padding.
 */
typedef enum __packed
{
   eBTS_IDLE = 0,                            /**< No transfer.                            */
   eBTS_START_PENDING,                       /**< gi_BLKC_Send() accepted, START not sent.*/
   eBTS_START_SENT,                          /**< START sent, waiting for ACK(seq 0).     */
   eBTS_SENDING,                             /**< DATA flowing, waiting for END.          */
} BlkTxState_E;

/**
 * @enum          BlkCliState_E
 * @brief         Binding of the Client to a connection.
 */
typedef enum
{
   eBCS_IDLE = 0,                            /**< No connection bound.                    */
   eBCS_ATTACHING,                           /**< Discovery / subscription running.       */
   eBCS_READY,                               /**< Transfers may be started.               */
} BlkCliState_E;

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
   BlkTxState_E e_state;                     /**< Current state of the outgoing transfer. */
   uint8_t u8_xferId;                        /**< Transfer ID carried in every frame.     */
   uint8_t u8_appType;                       /**< Application type given to gi_BLKC_Send. */
   uint8_t u8_chunkSize;                     /**< Payload bytes per DATA frame.           */
   uint8_t u8_window;                        /**< Max unacknowledged frames in flight.    */
   uint8_t u8_retries;                       /**< Consecutive ACK timeouts so far.        */
   uint32_t u32_totalLen;                    /**< Total object length in bytes.           */
   uint32_t u32_crc32;                       /**< CRC-32 of the whole object.             */
   uint32_t u32_totalFrames;                 /**< DATA frames needed for the object.      */
   uint32_t u32_nextAbsFrame;                /**< Next frame to transmit.                 */
   uint32_t u32_ackedAbsFrame;               /**< All frames below this are acknowledged. */
   BlkSource_T st_source;                    /**< Data provider for the object bytes.     */
} BlkTxSession_T;

BUILD_ASSERT(sizeof(BlkTxState_E) == 1U, "BlkTxState_E must be 1 byte (__packed)");

/**
 * @struct        BlkCliPending_T
 * @brief         Callbacks owed for work cut short by a disconnect (BT
 *                context), reported from the engine thread.
 */
typedef struct
{
   bool b_txPending;                         /**< fpt_onTxDone owed for a dropped TX.     */
   uint8_t u8_txAppType;                     /**< Application type of the dropped TX.     */
   struct bt_conn *stpt_readyConn;           /**< fpt_onReady(-ENOTCONN) owed (ref held). */
} BlkCliPending_T;

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
static void sv_CliTimerExpiry(struct k_timer *stpt_timer);
static void sv_CliWriteComplete(struct bt_conn *stpt_conn, void *vpt_userData);
static void sv_CliResetCredits(void);
static int si_CliWriteWithCredit(struct bt_conn *stpt_conn, const uint8_t *u8pt_buf,
   uint16_t u16_len);
static int si_CliSendCtrl(const uint8_t *u8pt_buf, uint16_t u16_len);
static void sv_CliRelease(void);
static void sv_CliAttachDone(int i_status);
static void sv_CliApplyAttach(void);
static int si_CliDiscoverService(struct bt_conn *stpt_conn);
static uint8_t su8_CliDiscoverCb(struct bt_conn *stpt_conn, const struct bt_gatt_attr *stpt_attr,
   struct bt_gatt_discover_params *stpt_params);
static void sv_CliSubscribed(struct bt_conn *stpt_conn, uint8_t u8_err,
   struct bt_gatt_subscribe_params *stpt_params);
static uint8_t su8_CliCtrlNotify(struct bt_conn *stpt_conn,
   struct bt_gatt_subscribe_params *stpt_params, const void *vpt_data, uint16_t u16_length);
static void sv_CliMtuExchanged(struct bt_conn *stpt_conn, uint8_t u8_err,
   struct bt_gatt_exchange_params *stpt_params);
static void sv_CliHandleFrame(const uint8_t *u8pt_buf, uint16_t u16_len);
static void sv_TxFinish(BlkStatus_E e_status, bool b_sendAbort);
static void sv_TxOnTimeout(void);
static void sv_TxOnAck(const BlkFrame_T *stpt_frame);
static void sv_TxOnNack(const BlkFrame_T *stpt_frame);
static void sv_TxPump(void);
static int si_RamSourceRead(void *vpt_ctx, uint32_t u32_offset, uint8_t *u8pt_buf,
   uint16_t u16_len);

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
 * @var           sst_BLKC_credits
 * @brief         DATA writes that may still be handed to the host.
 */
static K_SEM_DEFINE(sst_BLKC_credits, BLK_CLI_WRITE_INFLIGHT_MAX, BLK_CLI_WRITE_INFLIGHT_MAX);

/**
 * @var           sst_BLKC_fifo
 * @brief         CTRL notifications, notify callback -> engine.
 */
static K_FIFO_DEFINE(sst_BLKC_fifo);

/**
 * @var           sst_BLKC_slab
 * @brief         Storage for queued CTRL notifications.
 */
K_MEM_SLAB_DEFINE_STATIC(sst_BLKC_slab, sizeof(BlkFrameBlock_T), BLK_CLI_CTRL_POOL_DEPTH, 4);

/**
 * @var           sst_BLKC_ackTimer
 * @brief         ACK progress timeout (raises eBE_CLI_TIMEOUT).
 */
static K_TIMER_DEFINE(sst_BLKC_ackTimer, sv_CliTimerExpiry, NULL);

/**
 * @var           sst_BLKC_retryTimer
 * @brief         Back-off after the host ran out of buffers (raises
 *                eBE_CLI_RETRY).
 */
static K_TIMER_DEFINE(sst_BLKC_retryTimer, sv_CliTimerExpiry, NULL);

/**
 * @var           sst_BLKC_cfg
 * @brief         Copy of the configuration passed to gi_BLKC_Init(), with the
 *                default UUIDs filled in.
 */
static BlkCliCfg_T sst_BLKC_cfg;

/**
 * @var           sst_BLKC_defSvcUuid
 * @brief         Default service UUID. File scope: BT_UUID_DECLARE_128() is a
 *                compound literal and would not outlive gi_BLKC_Init().
 */
static const struct bt_uuid_128 sst_BLKC_defSvcUuid = BT_UUID_INIT_128(BT_UUID_BLK_SVC_VAL);

/**
 * @var           sst_BLKC_defDataUuid
 * @brief         Default DATA characteristic UUID.
 */
static const struct bt_uuid_128 sst_BLKC_defDataUuid = BT_UUID_INIT_128(BT_UUID_BLK_DATA_VAL);

/**
 * @var           sst_BLKC_defCtrlUuid
 * @brief         Default CTRL characteristic UUID.
 */
static const struct bt_uuid_128 sst_BLKC_defCtrlUuid = BT_UUID_INIT_128(BT_UUID_BLK_CTRL_VAL);

/**
 * @var           sb_BLKC_initialized
 * @brief         Set once gi_BLKC_Init() has succeeded.
 */
static bool sb_BLKC_initialized = false;

/**
 * @var           se_BLKC_state
 * @brief         Binding state. Guarded by gst_BLK_lock.
 */
static BlkCliState_E se_BLKC_state = eBCS_IDLE;

/**
 * @var           sstpt_BLKC_conn
 * @brief         Bound connection (referenced). Guarded by gst_BLK_lock.
 */
static struct bt_conn *sstpt_BLKC_conn = NULL;

/**
 * @var           st_BLKC_hookConn
 * @brief         Lock-free copy of sstpt_BLKC_conn for BT-context callbacks
 *                (not referenced, only compared).
 */
static atomic_ptr_t st_BLKC_hookConn = ATOMIC_PTR_INIT(NULL);

/**
 * @var           st_BLKC_connGen
 * @brief         Incremented on every attach / release to discard stale
 *                frames and stale attach results.
 */
static atomic_t st_BLKC_connGen = ATOMIC_INIT(0);

/**
 * @var           st_BLKC_attachStatus
 * @brief         Result posted by the attach sequence (0 or negative errno).
 */
static atomic_t st_BLKC_attachStatus = ATOMIC_INIT(0);

/**
 * @var           st_BLKC_attachGen
 * @brief         st_BLKC_connGen at the time the attach result was posted.
 */
static atomic_t st_BLKC_attachGen = ATOMIC_INIT(0);

/**
 * @var           su16_BLKC_svcEnd
 * @brief         Last handle of the discovered service.
 */
static uint16_t su16_BLKC_svcEnd = 0U;

/**
 * @var           su16_BLKC_dataHandle
 * @brief         Value handle of the remote DATA characteristic.
 */
static uint16_t su16_BLKC_dataHandle = 0U;

/**
 * @var           su16_BLKC_ctrlHandle
 * @brief         Value handle of the remote CTRL characteristic.
 */
static uint16_t su16_BLKC_ctrlHandle = 0U;

/**
 * @var           su16_BLKC_ctrlEnd
 * @brief         Last handle of the CTRL characteristic (search range of its
 *                CCC); 0 until known.
 */
static uint16_t su16_BLKC_ctrlEnd = 0U;

/**
 * @var           sst_BLKC_discoverParams
 * @brief         Discovery parameters; the stack keeps the pointer until the
 *                last callback.
 */
static struct bt_gatt_discover_params sst_BLKC_discoverParams;

/**
 * @var           sst_BLKC_subscribeParams
 * @brief         CTRL subscription; the stack keeps the pointer while
 *                subscribed.
 */
static struct bt_gatt_subscribe_params sst_BLKC_subscribeParams;

/**
 * @var           sst_BLKC_mtuParams
 * @brief         MTU exchange parameters.
 */
static struct bt_gatt_exchange_params sst_BLKC_mtuParams = { .func = sv_CliMtuExchanged };

/**
 * @var           sst_BLKC_session
 * @brief         State of the outgoing transfer.
 */
static BlkTxSession_T sst_BLKC_session;

/**
 * @var           sst_BLKC_pending
 * @brief         Callbacks recorded at disconnect, reported by the engine.
 */
static BlkCliPending_T sst_BLKC_pending;

/**
 * @var           su8_BLKC_xferCounter
 * @brief         Source of the xferId of the next outgoing transfer.
 */
static uint8_t su8_BLKC_xferCounter = 0U;

/**
 * @var           su8ar_BLKC_frame
 * @brief         DATA frame build buffer (engine thread only).
 */
static uint8_t su8ar_BLKC_frame[BLK_MAX_FRAME_LEN];

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
 * @private       sv_CliTimerExpiry
 * @brief         Expiry handler of the Client timers (ISR context).
 * @param[in]     stpt_timer Expired timer.
 * @return        void
 */
static void sv_CliTimerExpiry(struct k_timer *stpt_timer)
{
   atomic_set_bit(&gt_BLK_events,
      (stpt_timer == &sst_BLKC_ackTimer) ? eBE_CLI_TIMEOUT : eBE_CLI_RETRY);
   gv_BLK_Kick();
}

/**
 * @private       sv_CliWriteComplete
 * @brief         DATA write handed to the controller: return its credit.
 * @note          Not called for writes lost with the link;
 *                sv_CliResetCredits() covers that case.
 * @param[in]     stpt_conn Connection (unused).
 * @param[in]     vpt_userData Unused.
 * @return        void
 */
static void sv_CliWriteComplete(struct bt_conn *stpt_conn, void *vpt_userData)
{
   ARG_UNUSED(stpt_conn);
   ARG_UNUSED(vpt_userData);

   k_sem_give(&sst_BLKC_credits);
   gv_BLK_Kick();
}

/**
 * @private       sv_CliResetCredits
 * @brief         Restore the full credit budget (after a disconnect).
 * @return        void
 */
static void sv_CliResetCredits(void)
{
   uint32_t u32_idx = 0U;

   k_sem_reset(&sst_BLKC_credits);

   for (u32_idx = 0U; u32_idx < BLK_CLI_WRITE_INFLIGHT_MAX; u32_idx++)
   {
      k_sem_give(&sst_BLKC_credits);
   }
}

/**
 * @private       si_CliWriteWithCredit
 * @brief         Write one frame to the remote DATA characteristic without
 *                response. The caller must already hold a credit; it is
 *                returned here if the write fails.
 * @param[in]     stpt_conn Connection.
 * @param[in]     u8pt_buf Frame (copied by the host before returning).
 * @param[in]     u16_len Frame length.
 * @return        0 on success or the negative errno of
 *                bt_gatt_write_without_response_cb().
 */
static int si_CliWriteWithCredit(struct bt_conn *stpt_conn, const uint8_t *u8pt_buf,
   uint16_t u16_len)
{
   int i_ret = bt_gatt_write_without_response_cb(stpt_conn, su16_BLKC_dataHandle, u8pt_buf,
      u16_len, false, sv_CliWriteComplete, NULL);

   // Check if the write was rejected; its credit is then still ours
   if (i_ret != 0)
   {
      k_sem_give(&sst_BLKC_credits);
   }

   return i_ret;
}

/**
 * @private       si_CliSendCtrl
 * @brief         Write a control frame (ABORT) on the bound connection,
 *                waiting up to BLK_CTRL_TX_TIMEOUT_MS for a credit. Engine
 *                thread only.
 * @param[in]     u8pt_buf Frame.
 * @param[in]     u16_len Frame length (0 = encoding failed, nothing sent).
 * @return        0 on success, negative errno otherwise.
 */
static int si_CliSendCtrl(const uint8_t *u8pt_buf, uint16_t u16_len)
{
   int i_ret = 0;

   // Check if there is a ready link and a valid frame
   if ((sstpt_BLKC_conn == NULL) || (se_BLKC_state != eBCS_READY) || (u16_len == 0U))
   {
      return -ENOTCONN;
   }

   // Check if a credit becomes available in time
   if (k_sem_take(&sst_BLKC_credits, K_MSEC(BLK_CTRL_TX_TIMEOUT_MS)) != 0)
   {
      APP_LOG_WRN("no credit for control frame 0x%02x", u8pt_buf[1]);
      return -EAGAIN;
   }

   i_ret = si_CliWriteWithCredit(sstpt_BLKC_conn, u8pt_buf, u16_len);

   // Check if the host refused the control frame
   if (i_ret != 0)
   {
      APP_LOG_WRN("control frame 0x%02x not sent (%d)", u8pt_buf[1], i_ret);
   }

   return i_ret;
}

/**
 * @private       sv_CliRelease
 * @brief         Unbind the connection without releasing its reference (the
 *                caller owns it afterwards). Caller holds gst_BLK_lock.
 * @return        void
 */
static void sv_CliRelease(void)
{
   (void)atomic_ptr_set(&st_BLKC_hookConn, NULL);
   (void)atomic_inc(&st_BLKC_connGen);

   atomic_clear_bit(&gt_BLK_events, eBE_CLI_TIMEOUT);
   atomic_clear_bit(&gt_BLK_events, eBE_CLI_RETRY);
   atomic_clear_bit(&gt_BLK_events, eBE_CLI_ABORT_REQ);
   atomic_clear_bit(&gt_BLK_events, eBE_CLI_ATTACH_DONE);
   k_timer_stop(&sst_BLKC_ackTimer);
   k_timer_stop(&sst_BLKC_retryTimer);

   // Writes lost with the link never complete: restore all credits
   sv_CliResetCredits();

   se_BLKC_state = eBCS_IDLE;
   sstpt_BLKC_conn = NULL;
}

/**
 * @private       sv_CliAttachDone
 * @brief         Post the attach result for the engine (BT context, no lock).
 * @param[in]     i_status 0 or negative errno.
 * @return        void
 */
static void sv_CliAttachDone(int i_status)
{
   (void)atomic_set(&st_BLKC_attachStatus, i_status);
   (void)atomic_set(&st_BLKC_attachGen, atomic_get(&st_BLKC_connGen));
   atomic_set_bit(&gt_BLK_events, eBE_CLI_ATTACH_DONE);
   gv_BLK_Kick();
}

/**
 * @private       sv_CliApplyAttach
 * @brief         Apply a posted attach result and report it (engine thread).
 * @return        void
 */
static void sv_CliApplyAttach(void)
{
   struct bt_conn *stpt_conn = sstpt_BLKC_conn;
   int i_status = (int)atomic_get(&st_BLKC_attachStatus);

   // Check if the result belongs to the attach still in progress
   if ((se_BLKC_state != eBCS_ATTACHING)
      || (atomic_get(&st_BLKC_attachGen) != atomic_get(&st_BLKC_connGen)))
   {
      return;
   }

   // Check if the Client is ready to send
   if (i_status == 0)
   {
      se_BLKC_state = eBCS_READY;
      APP_LOG_INF("ready: DATA 0x%04x, CTRL 0x%04x, ATT MTU %u", su16_BLKC_dataHandle,
         su16_BLKC_ctrlHandle, bt_gatt_get_mtu(stpt_conn));
   }
   else
   {
      APP_LOG_WRN("attach failed (%d)", i_status);
      sv_CliRelease();
   }

   // Check if the application wants the attach result
   if (sst_BLKC_cfg.fpt_onReady != NULL)
   {
      sst_BLKC_cfg.fpt_onReady(stpt_conn, i_status);
   }

   // Check if the reference was handed over by sv_CliRelease()
   if (i_status != 0)
   {
      bt_conn_unref(stpt_conn);
   }
}

/**
 * @private       si_CliDiscoverService
 * @brief         First discovery step: the BulkXfer primary service.
 * @param[in]     stpt_conn Connection.
 * @return        Result of bt_gatt_discover().
 */
static int si_CliDiscoverService(struct bt_conn *stpt_conn)
{
   (void)memset(&sst_BLKC_discoverParams, 0, sizeof(sst_BLKC_discoverParams));
   sst_BLKC_discoverParams.uuid = sst_BLKC_cfg.stpt_svcUuid;
   sst_BLKC_discoverParams.func = su8_CliDiscoverCb;
   sst_BLKC_discoverParams.start_handle = BT_ATT_FIRST_ATTRIBUTE_HANDLE;
   sst_BLKC_discoverParams.end_handle = BT_ATT_LAST_ATTRIBUTE_HANDLE;
   sst_BLKC_discoverParams.type = BT_GATT_DISCOVER_PRIMARY;

   return bt_gatt_discover(stpt_conn, &sst_BLKC_discoverParams);
}

/**
 * @private       su8_CliDiscoverCb
 * @brief         Discovery callback for all three steps (BT context).
 *                stpt_attr == NULL marks the end of a step.
 * @param[in]     stpt_conn Connection.
 * @param[in]     stpt_attr Found attribute, or NULL when the step is done.
 * @param[in]     stpt_params Discovery parameters (sst_BLKC_discoverParams).
 * @return        BT_GATT_ITER_CONTINUE or BT_GATT_ITER_STOP.
 */
static uint8_t su8_CliDiscoverCb(struct bt_conn *stpt_conn, const struct bt_gatt_attr *stpt_attr,
   struct bt_gatt_discover_params *stpt_params)
{
   const struct bt_gatt_service_val *stpt_svc = NULL;
   const struct bt_gatt_chrc *stpt_chrc = NULL;
   int i_ret = 0;

   // Check if the attach this belongs to is still current
   if (stpt_conn != atomic_ptr_get(&st_BLKC_hookConn))
   {
      return BT_GATT_ITER_STOP;
   }

   switch (stpt_params->type)
   {
      case BT_GATT_DISCOVER_PRIMARY:
         // Check if the peer hosts the service
         if (stpt_attr == NULL)
         {
            sv_CliAttachDone(-ENOENT);
            return BT_GATT_ITER_STOP;
         }

         stpt_svc = (const struct bt_gatt_service_val *)stpt_attr->user_data;
         su16_BLKC_svcEnd = stpt_svc->end_handle;

         stpt_params->uuid = NULL;
         stpt_params->start_handle = stpt_attr->handle + 1U;
         stpt_params->end_handle = su16_BLKC_svcEnd;
         stpt_params->type = BT_GATT_DISCOVER_CHARACTERISTIC;
         break;

      case BT_GATT_DISCOVER_CHARACTERISTIC:
         // Collect DATA and CTRL; the declaration after CTRL bounds CTRL's
         // descriptors
         if (stpt_attr != NULL)
         {
            stpt_chrc = (const struct bt_gatt_chrc *)stpt_attr->user_data;

            if ((su16_BLKC_ctrlHandle != 0U) && (su16_BLKC_ctrlEnd == 0U))
            {
               su16_BLKC_ctrlEnd = stpt_attr->handle - 1U;
            }

            if ((bt_uuid_cmp(stpt_chrc->uuid, sst_BLKC_cfg.stpt_dataUuid) == 0)
               && ((stpt_chrc->properties & BT_GATT_CHRC_WRITE_WITHOUT_RESP) != 0U))
            {
               su16_BLKC_dataHandle = stpt_chrc->value_handle;
            }
            else if ((bt_uuid_cmp(stpt_chrc->uuid, sst_BLKC_cfg.stpt_ctrlUuid) == 0)
               && ((stpt_chrc->properties & BT_GATT_CHRC_NOTIFY) != 0U))
            {
               su16_BLKC_ctrlHandle = stpt_chrc->value_handle;
            }
            return BT_GATT_ITER_CONTINUE;
         }

         // Check if both characteristics exist with the needed properties
         if ((su16_BLKC_dataHandle == 0U) || (su16_BLKC_ctrlHandle == 0U))
         {
            sv_CliAttachDone(-ENOENT);
            return BT_GATT_ITER_STOP;
         }

         stpt_params->uuid = BT_UUID_GATT_CCC;
         stpt_params->start_handle = su16_BLKC_ctrlHandle + 1U;
         stpt_params->end_handle = (su16_BLKC_ctrlEnd != 0U) ? su16_BLKC_ctrlEnd : su16_BLKC_svcEnd;
         stpt_params->type = BT_GATT_DISCOVER_DESCRIPTOR;
         break;

      default:
         // Check if CTRL has a CCC
         if (stpt_attr == NULL)
         {
            sv_CliAttachDone(-ENOENT);
            return BT_GATT_ITER_STOP;
         }

         (void)memset(&sst_BLKC_subscribeParams, 0, sizeof(sst_BLKC_subscribeParams));
         sst_BLKC_subscribeParams.notify = su8_CliCtrlNotify;
         sst_BLKC_subscribeParams.subscribe = sv_CliSubscribed;
         sst_BLKC_subscribeParams.value_handle = su16_BLKC_ctrlHandle;
         sst_BLKC_subscribeParams.ccc_handle = stpt_attr->handle;
         sst_BLKC_subscribeParams.value = BT_GATT_CCC_NOTIFY;
         // Volatile: dropped at disconnect even when bonded, so the next
         // attach can reuse these parameters
         atomic_set_bit(sst_BLKC_subscribeParams.flags, BT_GATT_SUBSCRIBE_FLAG_VOLATILE);

         i_ret = bt_gatt_subscribe(stpt_conn, &sst_BLKC_subscribeParams);

         // -EALREADY: already subscribed, and sv_CliSubscribed() will not run
         if ((i_ret != 0) && (i_ret != -EALREADY))
         {
            sv_CliAttachDone(-EIO);
         }
         else if (i_ret == -EALREADY)
         {
            sv_CliAttachDone(0);
         }
         return BT_GATT_ITER_STOP;
   }

   i_ret = bt_gatt_discover(stpt_conn, stpt_params);

   // Check if the next discovery step could be started
   if (i_ret != 0)
   {
      sv_CliAttachDone(i_ret);
   }

   return BT_GATT_ITER_STOP;
}

/**
 * @private       sv_CliSubscribed
 * @brief         CCC write of the CTRL subscription completed (BT context).
 * @param[in]     stpt_conn Connection.
 * @param[in]     u8_err ATT error (0 on success).
 * @param[in]     stpt_params Subscription parameters (unused).
 * @return        void
 */
static void sv_CliSubscribed(struct bt_conn *stpt_conn, uint8_t u8_err,
   struct bt_gatt_subscribe_params *stpt_params)
{
   ARG_UNUSED(stpt_params);

   // Check if the attach this belongs to is still current
   if (stpt_conn == atomic_ptr_get(&st_BLKC_hookConn))
   {
      sv_CliAttachDone((u8_err == 0U) ? 0 : -EIO);
   }
}

/**
 * @private       su8_CliCtrlNotify
 * @brief         CTRL notification from the server (BLE RX thread). Only
 *                validates and queues the frame; never blocks.
 * @param[in]     stpt_conn Connection.
 * @param[in]     stpt_params Subscription parameters (unused).
 * @param[in]     vpt_data Frame, or NULL when the subscription was removed.
 * @param[in]     u16_length Frame length.
 * @return        BT_GATT_ITER_CONTINUE, or BT_GATT_ITER_STOP once removed.
 */
static uint8_t su8_CliCtrlNotify(struct bt_conn *stpt_conn,
   struct bt_gatt_subscribe_params *stpt_params, const void *vpt_data, uint16_t u16_length)
{
   BlkFrameBlock_T *stpt_blk = NULL;

   ARG_UNUSED(stpt_params);

   // Check if the subscription is being removed
   if (vpt_data == NULL)
   {
      return BT_GATT_ITER_STOP;
   }

   // Check if the notification comes from the bound connection
   if (stpt_conn != atomic_ptr_get(&st_BLKC_hookConn))
   {
      return BT_GATT_ITER_CONTINUE;
   }

   // Check if the frame length is consistent with its len byte
   if (!gb_BLK_FrameLenValid((const uint8_t *)vpt_data, u16_length))
   {
      APP_LOG_WRN("malformed CTRL notification (len %u)", (unsigned)u16_length);
      return BT_GATT_ITER_CONTINUE;
   }

   // Check if a queue block is free. A lost ACK is covered by the next one,
   // a lost NACK / END by the ACK timeout.
   if (k_mem_slab_alloc(&sst_BLKC_slab, (void **)&stpt_blk, K_NO_WAIT) != 0)
   {
      APP_LOG_WRN("CTRL queue full, frame 0x%02x dropped", ((const uint8_t *)vpt_data)[1]);
      return BT_GATT_ITER_CONTINUE;
   }

   stpt_blk->t_connGen = atomic_get(&st_BLKC_connGen);
   stpt_blk->u16_len = u16_length;
   (void)memcpy(stpt_blk->u8ar_data, vpt_data, u16_length);
   k_fifo_put(&sst_BLKC_fifo, stpt_blk);
   gv_BLK_Kick();

   return BT_GATT_ITER_CONTINUE;
}

/**
 * @private       sv_CliMtuExchanged
 * @brief         MTU exchange finished (BT context): continue with discovery
 *                whatever the result.
 * @param[in]     stpt_conn Connection.
 * @param[in]     u8_err ATT error (0 on success).
 * @param[in]     stpt_params Exchange parameters (unused).
 * @return        void
 */
static void sv_CliMtuExchanged(struct bt_conn *stpt_conn, uint8_t u8_err,
   struct bt_gatt_exchange_params *stpt_params)
{
   int i_ret = 0;

   ARG_UNUSED(stpt_params);

   APP_LOG_INF("MTU exchange %s, ATT MTU %u", (u8_err == 0U) ? "done" : "failed",
      bt_gatt_get_mtu(stpt_conn));

   // Check if the attach this belongs to is still current
   if (stpt_conn != atomic_ptr_get(&st_BLKC_hookConn))
   {
      return;
   }

   i_ret = si_CliDiscoverService(stpt_conn);

   // Check if discovery could be started
   if (i_ret != 0)
   {
      sv_CliAttachDone(i_ret);
   }
}

/**
 * @private       sv_CliHandleFrame
 * @brief         Decode one CTRL notification and dispatch it. Frames that
 *                belong on DATA (START, DATA, sender ABORT) are dropped.
 * @param[in]     u8pt_buf Frame bytes.
 * @param[in]     u16_len Frame length.
 * @return        void
 */
static void sv_CliHandleFrame(const uint8_t *u8pt_buf, uint16_t u16_len)
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
      if (sst_BLKC_cfg.fpt_onRxShort != NULL)
      {
         sst_BLKC_cfg.fpt_onRxShort(st_frame.u8_type, st_frame.u8pt_payload,
            st_frame.u8_payloadLen);
      }
      return;
   }

   switch (st_frame.u8_type)
   {
      case eBFT_ACK:
         sv_TxOnAck(&st_frame);
         break;

      case eBFT_NACK:
         sv_TxOnNack(&st_frame);
         break;

      case eBFT_END:
         // Check if the END refers to the outgoing transfer
         if ((sst_BLKC_session.e_state >= eBTS_START_SENT)
            && (st_frame.u_body.st_end.u8_xferId == sst_BLKC_session.u8_xferId))
         {
            sv_TxFinish((BlkStatus_E)st_frame.u_body.st_end.u8_status, false);
         }
         break;

      case eBFT_ABORT:
         // Check if the receiver cancelled the outgoing transfer
         if ((st_frame.u_body.st_abort.u8_dir == eBAD_BY_RECEIVER)
            && (sst_BLKC_session.e_state >= eBTS_START_SENT)
            && (st_frame.u_body.st_abort.u8_xferId == sst_BLKC_session.u8_xferId))
         {
            sv_TxFinish((st_frame.u_body.st_abort.u8_reason == eBS_REJECTED)
               ? eBS_REJECTED : eBS_REMOTE_ABORTED, false);
         }
         break;

      default:
         APP_LOG_WRN("frame 0x%02x is not valid on CTRL", st_frame.u8_type);
         break;
   }
}

/**
 * @private       sv_TxFinish
 * @brief         End the outgoing transfer and report e_status.
 * @param[in]     e_status Result.
 * @param[in]     b_sendAbort true to tell the receiver with an ABORT frame.
 * @return        void
 */
static void sv_TxFinish(BlkStatus_E e_status, bool b_sendAbort)
{
   uint8_t u8_appType = sst_BLKC_session.u8_appType;

   // Check if the receiver must be told that the transfer is cancelled
   if (b_sendAbort)
   {
      uint8_t u8ar_frame[BLK_CTRL_FRAME_MAX_LEN];
      uint16_t u16_len = gu16_BLK_EncodeAbort(u8ar_frame, sizeof(u8ar_frame),
         sst_BLKC_session.u8_xferId, (uint8_t)e_status, eBAD_BY_SENDER);

      (void)si_CliSendCtrl(u8ar_frame, u16_len);
   }

   k_timer_stop(&sst_BLKC_ackTimer);
   k_timer_stop(&sst_BLKC_retryTimer);

   // Idle before the callback, so fpt_onTxDone can start the next transfer
   sst_BLKC_session.e_state = eBTS_IDLE;

   APP_LOG_INF("TX transfer %u done, status %u", sst_BLKC_session.u8_xferId, (unsigned)e_status);

   // Check if the application wants TX completions
   if (sst_BLKC_cfg.fpt_onTxDone != NULL)
   {
      sst_BLKC_cfg.fpt_onTxDone(u8_appType, e_status);
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
   if ((sst_BLKC_session.e_state != eBTS_START_SENT)
      && (sst_BLKC_session.e_state != eBTS_SENDING))
   {
      return;
   }

   sst_BLKC_session.u8_retries++;

   // Check if the peer has been silent for too long
   if (sst_BLKC_session.u8_retries > BLK_TX_MAX_RETRIES)
   {
      sv_TxFinish(eBS_TIMEOUT, true);
      return;
   }

   APP_LOG_WRN("TX transfer %u: ACK timeout, retry %u", sst_BLKC_session.u8_xferId,
      sst_BLKC_session.u8_retries);

   // Check if START itself was never acknowledged (the receiver re-ACKs a
   // repeated START)
   if (sst_BLKC_session.e_state == eBTS_START_SENT)
   {
      sst_BLKC_session.e_state = eBTS_START_PENDING;
   }
   // Go-Back-N: resend from the first unacknowledged frame
   else
   {
      sst_BLKC_session.u32_nextAbsFrame = sst_BLKC_session.u32_ackedAbsFrame;
   }

   k_timer_start(&sst_BLKC_ackTimer, K_MSEC(BLK_TX_ACK_TIMEOUT_MS), K_NO_WAIT);
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
   if ((stpt_frame->u_body.st_ack.u8_xferId != sst_BLKC_session.u8_xferId)
      || (sst_BLKC_session.e_state < eBTS_START_SENT))
   {
      return;
   }

   // Check if this is the receiver accepting START
   if (sst_BLKC_session.e_state == eBTS_START_SENT)
   {
      // Check if the ACK is the expected START acknowledgement (seq 0)
      if (stpt_frame->u_body.st_ack.u8_seq != 0U)
      {
         return;
      }

      // The window is negotiated only here: min of both sides, <= 128
      u8_window = stpt_frame->u_body.st_ack.u8_window;
      u8_window = CLAMP(u8_window, 1U, BLK_SEQ_HALF_RANGE);
      sst_BLKC_session.u8_window = MIN(sst_BLKC_session.u8_window, u8_window);
      sst_BLKC_session.e_state = eBTS_SENDING;
      sst_BLKC_session.u8_retries = 0U;
      k_timer_start(&sst_BLKC_ackTimer, K_MSEC(BLK_TX_ACK_TIMEOUT_MS), K_NO_WAIT);
      return;
   }

   // Cumulative: seq is the receiver's next expected frame. The last frame
   // is confirmed by END, not by an ACK.
   u32_abs = gu32_BLK_SeqToAbs(stpt_frame->u_body.st_ack.u8_seq, sst_BLKC_session.u32_ackedAbsFrame);

   // Check if the ACK acknowledges new frames that were actually sent.
   // Duplicates must not restart the timer, or stale ACKs would keep a
   // stuck transfer alive.
   if ((u32_abs > sst_BLKC_session.u32_ackedAbsFrame)
      && (u32_abs <= sst_BLKC_session.u32_nextAbsFrame))
   {
      sst_BLKC_session.u32_ackedAbsFrame = u32_abs;
      sst_BLKC_session.u8_retries = 0U;
      k_timer_start(&sst_BLKC_ackTimer, K_MSEC(BLK_TX_ACK_TIMEOUT_MS), K_NO_WAIT);
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
   if ((stpt_frame->u_body.st_nack.u8_xferId != sst_BLKC_session.u8_xferId)
      || (sst_BLKC_session.e_state != eBTS_SENDING))
   {
      return;
   }

   u32_abs = gu32_BLK_SeqToAbs(stpt_frame->u_body.st_nack.u8_seq, sst_BLKC_session.u32_ackedAbsFrame);

   // Check if the requested frame lies within what was sent
   if (u32_abs <= sst_BLKC_session.u32_nextAbsFrame)
   {
      // The NACK seq also acknowledges every frame before it
      APP_LOG_DBG("TX transfer %u: NACK, resend from %u (reason %u)", sst_BLKC_session.u8_xferId,
         u32_abs, stpt_frame->u_body.st_nack.u8_reason);
      sst_BLKC_session.u32_ackedAbsFrame = u32_abs;
      sst_BLKC_session.u32_nextAbsFrame = u32_abs;
      k_timer_start(&sst_BLKC_ackTimer, K_MSEC(BLK_TX_ACK_TIMEOUT_MS), K_NO_WAIT);
   }
}

/**
 * @private       sv_TxPump
 * @brief         Write START (if pending) and as many DATA frames as the
 *                window and the credits allow. Never blocks.
 * @return        void
 */
static void sv_TxPump(void)
{
   uint32_t u32_offset = 0U;
   uint16_t u16_dataLen = 0U;
   uint16_t u16_frameLen = 0U;
   int i_ret = 0;

   // Check if there is a ready link to send on
   if ((sstpt_BLKC_conn == NULL) || (se_BLKC_state != eBCS_READY))
   {
      return;
   }

   // Check if START still has to be sent
   if (sst_BLKC_session.e_state == eBTS_START_PENDING)
   {
      // Check if a credit is free; otherwise the completion kick retries
      if (k_sem_take(&sst_BLKC_credits, K_NO_WAIT) != 0)
      {
         return;
      }

      u16_frameLen = gu16_BLK_EncodeStart(su8ar_BLKC_frame, sizeof(su8ar_BLKC_frame),
         sst_BLKC_session.u8_xferId, sst_BLKC_session.u8_appType,
         sst_BLKC_session.u32_totalLen, sst_BLKC_session.u8_chunkSize,
         sst_BLKC_session.u8_window, sst_BLKC_session.u32_crc32);
      i_ret = si_CliWriteWithCredit(sstpt_BLKC_conn, su8ar_BLKC_frame, u16_frameLen);

      // Check if the host is temporarily out of buffers. Retry on a timer:
      // there may be no pending completion to kick the engine.
      if ((i_ret == -ENOMEM) || (i_ret == -EAGAIN))
      {
         k_timer_start(&sst_BLKC_retryTimer, K_MSEC(BLK_WRITE_RETRY_MS), K_NO_WAIT);
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

      sst_BLKC_session.e_state = eBTS_START_SENT;
      k_timer_start(&sst_BLKC_ackTimer, K_MSEC(BLK_TX_ACK_TIMEOUT_MS), K_NO_WAIT);
      return;
   }

   // Check if the transfer is in its data phase
   if (sst_BLKC_session.e_state != eBTS_SENDING)
   {
      return;
   }

   while ((sst_BLKC_session.u32_nextAbsFrame < sst_BLKC_session.u32_totalFrames)
      && ((sst_BLKC_session.u32_nextAbsFrame - sst_BLKC_session.u32_ackedAbsFrame)
         < sst_BLKC_session.u8_window))
   {
      // Check if a credit is free; otherwise the completion kick resumes
      if (k_sem_take(&sst_BLKC_credits, K_NO_WAIT) != 0)
      {
         break;
      }

      u32_offset = sst_BLKC_session.u32_nextAbsFrame * sst_BLKC_session.u8_chunkSize;
      u16_dataLen = (uint16_t)MIN((uint32_t)sst_BLKC_session.u8_chunkSize,
         sst_BLKC_session.u32_totalLen - u32_offset);
      u16_frameLen = gu16_BLK_EncodeDataHeader(su8ar_BLKC_frame, sizeof(su8ar_BLKC_frame),
         sst_BLKC_session.u8_xferId, (uint8_t)sst_BLKC_session.u32_nextAbsFrame, u16_dataLen);

      // Read the chunk straight into the frame, behind the header (no copy)
      i_ret = sst_BLKC_session.st_source.fpt_read(sst_BLKC_session.st_source.vpt_ctx, u32_offset,
         &su8ar_BLKC_frame[BLK_DATA_HDR_LEN], u16_dataLen);

      // Check if the application's source failed
      if (i_ret != 0)
      {
         k_sem_give(&sst_BLKC_credits);
         APP_LOG_ERR("source read failed at offset %u (%d)", u32_offset, i_ret);
         sv_TxFinish(eBS_SOURCE_ERROR, true);
         return;
      }

      i_ret = si_CliWriteWithCredit(sstpt_BLKC_conn, su8ar_BLKC_frame, u16_frameLen);

      // Check if the host is temporarily out of buffers
      if ((i_ret == -ENOMEM) || (i_ret == -EAGAIN))
      {
         k_timer_start(&sst_BLKC_retryTimer, K_MSEC(BLK_WRITE_RETRY_MS), K_NO_WAIT);
         break;
      }

      // Check if the write failed for good
      if (i_ret != 0)
      {
         APP_LOG_ERR("DATA not sent (%d)", i_ret);
         sv_TxFinish((i_ret == -ENOTCONN) ? eBS_DISCONNECTED
            : eBS_PROTOCOL_ERROR, false);
         return;
      }

      sst_BLKC_session.u32_nextAbsFrame++;
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

/******************************************************************************/
/*                                                                            */
/*                        PUBLIC FUNCTION DEFINITIONS                         */
/*                                                                            */
/******************************************************************************/
/**
 * @public        gv_BLKC_EnginePre
 * @brief         Engine pass, first half. Called once per wake-up by the core
 *                engine thread, with gst_BLK_lock held, after the Server's
 *                EnginePre and before either role's EnginePost.
 *
 *                Steps, in order:
 *                1. Owed completions recorded by gv_BLKC_OnDisconnected(),
 *                   which runs in BT context where callbacks must not be
 *                   called:
 *                   - A dropped transfer: fpt_onTxDone(eBS_DISCONNECTED).
 *                   - A cut-short attach: fpt_onReady(conn, -ENOTCONN). The
 *                     connection reference kept for it is released after
 *                     the callback.
 *                2. Attach result (eBE_CLI_ATTACH_DONE), posted lock-free by
 *                   the discovery / subscription callbacks. It is ignored if
 *                   the Client is no longer attaching or st_BLKC_connGen has
 *                   moved on (stale). Otherwise success moves the Client to
 *                   eBCS_READY and failure releases the binding; fpt_onReady
 *                   reports either result.
 *                3. Events: each bit is taken and cleared even without a
 *                   running transfer, which discards stale events.
 *                   - eBE_CLI_ABORT_REQ (gv_BLKC_AbortTx()): report
 *                     eBS_ABORTED. ABORT(eBAD_BY_SENDER) is sent only if START
 *                     has already gone out. Checked first, so a timeout in the
 *                     same pass does not retransmit a cancelled transfer.
 *                   - eBE_CLI_TIMEOUT (ACK timer): resend START, or go back to
 *                     the first unacknowledged frame (Go-Back-N). After
 *                     BLK_TX_MAX_RETRIES consecutive timeouts, send ABORT and
 *                     report eBS_TIMEOUT.
 *                   - eBE_CLI_RETRY (host buffer back-off): only wakes the
 *                     engine; gv_BLKC_EnginePost() retries the write.
 *                4. Queued CTRL notifications: drain sst_BLKC_fifo without
 *                   blocking. Frames from an older connection generation are
 *                   dropped. The rest are dispatched: short messages to
 *                   fpt_onRxShort; ACK, NACK, END and receiver ABORT to the TX
 *                   state machine. Every block goes back to sst_BLKC_slab.
 *
 *                Application callbacks (fpt_onReady, fpt_onTxDone,
 *                fpt_onRxShort) run from here, on the engine thread, with
 *                gst_BLK_lock held. Sending ABORT can block for up to
 *                BLK_CTRL_TX_TIMEOUT_MS while it waits for a credit.
 * @return        void
 */
void gv_BLKC_EnginePre(void)
{
   BlkFrameBlock_T *stpt_blk = NULL;
   struct bt_conn *stpt_readyConn = NULL;

   // Recorded by gv_BLKC_OnDisconnected(), which runs in BT context where
   // callbacks must not be called
   if (sst_BLKC_pending.b_txPending)
   {
      sst_BLKC_pending.b_txPending = false;

      // Check if the application wants TX completions
      if (sst_BLKC_cfg.fpt_onTxDone != NULL)
      {
         sst_BLKC_cfg.fpt_onTxDone(sst_BLKC_pending.u8_txAppType, eBS_DISCONNECTED);
      }
   }

   // Check if an attach was cut short by the disconnect
   if (sst_BLKC_pending.stpt_readyConn != NULL)
   {
      stpt_readyConn = sst_BLKC_pending.stpt_readyConn;
      sst_BLKC_pending.stpt_readyConn = NULL;

      // Check if the application wants the attach result
      if (sst_BLKC_cfg.fpt_onReady != NULL)
      {
         sst_BLKC_cfg.fpt_onReady(stpt_readyConn, -ENOTCONN);
      }
      bt_conn_unref(stpt_readyConn);
   }

   // Check if the attach sequence posted its result
   if (atomic_test_and_clear_bit(&gt_BLK_events, eBE_CLI_ATTACH_DONE))
   {
      sv_CliApplyAttach();
   }

   // Bits are cleared even when the session check fails, so stale events
   // are discarded. Abort first, so a timeout in the same pass does not
   // retransmit a cancelled transfer.

   // Check if the application aborted the outgoing transfer
   if (atomic_test_and_clear_bit(&gt_BLK_events, eBE_CLI_ABORT_REQ)
      && (sst_BLKC_session.e_state != eBTS_IDLE))
   {
      sv_TxFinish(eBS_ABORTED, sst_BLKC_session.e_state != eBTS_START_PENDING);
   }

   // Check if the ACK timer expired
   if (atomic_test_and_clear_bit(&gt_BLK_events, eBE_CLI_TIMEOUT))
   {
      sv_TxOnTimeout();
   }

   // The retry event only needs to wake the engine; sv_TxPump() does the work
   (void)atomic_test_and_clear_bit(&gt_BLK_events, eBE_CLI_RETRY);

   while ((stpt_blk = k_fifo_get(&sst_BLKC_fifo, K_NO_WAIT)) != NULL)
   {
      // Check if the frame belongs to the current connection
      if (stpt_blk->t_connGen == atomic_get(&st_BLKC_connGen))
      {
         sv_CliHandleFrame(stpt_blk->u8ar_data, stpt_blk->u16_len);
      }

      k_mem_slab_free(&sst_BLKC_slab, stpt_blk);
   }
}

/**
 * @public        gv_BLKC_EnginePost
 * @brief         Engine pass, second half. Called once per wake-up by the core
 *                engine thread, with gst_BLK_lock held, last in the pass
 *                (after the Server's EnginePost).
 *
 *                Pumps the outgoing transfer. Running after
 *                gv_BLKC_EnginePre() means it uses the window space freed by
 *                the ACKs just processed. Does nothing unless the Client is
 *                eBCS_READY.
 *                - eBTS_START_PENDING: write START and arm the ACK timer. The
 *                  transfer waits in eBTS_START_SENT for ACK(seq 0), which
 *                  also sets the negotiated window.
 *                - eBTS_SENDING: write DATA frames while frames remain, fewer
 *                  than the window are unacknowledged, and a credit is free.
 *                  Each chunk is read from the source (fpt_read, on this
 *                  thread) straight into the frame buffer.
 *
 *                Never blocks. When it stops for lack of credits, the write
 *                completion of an earlier frame wakes the engine again. On
 *                -ENOMEM / -EAGAIN from the host, it arms the retry timer
 *                (BLK_WRITE_RETRY_MS) instead, since no completion may be
 *                pending. Other failures end the transfer through
 *                fpt_onTxDone:
 *                - Source read error: eBS_SOURCE_ERROR, ABORT sent.
 *                - Write error: eBS_DISCONNECTED for -ENOTCONN, otherwise
 *                  eBS_PROTOCOL_ERROR. No ABORT is sent.
 * @return        void
 */
void gv_BLKC_EnginePost(void)
{
   sv_TxPump();
}

/**
 * @public        gi_BLKC_Init
 * @brief         Initialise the Client role. Call once, from thread context,
 *                before the first gi_BLKC_Attach().
 *
 *                - Copies *stpt_cfg into sst_BLKC_cfg; the caller's struct
 *                  need not outlive the call. Custom UUIDs are stored by
 *                  pointer and must stay valid for the program's lifetime.
 *                  NULL UUIDs select the BulkXfer_Uuid.h defaults.
 *                - All callbacks are optional. A NULL callback is simply not
 *                  called.
 *                - Clears the session and any owed completion.
 *                - Starts the shared engine thread. This is idempotent, so
 *                  the Server role may be initialised before or after.
 *
 *                The engine and BT callbacks read sst_BLKC_cfg without the
 *                lock. This is safe because the configuration is written
 *                once, before sb_BLKC_initialized is set, and there is no
 *                de-init.
 * @param[in]     stpt_cfg Configuration (copied).
 * @return        0 on success.
 *                -EINVAL if stpt_cfg is NULL.
 *                -EALREADY if the Client is already initialised (the stored
 *                configuration is left unchanged).
 */
int gi_BLKC_Init(const BlkCliCfg_T *stpt_cfg)
{
   // Check if a configuration was given
   if (stpt_cfg == NULL)
   {
      return -EINVAL;
   }

   (void)k_mutex_lock(&gst_BLK_lock, K_FOREVER);

   // Check if the Client is already running
   if (sb_BLKC_initialized)
   {
      k_mutex_unlock(&gst_BLK_lock);
      return -EALREADY;
   }

   // The engine and the BT callbacks read sst_BLKC_cfg without further
   // locking. That is safe only because it is written once, here.
   sst_BLKC_cfg = *stpt_cfg;
   sst_BLKC_cfg.stpt_svcUuid = (stpt_cfg->stpt_svcUuid != NULL)
      ? stpt_cfg->stpt_svcUuid : &sst_BLKC_defSvcUuid.uuid;
   sst_BLKC_cfg.stpt_dataUuid = (stpt_cfg->stpt_dataUuid != NULL)
      ? stpt_cfg->stpt_dataUuid : &sst_BLKC_defDataUuid.uuid;
   sst_BLKC_cfg.stpt_ctrlUuid = (stpt_cfg->stpt_ctrlUuid != NULL)
      ? stpt_cfg->stpt_ctrlUuid : &sst_BLKC_defCtrlUuid.uuid;

   // Relies on eBTS_IDLE == 0
   (void)memset(&sst_BLKC_session, 0, sizeof(sst_BLKC_session));
   (void)memset(&sst_BLKC_pending, 0, sizeof(sst_BLKC_pending));
   sb_BLKC_initialized = true;

   k_mutex_unlock(&gst_BLK_lock);

   gv_BLK_EngineStart();

   return 0;
}

/**
 * @public        gi_BLKC_Attach
 * @brief         Bind the Client to a connection and start the attach
 *                sequence. Asynchronous. Thread context only; not ISR-safe.
 *                Works in either link role (central or peripheral).
 *
 *                - One connection at a time. Takes its own reference on
 *                  stpt_conn, released at disconnect or when the attach
 *                  fails.
 *                - Increments st_BLKC_connGen, then publishes the connection
 *                  to the BT callbacks (st_BLKC_hookConn). In this order,
 *                  every CTRL notification accepted for the new link carries
 *                  the new generation.
 *                - If b_autoTuneLink is set, requests 2M PHY and maximum data
 *                  length (gv_BLK_TuneLink()) and starts an ATT MTU exchange.
 *                  Discovery then starts from the exchange callback, whatever
 *                  its result. If the exchange cannot start (e.g. already
 *                  done), discovery starts directly. These stack calls may
 *                  block and are made without gst_BLK_lock.
 *                - Attach sequence, one step per BT callback: primary
 *                  service -> DATA (Write Without Response) and CTRL (Notify)
 *                  characteristics -> CCC of CTRL -> subscribe.
 *
 *                The result arrives through fpt_onReady on the engine thread:
 *                0 when ready; -ENOENT if the service, a characteristic or the
 *                CCC is missing; -EIO if the subscription failed; -ENOTCONN if
 *                the link dropped first; another negative errno if a later
 *                discovery step failed to start. On failure the binding is
 *                released, so the call can be retried.
 *
 *                If this function itself returns an error, fpt_onReady is not
 *                called.
 *
 *                The chunk size of a transfer follows the ATT MTU at
 *                gi_BLKC_Send() time. Without b_autoTuneLink, the application
 *                is responsible for the MTU exchange.
 * @param[in]     stpt_conn Connection to bind.
 * @return        0 if the attach sequence started.
 *                -EPERM if gi_BLKC_Init() has not run.
 *                -EINVAL if stpt_conn is NULL.
 *                -EALREADY if this connection is already attached or
 *                attaching.
 *                -EBUSY if another connection is bound.
 *                Any other negative value is a bt_gatt_discover() error; the
 *                binding is undone.
 */
int gi_BLKC_Attach(struct bt_conn *stpt_conn)
{
   bool b_tune = false;
   int i_ret = 0;

   // Check if the Client is running and the argument is valid
   if (!sb_BLKC_initialized)
   {
      return -EPERM;
   }
   if (stpt_conn == NULL)
   {
      return -EINVAL;
   }

   (void)k_mutex_lock(&gst_BLK_lock, K_FOREVER);

   // Check if a connection is already bound
   if (sstpt_BLKC_conn != NULL)
   {
      i_ret = (sstpt_BLKC_conn == stpt_conn) ? -EALREADY : -EBUSY;
      k_mutex_unlock(&gst_BLK_lock);
      return i_ret;
   }

   // Own reference, released at disconnect or on a failed attach
   sstpt_BLKC_conn = bt_conn_ref(stpt_conn);
   se_BLKC_state = eBCS_ATTACHING;
   su16_BLKC_svcEnd = 0U;
   su16_BLKC_dataHandle = 0U;
   su16_BLKC_ctrlHandle = 0U;
   su16_BLKC_ctrlEnd = 0U;

   // Bumped before publishing st_BLKC_hookConn so the first notification
   // accepted for this link already carries the new generation
   (void)atomic_inc(&st_BLKC_connGen);
   (void)atomic_ptr_set(&st_BLKC_hookConn, stpt_conn);

   b_tune = sst_BLKC_cfg.b_autoTuneLink;
   k_mutex_unlock(&gst_BLK_lock);

   // Stack calls may block: made without the lock
   if (b_tune)
   {
      gv_BLK_TuneLink(stpt_conn);

      // Check if the exchange started; discovery then follows from its
      // callback. -EALREADY (done before) falls through to discovery.
      if (bt_gatt_exchange_mtu(stpt_conn, &sst_BLKC_mtuParams) == 0)
      {
         return 0;
      }
   }

   i_ret = si_CliDiscoverService(stpt_conn);

   // Check if discovery failed to start: undo the binding
   if (i_ret != 0)
   {
      (void)k_mutex_lock(&gst_BLK_lock, K_FOREVER);

      // Check if a disconnect has not already released it
      if ((sstpt_BLKC_conn == stpt_conn) && (se_BLKC_state == eBCS_ATTACHING))
      {
         sv_CliRelease();
         bt_conn_unref(stpt_conn);
      }

      k_mutex_unlock(&gst_BLK_lock);
   }

   return i_ret;
}

/**
 * @public        gv_BLKC_OnDisconnected
 * @brief         Release the connection and fail whatever was in progress.
 *                Normally reached through gv_BLK_OnDisconnected(), from the
 *                application's bt_conn_cb.disconnected callback (BT context).
 *
 *                - Ignored if stpt_conn is NULL or is not the bound
 *                  connection. The caller need not filter.
 *                - If a transfer was running, marks it idle and records the
 *                  completion. The engine reports it through
 *                  fpt_onTxDone(eBS_DISCONNECTED), because callbacks must not
 *                  run in BT context. No ABORT is sent; the link is gone.
 *                - If an attach was in progress, the connection reference
 *                  moves to the owed fpt_onReady(conn, -ENOTCONN). The engine
 *                  releases it after the callback. Otherwise the reference
 *                  is released here.
 *                - Unbinds the connection: unpublishes it from the BT
 *                  callbacks and increments st_BLKC_connGen, so queued CTRL
 *                  frames and a late attach result for the old link are
 *                  discarded. Also clears the Client events, stops both
 *                  timers and restores the full credit budget
 *                  (sv_CliWriteComplete() is never called for writes lost
 *                  with the link).
 *                - Wakes the engine to deliver the owed callbacks.
 * @param[in]     stpt_conn Connection that went down.
 * @return        void
 */
void gv_BLKC_OnDisconnected(struct bt_conn *stpt_conn)
{
   (void)k_mutex_lock(&gst_BLK_lock, K_FOREVER);

   // Check if this is the connection the Client is bound to
   if ((stpt_conn == NULL) || (stpt_conn != sstpt_BLKC_conn))
   {
      k_mutex_unlock(&gst_BLK_lock);
      return;
   }

   // Callbacks cannot run in BT context: record them for the engine
   if (sst_BLKC_session.e_state != eBTS_IDLE)
   {
      sst_BLKC_pending.b_txPending = true;
      sst_BLKC_pending.u8_txAppType = sst_BLKC_session.u8_appType;
      sst_BLKC_session.e_state = eBTS_IDLE;
   }

   // Check if an attach was in progress: its reference moves to the
   // pending callback, which releases it
   if ((se_BLKC_state == eBCS_ATTACHING) && (sst_BLKC_pending.stpt_readyConn == NULL))
   {
      sst_BLKC_pending.stpt_readyConn = stpt_conn;
      sv_CliRelease();
   }
   else
   {
      sv_CliRelease();
      bt_conn_unref(stpt_conn);
   }

   k_mutex_unlock(&gst_BLK_lock);

   gv_BLK_Kick();
}

/**
 * @public        gi_BLKC_Send
 * @brief         Start a multi-frame transfer to the server. Asynchronous.
 *                Thread context only; not ISR-safe.
 *
 *                1. CRC pre-pass: the whole object is read once, in
 *                   BLK_CRC_READ_CHUNK pieces, in the caller's thread and
 *                   without gst_BLK_lock, so the engine keeps running. Called
 *                   from a BulkXfer callback (e.g. fpt_onTxDone), the pass
 *                   blocks the engine for its whole duration.
 *                2. Under the lock: check the link and that no transfer is
 *                   running, then set up the session and wake the engine,
 *                   which sends START.
 *                   - xferId: next value of a wrapping counter; it only has to
 *                     differ from the previous transfer.
 *                   - Chunk size: ATT frame capacity minus the DATA header,
 *                     fixed for the whole transfer even if the MTU grows.
 *                   - Window: BLK_WINDOW_DEFAULT, reduced to the receiver's
 *                     value when it acknowledges START.
 *
 *                The source is read again, on the engine thread, for sending
 *                and for every retransmission. It must stay readable and
 *                return the same bytes until fpt_onTxDone; otherwise the
 *                receiver reports eBS_CRC_ERROR. *stpt_source is copied, the
 *                context it points to is not.
 *
 *                fpt_onTxDone reports the result: the status carried by the
 *                receiver's END (eBS_OK on success), or eBS_REJECTED,
 *                eBS_REMOTE_ABORTED, eBS_ABORTED, eBS_TIMEOUT,
 *                eBS_SOURCE_ERROR, eBS_DISCONNECTED or eBS_PROTOCOL_ERROR.
 *                The session is idle before the callback runs, so the next
 *                transfer may be started from it.
 *
 * @param[in]     u8_appType Application type (0x00..BLK_APP_TYPE_MAX).
 * @param[in]     stpt_source Data source (copied).
 * @param[in]     u32_totalLen Object size in bytes. 0 is allowed: START is
 *                followed directly by the receiver's END.
 * @return        0 if the transfer was queued (fpt_onTxDone follows).
 *                -EPERM if gi_BLKC_Init() has not run.
 *                -EINVAL if stpt_source or its fpt_read is NULL, or
 *                u8_appType > BLK_APP_TYPE_MAX.
 *                -EIO if the source failed during the CRC pre-pass.
 *                -ENOTCONN if no connection is bound.
 *                -EAGAIN if the attach has not finished.
 *                -EBUSY if a transfer is already running.
 *                -EMSGSIZE if the ATT MTU leaves no room for a data byte.
 *                No callback follows an error return.
 */
int gi_BLKC_Send(uint8_t u8_appType, const BlkSource_T *stpt_source,
   uint32_t u32_totalLen)
{
   uint8_t u8ar_buf[BLK_CRC_READ_CHUNK];
   uint32_t u32_crc = 0U;
   uint32_t u32_offset = 0U;
   uint16_t u16_len = 0U;
   uint16_t u16_frameCap = 0U;
   int i_ret = 0;

   // Check if the Client is running and the arguments are valid
   if (!sb_BLKC_initialized)
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
      // Compared as uint32_t so a remainder above 65535 is not truncated;
      // the result never exceeds BLK_CRC_READ_CHUNK
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
   (void)k_mutex_lock(&gst_BLK_lock, K_FOREVER);

   // Check if there is a ready connection
   if (sstpt_BLKC_conn == NULL)
   {
      i_ret = -ENOTCONN;
   }
   else if (se_BLKC_state != eBCS_READY)
   {
      i_ret = -EAGAIN;
   }
   // Check if the previous transfer has finished
   else if (sst_BLKC_session.e_state != eBTS_IDLE)
   {
      i_ret = -EBUSY;
   }
   else
   {
      u16_frameCap = gu16_BLK_FrameCapacity(sstpt_BLKC_conn);

      // Check if the MTU leaves room for at least one data byte per frame
      if (u16_frameCap < BLK_MIN_FRAME_LEN)
      {
         i_ret = -EMSGSIZE;
      }
   }

   // Check if all preconditions were met
   if (i_ret == 0)
   {
      // xferId only has to differ from the previous transfer. The chunk size
      // is fixed for the whole transfer, even if the MTU grows later.
      (void)memset(&sst_BLKC_session, 0, sizeof(sst_BLKC_session));
      sst_BLKC_session.u8_xferId = ++su8_BLKC_xferCounter;
      sst_BLKC_session.u8_appType = u8_appType;
      sst_BLKC_session.u8_chunkSize = (uint8_t)(u16_frameCap - BLK_DATA_HDR_LEN);
      sst_BLKC_session.u8_window = BLK_WINDOW_DEFAULT;
      sst_BLKC_session.u32_totalLen = u32_totalLen;
      sst_BLKC_session.u32_crc32 = u32_crc;
      sst_BLKC_session.u32_totalFrames = gu32_BLK_FrameCount(u32_totalLen,
         sst_BLKC_session.u8_chunkSize);
      sst_BLKC_session.st_source = *stpt_source;
      sst_BLKC_session.e_state = eBTS_START_PENDING;

      APP_LOG_INF("TX transfer %u: type 0x%02x, %u bytes, %u B/frame", sst_BLKC_session.u8_xferId,
         u8_appType, u32_totalLen, sst_BLKC_session.u8_chunkSize);
   }

   k_mutex_unlock(&gst_BLK_lock);

   // Check if the engine has work to do
   if (i_ret == 0)
   {
      gv_BLK_Kick();
   }

   return i_ret;
}

/**
 * @public        gi_BLKC_SendBuffer
 * @brief         gi_BLKC_Send() for an object held in RAM. Asynchronous.
 *                Thread context only; not ISR-safe.
 *
 *                Wraps vpt_data in a built-in source that memcpy()s from it.
 *                The buffer is not copied: it is read during the CRC pass and
 *                again on the engine thread for every (re)transmission, so it
 *                must stay valid and unchanged until fpt_onTxDone.
 * @param[in]     u8_appType Application type (0x00..BLK_APP_TYPE_MAX).
 * @param[in]     vpt_data Object. May be NULL only if u32_totalLen is 0.
 * @param[in]     u32_totalLen Object size in bytes.
 * @return        -EINVAL if vpt_data is NULL with u32_totalLen > 0.
 *                Otherwise as gi_BLKC_Send() (-EIO cannot occur).
 */
int gi_BLKC_SendBuffer(uint8_t u8_appType, const void *vpt_data, uint32_t u32_totalLen)
{
   BlkSource_T st_source;

   // Check if a buffer was given for a non-empty object
   if ((vpt_data == NULL) && (u32_totalLen != 0U))
   {
      return -EINVAL;
   }

   st_source.fpt_read = si_RamSourceRead;
   st_source.vpt_ctx = (void *)vpt_data;

   return gi_BLKC_Send(u8_appType, &st_source, u32_totalLen);
}

/**
 * @public        gi_BLKC_SendShort
 * @brief         Write a single-frame application message [len][type][data]
 *                to DATA (Write Without Response). Synchronous. Thread
 *                context only; not ISR-safe.
 *
 *                - Best effort: no ACK, no retransmission. 0 means that the
 *                  host accepted the write, not that the server received it.
 *                  The server delivers it through its fpt_onRxShort.
 *                - Can be used while a transfer is running. It shares the
 *                  write credits with the DATA frames, so during a transfer
 *                  it may have to wait for one, and it slows the transfer
 *                  down.
 *                - The connection checks are made under gst_BLK_lock. The
 *                  credit wait is made without it, holding a connection
 *                  reference, so the engine keeps running meanwhile. From
 *                  inside a BulkXfer callback the engine already holds the
 *                  lock. The engine is then blocked for up to t_timeout, so
 *                  use K_NO_WAIT there.
 *                - vpt_data is copied before the call returns.
 * @param[in]     u8_appType Application type (0x00..BLK_APP_TYPE_MAX).
 * @param[in]     vpt_data Payload. May be NULL only if u8_len is 0.
 * @param[in]     u8_len Payload length (<= gu16_BLKC_GetMaxShortPayload()).
 * @param[in]     t_timeout Maximum wait for a free credit.
 * @return        0 on success (write queued in the host).
 *                -EPERM if gi_BLKC_Init() has not run.
 *                -EINVAL if u8_appType > BLK_APP_TYPE_MAX, or vpt_data is
 *                NULL with u8_len > 0.
 *                -ENOTCONN if no connection is bound.
 *                -EAGAIN if the attach has not finished, or no credit became
 *                free within t_timeout.
 *                -EMSGSIZE if the frame does not fit the current ATT MTU.
 *                Any other negative value is a
 *                bt_gatt_write_without_response_cb() error. The credit is
 *                returned in that case.
 */
int gi_BLKC_SendShort(uint8_t u8_appType, const void *vpt_data, uint8_t u8_len,
   k_timeout_t t_timeout)
{
   uint8_t u8ar_frame[BLK_MAX_FRAME_LEN];
   struct bt_conn *stpt_conn = NULL;
   uint16_t u16_frameLen = 0U;
   int i_ret = 0;

   // Check if the Client is running and the arguments are valid
   if (!sb_BLKC_initialized)
   {
      return -EPERM;
   }
   if ((u8_appType > BLK_APP_TYPE_MAX) || ((vpt_data == NULL) && (u8_len != 0U)))
   {
      return -EINVAL;
   }

   (void)k_mutex_lock(&gst_BLK_lock, K_FOREVER);

   // Check if there is a ready connection with a large enough MTU
   if (sstpt_BLKC_conn == NULL)
   {
      i_ret = -ENOTCONN;
   }
   else if (se_BLKC_state != eBCS_READY)
   {
      i_ret = -EAGAIN;
   }
   else if (((uint16_t)u8_len + BLK_FRAME_HDR_LEN) > gu16_BLK_FrameCapacity(sstpt_BLKC_conn))
   {
      i_ret = -EMSGSIZE;
   }
   else
   {
      // Own reference: the credit wait below runs without the lock
      stpt_conn = bt_conn_ref(sstpt_BLKC_conn);
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
   if (k_sem_take(&sst_BLKC_credits, t_timeout) != 0)
   {
      i_ret = -EAGAIN;
   }
   else
   {
      i_ret = si_CliWriteWithCredit(stpt_conn, u8ar_frame, u16_frameLen);
   }

   bt_conn_unref(stpt_conn);

   return i_ret;
}

/**
 * @public        gv_BLKC_AbortTx
 * @brief         Request cancellation of the outgoing transfer. Asynchronous;
 *                safe from any context, including ISRs and BulkXfer
 *                callbacks (it only sets an event bit and wakes the engine).
 *
 *                The engine acts on the request in its next
 *                gv_BLKC_EnginePre() and reports fpt_onTxDone(eBS_ABORTED).
 *                ABORT(eBAD_BY_SENDER) is sent to the server only if START
 *                has already gone out; before that the server knows nothing
 *                of the transfer.
 *
 *                The request applies to whatever transfer is running when the
 *                engine handles it. This includes one started by
 *                gi_BLKC_Send() after this call but before that pass. It has
 *                no effect in these cases:
 *                - No transfer is running at that time.
 *                - The transfer ends first (END, timeout, disconnect).
 *                  fpt_onTxDone then reports that result instead.
 * @return        void
 */
void gv_BLKC_AbortTx(void)
{
   // A request made while idle is discarded on the next engine pass; a
   // gi_BLKC_Send() before that pass is aborted.
   atomic_set_bit(&gt_BLK_events, eBE_CLI_ABORT_REQ);
   gv_BLK_Kick();
}

/**
 * @public        gb_BLKC_IsReady
 * @brief         Whether the attach finished and transfers may be started.
 *                Takes gst_BLK_lock, so thread context only (not ISR-safe).
 *                May be called from BulkXfer callbacks (the lock is
 *                recursive).
 *
 *                The value is a snapshot. The state is set before fpt_onReady
 *                runs, so it already returns true inside fpt_onReady(conn,
 *                0). It returns false as soon as gv_BLKC_OnDisconnected() has
 *                run, before any owed callback is delivered.
 * @return        true from a successful attach until the disconnect, false
 *                otherwise (including while the attach is running).
 */
bool gb_BLKC_IsReady(void)
{
   bool b_ready = false;

   (void)k_mutex_lock(&gst_BLK_lock, K_FOREVER);
   b_ready = (se_BLKC_state == eBCS_READY);
   k_mutex_unlock(&gst_BLK_lock);

   return b_ready;
}

/**
 * @public        gb_BLKC_IsTxBusy
 * @brief         Whether an outgoing transfer is in progress. Takes
 *                gst_BLK_lock, so thread context only (not ISR-safe). May be
 *                called from BulkXfer callbacks (the lock is recursive).
 *
 *                The value is a snapshot and can change right after the
 *                call. The session is set idle before fpt_onTxDone runs, so:
 *                - Inside fpt_onTxDone it already returns false.
 *                - After a disconnect it returns false immediately, even
 *                  though fpt_onTxDone(eBS_DISCONNECTED) is delivered later
 *                  by the engine.
 * @return        true from a successful gi_BLKC_Send() until the transfer
 *                ends (while gi_BLKC_Send() would return -EBUSY), false
 *                otherwise.
 */
bool gb_BLKC_IsTxBusy(void)
{
   bool b_busy = false;

   (void)k_mutex_lock(&gst_BLK_lock, K_FOREVER);
   b_busy = (sst_BLKC_session.e_state != eBTS_IDLE);
   k_mutex_unlock(&gst_BLK_lock);

   return b_busy;
}

/**
 * @public        gu16_BLKC_GetMaxShortPayload
 * @brief         Largest payload gi_BLKC_SendShort() accepts on the current
 *                link. Takes gst_BLK_lock, so thread context only (not
 *                ISR-safe).
 *
 *                Computed as min(ATT_MTU - 3, BLK_MAX_FRAME_LEN) minus the
 *                2-byte frame header. Without b_autoTuneLink the ATT MTU may
 *                still be 23 when the attach completes. Query again after an
 *                MTU exchange, and do not cache the value across connections.
 * @return        Payload size in bytes (at most BLK_MAX_SHORT_PAYLOAD).
 *                0 unless the Client is attached and ready.
 */
uint16_t gu16_BLKC_GetMaxShortPayload(void)
{
   uint16_t u16_cap = 0U;

   (void)k_mutex_lock(&gst_BLK_lock, K_FOREVER);

   // Check if there is a ready connection to size against
   if ((sstpt_BLKC_conn != NULL) && (se_BLKC_state == eBCS_READY))
   {
      u16_cap = gu16_BLK_FrameCapacity(sstpt_BLKC_conn);
      u16_cap = (u16_cap > BLK_FRAME_HDR_LEN) ? (uint16_t)(u16_cap - BLK_FRAME_HDR_LEN) : 0U;
   }

   k_mutex_unlock(&gst_BLK_lock);

   return u16_cap;
}

#endif // BLK_ENABLE_CLIENT
