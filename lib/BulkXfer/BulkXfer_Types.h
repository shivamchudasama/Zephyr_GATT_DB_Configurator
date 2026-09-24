/**
 * @file          BulkXfer_Types.h
 * @brief         Types shared by the BulkXfer Server and Client roles:
 *                application callbacks, data source and capability record.
 *
 *                Threading: every callback in BlkSrvCfg_T / BlkCliCfg_T runs
 *                on the BulkXfer engine thread (never in BLE stack context),
 *                one at a time. Callbacks may call the BulkXfer API (e.g.
 *                start the next transfer from fpt_onTxDone). Long-running
 *                callbacks (a slow flash write in fpt_onRxData) throttle the
 *                link naturally: the receiver simply ACKs later.
 *
 * @date          22/09/2026
 * @author        Shivam Chudasama
 * @copyright     Shivam Chudasama
 * @license       MIT
 */

/* SPDX-License-Identifier: MIT */

#ifndef _BULK_XFER_TYPES_H
#define _BULK_XFER_TYPES_H

/******************************************************************************/
/*                                                                            */
/*                                  INCLUDES                                  */
/*                                                                            */
/******************************************************************************/
#include <stdint.h>
#include <stdbool.h>
#include <zephyr/bluetooth/gatt.h>
#include "BulkXfer_Frame.h"

/******************************************************************************/
/*                                                                            */
/*                                  DEFINES                                   */
/*                                                                            */
/******************************************************************************/
/**
 * @def           BLK_PROTOCOL_VERSION
 * @brief         Protocol version reported in BlkCaps_T. Version 2: the sender
 *                is the GATT client (writes DATA), the receiver the GATT
 *                server (notifies CTRL).
 */
#define BLK_PROTOCOL_VERSION                 (2U)

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
 * @typedef       BlkSourceRead_F
 * @brief         Sender data provider. Copies u16_len bytes starting at
 *                u32_offset of the object being sent into u8pt_buf.
 *
 *                The same range may be requested more than once (retransmit
 *                after NACK / timeout), so the source must stay readable until
 *                fpt_onTxDone is called.
 *
 * @param[in]     vpt_ctx Context pointer from BlkSource_T.
 * @param[in]     u32_offset Byte offset into the object.
 * @param[out]    u8pt_buf Destination (u16_len bytes).
 * @param[in]     u16_len Number of bytes to copy.
 * @return        0 on success, negative errno to abort the transfer with
 *                eBS_SOURCE_ERROR.
 */
typedef int (*BlkSourceRead_F)(void *vpt_ctx, uint32_t u32_offset,
   uint8_t *u8pt_buf, uint16_t u16_len);

/**
 * @struct        BlkSource_T
 * @brief         Data source of an outgoing multi-frame transfer.
 */
typedef struct
{
   BlkSourceRead_F fpt_read;                 /**< Data provider. Must not be NULL.       */
   void *vpt_ctx;                            /**< Passed back to fpt_read unchanged.     */
} BlkSource_T;

/**
 * @typedef       BlkRxStart_F
 * @brief         A peer announced an incoming transfer (START frame).
 * @param[in]     u8_appType Application type of the object.
 * @param[in]     u32_totalLen Total size in bytes.
 * @return        0 to accept, non-zero to reject (peer gets eBS_REJECTED).
 */
typedef int (*BlkRxStart_F)(uint8_t u8_appType, uint32_t u32_totalLen);

/**
 * @typedef       BlkRxData_F
 * @brief         In-order chunk of an incoming transfer. Offsets are strictly
 *                increasing and contiguous; no chunk is delivered twice.
 * @param[in]     u8_appType Application type of the object.
 * @param[in]     u32_offset Offset of this chunk within the object.
 * @param[in]     u8pt_data Chunk data (valid only during the call).
 * @param[in]     u16_len Chunk length.
 * @return        0 to continue, non-zero to abort (eBS_SINK_ERROR).
 */
typedef int (*BlkRxData_F)(uint8_t u8_appType, uint32_t u32_offset,
   const uint8_t *u8pt_data, uint16_t u16_len);

/**
 * @typedef       BlkRxDone_F
 * @brief         An incoming transfer finished. Only eBS_OK means the
 *                data delivered through BlkRxData_F is complete and verified.
 * @param[in]     u8_appType Application type of the object.
 * @param[in]     e_status Result.
 * @param[in]     u32_totalLen Size announced in START.
 */
typedef void (*BlkRxDone_F)(uint8_t u8_appType, BlkStatus_E e_status,
   uint32_t u32_totalLen);

/**
 * @typedef       BlkRxShort_F
 * @brief         A single-frame application message was received.
 * @param[in]     u8_appType Application type (0x00..BLK_APP_TYPE_MAX).
 * @param[in]     u8pt_data Payload (valid only during the call).
 * @param[in]     u8_len Payload length.
 */
typedef void (*BlkRxShort_F)(uint8_t u8_appType, const uint8_t *u8pt_data,
   uint8_t u8_len);

/**
 * @typedef       BlkTxDone_F
 * @brief         An outgoing transfer finished.
 * @param[in]     u8_appType Application type passed to gi_BLKC_Send().
 * @param[in]     e_status Result reported by the receiver, or a local error.
 */
typedef void (*BlkTxDone_F)(uint8_t u8_appType, BlkStatus_E e_status);

/**
 * @struct        BlkCaps_T
 * @brief         Capabilities exposed by the Server through the optional,
 *                read-only Caps characteristic (served by gt_GATT_GenericRead).
 */
typedef struct __packed
{
   uint8_t u8_protocolVersion;               /**< BLK_PROTOCOL_VERSION.                  */
   uint8_t u8_maxFrameLen;                   /**< BLK_MAX_FRAME_LEN.                     */
   uint8_t u8_window;                        /**< BLK_WINDOW_DEFAULT.                    */
   uint8_t u8_reserved;                      /**< 0.                                     */
} BlkCaps_T;

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

#endif // _BULK_XFER_TYPES_H
