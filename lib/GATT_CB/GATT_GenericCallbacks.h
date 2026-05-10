/**
 * @file          GATT_GenericCallbacks.h
 * @brief         Declarations of the shared GATT read/write callbacks used by
 *                every characteristic in the local GATT database framework.
 *
 *                Rather than writing a unique read or write callback per
 *                characteristic, every BT_GATT_CHARACTERISTIC macro in the
 *                project registers one of these two functions as its Zephyr
 *                callback and passes a pointer to a GATTCharDescriptor_t as
 *                user_data. The descriptor supplies everything the generic
 *                callback needs to service the request:
 *
 *                  - vpt_data / u16_actualLen → the local variable to read from
 *                                               or write to.
 *                  - u16_dataLen              → maximum allowed write length.
 *                  - b_variableLength         → length validation strategy.
 *                  - stpt_mutex               → optional thread-safety lock.
 *                  - fpt_customReadCb         → optional post-read side-effects.
 *                  - fpt_customWriteCb        → optional post-write side-effects.
 *
 * @date          23/03/2026
 * @author        Shivam Chudasama
 * @copyright     Shivam Chudasama
 * @license       MIT
 */

/* SPDX-License-Identifier: MIT */

#ifndef _GATT_GENERIC_CALLBACKS_H
#define _GATT_GENERIC_CALLBACKS_H

/******************************************************************************/
/*                                                                            */
/*                                  INCLUDES                                  */
/*                                                                            */
/******************************************************************************/
#include <string.h>
#include <errno.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/__assert.h>
#include <zephyr/bluetooth/gatt.h>
#include "GATT_CB_Types.h"
#include "AppLog.h"

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
/* ---- BLE stack callbacks (registered in BT_GATT_CHARACTERISTIC) --------- */
extern ssize_t gt_GATT_GenericRead(struct bt_conn *stpt_connHandle,
   const struct bt_gatt_attr *stpt_attr, void *vpt_buf, uint16_t u16_length,
   uint16_t u16_offset);
extern ssize_t gt_GATT_GenericWrite(struct bt_conn *stpt_connHandle,
   const struct bt_gatt_attr *stpt_attr, const void *vpt_buf, uint16_t u16_length,
   uint16_t u16_offset, uint8_t u8_flags);

/* ---- Application-side local GATT database access APIs ------------------- */
extern void gt_GATT_LocalRead(const GATTCharDescriptor_T *stpt_desc,
   void *vpt_buf, uint16_t u16_bufLen, uint16_t *u16pt_bytesRead);
extern void gv_GATT_LocalWrite(GATTCharDescriptor_T *stpt_desc,
   const void *vpt_buf, uint16_t u16_length);
extern void gt_GATT_GetActualLen(const GATTCharDescriptor_T *stpt_desc,
   uint16_t *u16pt_actualLen);

#endif /* !_GATT_GENERIC_CALLBACKS_H */
