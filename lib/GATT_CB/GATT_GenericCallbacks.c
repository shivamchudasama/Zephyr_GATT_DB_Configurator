/**
 * @file          GATT_GenericCallbacks.c
 * @brief         Implementation of the shared GATT read/write callbacks for
 *                the local GATT database framework.
 * @date          23/03/2026
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
#include "GATT_GenericCallbacks.h"

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
/*                       PRIVATE FUNCTION DECLARATIONS                        */
/*                                                                            */
/******************************************************************************/

/******************************************************************************/
/*                                                                            */
/*                              EXTERN VARIABLES                              */
/*                                                                            */
/******************************************************************************/

/******************************************************************************/
/*                                                                            */
/*                             PRIVATE VARIABLES                              */
/*                                                                            */
/******************************************************************************/

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

/******************************************************************************/
/*                                                                            */
/*                        PUBLIC FUNCTION DEFINITIONS                         */
/*                                                                            */
/******************************************************************************/
/**
 * @public        gt_GATT_GenericRead
 * @brief         Generic GATT read callback shared by all readable
 *                characteristics.
 *
 *                Execution sequence:
 *                  1. Retrieve the GATTCharDescriptor_t from attr->user_data.
 *                  2. Acquire stpt_mutex (if non-NULL).
 *                  3. Call bt_gatt_attr_read() to copy vpt_data (u16_actualLen
 *                     bytes) into vpt_buf, honouring u16_offset and u16_length.
 *                  4. Release stpt_mutex (if non-NULL).
 *                  5. If fpt_customReadCb is non-NULL, invoke it. If it returns
 *                     a non-zero value, substitute that for the return value.
 *
 * @param[inout]  stpt_connHandle Active connection handle.
 * @param[in]     stpt_attr Attribute being read. attr->user_data must
 *                point to a valid GATTCharDescriptor_t.
 * @param[out]    vpt_buf Buffer to fill with the characteristic value.
 * @param[in]     u16_length Maximum number of bytes the stack can accept.
 * @param[in]     u16_offset Offset into the value to start reading from.
 * @return        Number of bytes written to vpt_buf on success, or
 *                BT_GATT_ERR() on failure.
 */
ssize_t gt_GATT_GenericRead(struct bt_conn *stpt_connHandle,
   const struct bt_gatt_attr *stpt_attr, void *vpt_buf, uint16_t u16_length,
   uint16_t u16_offset)
{
   ssize_t t_retVal = 0;
   const GATTCharDescriptor_T *stpt_desc = NULL;
   ssize_t t_hookRetVal = 0;

   /* ------------------------------------------------------------------ */
   /* 1. Retrieve and validate the characteristic descriptor.            */
   /* ------------------------------------------------------------------ */
   __ASSERT(stpt_attr != NULL, "gt_GATT_GenericRead: stpt_attr is NULL");
   __ASSERT(stpt_attr->user_data != NULL,
      "gt_GATT_GenericRead: user_data is NULL - descriptor not set");

   stpt_desc = (const GATTCharDescriptor_T *)stpt_attr->user_data;

   // Check if the user data is assigned with the given characteristic
   if (stpt_desc->vpt_data == NULL)
   {
      APP_LOG_ERR("gt_GATT_GenericRead: vpt_data is NULL in descriptor");
      return BT_GATT_ERR(BT_ATT_ERR_UNLIKELY);
   }

   /* ------------------------------------------------------------------ */
   /* 2. Acquire mutex (if configured) before accessing the local        */
   /*    variable.                                                       */
   /* ------------------------------------------------------------------ */
   // Check if the mutex is assigned to the given characteristic
   if (stpt_desc->stpt_mutex != NULL)
   {
      (void)k_mutex_lock(stpt_desc->stpt_mutex, K_FOREVER);
   }

   /* ------------------------------------------------------------------ */
   /* 3. Copy the local variable into the output buffer.                 */
   /*    bt_gatt_attr_read() handles offset validation and clips         */
   /*    the copy to the smaller of u16_actualLen and the stack-supplied */
   /*    u16_length.                                                     */
   /* ------------------------------------------------------------------ */
   t_retVal = bt_gatt_attr_read(stpt_connHandle, stpt_attr, vpt_buf,
      u16_length, u16_offset, stpt_desc->vpt_data, stpt_desc->u16_actualLen);

   /* ------------------------------------------------------------------ */
   /* 4. Release mutex before calling into user-supplied hook code.      */
   /* ------------------------------------------------------------------ */
   // Check if the mutex is assigned to the given characteristic
   if (stpt_desc->stpt_mutex != NULL)
   {
      k_mutex_unlock(stpt_desc->stpt_mutex);
   }

   /* ------------------------------------------------------------------ */
   /* 5. Invoke the optional post-read hook (if registered).             */
   /*    A non-zero return from the hook replaces t_retVal.              */
   /* ------------------------------------------------------------------ */
   // Check if any custom callback is assigned to the given characteristic
   if (stpt_desc->fpt_customReadCb != NULL)
   {
      t_hookRetVal = stpt_desc->fpt_customReadCb(stpt_connHandle, stpt_attr,
         vpt_buf, u16_length, u16_offset);

      // Check if the custom callback has returned with any error
      if (t_hookRetVal != 0)
      {
         t_retVal = t_hookRetVal;
      }
   }

   return t_retVal;
}

/**
 * @public        gt_GATT_GenericWrite
 * @brief         Generic GATT write callback shared by all writable
 *                characteristics.
 *
 *                Execution sequence:
 *                  1. Retrieve the GATTCharDescriptor_t from attr->user_data.
 *                  2. Validate that (u16_offset + u16_length) does not violate
 *                     the configured length constraints (exact match for
 *                     fixed-length; upper-bound check for variable-length).
 *                  3. Acquire stpt_mutex (if non-NULL).
 *                  4. memcpy vpt_buf into vpt_data at the given offset.
 *                  5. For variable-length characteristics, update u16_actualLen
 *                     to (u16_offset + u16_length).
 *                  6. Release stpt_mutex (if non-NULL).
 *                  7. If fpt_customWriteCb is non-NULL, invoke it. If it returns
 *                     a non-zero value, substitute that for the return value.
 *
 * @param[inout]  stpt_connHandle Active connection handle.
 * @param[in]     stpt_attr Attribute being written. attr->user_data must
 *                point to a valid GATTCharDescriptor_t.
 * @param[in]     vpt_buf Buffer containing the incoming data.
 * @param[in]     u16_length Number of bytes in vpt_buf.
 * @param[in]     u16_offset Offset into the characteristic value to start
 *                writing at (non-zero for long write procedures).
 * @param[in]     u8_flags Write flags (see bt_gatt_attr_write_flag).
 * @return        u16_length on success, or BT_GATT_ERR() on failure.
 */
ssize_t gt_GATT_GenericWrite(struct bt_conn *stpt_connHandle,
   const struct bt_gatt_attr *stpt_attr, const void *vpt_buf, uint16_t u16_length,
   uint16_t u16_offset, uint8_t u8_flags)
{
   ssize_t t_retVal = 0;
   GATTCharDescriptor_T *stpt_desc = NULL;
   uint32_t u32_writeEnd = 0U;
   ssize_t t_hookRetVal = 0;

   /* ------------------------------------------------------------------ */
   /* 1. Retrieve and validate the characteristic descriptor.            */
   /* ------------------------------------------------------------------ */
   __ASSERT(stpt_attr != NULL, "gt_GATT_GenericWrite: stpt_attr is NULL");
   __ASSERT(stpt_attr->user_data != NULL,
      "gt_GATT_GenericWrite: user_data is NULL - descriptor not set");

   stpt_desc = (GATTCharDescriptor_T *)stpt_attr->user_data;

   // Check if the user data is assigned with the given characteristic
   if (stpt_desc->vpt_data == NULL)
   {
      APP_LOG_ERR("gt_GATT_GenericWrite: vpt_data is NULL in descriptor");
      return BT_GATT_ERR(BT_ATT_ERR_UNLIKELY);
   }

   /* ------------------------------------------------------------------ */
   /* 2. Length validation.                                              */
   /*                                                                    */
   /*    u32_writeEnd holds (u16_offset + u16_length) as a 32-bit value  */
   /*    to detect wrapping before the comparison against u16_dataLen.   */
   /*                                                                    */
   /*    Fixed-length:    the combined offset + length must equal        */
   /*                     u16_dataLen exactly.                           */
   /*    Variable-length: the combined offset + length must not exceed   */
   /*                     u16_dataLen (partial writes are accepted).     */
   /* ------------------------------------------------------------------ */
   u32_writeEnd = (uint32_t)u16_offset + (uint32_t)u16_length;

   // Check if the maximum write length exceeds the data length
   if (u32_writeEnd > (uint32_t)stpt_desc->u16_dataLen)
   {
      APP_LOG_ERR("gt_GATT_GenericWrite: write of %u bytes at offset %u exceeds "
         "max length %u", (unsigned)u16_length, (unsigned)u16_offset,
         (unsigned)stpt_desc->u16_dataLen);
      return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
   }

   // Check if the characteristic is not of variable length
   if (!stpt_desc->b_variableLength)
   {
      // Check if the maximum write length is not equal to data length
      if (u32_writeEnd != (uint32_t)stpt_desc->u16_dataLen)
      {
         APP_LOG_ERR("gt_GATT_GenericWrite: fixed-length characteristic requires "
            "exactly %u bytes, got offset=%u length=%u", (unsigned)stpt_desc->u16_dataLen,
            (unsigned)u16_offset, (unsigned)u16_length);
         return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
      }
   }

   /* ------------------------------------------------------------------ */
   /* 3. Acquire mutex (if configured) before modifying the local        */
   /*    variable.                                                       */
   /* ------------------------------------------------------------------ */
   // Check if the mutex is assigned to the given characteristic
   if (stpt_desc->stpt_mutex != NULL)
   {
      (void)k_mutex_lock(stpt_desc->stpt_mutex, K_FOREVER);
   }

   /* ------------------------------------------------------------------ */
   /* 4. Write incoming data into the local variable at the given offset.*/
   /* ------------------------------------------------------------------ */
   (void)memcpy((uint8_t *)stpt_desc->vpt_data + u16_offset, vpt_buf, u16_length);

   /* ------------------------------------------------------------------ */
   /* 5. For variable-length characteristics, advance u16_actualLen to   */
   /*    reflect the new populated length. For fixed-length,             */
   /*    u16_actualLen always equals u16_dataLen and is never modified   */
   /*    here.                                                           */
   /* ------------------------------------------------------------------ */
   // Check if the characteristic is of variable length
   if (stpt_desc->b_variableLength)
   {
      // Check if the maximum write length is more than actual length
      if ((uint16_t)u32_writeEnd > stpt_desc->u16_actualLen)
      {
         stpt_desc->u16_actualLen = (uint16_t)u32_writeEnd;
      }
   }

   t_retVal = (ssize_t)u16_length;

   /* ------------------------------------------------------------------ */
   /* 6. Release mutex before calling into user-supplied hook code.      */
   /* ------------------------------------------------------------------ */
   // Check if the mutex is assigned to the given characteristic
   if (stpt_desc->stpt_mutex != NULL)
   {
      k_mutex_unlock(stpt_desc->stpt_mutex);
   }

   /* ------------------------------------------------------------------ */
   /* 7. Invoke the optional post-write hook (if registered).            */
   /*    A non-zero return from the hook replaces t_retVal.              */
   /* ------------------------------------------------------------------ */
   // Check if any custom callback is assigned to the given characteristic
   if (stpt_desc->fpt_customWriteCb != NULL)
   {
      t_hookRetVal = stpt_desc->fpt_customWriteCb(stpt_connHandle, stpt_attr,
         vpt_buf, u16_length, u16_offset, u8_flags);

      // Check if the custom callback has returned with any error
      if (t_hookRetVal != 0)
      {
         t_retVal = t_hookRetVal;
      }
   }

   return t_retVal;
}

/**
 * @public        gt_GATT_LocalRead
 * @brief         Application-side API to fetch the current value of a
 *                characteristic from the local GATT database.
 *
 *                This function is the complement of gt_GATT_GenericRead for
 *                application threads: whereas gt_GATT_GenericRead is invoked by
 *                the BLE stack in response to a remote GATT client read request,
 *                gt_GATT_LocalRead allows any application thread to retrieve the
 *                same data locally.
 *
 *                Execution sequence:
 *                  1. Assert stpt_desc, vpt_buf, u16pt_bytesRead, and
 *                     vpt_data pointers.
 *                  2. Acquire stpt_mutex (if non-NULL).
 *                  3. Clip the copy length to the smaller of u16_bufLen and
 *                     u16_actualLen. memcpy the clipped amount from vpt_data
 *                     into vpt_buf.
 *                  4. Release stpt_mutex (if non-NULL).
 *                  5. Write the number of bytes copied to *u16pt_bytesRead.
 *
 * @param[in]     stpt_desc Pointer to the characteristic descriptor whose
 *                local value is to be read. Must not be NULL.
 * @param[out]    vpt_buf Caller-supplied buffer to receive the data. Must not
 *                be NULL.
 * @param[in]     u16_bufLen Size of vpt_buf in bytes. If smaller than the
 *                characteristic's u16_actualLen, only u16_bufLen bytes are
 *                copied (no error is raised; the caller can detect truncation
 *                by comparing *u16pt_bytesRead with a subsequent
 *                gt_GATT_GetActualLen call).
 * @param[out]    u16pt_bytesRead Receives the number of bytes actually copied
 *                into vpt_buf. Must not be NULL.
 * @return        void
 */
void gt_GATT_LocalRead(const GATTCharDescriptor_T *stpt_desc,
   void *vpt_buf, uint16_t u16_bufLen, uint16_t *u16pt_bytesRead)
{
   uint16_t u16_copyLen = 0U;

   /* ------------------------------------------------------------------ */
   /* 1. Assert that all pointers are non-NULL.                          */
   /* ------------------------------------------------------------------ */
   __ASSERT(stpt_desc != NULL,
      "gt_GATT_LocalRead: stpt_desc is NULL");
   __ASSERT(vpt_buf != NULL,
      "gt_GATT_LocalRead: vpt_buf is NULL");
   __ASSERT(u16pt_bytesRead != NULL,
      "gt_GATT_LocalRead: u16pt_bytesRead is NULL");
   __ASSERT(stpt_desc->vpt_data != NULL,
      "gt_GATT_LocalRead: vpt_data is NULL in descriptor");

   /* ------------------------------------------------------------------ */
   /* 2. Acquire mutex (if configured) before accessing the local        */
   /*    variable.                                                       */
   /* ------------------------------------------------------------------ */
   // Check if the mutex is assigned to the given characteristic
   if (stpt_desc->stpt_mutex != NULL)
   {
      (void)k_mutex_lock(stpt_desc->stpt_mutex, K_FOREVER);
   }

   /* ------------------------------------------------------------------ */
   /* 3. Copy the local variable into the caller-supplied buffer.        */
   /*    Clip to the smaller of the caller's buffer size and the         */
   /*    characteristic's current actual length.                         */
   /* ------------------------------------------------------------------ */
   u16_copyLen = (u16_bufLen < stpt_desc->u16_actualLen)
      ? u16_bufLen
      : stpt_desc->u16_actualLen;

   (void)memcpy(vpt_buf, stpt_desc->vpt_data, u16_copyLen);

   /* ------------------------------------------------------------------ */
   /* 4. Release mutex.                                                  */
   /* ------------------------------------------------------------------ */
   // Check if the mutex is assigned to the given characteristic
   if (stpt_desc->stpt_mutex != NULL)
   {
      k_mutex_unlock(stpt_desc->stpt_mutex);
   }

   /* ------------------------------------------------------------------ */
   /* 5. Report the number of bytes copied.                              */
   /* ------------------------------------------------------------------ */
   *u16pt_bytesRead = u16_copyLen;
}

/**
 * @public        gv_GATT_LocalWrite
 * @brief         Application-side API to update the value of a characteristic
 *                in the local GATT database.
 *
 *                This function is the complement of gt_GATT_GenericWrite for
 *                application threads: whereas gt_GATT_GenericWrite is invoked
 *                by the BLE stack when a remote GATT client writes, this API
 *                lets any application thread update the same data locally so
 *                that subsequent remote reads (or local reads) see the new
 *                value.
 *
 *                Execution sequence:
 *                  1. Assert stpt_desc, vpt_buf, and vpt_data pointers.
 *                  2. Validate that u16_length does not violate the configured
 *                     length constraints (exact match for fixed-length;
 *                     upper-bound check for variable-length).
 *                  3. Acquire stpt_mutex (if non-NULL).
 *                  4. memcpy vpt_buf into vpt_data.
 *                  5. For variable-length characteristics, update u16_actualLen
 *                     to u16_length.
 *                  6. Release stpt_mutex (if non-NULL).
 *
 * @note          This function does NOT invoke the custom write hook
 *                (fpt_customWriteCb). The custom hook is reserved for
 *                writes originating from a remote GATT client. If the
 *                application needs side-effect processing when writing
 *                locally, it must handle that at the call site.
 *
 * @param[inout]  stpt_desc Pointer to the characteristic descriptor whose
 *                local value is to be updated. Must not be NULL.
 * @param[in]     vpt_buf Caller-supplied buffer containing the new data.
 *                Must not be NULL.
 * @param[in]     u16_length Number of bytes in vpt_buf.
 *                Fixed-length:    must equal stpt_desc->u16_dataLen exactly.
 *                Variable-length: must not exceed stpt_desc->u16_dataLen.
 * @return        void
 */
void gv_GATT_LocalWrite(GATTCharDescriptor_T *stpt_desc,
   const void *vpt_buf, uint16_t u16_length)
{
   /* ------------------------------------------------------------------ */
   /* 1. Assert that all pointers are non-NULL.                          */
   /* ------------------------------------------------------------------ */
   __ASSERT(stpt_desc != NULL,
      "gv_GATT_LocalWrite: stpt_desc is NULL");
   __ASSERT(vpt_buf != NULL,
      "gv_GATT_LocalWrite: vpt_buf is NULL");
   __ASSERT(stpt_desc->vpt_data != NULL,
      "gv_GATT_LocalWrite: vpt_data is NULL in descriptor");

   /* ------------------------------------------------------------------ */
   /* 2. Length validation.                                              */
   /*                                                                    */
   /*    Fixed-length:    u16_length must equal u16_dataLen exactly.     */
   /*    Variable-length: u16_length must not exceed u16_dataLen.        */
   /* ------------------------------------------------------------------ */
   // Check if the write length exceeds the maximum data length
   if ((uint32_t)u16_length > (uint32_t)stpt_desc->u16_dataLen)
   {
      APP_LOG_ERR("gv_GATT_LocalWrite: write of %u bytes exceeds max length %u",
         (unsigned)u16_length, (unsigned)stpt_desc->u16_dataLen);
      return;
   }

   // Check if the characteristic is not of variable length
   if (!stpt_desc->b_variableLength)
   {
      // Check if the write length is not equal to the required fixed length
      if (u16_length != stpt_desc->u16_dataLen)
      {
         APP_LOG_ERR("gv_GATT_LocalWrite: fixed-length characteristic requires "
            "exactly %u bytes, got %u", (unsigned)stpt_desc->u16_dataLen,
            (unsigned)u16_length);
         return;
      }
   }

   /* ------------------------------------------------------------------ */
   /* 3. Acquire mutex (if configured) before modifying the local        */
   /*    variable.                                                       */
   /* ------------------------------------------------------------------ */
   // Check if the mutex is assigned to the given characteristic
   if (stpt_desc->stpt_mutex != NULL)
   {
      (void)k_mutex_lock(stpt_desc->stpt_mutex, K_FOREVER);
   }

   /* ------------------------------------------------------------------ */
   /* 4. Write caller data into the local variable.                      */
   /* ------------------------------------------------------------------ */
   (void)memcpy(stpt_desc->vpt_data, vpt_buf, u16_length);

   /* ------------------------------------------------------------------ */
   /* 5. For variable-length characteristics, update u16_actualLen to    */
   /*    reflect the new populated length. For fixed-length,             */
   /*    u16_actualLen always equals u16_dataLen and is never modified   */
   /*    here.                                                           */
   /* ------------------------------------------------------------------ */
   // Check if the characteristic is of variable length
   if (stpt_desc->b_variableLength)
   {
      stpt_desc->u16_actualLen = u16_length;
   }

   /* ------------------------------------------------------------------ */
   /* 6. Release mutex.                                                  */
   /* ------------------------------------------------------------------ */
   // Check if the mutex is assigned to the given characteristic
   if (stpt_desc->stpt_mutex != NULL)
   {
      k_mutex_unlock(stpt_desc->stpt_mutex);
   }
}

/**
 * @public        gt_GATT_GetActualLen
 * @brief         Application-side API to query the current valid data length
 *                of a characteristic in the local GATT database.
 *
 *                For fixed-length characteristics this always returns
 *                u16_dataLen. For variable-length characteristics it returns
 *                the number of bytes that have been populated by the most
 *                recent write (remote or local).
 *
 *                Execution sequence:
 *                  1. Assert stpt_desc and u16pt_actualLen pointers.
 *                  2. Acquire stpt_mutex (if non-NULL).
 *                  3. Copy u16_actualLen into *u16pt_actualLen.
 *                  4. Release stpt_mutex (if non-NULL).
 *
 * @param[in]     stpt_desc Pointer to the characteristic descriptor whose
 *                actual length is to be queried. Must not be NULL.
 * @param[out]    u16pt_actualLen Receives the current valid data length in
 *                bytes. Must not be NULL.
 * @return        void
 */
void gt_GATT_GetActualLen(const GATTCharDescriptor_T *stpt_desc,
   uint16_t *u16pt_actualLen)
{

   /* ------------------------------------------------------------------ */
   /* 1. Assert that all pointers are non-NULL.                          */
   /* ------------------------------------------------------------------ */
   __ASSERT(stpt_desc != NULL,
      "gt_GATT_GetActualLen: stpt_desc is NULL");
   __ASSERT(u16pt_actualLen != NULL,
      "gt_GATT_GetActualLen: u16pt_actualLen is NULL");

   /* ------------------------------------------------------------------ */
   /* 2. Acquire mutex (if configured) before reading the length field.  */
   /* ------------------------------------------------------------------ */
   // Check if the mutex is assigned to the given characteristic
   if (stpt_desc->stpt_mutex != NULL)
   {
      (void)k_mutex_lock(stpt_desc->stpt_mutex, K_FOREVER);
   }

   /* ------------------------------------------------------------------ */
   /* 3. Copy the actual length.                                         */
   /* ------------------------------------------------------------------ */
   *u16pt_actualLen = stpt_desc->u16_actualLen;

   /* ------------------------------------------------------------------ */
   /* 4. Release mutex.                                                  */
   /* ------------------------------------------------------------------ */
   // Check if the mutex is assigned to the given characteristic
   if (stpt_desc->stpt_mutex != NULL)
   {
      k_mutex_unlock(stpt_desc->stpt_mutex);
   }
}
