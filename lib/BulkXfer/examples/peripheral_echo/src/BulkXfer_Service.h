/**
 * @file          BulkXfer_Service.h
 * @brief         UUIDs and init API of the example BulkXfer GATT service.
 * @date          22/09/2026
 * @author        Shivam Chudasama
 * @copyright     Shivam Chudasama
 * @license       MIT
 */

/* SPDX-License-Identifier: MIT */

#ifndef _BULK_XFER_SERVICE_H
#define _BULK_XFER_SERVICE_H

/******************************************************************************/
/*                                                                            */
/*                                  INCLUDES                                  */
/*                                                                            */
/******************************************************************************/
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/bluetooth/gatt.h>

/******************************************************************************/
/*                                                                            */
/*                                  DEFINES                                   */
/*                                                                            */
/******************************************************************************/
/**
 * @def           BT_UUID_BULK_XFER_SVC_VAL
 * @brief         128-bit UUID of the BulkXfer service:
 *                B1C00001-2F5B-4E6A-9C1D-7A3E5F8B0C21.
 */
#define BT_UUID_BULK_XFER_SVC_VAL \
   BT_UUID_128_ENCODE(0xB1C00001, 0x2F5B, 0x4E6A, 0x9C1D, 0x7A3E5F8B0C21)

/**
 * @def           BT_UUID_BULK_XFER_RX_VAL
 * @brief         128-bit UUID of the BulkXfer RX characteristic
 *                (central -> device frames):
 *                B1C00002-2F5B-4E6A-9C1D-7A3E5F8B0C21.
 */
#define BT_UUID_BULK_XFER_RX_VAL \
   BT_UUID_128_ENCODE(0xB1C00002, 0x2F5B, 0x4E6A, 0x9C1D, 0x7A3E5F8B0C21)

/**
 * @def           BT_UUID_BULK_XFER_TX_VAL
 * @brief         128-bit UUID of the BulkXfer TX characteristic
 *                (device -> central frames):
 *                B1C00003-2F5B-4E6A-9C1D-7A3E5F8B0C21.
 */
#define BT_UUID_BULK_XFER_TX_VAL \
   BT_UUID_128_ENCODE(0xB1C00003, 0x2F5B, 0x4E6A, 0x9C1D, 0x7A3E5F8B0C21)

/**
 * @def           BT_UUID_BULK_XFER_CAPS_VAL
 * @brief         128-bit UUID of the BulkXfer Caps characteristic (BlkCaps_T):
 *                B1C00004-2F5B-4E6A-9C1D-7A3E5F8B0C21.
 */
#define BT_UUID_BULK_XFER_CAPS_VAL \
   BT_UUID_128_ENCODE(0xB1C00004, 0x2F5B, 0x4E6A, 0x9C1D, 0x7A3E5F8B0C21)

/**
 * @def           BT_UUID_BULK_XFER_SVC
 * @brief         bt_uuid form of BT_UUID_BULK_XFER_SVC_VAL.
 */
#define BT_UUID_BULK_XFER_SVC      BT_UUID_DECLARE_128(BT_UUID_BULK_XFER_SVC_VAL)

/**
 * @def           BT_UUID_BULK_XFER_RX
 * @brief         bt_uuid form of BT_UUID_BULK_XFER_RX_VAL.
 */
#define BT_UUID_BULK_XFER_RX       BT_UUID_DECLARE_128(BT_UUID_BULK_XFER_RX_VAL)

/**
 * @def           BT_UUID_BULK_XFER_TX
 * @brief         bt_uuid form of BT_UUID_BULK_XFER_TX_VAL.
 */
#define BT_UUID_BULK_XFER_TX       BT_UUID_DECLARE_128(BT_UUID_BULK_XFER_TX_VAL)

/**
 * @def           BT_UUID_BULK_XFER_CAPS
 * @brief         bt_uuid form of BT_UUID_BULK_XFER_CAPS_VAL.
 */
#define BT_UUID_BULK_XFER_CAPS     BT_UUID_DECLARE_128(BT_UUID_BULK_XFER_CAPS_VAL)

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
extern const struct bt_gatt_attr *gstpt_BulkSvc_Init(void);

#endif /* !_BULK_XFER_SERVICE_H */
