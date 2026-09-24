/**
 * @file          BulkXfer_Core_Priv.h
 * @brief         Internal interface between the BulkXfer engine core and the
 *                Server / Client roles. Not for applications.
 *
 *                The core owns the engine thread, its wake semaphore, the lock
 *                and the event bits. Each role owns its session, connection,
 *                credits, timers and incoming-frame queue, and plugs into the
 *                engine pass through gv_BLKx_EnginePre() / gv_BLKx_EnginePost().
 *
 * @date          24/09/2026
 * @author        Shivam Chudasama
 * @copyright     Shivam Chudasama
 * @license       MIT
 */

/* SPDX-License-Identifier: MIT */

#ifndef _BULK_XFER_CORE_PRIV_H
#define _BULK_XFER_CORE_PRIV_H

/******************************************************************************/
/*                                                                            */
/*                                  INCLUDES                                  */
/*                                                                            */
/******************************************************************************/
#include <zephyr/kernel.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/sys/atomic.h>
#include "BulkXfer_Types.h"

/******************************************************************************/
/*                                                                            */
/*                                  DEFINES                                   */
/*                                                                            */
/******************************************************************************/
/**
 * @def           BLK_SEQ_HALF_RANGE
 * @brief         Half of the 8-bit sequence space: separates "ahead" from
 *                "behind".
 */
#define BLK_SEQ_HALF_RANGE                   (128U)

/******************************************************************************/
/*                                                                            */
/*                                   ENUMS                                    */
/*                                                                            */
/******************************************************************************/
/**
 * @enum          BlkEvent_E
 * @brief         Bit positions in gt_BLK_events.
 */
typedef enum
{
   eBE_SRV_ACK_DUE = 0,                      /**< Server delayed-ACK timer expired.       */
   eBE_SRV_IDLE,                             /**< Server inactivity timer expired.        */
   eBE_SRV_OVERFLOW,                         /**< DATA hook dropped a frame (no block).   */
   eBE_SRV_ABORT_REQ,                        /**< Application called gv_BLKS_AbortRx().   */
   eBE_CLI_TIMEOUT,                          /**< Client ACK timer expired.               */
   eBE_CLI_RETRY,                            /**< Host was out of buffers, retry pumping. */
   eBE_CLI_ABORT_REQ,                        /**< Application called gv_BLKC_AbortTx().   */
   eBE_CLI_ATTACH_DONE,                      /**< Discovery / subscription finished.      */
} BlkEvent_E;

/******************************************************************************/
/*                                                                            */
/*                                 STRUCTURES                                 */
/*                                                                            */
/******************************************************************************/
/**
 * @struct        BlkFrameBlock_T
 * @brief         One received frame queued from BLE RX context to the engine.
 */
typedef struct
{
   void *vpt_fifoReserved;                   /**< Required by k_fifo (first word).        */
   atomic_val_t t_connGen;                   /**< Role's connection generation.           */
   uint16_t u16_len;                         /**< Valid bytes in u8ar_data.               */
   uint8_t u8ar_data[BLK_MAX_FRAME_LEN];     /**< Raw frame.                              */
} BlkFrameBlock_T;

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
extern struct k_mutex gst_BLK_lock;
extern atomic_t gt_BLK_events;

/******************************************************************************/
/*                                                                            */
/*                              EXTERN FUNCTIONS                              */
/*                                                                            */
/******************************************************************************/
/* ---- Core ---------------------------------------------------------------- */
extern void gv_BLK_EngineStart(void);
extern void gv_BLK_Kick(void);
extern void gv_BLK_TuneLink(struct bt_conn *stpt_conn);
extern bool gb_BLK_FrameLenValid(const uint8_t *u8pt_buf, uint16_t u16_len);
extern uint16_t gu16_BLK_FrameCapacity(struct bt_conn *stpt_conn);
extern uint32_t gu32_BLK_FrameCount(uint32_t u32_totalLen, uint8_t u8_chunkSize);
extern uint32_t gu32_BLK_SeqToAbs(uint8_t u8_seq, uint32_t u32_base);

/* ---- Role hooks, called by the engine with gst_BLK_lock held ------------- */
#if BLK_ENABLE_SERVER
extern void gv_BLKS_EnginePre(void);
extern void gv_BLKS_EnginePost(void);
#endif // BLK_ENABLE_SERVER
#if BLK_ENABLE_CLIENT
extern void gv_BLKC_EnginePre(void);
extern void gv_BLKC_EnginePost(void);
#endif // BLK_ENABLE_CLIENT

#endif // _BULK_XFER_CORE_PRIV_H
