/**
 * @file          BulkXfer.h
 * @brief         Public API of the BLE bulk transfer (BulkXfer) framework.
 *
 *                BulkXfer moves arbitrarily large objects in both directions
 *                over two GATT characteristics that are registered with the
 *                generic callbacks of GATT_GenericCallbacks.c:
 *
 *                  RX char (Write / Write Without Response):
 *                     gt_GATT_GenericWrite + fpt_customWriteCb that forwards to
 *                     gt_BLK_RxWriteHook(). Buffer >= BLK_MAX_FRAME_LEN bytes.
 *                  TX char (Notify + CCC):
 *                     frames are sent with bt_gatt_notify_cb().
 *
 *                Reliability: windowed cumulative ACKs + Go-Back-N retransmit
 *                + CRC-32 over the whole object. See BulkXfer_Frame.h for the
 *                wire format.
 *
 *                Typical integration:
 * @code
 *                // Generated service .c (hook name set in the configurator)
 *                static ssize_t st_OnBulkRx(struct bt_conn *c,
 *                   const struct bt_gatt_attr *a, const void *b, uint16_t l,
 *                   uint16_t o, uint8_t f)
 *                {
 *                   return gt_BLK_RxWriteHook(c, a, b, l, o, f);
 *                }
 *
 *                // Application
 *                BT_CONN_CB_DEFINE(conn_cb) = {
 *                   .connected    = on_connected,    // -> gv_BLK_OnConnected()
 *                   .disconnected = on_disconnected, // -> gv_BLK_OnDisconnected()
 *                };
 *                gi_BLK_Init(&cfg);
 *                gi_BLK_SendBuffer(MY_TYPE_LOG, buf, len);
 * @endcode
 *
 *                Version 1 serves a single connection at a time.
 *
 * @date          22/09/2026
 * @author        Shivam Chudasama
 * @copyright     Shivam Chudasama
 * @license       MIT
 */

/* SPDX-License-Identifier: MIT */

#ifndef _BULK_XFER_H
#define _BULK_XFER_H

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
/**
 * @def           BLK_PROTOCOL_VERSION
 * @brief         Protocol version reported in BlkCaps_T.
 */
#define BLK_PROTOCOL_VERSION                 (1U)

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
/*                              EXTERN VARIABLES                              */
/*                                                                            */
/******************************************************************************/

/******************************************************************************/
/*                                                                            */
/*                              EXTERN FUNCTIONS                              */
/*                                                                            */
/******************************************************************************/
/* ---- Lifecycle ----------------------------------------------------------- */
extern int gi_BLK_Init(const BlkCfg_T *stpt_cfg);
extern void gv_BLK_OnConnected(struct bt_conn *stpt_conn);
extern void gv_BLK_OnDisconnected(struct bt_conn *stpt_conn);

/* ---- Sending ------------------------------------------------------------- */
extern int gi_BLK_Send(uint8_t u8_appType, const BlkSource_T *stpt_source,
   uint32_t u32_totalLen);
extern int gi_BLK_SendBuffer(uint8_t u8_appType, const void *vpt_data,
   uint32_t u32_totalLen);
extern int gi_BLK_SendShort(uint8_t u8_appType, const void *vpt_data,
   uint8_t u8_len, k_timeout_t t_timeout);
extern void gv_BLK_AbortTx(void);
extern void gv_BLK_AbortRx(void);

/* ---- Queries ------------------------------------------------------------- */
extern bool gb_BLK_IsTxBusy(void);
extern uint16_t gu16_BLK_GetMaxShortPayload(void);
extern void gv_BLK_GetCaps(BlkCaps_T *stpt_caps);

/* ---- GATT hook (GATTCustomWriteCb_F signature) --------------------------- */
extern ssize_t gt_BLK_RxWriteHook(struct bt_conn *stpt_connHandle,
   const struct bt_gatt_attr *stpt_attr, const void *vpt_buf, uint16_t u16_length,
   uint16_t u16_offset, uint8_t u8_flags);

#endif /* !_BULK_XFER_H */
