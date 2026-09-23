/**
 * @file          test_engine.c
 * @brief         Host tests of the real BulkXfer engine (BulkXfer.c) against a
 *                simulated BLE link and a scripted central ("peer") that
 *                implements the other side of the protocol.
 *
 *                The engine is compiled against shim/zephyr_shim.h. A small
 *                RX pool (BLK_RX_POOL_DEPTH=6) is used so that the overflow /
 *                NACK recovery path can be provoked.
 *
 * @code
 *                gcc -std=gnu99 -Wall -Wextra -Werror -Ishim -I.. \
 *                    -Wno-missing-field-initializers \
 *                    -DBLK_RX_POOL_DEPTH=6 -o test_engine \
 *                    test_engine.c ../BulkXfer_Frame.c && ./test_engine
 * @endcode
 *
 * @date          22/09/2026
 * @author        Shivam Chudasama
 * @copyright     Shivam Chudasama
 * @license       MIT
 */

/* SPDX-License-Identifier: MIT */

#include "../BulkXfer.c"

/******************************************************************************/
/*  Test framework                                                            */
/******************************************************************************/
static int si_failures = 0;

#define CHECK(cond)                                                            \
   do                                                                          \
   {                                                                           \
      if (!(cond))                                                             \
      {                                                                        \
         printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);              \
         si_failures++;                                                        \
      }                                                                        \
   } while (0)

#define MAX_OBJ              (120U * 1024U)
#define STATUS_NONE          (-1)
#define PEER_ABORT_BASE      (100)    /* peer tx ended by device ABORT      */
#define PEER_RX_ABORT_BASE   (200)    /* peer rx ended by device ABORT      */

int64_t gi64_simNowMs = 0;
bool gb_simVerbose = false;

/******************************************************************************/
/*  Simulated link                                                            */
/******************************************************************************/
#define LINK_HOST_BUFS       (10U)    /* like CONFIG_BT_ATT_TX_COUNT         */
#define LINK_PER_EVENT       (6U)     /* packets per connection event        */

typedef struct
{
   uint8_t u8ar_data[260];
   uint16_t u16_len;
   bt_gatt_complete_func_t fpt_func;
} LinkPdu_T;

static LinkPdu_T sstar_linkQ[LINK_HOST_BUFS];
static uint32_t su32_linkHead = 0U;
static uint32_t su32_linkCount = 0U;
static struct bt_conn sst_conn = { 1 };
static struct bt_gatt_attr sst_txAttr = { NULL, NULL };
static struct bt_gatt_attr sst_rxAttr = { NULL, NULL };
static bool sb_connected = false;
static bool sb_subscribed = true;
static uint16_t su16_mtu = 247U;
static uint32_t su32_maxInFlight = 0U;
static struct k_timer *sstpt_timers[16];
static uint32_t su32_timerCount = 0U;

static void sv_PeerOnFrame(const uint8_t *u8pt_buf, uint16_t u16_len);

void gv_SimRegisterTimer(struct k_timer *t)
{
   uint32_t i;
   for (i = 0U; i < su32_timerCount; i++)
   {
      if (sstpt_timers[i] == t) { return; }
   }
   sstpt_timers[su32_timerCount++] = t;
}

static void sv_SimFireTimers(void)
{
   uint32_t i;
   for (i = 0U; i < su32_timerCount; i++)
   {
      if (sstpt_timers[i]->running && (sstpt_timers[i]->deadline <= gi64_simNowMs))
      {
         sstpt_timers[i]->running = false;
         sstpt_timers[i]->expiry(sstpt_timers[i]);
      }
   }
}

/** One connection event: deliver queued notifications to the peer. */
static void sv_LinkEvent(void)
{
   uint32_t u32_n = MIN(su32_linkCount, LINK_PER_EVENT);

   while (u32_n-- > 0U)
   {
      LinkPdu_T *stpt_pdu = &sstar_linkQ[su32_linkHead];
      su32_linkHead = (su32_linkHead + 1U) % LINK_HOST_BUFS;
      su32_linkCount--;
      sv_PeerOnFrame(stpt_pdu->u8ar_data, stpt_pdu->u16_len);
      if (stpt_pdu->fpt_func != NULL) { stpt_pdu->fpt_func(&sst_conn, NULL); }
   }
   gi64_simNowMs++;
   sv_SimFireTimers();
}

void gv_SimOnBlock(struct k_sem *stpt_sem)
{
   // A blocked credit wait means the engine waits for the link to drain
   if ((stpt_sem == &sst_BLK_txCredits) && (su32_linkCount > 0U))
   {
      sv_LinkEvent();
   }
}

int bt_gatt_notify_cb(struct bt_conn *conn, struct bt_gatt_notify_params *params)
{
   LinkPdu_T *stpt_pdu;

   if (!sb_connected || (conn != &sst_conn)) { return -ENOTCONN; }
   if (!sb_subscribed) { return -EINVAL; }
   if (params->attr != &sst_txAttr) { return -EINVAL; }
   if (params->len > (su16_mtu - 3U)) { return -EMSGSIZE; }
   if (su32_linkCount >= LINK_HOST_BUFS) { return -ENOMEM; }

   stpt_pdu = &sstar_linkQ[(su32_linkHead + su32_linkCount) % LINK_HOST_BUFS];
   (void)memcpy(stpt_pdu->u8ar_data, params->data, params->len);
   stpt_pdu->u16_len = params->len;
   stpt_pdu->fpt_func = params->func;
   su32_linkCount++;
   su32_maxInFlight = MAX(su32_maxInFlight, su32_linkCount);
   return 0;
}

bool bt_gatt_is_subscribed(struct bt_conn *conn, const struct bt_gatt_attr *attr,
   uint16_t ccc_type)
{
   (void)conn; (void)attr; (void)ccc_type;
   return sb_subscribed;
}

uint16_t bt_gatt_get_mtu(struct bt_conn *conn)
{
   (void)conn;
   return su16_mtu;
}

/******************************************************************************/
/*  Device-side application (callbacks registered with BulkXfer)              */
/******************************************************************************/
static uint8_t su8ar_devRx[MAX_OBJ];
static uint32_t su32_devRxLen = 0U;
static bool sb_devRxOrderError = false;
static int si_devRxDone = STATUS_NONE;
static uint8_t su8_devRxType = 0U;
static int si_devTxDone = STATUS_NONE;
static uint32_t su32_devRejectAbove = MAX_OBJ;
static int si_devShortCount = 0;
static uint8_t su8_devShortType = 0U;
static uint8_t su8ar_devShort[256];
static uint8_t su8_devShortLen = 0U;

static int si_devRxStartCalls = 0;

static int si_DevRxStart(uint8_t t, uint32_t n)
{
   (void)t;
   si_devRxStartCalls++;
   su32_devRxLen = 0U;
   return (n > su32_devRejectAbove) ? -ENOMEM : 0;
}

static int si_DevRxData(uint8_t t, uint32_t off, const uint8_t *d, uint16_t n)
{
   (void)t;
   // Chunks must arrive contiguous and exactly once
   if (off != su32_devRxLen) { sb_devRxOrderError = true; }
   (void)memcpy(&su8ar_devRx[off], d, n);
   su32_devRxLen = off + n;
   return 0;
}

static void sv_DevRxDone(uint8_t t, BlkStatus_E s, uint32_t n)
{
   (void)n;
   su8_devRxType = t;
   si_devRxDone = (int)s;
}

static void sv_DevTxDone(uint8_t t, BlkStatus_E s)
{
   (void)t;
   si_devTxDone = (int)s;
}

static void sv_DevRxShort(uint8_t t, const uint8_t *d, uint8_t n)
{
   si_devShortCount++;
   su8_devShortType = t;
   su8_devShortLen = n;
   (void)memcpy(su8ar_devShort, d, n);
}

/******************************************************************************/
/*  Peer (central) - independent implementation of the other side             */
/******************************************************************************/
typedef struct
{
   /* receiver role: device -> peer */
   uint8_t u8ar_rx[MAX_OBJ];
   bool b_rxActive;
   uint8_t u8_rxId, u8_rxType, u8_rxChunk, u8_rxWindow, u8_sinceAck;
   uint32_t u32_rxTotal, u32_rxCrcExp, u32_rxCrc, u32_rxNext, u32_rxFrames;
   bool b_rxNackSent;
   int i_rxDone;
   int32_t i32_dropAbsOnce;         /* simulate app-level loss of one frame */
   uint8_t u8_ackEvery;             /* 0 = window/2, else ACK every N frames */
   bool b_ackOnlyWhenIdle;          /* ACK only once the sender goes quiet   */
   bool b_silent;                   /* never answer                         */
   uint32_t u32_nacksSent;
   uint32_t u32_rxAckedAbs;         /* last ACK/NACK position sent          */
   uint32_t u32_rxMaxAhead;         /* max frames seen beyond that position */

   /* sender role: peer -> device */
   const uint8_t *u8pt_tx;
   uint32_t u32_txLen;
   bool b_txActive, b_txStarted;
   uint8_t u8_txId, u8_txChunk, u8_txWindow;
   uint32_t u32_txFrames, u32_txNext, u32_txAcked, u32_txBurst;
   int64_t i64_txLastProgress;
   int i_txDone;
   uint32_t u32_nacksRcvd;

   /* short messages from the device */
   int i_shortCount;
   uint8_t u8_shortType, u8_shortLen;
   uint8_t u8ar_short[256];
} Peer_T;

static Peer_T sst_peer;

static void sv_PeerWrite(const uint8_t *u8pt_buf, uint16_t u16_len)
{
   // Write Without Response: a hook error just loses the frame
   (void)gt_BLK_RxWriteHook(&sst_conn, &sst_rxAttr, u8pt_buf, u16_len, 0U,
      BT_GATT_WRITE_FLAG_CMD);
}

static void sv_PeerCtrl3(uint8_t u8_type, uint8_t a, uint8_t b, uint8_t c)
{
   uint8_t u8ar_f[5] = { 3U, u8_type, a, b, c };
   sv_PeerWrite(u8ar_f, sizeof(u8ar_f));
}

static void sv_PeerRxFinish(void)
{
   uint8_t u8_st = (sst_peer.u32_rxCrc == sst_peer.u32_rxCrcExp) ? eBS_OK : eBS_CRC_ERROR;
   uint8_t u8ar_f[4] = { 2U, eBFT_END, sst_peer.u8_rxId, u8_st };

   sv_PeerWrite(u8ar_f, sizeof(u8ar_f));
   sst_peer.b_rxActive = false;
   sst_peer.i_rxDone = u8_st;
}

static void sv_PeerOnFrame(const uint8_t *u8pt_buf, uint16_t u16_len)
{
   BlkFrame_T f;
   uint8_t u8_diff;
   uint32_t u32_abs;

   CHECK(gi_BLK_FrameParse(u8pt_buf, u16_len, &f) == 0);
   CHECK(u16_len <= (su16_mtu - 3U));

   if (sst_peer.b_silent) { return; }

   if (f.u8_type <= BLK_APP_TYPE_MAX)
   {
      sst_peer.i_shortCount++;
      sst_peer.u8_shortType = f.u8_type;
      sst_peer.u8_shortLen = f.u8_payloadLen;
      (void)memcpy(sst_peer.u8ar_short, f.u8pt_payload, f.u8_payloadLen);
      return;
   }

   switch (f.u8_type)
   {
      case eBFT_START:
         sst_peer.b_rxActive = true;
         sst_peer.u8_rxId = f.u_body.st_start.u8_xferId;
         sst_peer.u8_rxType = f.u_body.st_start.u8_appType;
         sst_peer.u8_rxChunk = f.u_body.st_start.u8_chunkSize;
         sst_peer.u8_rxWindow = MIN(f.u_body.st_start.u8_window, 16U);
         sst_peer.u32_rxTotal = f.u_body.st_start.u32_totalLen;
         sst_peer.u32_rxCrcExp = f.u_body.st_start.u32_crc32;
         sst_peer.u32_rxCrc = 0U;
         sst_peer.u32_rxNext = 0U;
         sst_peer.u8_sinceAck = 0U;
         sst_peer.b_rxNackSent = false;
         sst_peer.u32_rxFrames = (sst_peer.u32_rxTotal + sst_peer.u8_rxChunk - 1U) / sst_peer.u8_rxChunk;
         CHECK(sst_peer.u8_rxChunk == (MIN(su16_mtu - 3U, BLK_MAX_FRAME_LEN) - BLK_DATA_HDR_LEN));
         if (sst_peer.u32_rxFrames == 0U) { sv_PeerRxFinish(); break; }
         sst_peer.u32_rxAckedAbs = 0U;
         sst_peer.u32_rxMaxAhead = 0U;
         sv_PeerCtrl3(eBFT_ACK, sst_peer.u8_rxId, 0U, sst_peer.u8_rxWindow);
         break;

      case eBFT_DATA:
         if (!sst_peer.b_rxActive || (f.u_body.st_data.u8_xferId != sst_peer.u8_rxId)) { break; }
         u8_diff = (uint8_t)(f.u_body.st_data.u8_seq - (uint8_t)sst_peer.u32_rxNext);
         // How far beyond the last acknowledged position did the device send?
         if (u8_diff < 128U)
         {
            sst_peer.u32_rxMaxAhead = MAX(sst_peer.u32_rxMaxAhead,
               sst_peer.u32_rxNext + u8_diff + 1U - sst_peer.u32_rxAckedAbs);
         }
         if (u8_diff == 0U)
         {
            if (sst_peer.i32_dropAbsOnce == (int32_t)sst_peer.u32_rxNext)
            {
               sst_peer.i32_dropAbsOnce = -1;
               break;
            }
            (void)memcpy(&sst_peer.u8ar_rx[sst_peer.u32_rxNext * sst_peer.u8_rxChunk],
               f.u_body.st_data.u8pt_data, f.u_body.st_data.u8_dataLen);
            sst_peer.u32_rxCrc = crc32_ieee_update(sst_peer.u32_rxCrc,
               f.u_body.st_data.u8pt_data, f.u_body.st_data.u8_dataLen);
            sst_peer.u32_rxNext++;
            sst_peer.u8_sinceAck++;
            sst_peer.b_rxNackSent = false;
            if (sst_peer.u32_rxNext == sst_peer.u32_rxFrames) { sv_PeerRxFinish(); break; }
            if (!sst_peer.b_ackOnlyWhenIdle && (sst_peer.u8_sinceAck >= ((sst_peer.u8_ackEvery != 0U)
               ? sst_peer.u8_ackEvery : (sst_peer.u8_rxWindow / 2U))))
            {
               sst_peer.u8_sinceAck = 0U;
               sst_peer.u32_rxAckedAbs = sst_peer.u32_rxNext;
               sv_PeerCtrl3(eBFT_ACK, sst_peer.u8_rxId, (uint8_t)sst_peer.u32_rxNext,
                  sst_peer.u8_rxWindow);
            }
         }
         else if (u8_diff < 128U)
         {
            if (!sst_peer.b_rxNackSent)
            {
               sst_peer.b_rxNackSent = true;
               sst_peer.u32_nacksSent++;
               sst_peer.u32_rxAckedAbs = sst_peer.u32_rxNext;
               sv_PeerCtrl3(eBFT_NACK, sst_peer.u8_rxId, (uint8_t)sst_peer.u32_rxNext,
                  eBS_OUT_OF_ORDER);
            }
         }
         else
         {
            sst_peer.u32_rxAckedAbs = sst_peer.u32_rxNext;
            sv_PeerCtrl3(eBFT_ACK, sst_peer.u8_rxId, (uint8_t)sst_peer.u32_rxNext,
               sst_peer.u8_rxWindow);
         }
         break;

      case eBFT_ACK:
         if (!sst_peer.b_txActive || (f.u_body.st_ack.u8_xferId != sst_peer.u8_txId)) { break; }
         if (!sst_peer.b_txStarted)
         {
            if (f.u_body.st_ack.u8_seq == 0U)
            {
               sst_peer.b_txStarted = true;
               sst_peer.u8_txWindow = MIN(sst_peer.u8_txWindow, f.u_body.st_ack.u8_window);
               sst_peer.i64_txLastProgress = gi64_simNowMs;
            }
            break;
         }
         u32_abs = sst_peer.u32_txAcked + (uint8_t)(f.u_body.st_ack.u8_seq - (uint8_t)sst_peer.u32_txAcked);
         if ((u32_abs > sst_peer.u32_txAcked) && (u32_abs <= sst_peer.u32_txNext))
         {
            sst_peer.u32_txAcked = u32_abs;
            sst_peer.i64_txLastProgress = gi64_simNowMs;
         }
         break;

      case eBFT_NACK:
         if (!sst_peer.b_txActive || (f.u_body.st_nack.u8_xferId != sst_peer.u8_txId)) { break; }
         sst_peer.u32_nacksRcvd++;
         u32_abs = sst_peer.u32_txAcked + (uint8_t)(f.u_body.st_nack.u8_seq - (uint8_t)sst_peer.u32_txAcked);
         if (u32_abs <= sst_peer.u32_txNext)
         {
            sst_peer.u32_txAcked = u32_abs;
            sst_peer.u32_txNext = u32_abs;
            sst_peer.i64_txLastProgress = gi64_simNowMs;
         }
         break;

      case eBFT_END:
         if (sst_peer.b_txActive && (f.u_body.st_end.u8_xferId == sst_peer.u8_txId))
         {
            sst_peer.b_txActive = false;
            sst_peer.i_txDone = f.u_body.st_end.u8_status;
         }
         break;

      case eBFT_ABORT:
         if ((f.u_body.st_abort.u8_dir == eBAD_BY_RECEIVER) && sst_peer.b_txActive
            && (f.u_body.st_abort.u8_xferId == sst_peer.u8_txId))
         {
            sst_peer.b_txActive = false;
            sst_peer.i_txDone = PEER_ABORT_BASE + f.u_body.st_abort.u8_reason;
         }
         else if ((f.u_body.st_abort.u8_dir == eBAD_BY_SENDER) && sst_peer.b_rxActive
            && (f.u_body.st_abort.u8_xferId == sst_peer.u8_rxId))
         {
            sst_peer.b_rxActive = false;
            sst_peer.i_rxDone = PEER_RX_ABORT_BASE + f.u_body.st_abort.u8_reason;
         }
         break;

      default:
         CHECK(false);
         break;
   }
}

static void sv_PeerStartSend(const uint8_t *u8pt_data, uint32_t u32_len, uint32_t u32_burst,
   bool b_badCrc)
{
   uint8_t u8ar_f[BLK_CTRL_FRAME_MAX_LEN];
   uint32_t u32_crc = crc32_ieee_update(0U, u8pt_data, u32_len) ^ (b_badCrc ? 1U : 0U);

   sst_peer.u8pt_tx = u8pt_data;
   sst_peer.u32_txLen = u32_len;
   sst_peer.b_txActive = true;
   sst_peer.b_txStarted = false;
   sst_peer.u8_txId++;
   sst_peer.u8_txChunk = (uint8_t)(MIN(su16_mtu - 3U, BLK_MAX_FRAME_LEN) - BLK_DATA_HDR_LEN);
   sst_peer.u8_txWindow = 32U;
   sst_peer.u32_txFrames = (u32_len + sst_peer.u8_txChunk - 1U) / sst_peer.u8_txChunk;
   sst_peer.u32_txNext = 0U;
   sst_peer.u32_txAcked = 0U;
   sst_peer.u32_txBurst = u32_burst;
   sst_peer.i64_txLastProgress = gi64_simNowMs;
   sst_peer.i_txDone = STATUS_NONE;

   (void)gu16_BLK_EncodeStart(u8ar_f, sizeof(u8ar_f), sst_peer.u8_txId, 0x42U, u32_len,
      sst_peer.u8_txChunk, sst_peer.u8_txWindow, u32_crc);
   sv_PeerWrite(u8ar_f, BLK_CTRL_FRAME_MAX_LEN);
}

/** Peer sends up to one burst of DATA frames. Returns true if it sent any. */
static bool sb_PeerPump(void)
{
   uint8_t u8ar_f[BLK_MAX_FRAME_LEN];
   uint32_t u32_sent = 0U;
   uint32_t u32_off;
   uint16_t u16_n;

   if (!sst_peer.b_txActive || !sst_peer.b_txStarted) { return false; }

   // Peer-side ACK timeout: go back to the last acknowledged frame
   if ((gi64_simNowMs - sst_peer.i64_txLastProgress) > 1500)
   {
      sst_peer.u32_txNext = sst_peer.u32_txAcked;
      sst_peer.i64_txLastProgress = gi64_simNowMs;
   }

   while ((sst_peer.u32_txNext < sst_peer.u32_txFrames)
      && ((sst_peer.u32_txNext - sst_peer.u32_txAcked) < sst_peer.u8_txWindow)
      && (u32_sent < sst_peer.u32_txBurst))
   {
      u32_off = sst_peer.u32_txNext * sst_peer.u8_txChunk;
      u16_n = (uint16_t)MIN((uint32_t)sst_peer.u8_txChunk, sst_peer.u32_txLen - u32_off);
      (void)gu16_BLK_EncodeDataHeader(u8ar_f, sizeof(u8ar_f), sst_peer.u8_txId,
         (uint8_t)sst_peer.u32_txNext, u16_n);
      (void)memcpy(&u8ar_f[BLK_DATA_HDR_LEN], &sst_peer.u8pt_tx[u32_off], u16_n);
      sv_PeerWrite(u8ar_f, (uint16_t)(BLK_DATA_HDR_LEN + u16_n));
      sst_peer.u32_txNext++;
      u32_sent++;
   }

   return u32_sent > 0U;
}

/******************************************************************************/
/*  Scheduler                                                                 */
/******************************************************************************/
static bool sb_Step(void)
{
   bool b_active = false;

   // Engine: run while it has been kicked
   while (sst_BLK_wakeSem.count > 0U)
   {
      sst_BLK_wakeSem.count = 0U;
      sv_EngineRunOnce();
      b_active = true;
   }

   if (su32_linkCount > 0U)
   {
      sv_LinkEvent();
      b_active = true;
   }

   if (sb_PeerPump())
   {
      gi64_simNowMs++;
      sv_SimFireTimers();
      b_active = true;
   }

   return b_active;
}

/** Nothing moved during a step: a lazy central acknowledges now. */
static void sv_PeerIdle(void)
{
   if (sst_peer.b_ackOnlyWhenIdle && sst_peer.b_rxActive && (sst_peer.u8_sinceAck > 0U))
   {
      sst_peer.u8_sinceAck = 0U;
      sst_peer.u32_rxAckedAbs = sst_peer.u32_rxNext;
      sv_PeerCtrl3(eBFT_ACK, sst_peer.u8_rxId, (uint8_t)sst_peer.u32_rxNext,
         sst_peer.u8_rxWindow);
   }
}

static bool sb_RunUntil(bool (*fpt_cond)(void), int64_t i64_maxMs)
{
   int64_t i64_end = gi64_simNowMs + i64_maxMs;
   uint32_t u32_iter = 0U;

   while (!fpt_cond() && (gi64_simNowMs < i64_end) && (u32_iter++ < 20000000U))
   {
      if (!sb_Step())
      {
         sv_PeerIdle();
         gi64_simNowMs++;
         sv_SimFireTimers();
      }
   }

   return fpt_cond();
}

static void sv_Settle(int64_t i64_ms)
{
   int64_t i64_end = gi64_simNowMs + i64_ms;

   while (gi64_simNowMs < i64_end)
   {
      if (!sb_Step()) { gi64_simNowMs++; sv_SimFireTimers(); }
   }
}

static bool sb_DevTxDone(void) { return si_devTxDone != STATUS_NONE; }
static bool sb_DevRxDone(void) { return si_devRxDone != STATUS_NONE; }
static bool sb_PeerTxDone(void) { return sst_peer.i_txDone != STATUS_NONE; }
static bool sb_BothDone(void) { return sb_DevTxDone() && sb_DevRxDone(); }
static bool sb_PeerShort(void) { return sst_peer.i_shortCount > 0; }
static bool sb_DevShort(void) { return si_devShortCount > 0; }

/******************************************************************************/
/*  Fixtures                                                                  */
/******************************************************************************/
static uint8_t su8ar_pattern[MAX_OBJ];

static void sv_Connect(uint16_t u16_mtu)
{
   (void)memset(&sst_peer, 0, sizeof(sst_peer));
   sst_peer.i_rxDone = STATUS_NONE;
   sst_peer.i_txDone = STATUS_NONE;
   sst_peer.i32_dropAbsOnce = -1;
   si_devTxDone = STATUS_NONE;
   si_devRxDone = STATUS_NONE;
   su32_devRxLen = 0U;
   sb_devRxOrderError = false;
   si_devShortCount = 0;
   su32_devRejectAbove = MAX_OBJ;
   su32_linkHead = 0U;
   su32_linkCount = 0U;
   su16_mtu = u16_mtu;
   sb_subscribed = true;
   sb_connected = true;
   gv_BLK_OnConnected(&sst_conn);
}

static void sv_Disconnect(void)
{
   sb_connected = false;
   su32_linkCount = 0U;              /* queued PDUs die with the link */
   gv_BLK_OnDisconnected(&sst_conn);
   sv_Settle(5);
}

/******************************************************************************/
/*  Tests                                                                     */
/******************************************************************************/
static void sv_TestDeviceToPeerSizes(void)
{
   static const uint32_t su32ar_sizes[] = { 0U, 1U, 239U, 240U, 241U, 480U, 100000U };
   uint32_t i;

   printf("device -> central, assorted sizes (MTU 247)\n");
   for (i = 0U; i < ARRAY_SIZE(su32ar_sizes); i++)
   {
      sv_Connect(247U);
      su32_maxInFlight = 0U;
      CHECK(gi_BLK_SendBuffer(0x10U, su8ar_pattern, su32ar_sizes[i]) == 0);
      CHECK(sb_RunUntil(sb_DevTxDone, 60000));
      CHECK(si_devTxDone == eBS_OK);
      CHECK(sst_peer.i_rxDone == eBS_OK);
      CHECK(sst_peer.u32_rxTotal == su32ar_sizes[i]);
      CHECK(memcmp(sst_peer.u8ar_rx, su8ar_pattern, su32ar_sizes[i]) == 0);
      CHECK(su32_maxInFlight <= BLK_TX_INFLIGHT_MAX);
      CHECK(sst_peer.u32_rxMaxAhead <= BLK_WINDOW_DEFAULT);
      CHECK(sst_BLK_txCredits.count == BLK_TX_INFLIGHT_MAX);
      sv_Disconnect();
   }
}

static void sv_TestDeviceToPeerLoss(void)
{
   int64_t i64_start;

   printf("device -> central, central drops frame 300 (after seq wrap) -> NACK\n");
   sv_Connect(247U);
   sst_peer.i32_dropAbsOnce = 300;
   i64_start = gi64_simNowMs;
   CHECK(gi_BLK_SendBuffer(0x11U, su8ar_pattern, 100000U) == 0);
   CHECK(sb_RunUntil(sb_DevTxDone, 60000));
   printf("  done in %d ms simulated\n", (int)(gi64_simNowMs - i64_start));
   // Recovery must come from the NACK, not from the ACK timeout
   CHECK((gi64_simNowMs - i64_start) < (int64_t)BLK_TX_ACK_TIMEOUT_MS);
   CHECK(si_devTxDone == eBS_OK);
   CHECK(sst_peer.u32_nacksSent == 1U);
   CHECK(memcmp(sst_peer.u8ar_rx, su8ar_pattern, 100000U) == 0);
   sv_Disconnect();
}

static void sv_TestDeviceToPeerTimeout(void)
{
   int64_t i64_start;

   printf("device -> central, central never answers -> TIMEOUT\n");
   sv_Connect(247U);
   sst_peer.b_silent = true;
   i64_start = gi64_simNowMs;
   CHECK(gi_BLK_SendBuffer(0x12U, su8ar_pattern, 5000U) == 0);
   CHECK(sb_RunUntil(sb_DevTxDone, 60000));
   CHECK(si_devTxDone == eBS_TIMEOUT);
   CHECK((gi64_simNowMs - i64_start) >= (int64_t)(BLK_TX_ACK_TIMEOUT_MS * BLK_TX_MAX_RETRIES));
   CHECK(!gb_BLK_IsTxBusy());
   sv_Disconnect();
}

static void sv_TestPeerToDeviceSizes(void)
{
   static const uint32_t su32ar_sizes[] = { 0U, 1U, 240U, 241U, 100000U };
   uint32_t i;

   printf("central -> device, assorted sizes, burst 4\n");
   for (i = 0U; i < ARRAY_SIZE(su32ar_sizes); i++)
   {
      sv_Connect(247U);
      sv_PeerStartSend(su8ar_pattern, su32ar_sizes[i], 4U, false);
      CHECK(sb_RunUntil(sb_PeerTxDone, 60000));
      CHECK(sst_peer.i_txDone == eBS_OK);
      CHECK(si_devRxDone == eBS_OK);
      CHECK(su8_devRxType == 0x42U);
      CHECK(su32_devRxLen == su32ar_sizes[i]);
      CHECK(!sb_devRxOrderError);
      CHECK(memcmp(su8ar_devRx, su8ar_pattern, su32ar_sizes[i]) == 0);
      CHECK(sst_BLK_rxSlab.used == 0U);
      sv_Disconnect();
   }
}

static void sv_TestPeerToDeviceOverflow(void)
{
   int64_t i64_start;

   printf("central -> device, bursts of 16 into a %u-deep RX pool -> overflow NACKs\n",
      (unsigned)BLK_RX_POOL_DEPTH);
   sv_Connect(247U);
   i64_start = gi64_simNowMs;
   sv_PeerStartSend(su8ar_pattern, 100000U, 16U, false);
   CHECK(sb_RunUntil(sb_PeerTxDone, 120000));
   // Recovery must come from NACKs, not from the central's 1500 ms timeout
   CHECK((gi64_simNowMs - i64_start) < 1500);
   CHECK(sst_peer.i_txDone == eBS_OK);
   CHECK(si_devRxDone == eBS_OK);
   CHECK(sst_peer.u32_nacksRcvd > 0U);
   CHECK(!sb_devRxOrderError);
   CHECK(memcmp(su8ar_devRx, su8ar_pattern, 100000U) == 0);
   printf("  recovered after %u NACKs in %d ms simulated\n", sst_peer.u32_nacksRcvd,
      (int)(gi64_simNowMs - i64_start));
   sv_Disconnect();
}

static void sv_TestPeerToDeviceCrcAndReject(void)
{
   printf("central -> device, bad CRC -> CRC_ERROR; oversize -> REJECTED\n");
   sv_Connect(247U);
   sv_PeerStartSend(su8ar_pattern, 3000U, 4U, true);
   CHECK(sb_RunUntil(sb_PeerTxDone, 60000));
   CHECK(sst_peer.i_txDone == eBS_CRC_ERROR);
   CHECK(si_devRxDone == eBS_CRC_ERROR);

   si_devRxDone = STATUS_NONE;
   su32_devRejectAbove = 1000U;
   sv_PeerStartSend(su8ar_pattern, 3000U, 4U, false);
   CHECK(sb_RunUntil(sb_PeerTxDone, 60000));
   CHECK(sst_peer.i_txDone == PEER_ABORT_BASE + eBS_REJECTED);
   CHECK(si_devRxDone == STATUS_NONE);
   sv_Disconnect();
}

static void sv_TestShortMessages(void)
{
   uint8_t u8ar_f[BLK_MAX_FRAME_LEN];
   uint16_t u16_len;

   printf("short messages both ways, size limits\n");
   sv_Connect(247U);
   CHECK(gu16_BLK_GetMaxShortPayload() == BLK_MAX_SHORT_PAYLOAD);
   CHECK(gi_BLK_SendShort(0x05U, su8ar_pattern, 242U, K_MSEC(10)) == 0);
   CHECK(sb_RunUntil(sb_PeerShort, 1000));
   CHECK(sst_peer.u8_shortType == 0x05U && sst_peer.u8_shortLen == 242U);
   CHECK(memcmp(sst_peer.u8ar_short, su8ar_pattern, 242U) == 0);
   CHECK(gi_BLK_SendShort(0x05U, su8ar_pattern, 243U, K_MSEC(10)) == -EMSGSIZE);
   CHECK(gi_BLK_SendShort(eBFT_START, su8ar_pattern, 1U, K_MSEC(10)) == -EINVAL);

   u16_len = gu16_BLK_EncodeShort(u8ar_f, sizeof(u8ar_f), 0x07U, su8ar_pattern, 100U);
   sv_PeerWrite(u8ar_f, u16_len);
   CHECK(sb_RunUntil(sb_DevShort, 1000));
   CHECK(su8_devShortType == 0x07U && su8_devShortLen == 100U);

   // Malformed writes are rejected by the hook with an ATT error
   u8ar_f[0] = 50U;
   CHECK(gt_BLK_RxWriteHook(&sst_conn, &sst_rxAttr, u8ar_f, 10U, 0U, 0U)
      == BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN));
   CHECK(gt_BLK_RxWriteHook(&sst_conn, &sst_rxAttr, u8ar_f, 10U, 5U, 0U)
      == BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET));
   sv_Disconnect();

   // MTU 23: 20-byte frames -> 18-byte short payload
   sv_Connect(23U);
   CHECK(gu16_BLK_GetMaxShortPayload() == 18U);
   CHECK(gi_BLK_SendShort(0x05U, su8ar_pattern, 19U, K_MSEC(10)) == -EMSGSIZE);
   sv_Disconnect();
}

static void sv_TestBidirectional(void)
{
   printf("simultaneous transfers in both directions\n");
   sv_Connect(247U);
   CHECK(gi_BLK_SendBuffer(0x20U, su8ar_pattern, 60000U) == 0);
   sv_PeerStartSend(&su8ar_pattern[1000], 50000U, 4U, false);
   CHECK(sb_RunUntil(sb_BothDone, 60000));
   CHECK(si_devTxDone == eBS_OK);
   CHECK(si_devRxDone == eBS_OK);
   CHECK(memcmp(sst_peer.u8ar_rx, su8ar_pattern, 60000U) == 0);
   CHECK(memcmp(su8ar_devRx, &su8ar_pattern[1000], 50000U) == 0);
   sv_Disconnect();
}

static void sv_TestSmallMtu(void)
{
   printf("MTU 23 (16-byte chunks, ~6250 frames, many seq wraps) both ways\n");
   sv_Connect(23U);
   CHECK(gi_BLK_SendBuffer(0x21U, su8ar_pattern, 100000U) == 0);
   CHECK(sb_RunUntil(sb_DevTxDone, 120000));
   CHECK(si_devTxDone == eBS_OK);
   CHECK(memcmp(sst_peer.u8ar_rx, su8ar_pattern, 100000U) == 0);

   sv_PeerStartSend(su8ar_pattern, 30000U, 4U, false);
   CHECK(sb_RunUntil(sb_PeerTxDone, 120000));
   CHECK(si_devRxDone == eBS_OK);
   CHECK(memcmp(su8ar_devRx, su8ar_pattern, 30000U) == 0);
   sv_Disconnect();
}

static void sv_TestAbortAndDisconnect(void)
{
   printf("local abort, disconnect mid-transfer, API preconditions\n");

   // Local abort of TX: device reports ABORTED, central gets ABORT(by sender)
   sv_Connect(247U);
   CHECK(gi_BLK_SendBuffer(0x30U, su8ar_pattern, 100000U) == 0);
   CHECK(gi_BLK_SendBuffer(0x30U, su8ar_pattern, 10U) == -EBUSY);
   sv_Settle(20);
   gv_BLK_AbortTx();
   CHECK(sb_RunUntil(sb_DevTxDone, 1000));
   CHECK(si_devTxDone == eBS_ABORTED);
   sv_Settle(10);
   CHECK(sst_peer.i_rxDone == PEER_RX_ABORT_BASE + eBS_ABORTED);

   // Local abort of RX: central gets ABORT(by receiver)
   si_devRxDone = STATUS_NONE;
   sv_PeerStartSend(su8ar_pattern, 100000U, 4U, false);
   sv_Settle(20);
   gv_BLK_AbortRx();
   CHECK(sb_RunUntil(sb_PeerTxDone, 1000));
   CHECK(sst_peer.i_txDone == PEER_ABORT_BASE + eBS_ABORTED);
   CHECK(si_devRxDone == eBS_ABORTED);
   sv_Disconnect();

   // Disconnect with transfers running in both directions
   sv_Connect(247U);
   CHECK(gi_BLK_SendBuffer(0x31U, su8ar_pattern, 100000U) == 0);
   sv_PeerStartSend(su8ar_pattern, 100000U, 4U, false);
   sv_Settle(30);
   CHECK(gb_BLK_IsTxBusy());
   sv_Disconnect();
   CHECK(si_devTxDone == eBS_DISCONNECTED);
   CHECK(si_devRxDone == eBS_DISCONNECTED);
   CHECK(!gb_BLK_IsTxBusy());
   CHECK(sst_BLK_txCredits.count == BLK_TX_INFLIGHT_MAX);

   // Disconnect while notifications are queued in the host: their completion
   // callbacks never run, so the engine must restore the credits itself
   sv_Connect(247U);
   CHECK(gi_BLK_SendBuffer(0x33U, su8ar_pattern, 100000U) == 0);
   sst_BLK_wakeSem.count = 0U;
   sv_EngineRunOnce();               /* START                              */
   sv_LinkEvent();                   /* central ACKs START                 */
   sst_BLK_wakeSem.count = 0U;
   sv_EngineRunOnce();               /* DATA burst takes the credits       */
   CHECK(su32_linkCount > 0U);
   CHECK(sst_BLK_txCredits.count < BLK_TX_INFLIGHT_MAX);
   sv_Disconnect();
   CHECK(si_devTxDone == eBS_DISCONNECTED);
   CHECK(sst_BLK_txCredits.count == BLK_TX_INFLIGHT_MAX);

   // No connection / not subscribed
   CHECK(gi_BLK_SendBuffer(0x31U, su8ar_pattern, 10U) == -ENOTCONN);
   sv_Connect(247U);
   sb_subscribed = false;
   CHECK(gi_BLK_SendBuffer(0x31U, su8ar_pattern, 10U) == -EACCES);
   sb_subscribed = true;

   // Reconnected link works normally again
   CHECK(gi_BLK_SendBuffer(0x32U, su8ar_pattern, 20000U) == 0);
   CHECK(sb_RunUntil(sb_DevTxDone, 60000));
   CHECK(si_devTxDone == eBS_OK);
   sv_Disconnect();
}

static void sv_TestWindowStall(void)
{
   printf("central ACKs only when the sender goes quiet -> sender stops at the window\n");
   sv_Connect(247U);
   sst_peer.b_ackOnlyWhenIdle = true;
   CHECK(gi_BLK_SendBuffer(0x40U, su8ar_pattern, 50000U) == 0);
   CHECK(sb_RunUntil(sb_DevTxDone, 60000));
   CHECK(si_devTxDone == eBS_OK);
   CHECK(sst_peer.u32_rxMaxAhead == BLK_WINDOW_DEFAULT);
   CHECK(memcmp(sst_peer.u8ar_rx, su8ar_pattern, 50000U) == 0);
   sv_Disconnect();
}

static void sv_TestStaleFramesAfterReconnect(void)
{
   uint8_t u8ar_f[BLK_CTRL_FRAME_MAX_LEN];

   printf("frames queued before a disconnect are discarded after reconnect\n");
   sv_Connect(247U);
   si_devRxStartCalls = 0;
   (void)gu16_BLK_EncodeStart(u8ar_f, sizeof(u8ar_f), 99U, 0x42U, 1000U, 240U, 16U, 0U);
   sv_PeerWrite(u8ar_f, BLK_CTRL_FRAME_MAX_LEN);    /* queued, engine not run */
   sb_connected = false;
   gv_BLK_OnDisconnected(&sst_conn);
   sb_connected = true;
   gv_BLK_OnConnected(&sst_conn);
   sv_Settle(20);
   CHECK(si_devRxStartCalls == 0);
   CHECK(!sst_BLK_rxSession.b_active);
   CHECK(sst_BLK_rxSlab.used == 0U);
   sv_Disconnect();
}

static void sv_TestMalformedData(void)
{
   uint8_t u8ar_f[BLK_MAX_FRAME_LEN];

   printf("DATA chunk shorter than announced -> receiver aborts with PROTOCOL_ERROR\n");
   sv_Connect(247U);
   sv_PeerStartSend(su8ar_pattern, 1000U, 0U, false);  /* burst 0: manual */
   sv_Settle(5);
   CHECK(sst_peer.b_txStarted);
   (void)gu16_BLK_EncodeDataHeader(u8ar_f, sizeof(u8ar_f), sst_peer.u8_txId, 0U, 10U);
   sv_PeerWrite(u8ar_f, BLK_DATA_HDR_LEN + 10U);
   CHECK(sb_RunUntil(sb_PeerTxDone, 1000));
   CHECK(sst_peer.i_txDone == PEER_ABORT_BASE + eBS_PROTOCOL_ERROR);
   CHECK(si_devRxDone == eBS_PROTOCOL_ERROR);
   sv_Disconnect();
}

int main(int argc, char **argv)
{
   BlkCfg_T st_cfg = { 0 };
   uint32_t i;

   gb_simVerbose = (argc > 1) && (strcmp(argv[1], "-v") == 0);

   for (i = 0U; i < MAX_OBJ; i++)
   {
      su8ar_pattern[i] = (uint8_t)((i * 31U) ^ (i >> 8));
   }

   st_cfg.stpt_txAttr = &sst_txAttr;
   st_cfg.fpt_onRxStart = si_DevRxStart;
   st_cfg.fpt_onRxData = si_DevRxData;
   st_cfg.fpt_onRxDone = sv_DevRxDone;
   st_cfg.fpt_onRxShort = sv_DevRxShort;
   st_cfg.fpt_onTxDone = sv_DevTxDone;
   CHECK(gi_BLK_Init(&st_cfg) == 0);
   CHECK(gi_BLK_Init(&st_cfg) == -EALREADY);

   sv_TestDeviceToPeerSizes();
   sv_TestDeviceToPeerLoss();
   sv_TestDeviceToPeerTimeout();
   sv_TestPeerToDeviceSizes();
   sv_TestPeerToDeviceOverflow();
   sv_TestPeerToDeviceCrcAndReject();
   sv_TestShortMessages();
   sv_TestBidirectional();
   sv_TestSmallMtu();
   sv_TestAbortAndDisconnect();
   sv_TestWindowStall();
   sv_TestStaleFramesAfterReconnect();
   sv_TestMalformedData();

   if (si_failures == 0)
   {
      printf("All BulkXfer engine tests passed\n");
      return 0;
   }

   printf("%d check(s) failed\n", si_failures);
   return 1;
}
