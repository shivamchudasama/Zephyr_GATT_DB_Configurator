/**
 * @file          GATT_CB_Types.h
 * @brief         Common type definitions for the local GATT database framework.
 *
 *                Defines the GATTCharDescriptor_T structure that is passed as
 *                user_data to every BT_GATT_CHARACTERISTIC macro call. It links
 *                each characteristic to:
 *                  - Its local data variable (any type, via void pointer).
 *                  - Its maximum and current data lengths.
 *                  - An optional mutex for thread-safe access to the local
 *                    variable from outside BLE callback context.
 *                  - Optional custom read/write hooks for side-effect logic
 *                    beyond simple local DB access.
 *
 *                The two generic callbacks (gt_GATT_GenericRead / gt_GATT_GenericWrite,
 *                declared in GATT_GenericCallbacks.h) are the only read/write
 *                callbacks ever passed to BT_GATT_CHARACTERISTIC across the
 *                entire project. Per-characteristic behaviour that goes beyond
 *                a plain local DB copy is handled exclusively through the custom
 *                hooks registered in GATTCharDescriptor_T.
 *
 * @date          23/03/2026
 * @author        Shivam Chudasama
 * @copyright     Shivam Chudasama
 * @license       MIT
 */

/* SPDX-License-Identifier: MIT */

#ifndef _GATT_CB_TYPES_H
#define _GATT_CB_TYPES_H

/******************************************************************************/
/*                                                                            */
/*                                  INCLUDES                                  */
/*                                                                            */
/******************************************************************************/
#include <zephyr/kernel.h>
#include <zephyr/bluetooth/gatt.h>
#include <stdint.h>
#include <stdbool.h>

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
 * @typedef       GATTCustomReadCb_F
 * @brief         Function pointer type for an optional post-read hook.
 *
 *                Called by gt_GATT_GenericRead after it has already placed the
 *                current value of the local data variable into the output
 *                buffer via bt_gatt_attr_read(). The hook may inspect or
 *                overwrite the buffer contents and may override the return
 *                value returned to the BLE stack.
 *
 *                Return 0 to keep gt_GATT_GenericRead's return value unchanged.
 *                Return any non-zero ssize_t (including BT_GATT_ERR()) to
 *                replace it.
 *
 * @note          The mutex (if configured) is released before this hook is
 *                called. Do not assume exclusive access to the local variable
 *                inside this hook unless you re-acquire the mutex yourself.
 *
 * @param[inout]  stpt_connHandle  Active connection handle.
 * @param[in]     stpt_attr Attribute that was read.
 * @param[inout]  vpt_buf Output buffer already populated by the generic callback.
 * @param[in]     u16_length Requested read length, as passed by the stack.
 * @param[in]     u16_offset Read offset, as passed by the stack.
 * @return        0 to keep the generic callback's return value, or a custom
 *                byte count / BT_GATT_ERR() to override it.
 */
typedef ssize_t (*GATTCustomReadCb_F)(struct bt_conn *stpt_connHandle,
   const struct bt_gatt_attr *stpt_attr, void *vpt_buf, uint16_t u16_length,
   uint16_t u16_offset);

/**
 * @typedef       GATTCustomWriteCb_F
 * @brief         Function pointer type for an optional post-write hook.
 *
 *                Called by gt_GATT_GenericWrite after it has already validated the
 *                incoming length and written the data into the local variable.
 *                The hook is intended for side effects only: notifying a state
 *                machine, triggering a BLE indication, or validating and
 *                rejecting a semantically invalid value.
 *
 *                Return 0 to keep gt_GATT_GenericWrite's return value unchanged.
 *                Return any non-zero ssize_t (including BT_GATT_ERR()) to
 *                replace it. Returning BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED)
 *                is the correct way to reject a write based on its value after
 *                inspecting it in this hook.
 *
 * @note          The local variable has already been updated when this hook is
 *                called. If the hook rejects the value via BT_GATT_ERR(), the
 *                remote client is notified of the failure but the local variable
 *                is NOT automatically rolled back. If rollback is required,
 *                implement it inside the hook before returning the error code.
 *
 * @note          The mutex (if configured) is released before this hook is
 *                called. Do not assume exclusive access to the local variable
 *                inside this hook unless you re-acquire the mutex yourself.
 *
 * @param[inout]  stpt_connHandle Active connection handle.
 * @param[in]     stpt_attr Attribute that was written.
 * @param[in]     vpt_buf Buffer containing the data that was written.
 *                Points to the same data that now lives in the local variable.
 * @param[in]     u16_length Number of bytes written in this operation.
 * @param[in]     u16_offset Write offset, as passed by the stack.
 * @param[in]     u8_flags Write flags (see bt_gatt_attr_write_flag).
 * @return        0 to keep the generic callback's return value, or a custom
 *                byte count / BT_GATT_ERR() to override it.
 */
typedef ssize_t (*GATTCustomWriteCb_F)(struct bt_conn *stpt_connHandle,
   const struct bt_gatt_attr *stpt_attr, const void *vpt_buf, uint16_t u16_length,
   uint16_t u16_offset, uint8_t u8_flags);

/**
 * @struct        GATTCharDescriptor_T
 * @brief         Per-characteristic descriptor for the local GATT database.
 *
 *                One instance of this struct is defined per characteristic (as
 *                a file-scope static variable inside the relevant service .c
 *                file) and passed as user_data to BT_GATT_CHARACTERISTIC. The
 *                generic callbacks (gt_GATT_GenericRead / gt_GATT_GenericWrite)
 *                retrieve it via attr->user_data at runtime.
 * @par           Fixed-length characteristic, no custom hooks, no mutex:
 * @code
 *                static uint8_t gu8_pairingStatus = 0U;
 *
 *                static GATTCharDescriptor_T gst_pairingStatusDesc = {
 *                    .vpt_data         = &gu8_pairingStatus,
 *                    .u16_dataLen      = sizeof(gu8_pairingStatus),
 *                    .u16_actualLen    = sizeof(gu8_pairingStatus),
 *                    .b_variableLength = false,
 *                    .stpt_mutex       = NULL,
 *                    .fpt_customReadCb = NULL,
 *                    .fpt_customWriteCb= NULL,
 *                };
 * @endcode
 * @par           Variable-length characteristic with mutex and a custom write hook:
 * @code
 *                static uint8_t gu8ar_csrData[244U] = { 0U };
 *                static K_MUTEX_DEFINE(sst_csrDataMutex);
 *
 *                static GATTCharDescriptor_T sst_csrDataDesc = {
 *                    .vpt_data         = gu8ar_csrData,
 *                    .u16_dataLen      = sizeof(gu8ar_csrData),     // buffer / maximum size
 *                    .u16_actualLen    = 0U,                        // no data held yet
 *                    .b_variableLength = true,
 *                    .stpt_mutex       = &sst_csrDataMutex,
 *                    .fpt_customReadCb = NULL,
 *                    .fpt_customWriteCb= st_OnCSRDataWrite,
 *                };
 * @endcode
 */
typedef struct
{
   /**
    * @brief      Pointer to the local variable that stores the characteristic's
    *             current value. May point to any type (uint8_t, struct, array,
    *             etc.). Must not be NULL.
    */
   void *vpt_data;

   /**
    * @brief      Maximum byte length of the buffer pointed to by vpt_data.
    *
    *             Fixed-length: set to sizeof(*vpt_data). Never changes.
    *
    *             Variable-length: set to the total size of the allocated buffer
    *             (e.g. 244 for a 244-byte CSR buffer). Incoming writes are
    *             rejected with BT_ATT_ERR_INVALID_ATTRIBUTE_LEN when:
    *               (u16_offset + incoming_length) > u16_dataLen.
    */
   uint16_t u16_dataLen;

   /**
    * @brief      Current byte length of the valid data in vpt_data.
    *
    *             Fixed-length: always equals u16_dataLen. Never modified by
    *             the framework.
    *
    *             Variable-length: updated by gt_GATT_GenericWrite to
    *             (u16_offset + u16_length) after each successful write, so
    *             that gt_GATT_GenericRead returns exactly the bytes that have
    *             been populated, not the full buffer size.
    *             Initialise to 0 when the buffer is empty, or to the
    *             pre-populated data length when the buffer already contains
    *             valid data at service registration time.
    */
   uint16_t u16_actualLen;

   /**
    * @brief      Set to true for characteristics declared with
    *             variable_length="true" in the GATT XML descriptor.
    *
    *             When true:  gt_GATT_GenericWrite accepts any incoming length
    *             where (u16_offset + incoming_length) <= u16_dataLen, and
    *             updates u16_actualLen after a successful write.
    *
    *             When false: gt_GATT_GenericWrite requires that
    *             (u16_offset + incoming_length) == u16_dataLen exactly,
    *             rejecting partial or oversized writes.
    */
   bool b_variableLength;

   /**
    * @brief      Optional Zephyr mutex for thread-safe access to vpt_data.
    *
    *             Set to NULL when the characteristic's local variable is only
    *             ever accessed from BLE stack callback context (i.e. no
    *             concurrent access from application threads).
    *
    *             When non-NULL, gt_GATT_GenericRead and gt_GATT_GenericWrite acquire
    *             this mutex (K_FOREVER) around the copy to/from vpt_data. The
    *             mutex is released before the custom hook is called, so the
    *             hook does NOT hold the mutex. If the hook also needs to access
    *             vpt_data safely, it must re-acquire the mutex itself.
    *
    *             Declare and initialise with K_MUTEX_DEFINE() at file scope in
    *             the same .c file as the local variable.
    */
   struct k_mutex *stpt_mutex;

   /**
    * @brief      Optional post-read hook. Set to NULL to perform a plain local
    *             DB read with no additional logic.
    *             See GATTCustomReadCb_F for the full calling convention.
    */
   GATTCustomReadCb_F fpt_customReadCb;

   /**
    * @brief      Optional post-write hook. Set to NULL to perform a plain local
    *             DB write with no additional logic.
    *             See GATTCustomWriteCb_F for the full calling convention.
    */
   GATTCustomWriteCb_F fpt_customWriteCb;

} GATTCharDescriptor_T;

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

#endif /* !_GATT_CB_TYPES_H */
