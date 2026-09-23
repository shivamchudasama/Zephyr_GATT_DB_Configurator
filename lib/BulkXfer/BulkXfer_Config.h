/**
 * @file          BulkXfer_Config.h
 * @brief         Compile-time configuration of the BLE bulk transfer (BulkXfer)
 *                framework.
 *
 *                Every value is wrapped in #ifndef so it can be overridden from
 *                the application's CMakeLists.txt, e.g.:
 *
 * @code
 *                zephyr_compile_definitions(BLK_WINDOW_DEFAULT=32)
 * @endcode
 *
 *                This header is intentionally free of Zephyr includes so that
 *                BulkXfer_Frame.c can be unit-tested on any host.
 *
 * @date          22/09/2026
 * @author        Shivam Chudasama
 * @copyright     Shivam Chudasama
 * @license       MIT
 */

/* SPDX-License-Identifier: MIT */

#ifndef _BULK_XFER_CONFIG_H
#define _BULK_XFER_CONFIG_H

/******************************************************************************/
/*                                                                            */
/*                                  INCLUDES                                  */
/*                                                                            */
/******************************************************************************/

/******************************************************************************/
/*                                                                            */
/*                                  DEFINES                                   */
/*                                                                            */
/******************************************************************************/
/**
 * @def           BLK_MAX_FRAME_LEN
 * @brief         Largest frame (len + type + payload) the framework will build
 *                or accept, in bytes.
 *
 *                244 = 251 (LL payload with DLE) - 4 (L2CAP hdr) - 3 (ATT hdr),
 *                i.e. exactly one Link Layer packet at ATT_MTU 247. The frame
 *                used on a given link is min(ATT_MTU - 3, BLK_MAX_FRAME_LEN).
 *                Must not exceed 257 (1-byte length field + 2-byte header).
 *                The RX characteristic buffer must be at least this large.
 */
#ifndef BLK_MAX_FRAME_LEN
#define BLK_MAX_FRAME_LEN                    (244U)
#endif // BLK_MAX_FRAME_LEN

/**
 * @def           BLK_WINDOW_DEFAULT
 * @brief         Number of DATA frames that may be in flight without an ACK.
 *                The effective window is the minimum of both peers' values.
 *                Must be in the range 2..128 (8-bit sequence numbers).
 */
#ifndef BLK_WINDOW_DEFAULT
#define BLK_WINDOW_DEFAULT                   (16U)
#endif // BLK_WINDOW_DEFAULT

/**
 * @def           BLK_RX_POOL_DEPTH
 * @brief         Number of received frames that can be queued between the BLE
 *                RX thread (write hook) and the BulkXfer engine thread. Should
 *                be at least BLK_WINDOW_DEFAULT + a few for control frames.
 *                Each entry costs roughly BLK_MAX_FRAME_LEN + 12 bytes of RAM.
 */
#ifndef BLK_RX_POOL_DEPTH
#define BLK_RX_POOL_DEPTH                    (BLK_WINDOW_DEFAULT + 4U)
#endif // BLK_RX_POOL_DEPTH

/**
 * @def           BLK_TX_INFLIGHT_MAX
 * @brief         Maximum number of notifications handed to the Bluetooth host
 *                but not yet completed. Keep this below CONFIG_BT_ATT_TX_COUNT
 *                so that the host never blocks on ATT buffer allocation and a
 *                few buffers remain free for other ATT traffic.
 */
#ifndef BLK_TX_INFLIGHT_MAX
#define BLK_TX_INFLIGHT_MAX                  (6U)
#endif // BLK_TX_INFLIGHT_MAX

/**
 * @def           BLK_TX_ACK_TIMEOUT_MS
 * @brief         Sender: time without ACK progress after which unacknowledged
 *                frames are retransmitted (Go-Back-N) or START is resent.
 */
#ifndef BLK_TX_ACK_TIMEOUT_MS
#define BLK_TX_ACK_TIMEOUT_MS                (1000U)
#endif // BLK_TX_ACK_TIMEOUT_MS

/**
 * @def           BLK_TX_MAX_RETRIES
 * @brief         Sender: consecutive ACK timeouts before the transfer is
 *                aborted with eBS_TIMEOUT.
 */
#ifndef BLK_TX_MAX_RETRIES
#define BLK_TX_MAX_RETRIES                   (5U)
#endif // BLK_TX_MAX_RETRIES

/**
 * @def           BLK_RX_ACK_DELAY_MS
 * @brief         Receiver: maximum delay before acknowledging received frames
 *                when fewer than window/2 frames have arrived since the last
 *                ACK (e.g. the sender's source is slow).
 */
#ifndef BLK_RX_ACK_DELAY_MS
#define BLK_RX_ACK_DELAY_MS                  (20U)
#endif // BLK_RX_ACK_DELAY_MS

/**
 * @def           BLK_RX_IDLE_TIMEOUT_MS
 * @brief         Receiver: time without any frame of the active transfer
 *                after which the transfer is dropped with eBS_TIMEOUT.
 */
#ifndef BLK_RX_IDLE_TIMEOUT_MS
#define BLK_RX_IDLE_TIMEOUT_MS               (5000U)
#endif // BLK_RX_IDLE_TIMEOUT_MS

/**
 * @def           BLK_CTRL_TX_TIMEOUT_MS
 * @brief         Maximum time to wait for a free TX credit when sending a
 *                control frame (ACK / NACK / END / ABORT).
 */
#ifndef BLK_CTRL_TX_TIMEOUT_MS
#define BLK_CTRL_TX_TIMEOUT_MS               (200U)
#endif // BLK_CTRL_TX_TIMEOUT_MS

/**
 * @def           BLK_NOTIFY_RETRY_MS
 * @brief         Back-off before retrying when the host reports it is
 *                temporarily out of buffers (-ENOMEM / -EAGAIN).
 */
#ifndef BLK_NOTIFY_RETRY_MS
#define BLK_NOTIFY_RETRY_MS                  (5U)
#endif // BLK_NOTIFY_RETRY_MS

/**
 * @def           BLK_THREAD_STACK_SIZE
 * @brief         Stack size of the BulkXfer engine thread. Application
 *                callbacks (sink / source / done) run on this stack.
 */
#ifndef BLK_THREAD_STACK_SIZE
#define BLK_THREAD_STACK_SIZE                (2048U)
#endif // BLK_THREAD_STACK_SIZE

/**
 * @def           BLK_THREAD_PRIORITY
 * @brief         Priority of the BulkXfer engine thread (preemptible).
 */
#ifndef BLK_THREAD_PRIORITY
#define BLK_THREAD_PRIORITY                  (5)
#endif // BLK_THREAD_PRIORITY

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

#endif // _BULK_XFER_CONFIG_H
