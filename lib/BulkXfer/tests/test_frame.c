/**
 * @file          test_frame.c
 * @brief         Host unit tests for BulkXfer_Frame.c (no Zephyr needed).
 *
 * @code
 *                gcc -std=c99 -Wall -Wextra -Werror -I.. -o test_frame \
 *                    test_frame.c ../BulkXfer_Frame.c && ./test_frame
 * @endcode
 *
 * @date          22/09/2026
 * @author        Shivam Chudasama
 * @copyright     Shivam Chudasama
 * @license       MIT
 */

/* SPDX-License-Identifier: MIT */

#include <stdio.h>
#include <string.h>
#include "BulkXfer_Frame.h"

static int si_failures = 0;

#define CHECK(cond)                                                            \
   do                                                                          \
   {                                                                           \
      if (!(cond))                                                             \
      {                                                                        \
         printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                \
         si_failures++;                                                        \
      }                                                                        \
   } while (0)

static void sv_TestShortRoundTrip(void)
{
   uint8_t u8ar_buf[BLK_MAX_FRAME_LEN];
   uint8_t u8ar_data[BLK_MAX_SHORT_PAYLOAD];
   BlkFrame_T st_f;
   uint16_t u16_len = 0U;

   (void)memset(u8ar_data, 0xA5, sizeof(u8ar_data));

   // Maximum payload (242) fills exactly one 244-byte frame
   u16_len = gu16_BLK_EncodeShort(u8ar_buf, sizeof(u8ar_buf), 0x12U, u8ar_data,
      sizeof(u8ar_data));
   CHECK(u16_len == BLK_MAX_FRAME_LEN);
   CHECK(u8ar_buf[0] == BLK_MAX_SHORT_PAYLOAD);
   CHECK(gi_BLK_FrameParse(u8ar_buf, u16_len, &st_f) == 0);
   CHECK(st_f.u8_type == 0x12U);
   CHECK(st_f.u8_payloadLen == BLK_MAX_SHORT_PAYLOAD);
   CHECK(memcmp(st_f.u8pt_payload, u8ar_data, sizeof(u8ar_data)) == 0);

   // Empty payload
   u16_len = gu16_BLK_EncodeShort(u8ar_buf, sizeof(u8ar_buf), 0x00U, NULL, 0U);
   CHECK(u16_len == 2U);
   CHECK(gi_BLK_FrameParse(u8ar_buf, u16_len, &st_f) == 0);
   CHECK(st_f.u8_payloadLen == 0U);

   // Reserved type and too-small buffer are refused
   CHECK(gu16_BLK_EncodeShort(u8ar_buf, sizeof(u8ar_buf), eBFT_START, u8ar_data, 1U) == 0U);
   CHECK(gu16_BLK_EncodeShort(u8ar_buf, 10U, 0x01U, u8ar_data, 9U) == 0U);
}

static void sv_TestStartRoundTrip(void)
{
   uint8_t u8ar_buf[BLK_CTRL_FRAME_MAX_LEN];
   BlkFrame_T st_f;
   uint16_t u16_len = gu16_BLK_EncodeStart(u8ar_buf, sizeof(u8ar_buf), 7U, 0x33U,
      0x12345678UL, 240U, 16U, 0xCAFEBABEUL);

   CHECK(u16_len == 14U);
   // Little-endian check of totalLen on the wire
   CHECK(u8ar_buf[4] == 0x78U && u8ar_buf[7] == 0x12U);
   CHECK(gi_BLK_FrameParse(u8ar_buf, u16_len, &st_f) == 0);
   CHECK(st_f.u8_type == eBFT_START);
   CHECK(st_f.u_body.st_start.u8_xferId == 7U);
   CHECK(st_f.u_body.st_start.u8_appType == 0x33U);
   CHECK(st_f.u_body.st_start.u32_totalLen == 0x12345678UL);
   CHECK(st_f.u_body.st_start.u8_chunkSize == 240U);
   CHECK(st_f.u_body.st_start.u8_window == 16U);
   CHECK(st_f.u_body.st_start.u32_crc32 == 0xCAFEBABEUL);
}

static void sv_TestDataFrame(void)
{
   uint8_t u8ar_buf[BLK_MAX_FRAME_LEN];
   BlkFrame_T st_f;
   uint16_t u16_len = 0U;
   uint16_t u16_idx = 0U;

   // Full 240-byte chunk at MTU 247
   u16_len = gu16_BLK_EncodeDataHeader(u8ar_buf, sizeof(u8ar_buf), 3U, 255U,
      BLK_MAX_CHUNK_LEN);
   CHECK(u16_len == BLK_MAX_FRAME_LEN);
   for (u16_idx = 0U; u16_idx < BLK_MAX_CHUNK_LEN; u16_idx++)
   {
      u8ar_buf[BLK_DATA_HDR_LEN + u16_idx] = (uint8_t)u16_idx;
   }
   CHECK(gi_BLK_FrameParse(u8ar_buf, u16_len, &st_f) == 0);
   CHECK(st_f.u8_type == eBFT_DATA);
   CHECK(st_f.u_body.st_data.u8_xferId == 3U);
   CHECK(st_f.u_body.st_data.u8_seq == 255U);
   CHECK(st_f.u_body.st_data.u8_dataLen == BLK_MAX_CHUNK_LEN);
   CHECK(st_f.u_body.st_data.u8pt_data[239] == 239U);

   // Zero-length data and overflowing buffers are refused
   CHECK(gu16_BLK_EncodeDataHeader(u8ar_buf, sizeof(u8ar_buf), 1U, 0U, 0U) == 0U);
   CHECK(gu16_BLK_EncodeDataHeader(u8ar_buf, 20U, 1U, 0U, 17U) == 0U);
   CHECK(gu16_BLK_EncodeDataHeader(u8ar_buf, 20U, 1U, 0U, 16U) == 20U);

   // A DATA frame without any data byte is malformed
   u8ar_buf[0] = 2U;
   u8ar_buf[1] = eBFT_DATA;
   CHECK(gi_BLK_FrameParse(u8ar_buf, 4U, &st_f) == -EINVAL);
}

static void sv_TestControlFrames(void)
{
   uint8_t u8ar_buf[BLK_CTRL_FRAME_MAX_LEN];
   BlkFrame_T st_f;
   uint16_t u16_len = 0U;

   u16_len = gu16_BLK_EncodeAck(u8ar_buf, sizeof(u8ar_buf), 9U, 200U, 16U);
   CHECK(u16_len == 5U);
   CHECK(gi_BLK_FrameParse(u8ar_buf, u16_len, &st_f) == 0);
   CHECK(st_f.u8_type == eBFT_ACK && st_f.u_body.st_ack.u8_seq == 200U
      && st_f.u_body.st_ack.u8_window == 16U && st_f.u_body.st_ack.u8_xferId == 9U);

   u16_len = gu16_BLK_EncodeNack(u8ar_buf, sizeof(u8ar_buf), 9U, 4U, eBS_NO_RESOURCES);
   CHECK(gi_BLK_FrameParse(u8ar_buf, u16_len, &st_f) == 0);
   CHECK(st_f.u8_type == eBFT_NACK && st_f.u_body.st_nack.u8_seq == 4U
      && st_f.u_body.st_nack.u8_reason == eBS_NO_RESOURCES);

   u16_len = gu16_BLK_EncodeEnd(u8ar_buf, sizeof(u8ar_buf), 9U, eBS_CRC_ERROR);
   CHECK(u16_len == 4U);
   CHECK(gi_BLK_FrameParse(u8ar_buf, u16_len, &st_f) == 0);
   CHECK(st_f.u8_type == eBFT_END && st_f.u_body.st_end.u8_status == eBS_CRC_ERROR);

   u16_len = gu16_BLK_EncodeAbort(u8ar_buf, sizeof(u8ar_buf), 9U, eBS_REJECTED,
      eBAD_BY_RECEIVER);
   CHECK(gi_BLK_FrameParse(u8ar_buf, u16_len, &st_f) == 0);
   CHECK(st_f.u8_type == eBFT_ABORT && st_f.u_body.st_abort.u8_reason == eBS_REJECTED
      && st_f.u_body.st_abort.u8_dir == eBAD_BY_RECEIVER);

   // Buffer one byte too small
   CHECK(gu16_BLK_EncodeAck(u8ar_buf, 4U, 1U, 1U, 1U) == 0U);
}

static void sv_TestMalformed(void)
{
   uint8_t u8ar_buf[8] = { 0 };
   BlkFrame_T st_f;

   // Shorter than the header
   CHECK(gi_BLK_FrameParse(u8ar_buf, 1U, &st_f) == -EINVAL);
   CHECK(gi_BLK_FrameParse(NULL, 4U, &st_f) == -EINVAL);

   // len byte disagrees with the ATT length (both directions)
   u8ar_buf[0] = 3U;
   u8ar_buf[1] = 0x01U;
   CHECK(gi_BLK_FrameParse(u8ar_buf, 4U, &st_f) == -EINVAL);
   CHECK(gi_BLK_FrameParse(u8ar_buf, 6U, &st_f) == -EINVAL);
   CHECK(gi_BLK_FrameParse(u8ar_buf, 5U, &st_f) == 0);

   // Control frame with wrong payload size
   u8ar_buf[0] = 2U;
   u8ar_buf[1] = eBFT_ACK;
   CHECK(gi_BLK_FrameParse(u8ar_buf, 4U, &st_f) == -EINVAL);

   // Unknown reserved type
   u8ar_buf[0] = 0U;
   u8ar_buf[1] = 0xFFU;
   CHECK(gi_BLK_FrameParse(u8ar_buf, 2U, &st_f) == -ENOTSUP);
}

static void sv_TestSeqWrap(void)
{
   // The engine expands 8-bit seqs with base + (uint8_t)(seq - (uint8_t)base).
   // Verify the arithmetic across the 255 -> 0 wrap for a 128-frame window.
   uint32_t u32_base = 250U;
   uint8_t u8_seq = (uint8_t)(u32_base + 127U);
   uint32_t u32_abs = u32_base + (uint8_t)(u8_seq - (uint8_t)u32_base);

   CHECK(u32_abs == 377U);

   u32_base = 0x1FFFFU;
   u8_seq = 0x03U;
   u32_abs = u32_base + (uint8_t)(u8_seq - (uint8_t)u32_base);
   CHECK(u32_abs == 0x20003U);
}

int main(void)
{
   sv_TestShortRoundTrip();
   sv_TestStartRoundTrip();
   sv_TestDataFrame();
   sv_TestControlFrames();
   sv_TestMalformed();
   sv_TestSeqWrap();

   // Report the overall result
   if (si_failures == 0)
   {
      printf("All BulkXfer frame tests passed\n");
      return 0;
   }

   printf("%d check(s) failed\n", si_failures);
   return 1;
}
