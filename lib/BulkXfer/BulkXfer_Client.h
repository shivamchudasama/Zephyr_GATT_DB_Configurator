/**
 * @file          BulkXfer_Client.h
 * @brief         BulkXfer Client role: sends bulk data by writing frames (Write
 *                Without Response) into the DATA characteristic of a remote
 *                BulkXfer Server, and receives its ACK / NACK / END / ABORT
 *                as CTRL notifications.
 *
 *                The GATT client role is independent of the link role: a
 *                peripheral can be the Client. Requires CONFIG_BT_GATT_CLIENT.
 *
 *                Typical integration:
 * @code
 *                gi_BLKC_Init(&cliCfg);
 *                // after connecting (either link role):
 *                gi_BLKC_Attach(conn);           // -> fpt_onReady(conn, 0)
 *                // in fpt_onReady, or later:
 *                gi_BLKC_SendBuffer(MY_TYPE_LOG, buf, len);
 *                // bt_conn_cb.disconnected -> gv_BLK_OnDisconnected(conn)
 * @endcode
 *
 *                One connection at a time.
 *
 * @date          24/09/2026
 * @author        Shivam Chudasama
 * @copyright     Shivam Chudasama
 * @license       MIT
 */

/* SPDX-License-Identifier: MIT */

#ifndef _BULK_XFER_CLIENT_H
#define _BULK_XFER_CLIENT_H

/******************************************************************************/
/*                                                                            */
/*                                  INCLUDES                                  */
/*                                                                            */
/******************************************************************************/
#include <zephyr/kernel.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include "BulkXfer_Types.h"

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
 * @typedef       BlkCliReady_F
 * @brief         Result of gi_BLKC_Attach(): discovery, CTRL subscription and
 *                (with b_autoTuneLink) MTU exchange finished.
 * @param[in]     stpt_conn Connection passed to gi_BLKC_Attach() (valid only
 *                during the call).
 * @param[in]     i_status 0 ready; -ENOENT service / characteristic / CCC not
 *                found; -EIO subscription failed; -ENOTCONN link lost first;
 *                other negative errno from bt_gatt_discover().
 */
typedef void (*BlkCliReady_F)(struct bt_conn *stpt_conn, int i_status);

/**
 * @struct        BlkCliCfg_T
 * @brief         Configuration passed to gi_BLKC_Init(). Copied internally;
 *                the UUIDs are kept by pointer and must have static storage
 *                (BT_UUID_DECLARE_128() inside a function does not).
 */
typedef struct
{
   const struct bt_uuid *stpt_svcUuid;       /**< NULL: BT_UUID_BLK_SVC.                 */
   const struct bt_uuid *stpt_dataUuid;      /**< NULL: BT_UUID_BLK_DATA.                */
   const struct bt_uuid *stpt_ctrlUuid;      /**< NULL: BT_UUID_BLK_CTRL.                */

   BlkCliReady_F fpt_onReady;                /**< Optional: attach result.               */
   BlkTxDone_F fpt_onTxDone;                 /**< Optional: outgoing transfer result.    */
   BlkRxShort_F fpt_onRxShort;               /**< Optional: server -> client shorts.     */

   /**
    * @brief      When true, gi_BLKC_Attach() requests 2M PHY and maximum data
    *             length, and runs an ATT MTU exchange before discovery so the
    *             chunk size is final once the Client is ready.
    */
   bool b_autoTuneLink;
} BlkCliCfg_T;

/******************************************************************************/
/*                                                                            */
/*                                   UNIONS                                   */
/*                                                                            */
/******************************************************************************/

/******************************************************************************/
/*                                                                            */
/*                              EXTERN VARIABLES                              */
/*                                                                            */
/******************************************************************************/

/******************************************************************************/
/*                                                                            */
/*                              EXTERN FUNCTIONS                              */
/*                                                                            */
/******************************************************************************/
/* ---- Lifecycle ----------------------------------------------------------- */
extern int gi_BLKC_Init(const BlkCliCfg_T *stpt_cfg);
extern int gi_BLKC_Attach(struct bt_conn *stpt_conn);
extern void gv_BLKC_OnDisconnected(struct bt_conn *stpt_conn);

/* ---- Sending ------------------------------------------------------------- */
extern int gi_BLKC_Send(uint8_t u8_appType, const BlkSource_T *stpt_source,
   uint32_t u32_totalLen);
extern int gi_BLKC_SendBuffer(uint8_t u8_appType, const void *vpt_data,
   uint32_t u32_totalLen);
extern int gi_BLKC_SendShort(uint8_t u8_appType, const void *vpt_data,
   uint8_t u8_len, k_timeout_t t_timeout);
extern void gv_BLKC_AbortTx(void);

/* ---- Queries ------------------------------------------------------------- */
extern bool gb_BLKC_IsReady(void);
extern bool gb_BLKC_IsTxBusy(void);
extern uint16_t gu16_BLKC_GetMaxShortPayload(void);

#endif // _BULK_XFER_CLIENT_H
