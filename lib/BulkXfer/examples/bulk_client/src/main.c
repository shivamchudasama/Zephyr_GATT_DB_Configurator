/**
 * @file          main.c
 * @brief         BulkXfer client central.
 *
 *                Scans for a board advertising the BulkXfer service
 *                (examples/bulk_server), connects, attaches the BulkXfer
 *                Client and then repeatedly writes CLIENT_XFER_SIZE bytes of
 *                a generated pattern into the server's DATA characteristic.
 *                Logs the throughput seen here and the result the server
 *                reports back as a short message.
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
#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/sys/byteorder.h>
#include "BulkXfer.h"
#include "AppLog.h"

/******************************************************************************/
/*                                                                            */
/*                                  DEFINES                                   */
/*                                                                            */
/******************************************************************************/
/**
 * @def           CLIENT_XFER_SIZE
 * @brief         Size of each object sent to the server.
 */
#define CLIENT_XFER_SIZE                     (100U * 1024U)

/**
 * @def           CLIENT_XFER_PERIOD_MS
 * @brief         Pause between two transfers.
 */
#define CLIENT_XFER_PERIOD_MS                (2000U)

/**
 * @def           APP_TYPE_BULK
 * @brief         Application type of the objects sent.
 */
#define APP_TYPE_BULK                        (0x10U)

/**
 * @def           APP_TYPE_RESULT
 * @brief         Short message type of the server's result report.
 */
#define APP_TYPE_RESULT                      (0x01U)

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
static int si_PatternRead(void *vpt_ctx, uint32_t u32_offset, uint8_t *u8pt_buf,
   uint16_t u16_len);
static void sv_OnReady(struct bt_conn *stpt_conn, int i_status);
static void sv_OnTxDone(uint8_t u8_appType, BlkStatus_E e_status);
static void sv_OnRxShort(uint8_t u8_appType, const uint8_t *u8pt_data, uint8_t u8_len);
static bool sb_AdHasService(struct bt_data *stpt_data, void *vpt_found);
static void sv_DeviceFound(const bt_addr_le_t *stpt_addr, int8_t i8_rssi, uint8_t u8_type,
   struct net_buf_simple *stpt_ad);
static void sv_Connected(struct bt_conn *stpt_conn, uint8_t u8_err);
static void sv_Disconnected(struct bt_conn *stpt_conn, uint8_t u8_reason);
static void sv_Recycled(void);
static void sv_ScanWork(struct k_work *stpt_work);
static void sv_SendWork(struct k_work *stpt_work);

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
 * @var           gst_connCallbacks
 * @brief         Connection callbacks. BT_CONN_CB_DEFINE gives the object
 *                external linkage.
 */
BT_CONN_CB_DEFINE(gst_connCallbacks) = {
   .connected    = sv_Connected,
   .disconnected = sv_Disconnected,
   .recycled     = sv_Recycled,
};

/******************************************************************************/
/*                                                                            */
/*                             PRIVATE VARIABLES                              */
/*                                                                            */
/******************************************************************************/
/**
 * @var           sst_svcUuid
 * @brief         Service UUID searched for in advertising data.
 */
static const struct bt_uuid_128 sst_svcUuid = BT_UUID_INIT_128(BT_UUID_BLK_SVC_VAL);

/**
 * @var           sstpt_conn
 * @brief         Connection to the server (reference from bt_conn_le_create).
 */
static struct bt_conn *sstpt_conn = NULL;

/**
 * @var           si64_txStartMs
 * @brief         Uptime at which the current transfer was started.
 */
static int64_t si64_txStartMs = 0;

/**
 * @var           sst_scanWork
 * @brief         (Re)starts scanning outside the Bluetooth callback context.
 */
static K_WORK_DEFINE(sst_scanWork, sv_ScanWork);

/**
 * @var           sst_sendWork
 * @brief         Starts the next transfer. gi_BLKC_Send() reads the whole
 *                source for its CRC, so it runs here, not on the engine.
 */
static K_WORK_DELAYABLE_DEFINE(sst_sendWork, sv_SendWork);

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
 * @private       si_PatternRead
 * @brief         BlkSourceRead_F: deterministic pattern, so the object never
 *                has to exist in RAM. Re-reads for retransmits return the
 *                same bytes.
 * @param[in]     vpt_ctx Unused.
 * @param[in]     u32_offset Byte offset.
 * @param[out]    u8pt_buf Destination.
 * @param[in]     u16_len Number of bytes.
 * @return        0.
 */
static int si_PatternRead(void *vpt_ctx, uint32_t u32_offset, uint8_t *u8pt_buf,
   uint16_t u16_len)
{
   uint16_t u16_idx = 0U;

   ARG_UNUSED(vpt_ctx);

   for (u16_idx = 0U; u16_idx < u16_len; u16_idx++)
   {
      uint32_t u32_pos = u32_offset + u16_idx;

      u8pt_buf[u16_idx] = (uint8_t)((u32_pos * 31U) ^ (u32_pos >> 8));
   }

   return 0;
}

/**
 * @private       sv_OnReady
 * @brief         BlkCliReady_F: start sending, or drop a server without the
 *                BulkXfer service.
 * @param[in]     stpt_conn Connection.
 * @param[in]     i_status Attach result.
 * @return        void
 */
static void sv_OnReady(struct bt_conn *stpt_conn, int i_status)
{
   // Check if the server exposes a usable BulkXfer service
   if (i_status != 0)
   {
      APP_LOG_ERR("attach failed (%d), disconnecting", i_status);
      (void)bt_conn_disconnect(stpt_conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
      return;
   }

   APP_LOG_INF("attached, %u B short payload", gu16_BLKC_GetMaxShortPayload());
   (void)k_work_reschedule(&sst_sendWork, K_NO_WAIT);
}

/**
 * @private       sv_OnTxDone
 * @brief         BlkTxDone_F: log the throughput and schedule the next one.
 * @param[in]     u8_appType Application type.
 * @param[in]     e_status Result reported by the server, or a local error.
 * @return        void
 */
static void sv_OnTxDone(uint8_t u8_appType, BlkStatus_E e_status)
{
   int64_t i64_ms = MAX(k_uptime_get() - si64_txStartMs, 1);

   APP_LOG_INF("TX type 0x%02x: status %u, %u kbit/s", u8_appType, (unsigned)e_status,
      (uint32_t)(((uint64_t)CLIENT_XFER_SIZE * 8U) / (uint64_t)i64_ms));

   // Check if the link is still there for another round
   if (e_status != eBS_DISCONNECTED)
   {
      (void)k_work_reschedule(&sst_sendWork, K_MSEC(CLIENT_XFER_PERIOD_MS));
   }
}

/**
 * @private       sv_OnRxShort
 * @brief         BlkRxShort_F: print the server's result report.
 * @param[in]     u8_appType Application type.
 * @param[in]     u8pt_data Payload.
 * @param[in]     u8_len Payload length.
 * @return        void
 */
static void sv_OnRxShort(uint8_t u8_appType, const uint8_t *u8pt_data, uint8_t u8_len)
{
   // Check if this is a complete result report
   if ((u8_appType == APP_TYPE_RESULT) && (u8_len >= 9U))
   {
      APP_LOG_INF("server: status %u, %u bytes, %u kbit/s", u8pt_data[0],
         sys_get_le32(&u8pt_data[1]), sys_get_le32(&u8pt_data[5]));
   }
}

/**
 * @private       sb_AdHasService
 * @brief         bt_data_parse() callback: look for the service UUID.
 * @param[in]     stpt_data One AD structure.
 * @param[out]    vpt_found bool set to true on a match.
 * @return        false to stop parsing once found.
 */
static bool sb_AdHasService(struct bt_data *stpt_data, void *vpt_found)
{
   uint8_t u8_idx = 0U;

   // Check if this AD structure lists 128-bit service UUIDs
   if ((stpt_data->type != BT_DATA_UUID128_ALL) && (stpt_data->type != BT_DATA_UUID128_SOME))
   {
      return true;
   }

   for (u8_idx = 0U; (u8_idx + 16U) <= stpt_data->data_len; u8_idx += 16U)
   {
      // Check if this UUID is the BulkXfer service
      if (memcmp(&stpt_data->data[u8_idx], sst_svcUuid.val, 16U) == 0)
      {
         *(bool *)vpt_found = true;
         return false;
      }
   }

   return true;
}

/**
 * @private       sv_DeviceFound
 * @brief         Scan callback: connect to the first BulkXfer server.
 * @param[in]     stpt_addr Advertiser address.
 * @param[in]     i8_rssi RSSI (unused).
 * @param[in]     u8_type Advertising PDU type.
 * @param[in]     stpt_ad Advertising / scan response data.
 * @return        void
 */
static void sv_DeviceFound(const bt_addr_le_t *stpt_addr, int8_t i8_rssi, uint8_t u8_type,
   struct net_buf_simple *stpt_ad)
{
   bool b_found = false;
   int i_ret = 0;

   ARG_UNUSED(i8_rssi);

   // Check if the report comes from a connectable advertiser
   if ((u8_type != BT_GAP_ADV_TYPE_ADV_IND) && (u8_type != BT_GAP_ADV_TYPE_SCAN_RSP))
   {
      return;
   }

   bt_data_parse(stpt_ad, sb_AdHasService, &b_found);

   // Check if it advertises the service and no connection is pending
   if (!b_found || (sstpt_conn != NULL))
   {
      return;
   }

   (void)bt_le_scan_stop();

   i_ret = bt_conn_le_create(stpt_addr, BT_CONN_LE_CREATE_CONN, BT_LE_CONN_PARAM(6, 12, 0, 400),
      &sstpt_conn);

   // Check if the connection could be initiated
   if (i_ret != 0)
   {
      APP_LOG_ERR("connect failed (%d)", i_ret);
      k_work_submit(&sst_scanWork);
   }
}

/**
 * @private       sv_Connected
 * @brief         bt_conn_cb.connected: attach the BulkXfer Client.
 * @param[in]     stpt_conn New connection.
 * @param[in]     u8_err HCI error (0 on success).
 * @return        void
 */
static void sv_Connected(struct bt_conn *stpt_conn, uint8_t u8_err)
{
   int i_ret = 0;

   // Check if this is our connection attempt
   if (stpt_conn != sstpt_conn)
   {
      return;
   }

   // Check if the connection attempt failed
   if (u8_err != 0U)
   {
      APP_LOG_ERR("connection failed (0x%02x)", u8_err);
      bt_conn_unref(sstpt_conn);
      sstpt_conn = NULL;
      k_work_submit(&sst_scanWork);
      return;
   }

   APP_LOG_INF("connected");
   i_ret = gi_BLKC_Attach(stpt_conn);

   // Check if the attach sequence started
   if (i_ret != 0)
   {
      APP_LOG_ERR("gi_BLKC_Attach failed (%d)", i_ret);
      (void)bt_conn_disconnect(stpt_conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
   }
}

/**
 * @private       sv_Disconnected
 * @brief         bt_conn_cb.disconnected: release BulkXfer and our reference.
 * @param[in]     stpt_conn Connection that went down.
 * @param[in]     u8_reason HCI disconnect reason.
 * @return        void
 */
static void sv_Disconnected(struct bt_conn *stpt_conn, uint8_t u8_reason)
{
   // Check if this is our connection
   if (stpt_conn != sstpt_conn)
   {
      return;
   }

   APP_LOG_INF("disconnected (0x%02x)", u8_reason);
   (void)k_work_cancel_delayable(&sst_sendWork);
   gv_BLK_OnDisconnected(stpt_conn);
   bt_conn_unref(sstpt_conn);
   sstpt_conn = NULL;
}

/**
 * @private       sv_Recycled
 * @brief         bt_conn_cb.recycled: the connection object is free again,
 *                so scanning can restart.
 * @return        void
 */
static void sv_Recycled(void)
{
   k_work_submit(&sst_scanWork);
}

/**
 * @private       sv_ScanWork
 * @brief         Start active scanning (system work queue). Active, because
 *                the server puts its service UUID in the scan response.
 * @param[in]     stpt_work Work item (unused).
 * @return        void
 */
static void sv_ScanWork(struct k_work *stpt_work)
{
   int i_ret = 0;

   ARG_UNUSED(stpt_work);

   i_ret = bt_le_scan_start(BT_LE_SCAN_ACTIVE, sv_DeviceFound);

   // Check if scanning started (-EALREADY is fine)
   if ((i_ret != 0) && (i_ret != -EALREADY))
   {
      APP_LOG_ERR("scan failed (%d)", i_ret);
      return;
   }

   APP_LOG_INF("scanning for a BulkXfer server");
}

/**
 * @private       sv_SendWork
 * @brief         Start one transfer (system work queue).
 * @param[in]     stpt_work Work item (unused).
 * @return        void
 */
static void sv_SendWork(struct k_work *stpt_work)
{
   static const BlkSource_T slst_source = { .fpt_read = si_PatternRead, .vpt_ctx = NULL };
   int i_ret = 0;

   ARG_UNUSED(stpt_work);

   si64_txStartMs = k_uptime_get();
   i_ret = gi_BLKC_Send(APP_TYPE_BULK, &slst_source, CLIENT_XFER_SIZE);

   // Check if the transfer was queued
   if (i_ret != 0)
   {
      APP_LOG_ERR("gi_BLKC_Send failed (%d)", i_ret);
   }
}

/******************************************************************************/
/*                                                                            */
/*                        PUBLIC FUNCTION DEFINITIONS                         */
/*                                                                            */
/******************************************************************************/
/**
 * @public        main
 * @brief         Enable Bluetooth, start the BulkXfer Client and scan.
 * @return        0.
 */
int main(void)
{
   BlkCliCfg_T st_cfg = { 0 };
   int i_ret = 0;

   i_ret = bt_enable(NULL);

   // Check if the Bluetooth stack started
   if (i_ret != 0)
   {
      APP_LOG_ERR("bt_enable failed (%d)", i_ret);
      return 0;
   }

   // NULL UUIDs select the library defaults
   st_cfg.fpt_onReady = sv_OnReady;
   st_cfg.fpt_onTxDone = sv_OnTxDone;
   st_cfg.fpt_onRxShort = sv_OnRxShort;
   st_cfg.b_autoTuneLink = true;

   i_ret = gi_BLKC_Init(&st_cfg);

   // Check if BulkXfer started
   if (i_ret != 0)
   {
      APP_LOG_ERR("gi_BLKC_Init failed (%d)", i_ret);
      return 0;
   }

   k_work_submit(&sst_scanWork);

   return 0;
}
