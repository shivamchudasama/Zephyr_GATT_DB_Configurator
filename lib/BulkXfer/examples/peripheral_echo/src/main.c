/**
 * @file          main.c
 * @brief         BulkXfer echo peripheral.
 *
 *                - Any multi-frame transfer received from the central (up to
 *                  ECHO_BUF_SIZE bytes) is sent back unchanged with the same
 *                  application type once it completed with eBS_OK.
 *                - Any short message is echoed back as a short message.
 *
 *                Use it with tools/bulkxfer_client.py to test both directions
 *                and measure throughput.
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
#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include "BulkXfer.h"
#include "BulkXfer_Service.h"
#include "AppLog.h"

/******************************************************************************/
/*                                                                            */
/*                                  DEFINES                                   */
/*                                                                            */
/******************************************************************************/
/**
 * @def           ECHO_BUF_SIZE
 * @brief         Largest object the peripheral accepts and echoes back.
 */
#define ECHO_BUF_SIZE                        (32U * 1024U)

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
static int si_OnRxStart(uint8_t u8_appType, uint32_t u32_totalLen);
static int si_OnRxData(uint8_t u8_appType, uint32_t u32_offset,
   const uint8_t *u8pt_data, uint16_t u16_len);
static void sv_OnRxDone(uint8_t u8_appType, BlkStatus_E e_status, uint32_t u32_totalLen);
static void sv_OnRxShort(uint8_t u8_appType, const uint8_t *u8pt_data, uint8_t u8_len);
static void sv_OnTxDone(uint8_t u8_appType, BlkStatus_E e_status);
static void sv_Connected(struct bt_conn *stpt_conn, uint8_t u8_err);
static void sv_Disconnected(struct bt_conn *stpt_conn, uint8_t u8_reason);
static void sv_Recycled(void);
static void sv_AdvWork(struct k_work *stpt_work);

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
 * @var           su8ar_echo
 * @brief         Holds the received object until it has been echoed back.
 */
static uint8_t su8ar_echo[ECHO_BUF_SIZE];

/**
 * @var           si64_rxStartMs
 * @brief         Uptime at which the current incoming transfer started.
 */
static int64_t si64_rxStartMs = 0;

/**
 * @var           sstar_ad
 * @brief         Advertising data.
 */
static const struct bt_data sstar_ad[] = {
   BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
   BT_DATA(BT_DATA_NAME_COMPLETE, CONFIG_BT_DEVICE_NAME,
      sizeof(CONFIG_BT_DEVICE_NAME) - 1),
};

/**
 * @var           sstar_sd
 * @brief         Scan response data (service UUID).
 */
static const struct bt_data sstar_sd[] = {
   BT_DATA_BYTES(BT_DATA_UUID128_ALL, BT_UUID_BULK_XFER_SVC_VAL),
};

/**
 * @var           sst_advWork
 * @brief         Restarts advertising outside the Bluetooth callback context.
 */
static K_WORK_DEFINE(sst_advWork, sv_AdvWork);

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
 * @private       si_OnRxStart
 * @brief         BlkRxStart_F: accept a transfer if it fits the echo buffer and
 *                the previous echo has been sent.
 * @param[in]     u8_appType Application type (unused).
 * @param[in]     u32_totalLen Announced object size.
 * @return        0 to accept, -ENOMEM to reject.
 */
static int si_OnRxStart(uint8_t u8_appType, uint32_t u32_totalLen)
{
   ARG_UNUSED(u8_appType);

   // Check if the object fits and the echo buffer is not being sent back
   // (it is also the source of the running echo)
   if ((u32_totalLen > sizeof(su8ar_echo)) || gb_BLK_IsTxBusy())
   {
      APP_LOG_WRN("rejecting %u bytes", u32_totalLen);
      return -ENOMEM;
   }

   si64_rxStartMs = k_uptime_get();
   return 0;
}

/**
 * @private       si_OnRxData
 * @brief         BlkRxData_F: copy the chunk into the echo buffer.
 * @param[in]     u8_appType Application type (unused).
 * @param[in]     u32_offset Offset of the chunk within the object.
 * @param[in]     u8pt_data Chunk data.
 * @param[in]     u16_len Chunk length.
 * @return        0.
 */
static int si_OnRxData(uint8_t u8_appType, uint32_t u32_offset,
   const uint8_t *u8pt_data, uint16_t u16_len)
{
   ARG_UNUSED(u8_appType);

   (void)memcpy(&su8ar_echo[u32_offset], u8pt_data, u16_len);
   return 0;
}

/**
 * @private       sv_OnRxDone
 * @brief         BlkRxDone_F: log the throughput and echo a complete object.
 * @param[in]     u8_appType Application type, reused for the echo.
 * @param[in]     e_status Result of the incoming transfer.
 * @param[in]     u32_totalLen Object size.
 * @return        void
 */
static void sv_OnRxDone(uint8_t u8_appType, BlkStatus_E e_status, uint32_t u32_totalLen)
{
   int64_t i64_ms = MAX(k_uptime_get() - si64_rxStartMs, 1);
   int i_ret = 0;

   APP_LOG_INF("RX type 0x%02x: %u bytes, status %u, %u kbit/s", u8_appType,
      u32_totalLen, (unsigned)e_status, (uint32_t)(((uint64_t)u32_totalLen * 8U) / i64_ms));

   // Check if the object arrived intact
   if (e_status != eBS_OK)
   {
      return;
   }

   i_ret = gi_BLK_SendBuffer(u8_appType, su8ar_echo, u32_totalLen);

   // Check if the echo could be started
   if (i_ret != 0)
   {
      APP_LOG_ERR("echo not started (%d)", i_ret);
   }
}

/**
 * @private       sv_OnRxShort
 * @brief         BlkRxShort_F: echo a short message back.
 * @param[in]     u8_appType Application type.
 * @param[in]     u8pt_data Payload.
 * @param[in]     u8_len Payload length.
 * @return        void
 */
static void sv_OnRxShort(uint8_t u8_appType, const uint8_t *u8pt_data, uint8_t u8_len)
{
   (void)gi_BLK_SendShort(u8_appType, u8pt_data, u8_len, K_MSEC(100));
}

/**
 * @private       sv_OnTxDone
 * @brief         BlkTxDone_F: log the result of an echo.
 * @param[in]     u8_appType Application type.
 * @param[in]     e_status Result of the outgoing transfer.
 * @return        void
 */
static void sv_OnTxDone(uint8_t u8_appType, BlkStatus_E e_status)
{
   APP_LOG_INF("TX type 0x%02x done, status %u", u8_appType, (unsigned)e_status);
}

/**
 * @private       sv_Connected
 * @brief         bt_conn_cb.connected: bind BulkXfer to the new connection.
 * @param[in]     stpt_conn New connection.
 * @param[in]     u8_err HCI error (0 on success).
 * @return        void
 */
static void sv_Connected(struct bt_conn *stpt_conn, uint8_t u8_err)
{
   // Check if the connection attempt failed
   if (u8_err != 0U)
   {
      APP_LOG_ERR("connection failed (0x%02x)", u8_err);
      return;
   }

   APP_LOG_INF("connected");
   gv_BLK_OnConnected(stpt_conn);
}

/**
 * @private       sv_Disconnected
 * @brief         bt_conn_cb.disconnected: release BulkXfer's connection.
 * @param[in]     stpt_conn Connection that went down.
 * @param[in]     u8_reason HCI disconnect reason.
 * @return        void
 */
static void sv_Disconnected(struct bt_conn *stpt_conn, uint8_t u8_reason)
{
   APP_LOG_INF("disconnected (0x%02x)", u8_reason);
   gv_BLK_OnDisconnected(stpt_conn);
}

/**
 * @private       sv_Recycled
 * @brief         bt_conn_cb.recycled: the connection object is free again,
 *                so advertising can restart.
 * @return        void
 */
static void sv_Recycled(void)
{
   // Only now is the conn object free for a new connection
   k_work_submit(&sst_advWork);
}

/**
 * @private       sv_AdvWork
 * @brief         Start connectable advertising (system work queue).
 * @param[in]     stpt_work Work item (unused).
 * @return        void
 */
static void sv_AdvWork(struct k_work *stpt_work)
{
   int i_ret = 0;

   ARG_UNUSED(stpt_work);

   i_ret = bt_le_adv_start(BT_LE_ADV_CONN_FAST_1, sstar_ad, ARRAY_SIZE(sstar_ad),
      sstar_sd, ARRAY_SIZE(sstar_sd));

   // Check if advertising started (-EALREADY is fine)
   if ((i_ret != 0) && (i_ret != -EALREADY))
   {
      APP_LOG_ERR("advertising failed (%d)", i_ret);
   }
}

/******************************************************************************/
/*                                                                            */
/*                        PUBLIC FUNCTION DEFINITIONS                         */
/*                                                                            */
/******************************************************************************/
/**
 * @public        main
 * @brief         Enable Bluetooth, start BulkXfer and begin advertising.
 * @return        0.
 */
int main(void)
{
   BlkCfg_T st_cfg = { 0 };
   int i_ret = 0;

   i_ret = bt_enable(NULL);

   // Check if the Bluetooth stack started
   if (i_ret != 0)
   {
      APP_LOG_ERR("bt_enable failed (%d)", i_ret);
      return 0;
   }

   st_cfg.stpt_txAttr = gstpt_BulkSvc_Init();
   st_cfg.fpt_onRxStart = si_OnRxStart;
   st_cfg.fpt_onRxData = si_OnRxData;
   st_cfg.fpt_onRxDone = sv_OnRxDone;
   st_cfg.fpt_onRxShort = sv_OnRxShort;
   st_cfg.fpt_onTxDone = sv_OnTxDone;
   st_cfg.b_autoTuneLink = true;

   i_ret = gi_BLK_Init(&st_cfg);

   // Check if BulkXfer started
   if (i_ret != 0)
   {
      APP_LOG_ERR("gi_BLK_Init failed (%d)", i_ret);
      return 0;
   }

   // Advertise only after init: earlier connections would be ignored
   k_work_submit(&sst_advWork);
   APP_LOG_INF("BulkXfer echo peripheral ready");

   return 0;
}
