/**
 * @file          BulkXfer_Server.h
 * @brief         BulkXfer Server role: receives bulk data that a remote GATT
 *                client writes into this device's GATT database.
 *
 *                The service (BulkXfer_Uuid.h) is declared with the generic
 *                callbacks of GATT_GenericCallbacks.c:
 *
 *                  DATA (Write Without Response + Write), buffer >= BLK_MAX_FRAME_LEN:
 *                     gt_GATT_GenericWrite + fpt_customWriteCb that forwards to
 *                     gt_BLKS_DataWriteHook(). Carries START, DATA, ABORT and
 *                     client -> server short messages.
 *                  CTRL (Notify + CCC):
 *                     ACK, NACK, END, ABORT and server -> client short messages,
 *                     sent with bt_gatt_notify_cb(). Bulk data never goes here.
 *
 *                Typical integration:
 * @code
 *                // Generated service .c (hook name set in the configurator)
 *                static ssize_t st_OnBulkData(struct bt_conn *c,
 *                   const struct bt_gatt_attr *a, const void *b, uint16_t l,
 *                   uint16_t o, uint8_t f)
 *                {
 *                   return gt_BLKS_DataWriteHook(c, a, b, l, o, f);
 *                }
 *
 *                // Application
 *                gi_BLKS_Init(&srvCfg);
 *                // bt_conn_cb: connected    -> gv_BLKS_OnConnected(conn)
 *                //             disconnected -> gv_BLK_OnDisconnected(conn)
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

#ifndef _BULK_XFER_SERVER_H
#define _BULK_XFER_SERVER_H

/******************************************************************************/
/*                                                                            */
/*                                  INCLUDES                                  */
/*                                                                            */
/******************************************************************************/
#include <zephyr/kernel.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
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
 * @struct        BlkSrvCfg_T
 * @brief         Configuration passed to gi_BLKS_Init(). Copied internally.
 */
typedef struct
{
   /**
    * @brief      Value attribute of the CTRL (notify) characteristic. Find it
    *             with bt_gatt_find_by_uuid(svc.attrs, svc.attr_count,
    *             BT_UUID_BLK_CTRL).
    */
   const struct bt_gatt_attr *stpt_ctrlAttr;

   BlkRxStart_F fpt_onRxStart;               /**< Optional: NULL accepts every transfer. */
   BlkRxData_F fpt_onRxData;                 /**< Required to receive transfers.         */
   BlkRxDone_F fpt_onRxDone;                 /**< Optional.                              */
   BlkRxShort_F fpt_onRxShort;               /**< Optional: client -> server shorts.     */

   /**
    * @brief      When true, gv_BLKS_OnConnected() requests 2M PHY and maximum
    *             data length, and (with CONFIG_BT_GATT_CLIENT) an ATT MTU
    *             exchange.
    */
   bool b_autoTuneLink;
} BlkSrvCfg_T;

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
extern int gi_BLKS_Init(const BlkSrvCfg_T *stpt_cfg);
extern void gv_BLKS_OnConnected(struct bt_conn *stpt_conn);
extern void gv_BLKS_OnDisconnected(struct bt_conn *stpt_conn);

/* ---- Server -> client ---------------------------------------------------- */
extern int gi_BLKS_SendShort(uint8_t u8_appType, const void *vpt_data, uint8_t u8_len,
   k_timeout_t t_timeout);
extern void gv_BLKS_AbortRx(void);

/* ---- Queries ------------------------------------------------------------- */
extern bool gb_BLKS_IsRxBusy(void);
extern uint16_t gu16_BLKS_GetMaxShortPayload(void);
extern void gv_BLKS_GetCaps(BlkCaps_T *stpt_caps);

/* ---- GATT hook (GATTCustomWriteCb_F signature) --------------------------- */
extern ssize_t gt_BLKS_DataWriteHook(struct bt_conn *stpt_connHandle,
   const struct bt_gatt_attr *stpt_attr, const void *vpt_buf, uint16_t u16_length,
   uint16_t u16_offset, uint8_t u8_flags);

#endif // _BULK_XFER_SERVER_H
