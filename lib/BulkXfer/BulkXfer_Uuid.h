/**
 * @file          BulkXfer_Uuid.h
 * @brief         Default 128-bit UUIDs of the BulkXfer GATT service.
 *
 *                The Server hosts the service; the Client discovers it with
 *                these UUIDs unless BlkCliCfg_T overrides them.
 *
 *                  Service  B1C00001-2F5B-4E6A-9C1D-7A3E5F8B0C21
 *                  DATA     B1C00002-...  Write Without Response: client -> server
 *                  CTRL     B1C00003-...  Notify: server -> client
 *                  Caps     B1C00004-...  Read: BlkCaps_T
 *
 * @date          24/09/2026
 * @author        Shivam Chudasama
 * @copyright     Shivam Chudasama
 * @license       MIT
 */

/* SPDX-License-Identifier: MIT */

#ifndef _BULK_XFER_UUID_H
#define _BULK_XFER_UUID_H

/******************************************************************************/
/*                                                                            */
/*                                  INCLUDES                                  */
/*                                                                            */
/******************************************************************************/
#include <zephyr/bluetooth/uuid.h>

/******************************************************************************/
/*                                                                            */
/*                                  DEFINES                                   */
/*                                                                            */
/******************************************************************************/
/**
 * @def           BT_UUID_BLK_SVC_VAL
 * @brief         Encoded UUID of the BulkXfer service.
 */
#define BT_UUID_BLK_SVC_VAL \
   BT_UUID_128_ENCODE(0xB1C00001, 0x2F5B, 0x4E6A, 0x9C1D, 0x7A3E5F8B0C21)

/**
 * @def           BT_UUID_BLK_DATA_VAL
 * @brief         Encoded UUID of the DATA characteristic (client -> server).
 */
#define BT_UUID_BLK_DATA_VAL \
   BT_UUID_128_ENCODE(0xB1C00002, 0x2F5B, 0x4E6A, 0x9C1D, 0x7A3E5F8B0C21)

/**
 * @def           BT_UUID_BLK_CTRL_VAL
 * @brief         Encoded UUID of the CTRL characteristic (server -> client).
 */
#define BT_UUID_BLK_CTRL_VAL \
   BT_UUID_128_ENCODE(0xB1C00003, 0x2F5B, 0x4E6A, 0x9C1D, 0x7A3E5F8B0C21)

/**
 * @def           BT_UUID_BLK_CAPS_VAL
 * @brief         Encoded UUID of the Caps characteristic.
 */
#define BT_UUID_BLK_CAPS_VAL \
   BT_UUID_128_ENCODE(0xB1C00004, 0x2F5B, 0x4E6A, 0x9C1D, 0x7A3E5F8B0C21)

/**
 * @def           BT_UUID_BLK_SVC
 * @brief         bt_uuid form of BT_UUID_BLK_SVC_VAL.
 */
#define BT_UUID_BLK_SVC                      BT_UUID_DECLARE_128(BT_UUID_BLK_SVC_VAL)

/**
 * @def           BT_UUID_BLK_DATA
 * @brief         bt_uuid form of BT_UUID_BLK_DATA_VAL.
 */
#define BT_UUID_BLK_DATA                     BT_UUID_DECLARE_128(BT_UUID_BLK_DATA_VAL)

/**
 * @def           BT_UUID_BLK_CTRL
 * @brief         bt_uuid form of BT_UUID_BLK_CTRL_VAL.
 */
#define BT_UUID_BLK_CTRL                     BT_UUID_DECLARE_128(BT_UUID_BLK_CTRL_VAL)

/**
 * @def           BT_UUID_BLK_CAPS
 * @brief         bt_uuid form of BT_UUID_BLK_CAPS_VAL.
 */
#define BT_UUID_BLK_CAPS                     BT_UUID_DECLARE_128(BT_UUID_BLK_CAPS_VAL)

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

#endif // _BULK_XFER_UUID_H
