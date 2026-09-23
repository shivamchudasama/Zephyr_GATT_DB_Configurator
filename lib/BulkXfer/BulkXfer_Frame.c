/**
 * @file          BulkXfer_Frame.c
 * @brief         Encode / decode functions for the BulkXfer wire format.
 *                Pure C, no Zephyr dependency.
 * @date          22/09/2026
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
#include <string.h>
#include "BulkXfer_Frame.h"

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
static void sv_PutLe32(uint8_t *u8pt_dst, uint32_t u32_val);
static uint32_t su32_GetLe32(const uint8_t *u8pt_src);
static uint16_t su16_EncodeCtrl(uint8_t *u8pt_buf, uint16_t u16_bufLen,
   uint8_t u8_type, uint8_t u8_b0, uint8_t u8_b1, uint8_t u8_b2,
   uint8_t u8_payloadLen);

/******************************************************************************/
/*                                                                            */
/*                              EXTERN VARIABLES                              */
/*                                                                            */
/******************************************************************************/

/******************************************************************************/
/*                                                                            */
/*                              PUBLIC VARIABLES                              */
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
/**
 * @private       sv_PutLe32
 * @brief         Store a 32-bit value in little-endian byte order.
 * @param[out]    u8pt_dst Destination (4 bytes).
 * @param[in]     u32_val Value to store.
 * @return        void
 */
static void sv_PutLe32(uint8_t *u8pt_dst, uint32_t u32_val)
{
   // Byte-wise: endian-independent, and frame fields are unaligned
   u8pt_dst[0] = (uint8_t)(u32_val);
   u8pt_dst[1] = (uint8_t)(u32_val >> 8);
   u8pt_dst[2] = (uint8_t)(u32_val >> 16);
   u8pt_dst[3] = (uint8_t)(u32_val >> 24);
}

/**
 * @private       su32_GetLe32
 * @brief         Load a little-endian 32-bit value.
 * @param[in]     u8pt_src Source (4 bytes).
 * @return        Decoded value.
 */
static uint32_t su32_GetLe32(const uint8_t *u8pt_src)
{
   return (uint32_t)u8pt_src[0]
      | ((uint32_t)u8pt_src[1] << 8)
      | ((uint32_t)u8pt_src[2] << 16)
      | ((uint32_t)u8pt_src[3] << 24);
}

/**
 * @private       su16_EncodeCtrl
 * @brief         Build a control frame with a payload of up to three bytes.
 * @param[out]    u8pt_buf Destination buffer.
 * @param[in]     u16_bufLen Size of u8pt_buf.
 * @param[in]     u8_type Frame type.
 * @param[in]     u8_b0 First payload byte.
 * @param[in]     u8_b1 Second payload byte.
 * @param[in]     u8_b2 Third payload byte (ignored if u8_payloadLen < 3).
 * @param[in]     u8_payloadLen Payload length (2 or 3).
 * @return        Frame length, or 0 if the buffer is too small.
 */
static uint16_t su16_EncodeCtrl(uint8_t *u8pt_buf, uint16_t u16_bufLen,
   uint8_t u8_type, uint8_t u8_b0, uint8_t u8_b1, uint8_t u8_b2,
   uint8_t u8_payloadLen)
{
   uint16_t u16_frameLen = (uint16_t)(BLK_FRAME_HDR_LEN + u8_payloadLen);

   // Check if the destination buffer can hold the frame
   if ((u8pt_buf == NULL) || (u16_bufLen < u16_frameLen))
   {
      return 0U;
   }

   u8pt_buf[0] = u8_payloadLen;
   u8pt_buf[1] = u8_type;
   u8pt_buf[2] = u8_b0;
   u8pt_buf[3] = u8_b1;

   // Check if the frame carries a third payload byte
   if (u8_payloadLen > 2U)
   {
      u8pt_buf[4] = u8_b2;
   }

   return u16_frameLen;
}

/******************************************************************************/
/*                                                                            */
/*                        PUBLIC FUNCTION DEFINITIONS                         */
/*                                                                            */
/******************************************************************************/
/**
 * @public        gi_BLK_FrameParse
 * @brief         Validate a received frame and decode it into a BlkFrame_T.
 *
 *                Checks performed:
 *                  1. The ATT length is at least the 2-byte header.
 *                  2. len byte + 2 equals the ATT length exactly.
 *                  3. For framework types, the payload has the exact size of
 *                     that frame type (DATA: at least xferId + seq + 1 byte).
 *
 * @param[in]     u8pt_buf Received bytes (one ATT value).
 * @param[in]     u16_len Number of bytes in u8pt_buf.
 * @param[out]    stpt_frame Decoded frame. Pointer members reference u8pt_buf.
 * @return        0 on success, -EINVAL for a malformed frame, -ENOTSUP for an
 *                unknown framework type (0xF6..0xFF).
 */
int gi_BLK_FrameParse(const uint8_t *u8pt_buf, uint16_t u16_len,
   BlkFrame_T *stpt_frame)
{
   const uint8_t *u8pt_pl = NULL;
   uint8_t u8_plLen = 0U;

   // Check if the arguments are usable and the header is present
   if ((u8pt_buf == NULL) || (stpt_frame == NULL) || (u16_len < BLK_FRAME_HDR_LEN))
   {
      return -EINVAL;
   }

   u8_plLen = u8pt_buf[0];

   // Check if the length byte matches the ATT length
   if (((uint16_t)u8_plLen + BLK_FRAME_HDR_LEN) != u16_len)
   {
      return -EINVAL;
   }

   // No copy: the pointers below reference u8pt_buf
   (void)memset(stpt_frame, 0, sizeof(*stpt_frame));
   u8pt_pl = &u8pt_buf[BLK_FRAME_HDR_LEN];
   stpt_frame->u8_type = u8pt_buf[1];
   stpt_frame->u8_payloadLen = u8_plLen;
   stpt_frame->u8pt_payload = u8pt_pl;

   // Check if this is an application frame (no further decoding needed)
   if (stpt_frame->u8_type <= BLK_APP_TYPE_MAX)
   {
      return 0;
   }

   // Payload offsets must stay in sync with the gu16_BLK_Encode*() functions
   switch (stpt_frame->u8_type)
   {
      case eBFT_START:
         // Check if the START payload has the expected size
         if (u8_plLen != BLK_START_PAYLOAD_LEN)
         {
            return -EINVAL;
         }
         stpt_frame->u_body.st_start.u8_xferId = u8pt_pl[0];
         stpt_frame->u_body.st_start.u8_appType = u8pt_pl[1];
         stpt_frame->u_body.st_start.u32_totalLen = su32_GetLe32(&u8pt_pl[2]);
         stpt_frame->u_body.st_start.u8_chunkSize = u8pt_pl[6];
         stpt_frame->u_body.st_start.u8_window = u8pt_pl[7];
         stpt_frame->u_body.st_start.u32_crc32 = su32_GetLe32(&u8pt_pl[8]);
         break;

      case eBFT_DATA:
         // Check if the DATA frame carries at least one data byte
         if (u8_plLen < 3U)
         {
            return -EINVAL;
         }
         stpt_frame->u_body.st_data.u8_xferId = u8pt_pl[0];
         stpt_frame->u_body.st_data.u8_seq = u8pt_pl[1];
         stpt_frame->u_body.st_data.u8pt_data = &u8pt_pl[2];
         stpt_frame->u_body.st_data.u8_dataLen = (uint8_t)(u8_plLen - 2U);
         break;

      case eBFT_ACK:
         // Check if the ACK payload has the expected size
         if (u8_plLen != BLK_ACK_PAYLOAD_LEN)
         {
            return -EINVAL;
         }
         stpt_frame->u_body.st_ack.u8_xferId = u8pt_pl[0];
         stpt_frame->u_body.st_ack.u8_seq = u8pt_pl[1];
         stpt_frame->u_body.st_ack.u8_window = u8pt_pl[2];
         break;

      case eBFT_NACK:
         // Check if the NACK payload has the expected size
         if (u8_plLen != BLK_NACK_PAYLOAD_LEN)
         {
            return -EINVAL;
         }
         stpt_frame->u_body.st_nack.u8_xferId = u8pt_pl[0];
         stpt_frame->u_body.st_nack.u8_seq = u8pt_pl[1];
         stpt_frame->u_body.st_nack.u8_reason = u8pt_pl[2];
         break;

      case eBFT_END:
         // Check if the END payload has the expected size
         if (u8_plLen != BLK_END_PAYLOAD_LEN)
         {
            return -EINVAL;
         }
         stpt_frame->u_body.st_end.u8_xferId = u8pt_pl[0];
         stpt_frame->u_body.st_end.u8_status = u8pt_pl[1];
         break;

      case eBFT_ABORT:
         // Check if the ABORT payload has the expected size
         if (u8_plLen != BLK_ABORT_PAYLOAD_LEN)
         {
            return -EINVAL;
         }
         stpt_frame->u_body.st_abort.u8_xferId = u8pt_pl[0];
         stpt_frame->u_body.st_abort.u8_reason = u8pt_pl[1];
         stpt_frame->u_body.st_abort.u8_dir = u8pt_pl[2];
         break;

      default:
         return -ENOTSUP;
   }

   return 0;
}

/**
 * @public        gu16_BLK_EncodeShort
 * @brief         Build a single-frame application message [len][appType][data].
 * @param[out]    u8pt_buf Destination buffer.
 * @param[in]     u16_bufLen Size of u8pt_buf.
 * @param[in]     u8_appType Application type (0x00..BLK_APP_TYPE_MAX).
 * @param[in]     u8pt_data Payload (may be NULL if u16_dataLen is 0).
 * @param[in]     u16_dataLen Payload length (<= 255).
 * @return        Frame length, or 0 on invalid arguments / small buffer.
 */
uint16_t gu16_BLK_EncodeShort(uint8_t *u8pt_buf, uint16_t u16_bufLen,
   uint8_t u8_appType, const uint8_t *u8pt_data, uint16_t u16_dataLen)
{
   uint16_t u16_frameLen = (uint16_t)(BLK_FRAME_HDR_LEN + u16_dataLen);

   // Check if the type is an application type and the payload fits
   if ((u8pt_buf == NULL) || (u8_appType > BLK_APP_TYPE_MAX) || (u16_dataLen > 255U)
      || (u16_bufLen < u16_frameLen) || ((u8pt_data == NULL) && (u16_dataLen != 0U)))
   {
      return 0U;
   }

   u8pt_buf[0] = (uint8_t)u16_dataLen;
   u8pt_buf[1] = u8_appType;

   // Check if there is any payload to copy
   if (u16_dataLen != 0U)
   {
      (void)memcpy(&u8pt_buf[BLK_FRAME_HDR_LEN], u8pt_data, u16_dataLen);
   }

   return u16_frameLen;
}

/**
 * @public        gu16_BLK_EncodeStart
 * @brief         Build a START frame announcing a multi-frame transfer.
 * @param[out]    u8pt_buf Destination buffer.
 * @param[in]     u16_bufLen Size of u8pt_buf.
 * @param[in]     u8_xferId Transfer identifier chosen by the sender.
 * @param[in]     u8_appType Application type of the transferred object.
 * @param[in]     u32_totalLen Total number of data bytes.
 * @param[in]     u8_chunkSize Data bytes per DATA frame (last may be shorter).
 * @param[in]     u8_window Sender's proposed window.
 * @param[in]     u32_crc32 CRC-32 (IEEE) over all data bytes.
 * @return        Frame length, or 0 if the buffer is too small.
 */
uint16_t gu16_BLK_EncodeStart(uint8_t *u8pt_buf, uint16_t u16_bufLen,
   uint8_t u8_xferId, uint8_t u8_appType, uint32_t u32_totalLen,
   uint8_t u8_chunkSize, uint8_t u8_window, uint32_t u32_crc32)
{
   const uint16_t u16_frameLen = BLK_FRAME_HDR_LEN + BLK_START_PAYLOAD_LEN;

   // Check if the destination buffer can hold the frame
   if ((u8pt_buf == NULL) || (u16_bufLen < u16_frameLen))
   {
      return 0U;
   }

   u8pt_buf[0] = BLK_START_PAYLOAD_LEN;
   u8pt_buf[1] = eBFT_START;
   u8pt_buf[2] = u8_xferId;
   u8pt_buf[3] = u8_appType;
   sv_PutLe32(&u8pt_buf[4], u32_totalLen);
   u8pt_buf[8] = u8_chunkSize;
   u8pt_buf[9] = u8_window;
   sv_PutLe32(&u8pt_buf[10], u32_crc32);

   return u16_frameLen;
}

/**
 * @public        gu16_BLK_EncodeDataHeader
 * @brief         Write the 4-byte DATA frame header. The caller places the
 *                data bytes directly at u8pt_buf + BLK_DATA_HDR_LEN, so the
 *                source can be read in place without an extra copy.
 * @param[out]    u8pt_buf Destination buffer.
 * @param[in]     u16_bufLen Size of u8pt_buf (must fit header + data).
 * @param[in]     u8_xferId Transfer identifier.
 * @param[in]     u8_seq Sequence number (absolute frame index mod 256).
 * @param[in]     u16_dataLen Number of data bytes that will follow (1..253).
 * @return        Total frame length (header + data), or 0 on error.
 */
uint16_t gu16_BLK_EncodeDataHeader(uint8_t *u8pt_buf, uint16_t u16_bufLen,
   uint8_t u8_xferId, uint8_t u8_seq, uint16_t u16_dataLen)
{
   uint16_t u16_frameLen = (uint16_t)(BLK_DATA_HDR_LEN + u16_dataLen);

   // Check if the data length is encodable and the frame fits the buffer.
   // The len byte covers xferId + seq + data, hence 253.
   if ((u8pt_buf == NULL) || (u16_dataLen == 0U) || (u16_dataLen > 253U)
      || (u16_bufLen < u16_frameLen))
   {
      return 0U;
   }

   u8pt_buf[0] = (uint8_t)(u16_dataLen + 2U);
   u8pt_buf[1] = eBFT_DATA;
   u8pt_buf[2] = u8_xferId;
   u8pt_buf[3] = u8_seq;

   return u16_frameLen;
}

/**
 * @public        gu16_BLK_EncodeAck
 * @brief         Build a cumulative ACK: every frame before u8_seq was received.
 * @param[out]    u8pt_buf Destination buffer.
 * @param[in]     u16_bufLen Size of u8pt_buf.
 * @param[in]     u8_xferId Transfer identifier.
 * @param[in]     u8_seq Next expected sequence number.
 * @param[in]     u8_window Receiver's window.
 * @return        Frame length, or 0 if the buffer is too small.
 */
uint16_t gu16_BLK_EncodeAck(uint8_t *u8pt_buf, uint16_t u16_bufLen,
   uint8_t u8_xferId, uint8_t u8_seq, uint8_t u8_window)
{
   return su16_EncodeCtrl(u8pt_buf, u16_bufLen, eBFT_ACK, u8_xferId, u8_seq,
      u8_window, BLK_ACK_PAYLOAD_LEN);
}

/**
 * @public        gu16_BLK_EncodeNack
 * @brief         Build a NACK: resend starting from u8_seq (Go-Back-N).
 * @param[out]    u8pt_buf Destination buffer.
 * @param[in]     u16_bufLen Size of u8pt_buf.
 * @param[in]     u8_xferId Transfer identifier.
 * @param[in]     u8_seq Next expected sequence number.
 * @param[in]     u8_reason BlkStatus_E reason code.
 * @return        Frame length, or 0 if the buffer is too small.
 */
uint16_t gu16_BLK_EncodeNack(uint8_t *u8pt_buf, uint16_t u16_bufLen,
   uint8_t u8_xferId, uint8_t u8_seq, uint8_t u8_reason)
{
   return su16_EncodeCtrl(u8pt_buf, u16_bufLen, eBFT_NACK, u8_xferId, u8_seq,
      u8_reason, BLK_NACK_PAYLOAD_LEN);
}

/**
 * @public        gu16_BLK_EncodeEnd
 * @brief         Build an END frame carrying the receiver's final status.
 * @param[out]    u8pt_buf Destination buffer.
 * @param[in]     u16_bufLen Size of u8pt_buf.
 * @param[in]     u8_xferId Transfer identifier.
 * @param[in]     u8_status BlkStatus_E result.
 * @return        Frame length, or 0 if the buffer is too small.
 */
uint16_t gu16_BLK_EncodeEnd(uint8_t *u8pt_buf, uint16_t u16_bufLen,
   uint8_t u8_xferId, uint8_t u8_status)
{
   return su16_EncodeCtrl(u8pt_buf, u16_bufLen, eBFT_END, u8_xferId, u8_status,
      0U, BLK_END_PAYLOAD_LEN);
}

/**
 * @public        gu16_BLK_EncodeAbort
 * @brief         Build an ABORT frame.
 * @param[out]    u8pt_buf Destination buffer.
 * @param[in]     u16_bufLen Size of u8pt_buf.
 * @param[in]     u8_xferId Transfer identifier.
 * @param[in]     u8_reason BlkStatus_E reason code.
 * @param[in]     u8_dir BlkAbortDir_E: which side is aborting.
 * @return        Frame length, or 0 if the buffer is too small.
 */
uint16_t gu16_BLK_EncodeAbort(uint8_t *u8pt_buf, uint16_t u16_bufLen,
   uint8_t u8_xferId, uint8_t u8_reason, uint8_t u8_dir)
{
   return su16_EncodeCtrl(u8pt_buf, u16_bufLen, eBFT_ABORT, u8_xferId, u8_reason,
      u8_dir, BLK_ABORT_PAYLOAD_LEN);
}
