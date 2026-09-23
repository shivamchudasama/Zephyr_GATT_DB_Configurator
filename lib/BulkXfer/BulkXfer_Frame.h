/**
 * @file          BulkXfer_Frame.h
 * @brief         Wire format of the BLE bulk transfer (BulkXfer) framework:
 *                frame layout, frame types, status codes and the pure
 *                encode / decode functions.
 *
 *                Every GATT write / notification carries exactly one frame:
 *
 * @code
 *                +--------+--------+---------------------------+
 *                | len(1) | type(1)| payload (len bytes)       |
 *                +--------+--------+---------------------------+
 * @endcode
 *
 *                - len  : payload length. len + 2 must equal the ATT length.
 *                - type : 0x00..0xEF application data type (single-frame
 *                         message, no ACK), 0xF0..0xFF framework control.
 *
 *                Multi-byte fields are little-endian.
 *
 *                This module has no Zephyr dependency so that it can be unit
 *                tested on the host.
 *
 * @date          22/09/2026
 * @author        Shivam Chudasama
 * @copyright     Shivam Chudasama
 * @license       MIT
 */

/* SPDX-License-Identifier: MIT */

#ifndef _BULK_XFER_FRAME_H
#define _BULK_XFER_FRAME_H

/******************************************************************************/
/*                                                                            */
/*                                  INCLUDES                                  */
/*                                                                            */
/******************************************************************************/
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <errno.h>
#include "BulkXfer_Config.h"

/******************************************************************************/
/*                                                                            */
/*                                  DEFINES                                   */
/*                                                                            */
/******************************************************************************/
/**
 * @def           BLK_FRAME_HDR_LEN
 * @brief         Frame header: len(1) + type(1).
 */
#define BLK_FRAME_HDR_LEN                    (2U)

/**
 * @def           BLK_DATA_HDR_LEN
 * @brief         DATA frame header: len(1) + type(1) + xferId(1) + seq(1).
 */
#define BLK_DATA_HDR_LEN                     (4U)

/**
 * @def           BLK_MIN_FRAME_LEN
 * @brief         Smallest frame that can carry a DATA frame with one data byte.
 */
#define BLK_MIN_FRAME_LEN                    (BLK_DATA_HDR_LEN + 1U)

/**
 * @def           BLK_MAX_SHORT_PAYLOAD
 * @brief         Largest application payload of a single-frame (short) message.
 */
#define BLK_MAX_SHORT_PAYLOAD                (BLK_MAX_FRAME_LEN - BLK_FRAME_HDR_LEN)

/**
 * @def           BLK_MAX_CHUNK_LEN
 * @brief         Largest data chunk carried by one DATA frame.
 */
#define BLK_MAX_CHUNK_LEN                    (BLK_MAX_FRAME_LEN - BLK_DATA_HDR_LEN)

/**
 * @def           BLK_APP_TYPE_MAX
 * @brief         Highest type value available to the application.
 */
#define BLK_APP_TYPE_MAX                     (0xEFU)

/**
 * @def           BLK_START_PAYLOAD_LEN
 * @brief         Payload size of a START frame.
 */
#define BLK_START_PAYLOAD_LEN                (12U)

/**
 * @def           BLK_ACK_PAYLOAD_LEN
 * @brief         Payload size of an ACK frame.
 */
#define BLK_ACK_PAYLOAD_LEN                  (3U)

/**
 * @def           BLK_NACK_PAYLOAD_LEN
 * @brief         Payload size of a NACK frame.
 */
#define BLK_NACK_PAYLOAD_LEN                 (3U)

/**
 * @def           BLK_END_PAYLOAD_LEN
 * @brief         Payload size of an END frame.
 */
#define BLK_END_PAYLOAD_LEN                  (2U)

/**
 * @def           BLK_ABORT_PAYLOAD_LEN
 * @brief         Payload size of an ABORT frame.
 */
#define BLK_ABORT_PAYLOAD_LEN                (3U)

/**
 * @def           BLK_CTRL_FRAME_MAX_LEN
 * @brief         Largest control frame (START). Useful for stack buffers.
 */
#define BLK_CTRL_FRAME_MAX_LEN               (BLK_FRAME_HDR_LEN + BLK_START_PAYLOAD_LEN)

/**
 * @def           ENOTSUP
 * @brief         Some host C libraries (old MinGW) lack ENOTSUP; Zephyr
 *                defines it.
 */
#ifndef ENOTSUP
#define ENOTSUP                              (134)
#endif

#if (BLK_MAX_FRAME_LEN > 257U) || (BLK_MAX_FRAME_LEN < 20U)
#error "BLK_MAX_FRAME_LEN must be in the range 20..257"
#endif

#if (BLK_WINDOW_DEFAULT < 2U) || (BLK_WINDOW_DEFAULT > 128U)
#error "BLK_WINDOW_DEFAULT must be in the range 2..128"
#endif

/******************************************************************************/
/*                                                                            */
/*                                   ENUMS                                    */
/*                                                                            */
/******************************************************************************/
/**
 * @enum          BlkFrameType_E
 * @brief         Framework-reserved frame types (0xF0..0xFF).
 *
 *                Sender -> receiver : START, DATA, ABORT(eBAD_BY_SENDER)
 *                Receiver -> sender : ACK, NACK, END, ABORT(eBAD_BY_RECEIVER)
 */
typedef enum
{
   /** xferId(1) appType(1) totalLen(4) chunkSize(1) window(1) crc32(4) */
   eBFT_START = 0xF0,
   /** xferId(1) seq(1) data(1..chunkSize) */
   eBFT_DATA  = 0xF1,
   /** xferId(1) nextExpectedSeq(1) window(1) - cumulative acknowledgement */
   eBFT_ACK   = 0xF2,
   /** xferId(1) nextExpectedSeq(1) reason(1) - go back to nextExpectedSeq */
   eBFT_NACK  = 0xF3,
   /** xferId(1) status(1) - final result, sent by the receiver */
   eBFT_END   = 0xF4,
   /** xferId(1) reason(1) direction(1) */
   eBFT_ABORT = 0xF5,
} BlkFrameType_E;

/**
 * @enum          BlkStatus_E
 * @brief         Transfer result. Also used on the wire as the END status and
 *                the ABORT / NACK reason.
 */
typedef enum
{
   eBS_OK              = 0x00, /**< Transfer complete, CRC matched.   */
   eBS_CRC_ERROR       = 0x01, /**< All data received, CRC mismatch.  */
   eBS_TIMEOUT         = 0x02, /**< Peer stopped responding.          */
   eBS_ABORTED         = 0x03, /**< Aborted by the local application. */
   eBS_REMOTE_ABORTED  = 0x04, /**< Aborted by the peer.              */
   eBS_REJECTED        = 0x05, /**< Receiver refused the transfer.    */
   eBS_DISCONNECTED    = 0x06, /**< Link lost during the transfer.    */
   eBS_SOURCE_ERROR    = 0x07, /**< Sender's data source failed.      */
   eBS_SINK_ERROR      = 0x08, /**< Receiver's data sink failed.      */
   eBS_PROTOCOL_ERROR  = 0x09, /**< Malformed / unexpected frame.     */
   eBS_NO_RESOURCES    = 0x0A, /**< RX queue overflow (NACK reason).  */
   eBS_OUT_OF_ORDER    = 0x0B, /**< Sequence gap (NACK reason).       */
} BlkStatus_E;

/**
 * @enum          BlkAbortDir_E
 * @brief         Which side of the transfer an ABORT frame refers to. Both
 *                peers may run a TX and an RX transfer at the same time, so the
 *                xferId alone is not enough to identify the session.
 */
typedef enum
{
   eBAD_BY_SENDER   = 0x00, /**< Sender cancels its outgoing transfer. */
   eBAD_BY_RECEIVER = 0x01, /**< Receiver cancels an incoming transfer.*/
} BlkAbortDir_E;

/******************************************************************************/
/*                                                                            */
/*                                 STRUCTURES                                 */
/*                                                                            */
/******************************************************************************/
/**
 * @struct        BlkFrame_T
 * @brief         Decoded view of a received frame. Pointer members reference
 *                the buffer passed to gi_BLK_FrameParse() (no copy).
 */
typedef struct
{
   uint8_t u8_type;                 /**< Frame type (app type or BlkFrameType_E). */
   uint8_t u8_payloadLen;           /**< Payload length (the len byte).           */
   const uint8_t *u8pt_payload;     /**< Payload (short app frames).              */

   union
   {
      /* Members are ordered by alignment (widest first) to avoid padding,
       * not in wire order. Access them by name only. */
      struct
      {
         uint32_t u32_totalLen;
         uint32_t u32_crc32;
         uint8_t u8_xferId;
         uint8_t u8_appType;
         uint8_t u8_chunkSize;
         uint8_t u8_window;
      } st_start;

      struct
      {
         const uint8_t *u8pt_data;
         uint8_t u8_xferId;
         uint8_t u8_seq;
         uint8_t u8_dataLen;
      } st_data;

      struct
      {
         uint8_t u8_xferId;
         uint8_t u8_seq;
         uint8_t u8_window;
      } st_ack;

      struct
      {
         uint8_t u8_xferId;
         uint8_t u8_seq;
         uint8_t u8_reason;
      } st_nack;

      struct
      {
         uint8_t u8_xferId;
         uint8_t u8_status;
      } st_end;

      struct
      {
         uint8_t u8_xferId;
         uint8_t u8_reason;
         uint8_t u8_dir;
      } st_abort;
   } u_body;                        /**< Type-specific decoded fields.            */
} BlkFrame_T;

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
/* ---- Decoding ------------------------------------------------------------ */
extern int gi_BLK_FrameParse(const uint8_t *u8pt_buf, uint16_t u16_len,
   BlkFrame_T *stpt_frame);

/* ---- Encoding (return total frame length, or 0 if the buffer is too small) */
extern uint16_t gu16_BLK_EncodeShort(uint8_t *u8pt_buf, uint16_t u16_bufLen,
   uint8_t u8_appType, const uint8_t *u8pt_data, uint16_t u16_dataLen);
extern uint16_t gu16_BLK_EncodeStart(uint8_t *u8pt_buf, uint16_t u16_bufLen,
   uint8_t u8_xferId, uint8_t u8_appType, uint32_t u32_totalLen,
   uint8_t u8_chunkSize, uint8_t u8_window, uint32_t u32_crc32);
extern uint16_t gu16_BLK_EncodeDataHeader(uint8_t *u8pt_buf, uint16_t u16_bufLen,
   uint8_t u8_xferId, uint8_t u8_seq, uint16_t u16_dataLen);
extern uint16_t gu16_BLK_EncodeAck(uint8_t *u8pt_buf, uint16_t u16_bufLen,
   uint8_t u8_xferId, uint8_t u8_seq, uint8_t u8_window);
extern uint16_t gu16_BLK_EncodeNack(uint8_t *u8pt_buf, uint16_t u16_bufLen,
   uint8_t u8_xferId, uint8_t u8_seq, uint8_t u8_reason);
extern uint16_t gu16_BLK_EncodeEnd(uint8_t *u8pt_buf, uint16_t u16_bufLen,
   uint8_t u8_xferId, uint8_t u8_status);
extern uint16_t gu16_BLK_EncodeAbort(uint8_t *u8pt_buf, uint16_t u16_bufLen,
   uint8_t u8_xferId, uint8_t u8_reason, uint8_t u8_dir);

#endif /* !_BULK_XFER_FRAME_H */
