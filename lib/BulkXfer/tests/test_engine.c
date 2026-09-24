/**
 * @file          test_engine.c
 * @brief         Host tests of the real BulkXfer engine (Core + Server +
 *                Client) against a simulated BLE link and a scripted peer
 *                that implements the other side of both roles:
 *
 *                  device Client --WwR DATA-->  peer server (GATT DB model)
 *                  device Client <--CTRL ntf--  peer server
 *                  device Server <--WwR DATA--  peer client
 *                  device Server --CTRL ntf-->  peer client
 *
 *                Build it three ways to prove each role builds alone:
 *
 * @code
 *                gcc -std=gnu99 -Wall -Wextra -Werror -Ishim -I.. -I../examples \
 *                    -Wno-missing-field-initializers -DCONFIG_BT_GATT_CLIENT \
 *                    -DBLK_RX_POOL_DEPTH=6 -o test_engine \
 *                    test_engine.c ../BulkXfer_Frame.c && ./test_engine
 *                ... -DBLK_ENABLE_CLIENT=0   (server only)
 *                ... -DBLK_ENABLE_SERVER=0   (client only)
 * @endcode
 *
 * @date          24/09/2026
 * @author        Shivam Chudasama
 * @copyright     Shivam Chudasama
 * @license       MIT
 */

/* SPDX-License-Identifier: MIT */

#include "../BulkXfer_Core.c"
#include "../BulkXfer_Server.c"
#include "../BulkXfer_Client.c"

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
#define STATUS_NONE          (-1000)
#define PEER_ABORT_BASE      (100)    /* peer tx ended by device ABORT      */
#define PEER_RX_ABORT_BASE   (200)    /* peer rx ended by device ABORT      */

/* Helpers used only by one role are unused in a single-role build */
#define TEST_HELPER          static __attribute__((unused))

int64_t gi64_simNowMs = 0;
bool gb_simVerbose = false;

/******************************************************************************/
/*  Peer GATT database (hosted by the peer, discovered by the device Client)  */
/******************************************************************************/
#define H_SVC                (10U)
#define H_DATA_DECL          (11U)
#define H_DATA               (12U)
#define H_CTRL_DECL          (13U)
#define H_CTRL               (14U)
#define H_CTRL_CCC           (15U)
#define H_CAPS_DECL          (16U)
#define H_CAPS               (17U)
#define H_SVC_END            (20U)

static bool sb_dbHasService = true;
static bool sb_dbHasCtrl = true;
static bool sb_subscribeFails = false;

/******************************************************************************/
/*  Simulated link                                                            */
/******************************************************************************/
#define LINK_HOST_BUFS       (10U)    /* like CONFIG_BT_ATT_TX_COUNT         */
#define LINK_PER_EVENT       (6U)     /* packets per connection event        */

typedef enum { ePDU_NOTIFY, ePDU_WRITE } PduKind_E;

typedef struct
{
   uint8_t u8ar_data[260];
   uint16_t u16_len;
   PduKind_E e_kind;
   bt_gatt_complete_func_t fpt_func;
} LinkPdu_T;

static LinkPdu_T sstar_linkQ[LINK_HOST_BUFS];
static uint32_t su32_linkHead = 0U;
static uint32_t su32_linkCount = 0U;
static struct bt_conn sst_conn = { 1 };
static struct bt_gatt_attr sst_ctrlAttr = { NULL, NULL, 0U };
TEST_HELPER struct bt_gatt_attr sst_dataAttr = { NULL, NULL, 0U };
static bool sb_connected = false;
static bool sb_peerSubscribed = true;        /* peer client subscribed to device CTRL */
static uint16_t su16_mtu = 247U;
static uint32_t su32_maxWrites = 0U;         /* max device writes queued in the host   */
static uint32_t su32_writesQueued = 0U;
static struct k_timer *sstpt_timers[16];
static uint32_t su32_timerCount = 0U;

/* Pending asynchronous GATT client operations of the device */
static struct bt_gatt_discover_params *sstpt_pendDiscover = NULL;
static struct bt_gatt_subscribe_params *sstpt_pendSubscribe = NULL;
static struct bt_gatt_exchange_params *sstpt_pendMtu = NULL;
static struct bt_gatt_subscribe_params *sstpt_devSub = NULL;   /* active subscription */
static int si_mtuExchanges = 0;

static void sv_PeerOnFrame(PduKind_E e_kind, const uint8_t *u8pt_buf, uint16_t u16_len);

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

/** Peer answers one pending discovery request, like the stack would. */
static void sv_SimRunDiscover(struct bt_gatt_discover_params *p)
{
   static struct bt_gatt_service_val st_svc;
   static struct bt_gatt_chrc star_chrc[3];
   static struct bt_gatt_attr st_attr;
   uint16_t au16_decl[3] = { H_DATA_DECL, H_CTRL_DECL, H_CAPS_DECL };
   uint32_t i;

   (void)memset(&st_attr, 0, sizeof(st_attr));

   if (p->type == BT_GATT_DISCOVER_PRIMARY)
   {
      if (sb_dbHasService && (bt_uuid_cmp(p->uuid, BT_UUID_BLK_SVC) == 0))
      {
         st_svc.uuid = BT_UUID_BLK_SVC;
         st_svc.end_handle = H_SVC_END;
         st_attr.handle = H_SVC;
         st_attr.user_data = &st_svc;
         if (p->func(&sst_conn, &st_attr, p) == BT_GATT_ITER_STOP) { return; }
      }
      (void)p->func(&sst_conn, NULL, p);
      return;
   }

   if (p->type == BT_GATT_DISCOVER_CHARACTERISTIC)
   {
      star_chrc[0].uuid = BT_UUID_BLK_DATA;
      star_chrc[0].value_handle = H_DATA;
      star_chrc[0].properties = BT_GATT_CHRC_WRITE | BT_GATT_CHRC_WRITE_WITHOUT_RESP;
      star_chrc[1].uuid = BT_UUID_BLK_CTRL;
      star_chrc[1].value_handle = H_CTRL;
      star_chrc[1].properties = BT_GATT_CHRC_NOTIFY;
      star_chrc[2].uuid = BT_UUID_BLK_CAPS;
      star_chrc[2].value_handle = H_CAPS;
      star_chrc[2].properties = 0x02U;
      for (i = 0U; i < 3U; i++)
      {
         if ((i == 1U) && !sb_dbHasCtrl) { continue; }
         if ((au16_decl[i] < p->start_handle) || (au16_decl[i] > p->end_handle)) { continue; }
         st_attr.handle = au16_decl[i];
         st_attr.user_data = &star_chrc[i];
         if (p->func(&sst_conn, &st_attr, p) == BT_GATT_ITER_STOP) { return; }
      }
      (void)p->func(&sst_conn, NULL, p);
      return;
   }

   // Descriptor: only CTRL has a CCC; the device must bound the search range
   CHECK(p->end_handle < H_CAPS_DECL);
   if (sb_dbHasCtrl && (bt_uuid_cmp(p->uuid, BT_UUID_GATT_CCC) == 0)
      && (H_CTRL_CCC >= p->start_handle) && (H_CTRL_CCC <= p->end_handle))
   {
      st_attr.handle = H_CTRL_CCC;
      if (p->func(&sst_conn, &st_attr, p) == BT_GATT_ITER_STOP) { return; }
   }
   (void)p->func(&sst_conn, NULL, p);
}

/** Run one pending GATT client operation (one ATT round trip). */
static bool sb_SimRunGattOp(void)
{
   struct bt_gatt_discover_params *pd = sstpt_pendDiscover;
   struct bt_gatt_subscribe_params *ps = sstpt_pendSubscribe;
   struct bt_gatt_exchange_params *pm = sstpt_pendMtu;

   if (pm != NULL)
   {
      sstpt_pendMtu = NULL;
      pm->func(&sst_conn, 0U, pm);
      return true;
   }
   if (pd != NULL)
   {
      sstpt_pendDiscover = NULL;
      sv_SimRunDiscover(pd);
      return true;
   }
   if (ps != NULL)
   {
      sstpt_pendSubscribe = NULL;
      if (sb_subscribeFails)
      {
         ps->subscribe(&sst_conn, 0x03U, ps);
      }
      else
      {
         sstpt_devSub = ps;
         ps->subscribe(&sst_conn, 0U, ps);
      }
      return true;
   }
   return false;
}

/** One connection event: deliver queued PDUs to the peer. */
static void sv_LinkEvent(void)
{
   uint32_t u32_n = MIN(su32_linkCount, LINK_PER_EVENT);

   while (u32_n-- > 0U)
   {
      LinkPdu_T *stpt_pdu = &sstar_linkQ[su32_linkHead];
      su32_linkHead = (su32_linkHead + 1U) % LINK_HOST_BUFS;
      su32_linkCount--;
      if (stpt_pdu->e_kind == ePDU_WRITE) { su32_writesQueued--; }
      sv_PeerOnFrame(stpt_pdu->e_kind, stpt_pdu->u8ar_data, stpt_pdu->u16_len);
      if (stpt_pdu->fpt_func != NULL) { stpt_pdu->fpt_func(&sst_conn, NULL); }
   }
   (void)sb_SimRunGattOp();
   gi64_simNowMs++;
   sv_SimFireTimers();
}

void gv_SimOnBlock(struct k_sem *stpt_sem)
{
   (void)stpt_sem;
   // A blocked credit wait means the engine waits for the link to drain
   if (su32_linkCount > 0U)
   {
      sv_LinkEvent();
   }
}

static int si_LinkQueue(PduKind_E e_kind, const void *vpt_data, uint16_t u16_len,
   bt_gatt_complete_func_t fpt_func)
{
   LinkPdu_T *stpt_pdu;

   if (u16_len > (su16_mtu - 3U)) { return -EMSGSIZE; }
   if (su32_linkCount >= LINK_HOST_BUFS) { return -ENOMEM; }

   stpt_pdu = &sstar_linkQ[(su32_linkHead + su32_linkCount) % LINK_HOST_BUFS];
   (void)memcpy(stpt_pdu->u8ar_data, vpt_data, u16_len);
   stpt_pdu->u16_len = u16_len;
   stpt_pdu->e_kind = e_kind;
   stpt_pdu->fpt_func = fpt_func;
   su32_linkCount++;
   if (e_kind == ePDU_WRITE)
   {
      su32_writesQueued++;
      su32_maxWrites = MAX(su32_maxWrites, su32_writesQueued);
   }
   return 0;
}

int bt_gatt_notify_cb(struct bt_conn *conn, struct bt_gatt_notify_params *params)
{
   if (!sb_connected || (conn != &sst_conn)) { return -ENOTCONN; }
   if (!sb_peerSubscribed) { return -EINVAL; }
   if (params->attr != &sst_ctrlAttr) { return -EINVAL; }
   return si_LinkQueue(ePDU_NOTIFY, params->data, params->len, params->func);
}

int bt_gatt_write_without_response_cb(struct bt_conn *conn, uint16_t handle,
   const void *data, uint16_t length, bool sign, bt_gatt_complete_func_t func, void *user_data)
{
   (void)sign; (void)user_data;
   if (!sb_connected || (conn != &sst_conn)) { return -ENOTCONN; }
   if (handle != H_DATA) { return -EINVAL; }
   return si_LinkQueue(ePDU_WRITE, data, length, func);
}

int bt_gatt_discover(struct bt_conn *conn, struct bt_gatt_discover_params *params)
{
   if (!sb_connected || (conn != &sst_conn)) { return -ENOTCONN; }
   CHECK(sstpt_pendDiscover == NULL);
   sstpt_pendDiscover = params;
   return 0;
}

int bt_gatt_subscribe(struct bt_conn *conn, struct bt_gatt_subscribe_params *params)
{
   if (!sb_connected || (conn != &sst_conn)) { return -ENOTCONN; }
   CHECK(params->value_handle == H_CTRL);
   CHECK(params->ccc_handle == H_CTRL_CCC);
   CHECK(params->value == BT_GATT_CCC_NOTIFY);
   CHECK((params->flags[0] & (1L << BT_GATT_SUBSCRIBE_FLAG_VOLATILE)) != 0);
   sstpt_pendSubscribe = params;
   return 0;
}

int bt_gatt_exchange_mtu(struct bt_conn *conn, struct bt_gatt_exchange_params *params)
{
   if (!sb_connected || (conn != &sst_conn)) { return -ENOTCONN; }
   si_mtuExchanges++;
   sstpt_pendMtu = params;
   return 0;
}

bool bt_gatt_is_subscribed(struct bt_conn *conn, const struct bt_gatt_attr *attr,
   uint16_t ccc_type)
{
   (void)conn; (void)ccc_type;
   CHECK(attr == &sst_ctrlAttr);
   return sb_peerSubscribed;
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
static int si_devRxStartCalls = 0;
static int si_devSrvShortCount = 0;          /* client -> device server shorts */
static int si_devCliShortCount = 0;          /* server -> device client shorts */
static uint8_t su8_devShortType = 0U;
static uint8_t su8_devShortLen = 0U;
static int si_devReady = STATUS_NONE;
static int si_devReadyCalls = 0;

TEST_HELPER int si_DevRxStart(uint8_t t, uint32_t n)
{
   (void)t;
   si_devRxStartCalls++;
   su32_devRxLen = 0U;
   return (n > su32_devRejectAbove) ? -ENOMEM : 0;
}

TEST_HELPER int si_DevRxData(uint8_t t, uint32_t off, const uint8_t *d, uint16_t n)
{
   (void)t;
   // Chunks must arrive contiguous and exactly once
   if (off != su32_devRxLen) { sb_devRxOrderError = true; }
   (void)memcpy(&su8ar_devRx[off], d, n);
   su32_devRxLen = off + n;
   return 0;
}

TEST_HELPER void sv_DevRxDone(uint8_t t, BlkStatus_E s, uint32_t n)
{
   (void)n;
   su8_devRxType = t;
   si_devRxDone = (int)s;
}

TEST_HELPER void sv_DevTxDone(uint8_t t, BlkStatus_E s)
{
   (void)t;
   si_devTxDone = (int)s;
}

TEST_HELPER void sv_DevSrvShort(uint8_t t, const uint8_t *d, uint8_t n)
{
   (void)d;
   si_devSrvShortCount++;
   su8_devShortType = t;
   su8_devShortLen = n;
}

TEST_HELPER void sv_DevCliShort(uint8_t t, const uint8_t *d, uint8_t n)
{
   (void)d;
   si_devCliShortCount++;
   su8_devShortType = t;
   su8_devShortLen = n;
}

TEST_HELPER void sv_DevReady(struct bt_conn *c, int i_status)
{
   CHECK(c == &sst_conn);
   si_devReadyCalls++;
   si_devReady = i_status;
}

/******************************************************************************/
/*  Peer - independent implementation of the other side of both roles         */
/******************************************************************************/
typedef struct
{
   /* peer as server: device Client -> peer */
   uint8_t u8ar_rx[MAX_OBJ];
   bool b_rxActive;
   uint8_t u8_rxId, u8_rxType, u8_rxChunk, u8_rxWindow, u8_sinceAck;
   uint32_t u32_rxTotal, u32_rxCrcExp, u32_rxCrc, u32_rxNext, u32_rxFrames;
   bool b_rxNackSent;
   int i_rxDone;
   int32_t i32_dropAbsOnce;         /* simulate app-level loss of one frame */
   bool b_ackOnlyWhenIdle;          /* ACK only once the sender goes quiet   */
   bool b_silent;                   /* never answer                         */
   uint32_t u32_nacksSent;
   uint32_t u32_rxAckedAbs;         /* last ACK/NACK position sent          */
   uint32_t u32_rxMaxAhead;         /* max frames seen beyond that position */

   /* peer as client: peer -> device Server */
   const uint8_t *u8pt_tx;
   uint32_t u32_txLen;
   bool b_txActive, b_txStarted;
   uint8_t u8_txId, u8_txChunk, u8_txWindow;
   uint32_t u32_txFrames, u32_txNext, u32_txAcked, u32_txBurst;
   int64_t i64_txLastProgress;
   int i_txDone;
   uint32_t u32_nacksRcvd;

   /* short messages from the device, per channel */
   int i_shortWrites, i_shortNotifies;
   uint8_t u8_shortType, u8_shortLen;
   uint8_t u8ar_short[256];
} Peer_T;

static Peer_T sst_peer;

/** Peer client writes a frame to the device Server's DATA characteristic. */
static void sv_PeerWrite(const uint8_t *u8pt_buf, uint16_t u16_len)
{
#if BLK_ENABLE_SERVER
   // Write Without Response: a hook error just loses the frame
   (void)gt_BLKS_DataWriteHook(&sst_conn, &sst_dataAttr, u8pt_buf, u16_len, 0U,
      BT_GATT_WRITE_FLAG_CMD);
#else
   (void)u8pt_buf; (void)u16_len;
#endif // BLK_ENABLE_SERVER
}

/** Peer server notifies a frame on its CTRL characteristic to the device Client. */
static void sv_PeerNotify(const uint8_t *u8pt_buf, uint16_t u16_len)
{
   if (sstpt_devSub != NULL)
   {
      (void)sstpt_devSub->notify(&sst_conn, sstpt_devSub, u8pt_buf, u16_len);
   }
}

static void sv_PeerCtrl3(uint8_t u8_type, uint8_t a, uint8_t b, uint8_t c)
{
   uint8_t u8ar_f[5] = { 3U, u8_type, a, b, c };
   sv_PeerNotify(u8ar_f, sizeof(u8ar_f));
}

static void sv_PeerRxFinish(void)
{
   uint8_t u8_st = (sst_peer.u32_rxCrc == sst_peer.u32_rxCrcExp) ? eBS_OK : eBS_CRC_ERROR;
   uint8_t u8ar_f[4] = { 2U, eBFT_END, sst_peer.u8_rxId, u8_st };

   sv_PeerNotify(u8ar_f, sizeof(u8ar_f));
   sst_peer.b_rxActive = false;
   sst_peer.i_rxDone = u8_st;
}

static void sv_PeerOnFrame(PduKind_E e_kind, const uint8_t *u8pt_buf, uint16_t u16_len)
{
   BlkFrame_T f;
   uint8_t u8_diff;
   uint32_t u32_abs;

   CHECK(gi_BLK_FrameParse(u8pt_buf, u16_len, &f) == 0);
   CHECK(u16_len <= (su16_mtu - 3U));

   if (f.u8_type <= BLK_APP_TYPE_MAX)
   {
      if (e_kind == ePDU_WRITE) { sst_peer.i_shortWrites++; } else { sst_peer.i_shortNotifies++; }
      sst_peer.u8_shortType = f.u8_type;
      sst_peer.u8_shortLen = f.u8_payloadLen;
      (void)memcpy(sst_peer.u8ar_short, f.u8pt_payload, f.u8_payloadLen);
      return;
   }

   // Sender frames travel as writes, receiver frames as notifications
   if (e_kind == ePDU_WRITE)
   {
      CHECK((f.u8_type == eBFT_START) || (f.u8_type == eBFT_DATA)
         || ((f.u8_type == eBFT_ABORT) && (f.u_body.st_abort.u8_dir == eBAD_BY_SENDER)));
   }
   else
   {
      CHECK((f.u8_type == eBFT_ACK) || (f.u8_type == eBFT_NACK) || (f.u8_type == eBFT_END)
         || ((f.u8_type == eBFT_ABORT) && (f.u_body.st_abort.u8_dir == eBAD_BY_RECEIVER)));
   }

   if (sst_peer.b_silent) { return; }

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
            if (!sst_peer.b_ackOnlyWhenIdle && (sst_peer.u8_sinceAck >= (sst_peer.u8_rxWindow / 2U)))
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

TEST_HELPER void sv_PeerStartSend(const uint8_t *u8pt_data, uint32_t u32_len, uint32_t u32_burst,
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

   if ((su32_linkCount > 0U) || (sstpt_pendDiscover != NULL) || (sstpt_pendSubscribe != NULL)
      || (sstpt_pendMtu != NULL))
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

/** Nothing moved during a step: a lazy peer acknowledges now. */
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

TEST_HELPER bool sb_DevTxDone(void) { return si_devTxDone != STATUS_NONE; }
TEST_HELPER bool sb_DevRxDone(void) { return si_devRxDone != STATUS_NONE; }
TEST_HELPER bool sb_PeerTxDone(void) { return sst_peer.i_txDone != STATUS_NONE; }
TEST_HELPER bool sb_BothDone(void) { return sb_DevTxDone() && sb_DevRxDone(); }
TEST_HELPER bool sb_PeerShortWrite(void) { return sst_peer.i_shortWrites > 0; }
TEST_HELPER bool sb_PeerShortNotify(void) { return sst_peer.i_shortNotifies > 0; }
TEST_HELPER bool sb_DevSrvShort(void) { return si_devSrvShortCount > 0; }
TEST_HELPER bool sb_DevCliShort(void) { return si_devCliShortCount > 0; }
TEST_HELPER bool sb_DevReady(void) { return si_devReadyCalls > 0; }

/******************************************************************************/
/*  Fixtures                                                                  */
/******************************************************************************/
static uint8_t su8ar_pattern[MAX_OBJ];

/** Connect; bind the Server and (if b_attach) attach the Client. */
static void sv_ConnectEx(uint16_t u16_mtu, bool b_attach)
{
   (void)memset(&sst_peer, 0, sizeof(sst_peer));
   sst_peer.i_rxDone = STATUS_NONE;
   sst_peer.i_txDone = STATUS_NONE;
   sst_peer.i32_dropAbsOnce = -1;
   si_devTxDone = STATUS_NONE;
   si_devRxDone = STATUS_NONE;
   su32_devRxLen = 0U;
   sb_devRxOrderError = false;
   si_devSrvShortCount = 0;
   si_devCliShortCount = 0;
   su32_devRejectAbove = MAX_OBJ;
   si_devReady = STATUS_NONE;
   si_devReadyCalls = 0;
   su32_linkHead = 0U;
   su32_linkCount = 0U;
   su32_writesQueued = 0U;
   su16_mtu = u16_mtu;
   sb_peerSubscribed = true;
   sb_connected = true;
#if BLK_ENABLE_SERVER
   gv_BLKS_OnConnected(&sst_conn);
#endif // BLK_ENABLE_SERVER
#if BLK_ENABLE_CLIENT
   if (b_attach)
   {
      CHECK(gi_BLKC_Attach(&sst_conn) == 0);
      CHECK(sb_RunUntil(sb_DevReady, 1000));
      CHECK(si_devReady == 0);
      CHECK(gb_BLKC_IsReady());
   }
#else
   (void)b_attach;
#endif // BLK_ENABLE_CLIENT
}

static void sv_Connect(uint16_t u16_mtu)
{
   sv_ConnectEx(u16_mtu, true);
}

static void sv_Disconnect(void)
{
   sb_connected = false;
   su32_linkCount = 0U;              /* queued PDUs die with the link      */
   su32_writesQueued = 0U;
   sstpt_pendDiscover = NULL;        /* outstanding ATT requests are lost  */
   sstpt_pendSubscribe = NULL;
   sstpt_pendMtu = NULL;
   // Volatile subscription: removed by the stack, which says so with NULL data
   if (sstpt_devSub != NULL)
   {
      (void)sstpt_devSub->notify(&sst_conn, sstpt_devSub, NULL, 0U);
      sstpt_devSub = NULL;
   }
   gv_BLK_OnDisconnected(&sst_conn);
   sv_Settle(5);
}

/******************************************************************************/
/*  Client (device -> peer) tests                                             */
/******************************************************************************/
#if BLK_ENABLE_CLIENT
static void sv_TestClientSizes(void)
{
   static const uint32_t su32ar_sizes[] = { 0U, 1U, 239U, 240U, 241U, 480U, 100000U };
   uint32_t i;

   printf("client -> peer server, assorted sizes (MTU 247)\n");
   for (i = 0U; i < ARRAY_SIZE(su32ar_sizes); i++)
   {
      sv_Connect(247U);
      su32_maxWrites = 0U;
      CHECK(gi_BLKC_SendBuffer(0x10U, su8ar_pattern, su32ar_sizes[i]) == 0);
      CHECK(sb_RunUntil(sb_DevTxDone, 60000));
      CHECK(si_devTxDone == eBS_OK);
      CHECK(sst_peer.i_rxDone == eBS_OK);
      CHECK(sst_peer.u32_rxTotal == su32ar_sizes[i]);
      CHECK(memcmp(sst_peer.u8ar_rx, su8ar_pattern, su32ar_sizes[i]) == 0);
      CHECK(su32_maxWrites <= BLK_CLI_WRITE_INFLIGHT_MAX);
      CHECK(sst_peer.u32_rxMaxAhead <= BLK_WINDOW_DEFAULT);
      CHECK(sst_BLKC_credits.count == BLK_CLI_WRITE_INFLIGHT_MAX);
      sv_Disconnect();
   }
}

static void sv_TestClientLoss(void)
{
   int64_t i64_start;

   printf("client -> peer server, peer drops frame 300 (after seq wrap) -> NACK\n");
   sv_Connect(247U);
   sst_peer.i32_dropAbsOnce = 300;
   i64_start = gi64_simNowMs;
   CHECK(gi_BLKC_SendBuffer(0x11U, su8ar_pattern, 100000U) == 0);
   CHECK(sb_RunUntil(sb_DevTxDone, 60000));
   printf("  done in %d ms simulated\n", (int)(gi64_simNowMs - i64_start));
   // Recovery must come from the NACK, not from the ACK timeout
   CHECK((gi64_simNowMs - i64_start) < (int64_t)BLK_TX_ACK_TIMEOUT_MS);
   CHECK(si_devTxDone == eBS_OK);
   CHECK(sst_peer.u32_nacksSent == 1U);
   CHECK(memcmp(sst_peer.u8ar_rx, su8ar_pattern, 100000U) == 0);
   sv_Disconnect();
}

static void sv_TestClientTimeout(void)
{
   int64_t i64_start;

   printf("client -> peer server, peer never answers -> TIMEOUT\n");
   sv_Connect(247U);
   sst_peer.b_silent = true;
   i64_start = gi64_simNowMs;
   CHECK(gi_BLKC_SendBuffer(0x12U, su8ar_pattern, 5000U) == 0);
   CHECK(sb_RunUntil(sb_DevTxDone, 60000));
   CHECK(si_devTxDone == eBS_TIMEOUT);
   CHECK((gi64_simNowMs - i64_start) >= (int64_t)(BLK_TX_ACK_TIMEOUT_MS * BLK_TX_MAX_RETRIES));
   CHECK(!gb_BLKC_IsTxBusy());
   sv_Disconnect();
}

static void sv_TestClientWindowStall(void)
{
   printf("peer server ACKs only when the client goes quiet -> client stops at the window\n");
   sv_Connect(247U);
   sst_peer.b_ackOnlyWhenIdle = true;
   CHECK(gi_BLKC_SendBuffer(0x40U, su8ar_pattern, 50000U) == 0);
   CHECK(sb_RunUntil(sb_DevTxDone, 60000));
   CHECK(si_devTxDone == eBS_OK);
   CHECK(sst_peer.u32_rxMaxAhead == BLK_WINDOW_DEFAULT);
   CHECK(memcmp(sst_peer.u8ar_rx, su8ar_pattern, 50000U) == 0);
   sv_Disconnect();
}

static void sv_TestClientSmallMtu(void)
{
   printf("client, MTU 23 (16-byte chunks, ~6250 frames, many seq wraps)\n");
   sv_Connect(23U);
   CHECK(gi_BLKC_SendBuffer(0x21U, su8ar_pattern, 100000U) == 0);
   CHECK(sb_RunUntil(sb_DevTxDone, 120000));
   CHECK(si_devTxDone == eBS_OK);
   CHECK(memcmp(sst_peer.u8ar_rx, su8ar_pattern, 100000U) == 0);
   CHECK(gu16_BLKC_GetMaxShortPayload() == 18U);
   CHECK(gi_BLKC_SendShort(0x05U, su8ar_pattern, 19U, K_MSEC(10)) == -EMSGSIZE);
   sv_Disconnect();
}

static void sv_TestClientAbortAndDisconnect(void)
{
   printf("client: local abort, disconnect mid-transfer, API preconditions\n");

   // Local abort: device reports ABORTED, peer server gets ABORT(by sender)
   sv_Connect(247U);
   CHECK(gi_BLKC_SendBuffer(0x30U, su8ar_pattern, 100000U) == 0);
   CHECK(gi_BLKC_SendBuffer(0x30U, su8ar_pattern, 10U) == -EBUSY);
   sv_Settle(20);
   gv_BLKC_AbortTx();
   CHECK(sb_RunUntil(sb_DevTxDone, 1000));
   CHECK(si_devTxDone == eBS_ABORTED);
   sv_Settle(10);
   CHECK(sst_peer.i_rxDone == PEER_RX_ABORT_BASE + eBS_ABORTED);
   sv_Disconnect();

   // Disconnect while writes are queued in the host: their completion
   // callbacks never run, so the engine must restore the credits itself
   sv_Connect(247U);
   CHECK(gi_BLKC_SendBuffer(0x33U, su8ar_pattern, 100000U) == 0);
   sst_BLK_wakeSem.count = 0U;
   sv_EngineRunOnce();               /* START                              */
   sv_LinkEvent();                   /* peer ACKs START                    */
   sst_BLK_wakeSem.count = 0U;
   sv_EngineRunOnce();               /* DATA burst takes the credits       */
   CHECK(su32_linkCount > 0U);
   CHECK(sst_BLKC_credits.count < BLK_CLI_WRITE_INFLIGHT_MAX);
   CHECK(gb_BLKC_IsTxBusy());
   sv_Disconnect();
   CHECK(si_devTxDone == eBS_DISCONNECTED);
   CHECK(!gb_BLKC_IsTxBusy());
   CHECK(!gb_BLKC_IsReady());
   CHECK(sst_BLKC_credits.count == BLK_CLI_WRITE_INFLIGHT_MAX);

   // Not attached / attach not finished / double attach
   CHECK(gi_BLKC_SendBuffer(0x31U, su8ar_pattern, 10U) == -ENOTCONN);
   CHECK(gi_BLKC_SendShort(0x31U, su8ar_pattern, 1U, K_NO_WAIT) == -ENOTCONN);
   sv_ConnectEx(247U, false);
   CHECK(gi_BLKC_Attach(&sst_conn) == 0);
   CHECK(gi_BLKC_Attach(&sst_conn) == -EALREADY);
   CHECK(gi_BLKC_SendBuffer(0x31U, su8ar_pattern, 10U) == -EAGAIN);
   CHECK(gu16_BLKC_GetMaxShortPayload() == 0U);
   CHECK(sb_RunUntil(sb_DevReady, 1000));
   CHECK(si_devReady == 0);

   // Reattached link works normally again
   CHECK(gi_BLKC_SendBuffer(0x32U, su8ar_pattern, 20000U) == 0);
   CHECK(sb_RunUntil(sb_DevTxDone, 60000));
   CHECK(si_devTxDone == eBS_OK);
   sv_Disconnect();
}

static void sv_TestClientAttach(void)
{
   uint8_t u8ar_f[BLK_CTRL_FRAME_MAX_LEN];

   printf("client attach: missing service / CTRL, subscribe failure, disconnect, MTU\n");

   sb_dbHasService = false;
   sv_ConnectEx(247U, false);
   CHECK(gi_BLKC_Attach(&sst_conn) == 0);
   CHECK(sb_RunUntil(sb_DevReady, 1000));
   CHECK(si_devReady == -ENOENT);
   CHECK(!gb_BLKC_IsReady());
   sb_dbHasService = true;
   sv_Disconnect();

   sb_dbHasCtrl = false;
   sv_ConnectEx(247U, false);
   CHECK(gi_BLKC_Attach(&sst_conn) == 0);
   CHECK(sb_RunUntil(sb_DevReady, 1000));
   CHECK(si_devReady == -ENOENT);
   sb_dbHasCtrl = true;
   // A failed attach releases the binding: attaching again works
   si_devReadyCalls = 0;
   CHECK(gi_BLKC_Attach(&sst_conn) == 0);
   CHECK(sb_RunUntil(sb_DevReady, 1000));
   CHECK(si_devReady == 0);
   sv_Disconnect();

   sb_subscribeFails = true;
   sv_ConnectEx(247U, false);
   CHECK(gi_BLKC_Attach(&sst_conn) == 0);
   CHECK(sb_RunUntil(sb_DevReady, 1000));
   CHECK(si_devReady == -EIO);
   sb_subscribeFails = false;
   sv_Disconnect();

   // Link lost during discovery: exactly one fpt_onReady(-ENOTCONN)
   sv_ConnectEx(247U, false);
   CHECK(gi_BLKC_Attach(&sst_conn) == 0);
   sv_Disconnect();
   sv_Settle(10);
   CHECK(si_devReadyCalls == 1);
   CHECK(si_devReady == -ENOTCONN);

   // b_autoTuneLink: MTU exchange first, discovery from its callback
   sst_BLKC_cfg.b_autoTuneLink = true;
   si_mtuExchanges = 0;
   sv_Connect(247U);
   CHECK(si_mtuExchanges == 1);
   sst_BLKC_cfg.b_autoTuneLink = false;

   // Sender frames notified on CTRL are not valid there and are dropped
   (void)gu16_BLK_EncodeStart(u8ar_f, sizeof(u8ar_f), 7U, 0x42U, 1000U, 240U, 16U, 0U);
   sv_PeerNotify(u8ar_f, BLK_CTRL_FRAME_MAX_LEN);
   sv_Settle(10);
   CHECK(si_devTxDone == STATUS_NONE);
   CHECK(sst_BLKC_slab.used == 0U);
   sv_Disconnect();
}
#endif // BLK_ENABLE_CLIENT

/******************************************************************************/
/*  Server (peer -> device) tests                                             */
/******************************************************************************/
#if BLK_ENABLE_SERVER
static void sv_TestServerSizes(void)
{
   static const uint32_t su32ar_sizes[] = { 0U, 1U, 240U, 241U, 100000U };
   uint32_t i;

   printf("peer client -> server, assorted sizes, burst 4\n");
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
      CHECK(sst_BLKS_slab.used == 0U);
      CHECK(sst_BLKS_credits.count == BLK_SRV_NOTIFY_INFLIGHT_MAX);
      sv_Disconnect();
   }
}

static void sv_TestServerOverflow(void)
{
   int64_t i64_start;

   printf("peer client -> server, bursts of 16 into a %u-deep RX pool -> overflow NACKs\n",
      (unsigned)BLK_RX_POOL_DEPTH);
   sv_Connect(247U);
   i64_start = gi64_simNowMs;
   sv_PeerStartSend(su8ar_pattern, 100000U, 16U, false);
   CHECK(sb_RunUntil(sb_PeerTxDone, 120000));
   // Recovery must come from NACKs, not from the peer's 1500 ms timeout
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

static void sv_TestServerCrcAndReject(void)
{
   printf("server: bad CRC -> CRC_ERROR; oversize -> REJECTED; unsubscribed -> ignored\n");
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

   // START from a client that cannot hear CTRL is not even offered to the app
   su32_devRejectAbove = MAX_OBJ;
   si_devRxStartCalls = 0;
   sb_peerSubscribed = false;
   sv_PeerStartSend(su8ar_pattern, 3000U, 4U, false);
   sv_Settle(20);
   CHECK(si_devRxStartCalls == 0);
   CHECK(!gb_BLKS_IsRxBusy());
   CHECK(sst_peer.i_txDone == STATUS_NONE);
   sv_Disconnect();
}

static void sv_TestServerAbortAndStale(void)
{
   uint8_t u8ar_f[BLK_CTRL_FRAME_MAX_LEN];

   printf("server: local abort, disconnect mid-transfer, stale frames, wrong channel\n");

   // Local abort of RX: peer client gets ABORT(by receiver)
   sv_Connect(247U);
   sv_PeerStartSend(su8ar_pattern, 100000U, 4U, false);
   sv_Settle(20);
   CHECK(gb_BLKS_IsRxBusy());
   gv_BLKS_AbortRx();
   CHECK(sb_RunUntil(sb_PeerTxDone, 1000));
   CHECK(sst_peer.i_txDone == PEER_ABORT_BASE + eBS_ABORTED);
   CHECK(si_devRxDone == eBS_ABORTED);
   sv_Disconnect();

   // Disconnect mid-transfer
   sv_Connect(247U);
   sv_PeerStartSend(su8ar_pattern, 100000U, 4U, false);
   sv_Settle(30);
   sv_Disconnect();
   CHECK(si_devRxDone == eBS_DISCONNECTED);
   CHECK(!gb_BLKS_IsRxBusy());
   CHECK(sst_BLKS_credits.count == BLK_SRV_NOTIFY_INFLIGHT_MAX);

   // Frames queued before a disconnect are discarded after reconnect
   sv_Connect(247U);
   si_devRxStartCalls = 0;
   (void)gu16_BLK_EncodeStart(u8ar_f, sizeof(u8ar_f), 99U, 0x42U, 1000U, 240U, 16U, 0U);
   sv_PeerWrite(u8ar_f, BLK_CTRL_FRAME_MAX_LEN);    /* queued, engine not run */
   sb_connected = false;
   gv_BLK_OnDisconnected(&sst_conn);
   sb_connected = true;
   gv_BLKS_OnConnected(&sst_conn);
   sv_Settle(20);
   CHECK(si_devRxStartCalls == 0);
   CHECK(!sst_BLKS_session.b_active);
   CHECK(sst_BLKS_slab.used == 0U);

   // Receiver frames written to DATA are not valid there and are dropped
   u8ar_f[0] = 3U; u8ar_f[1] = eBFT_ACK; u8ar_f[2] = 1U; u8ar_f[3] = 0U; u8ar_f[4] = 16U;
   sv_PeerWrite(u8ar_f, 5U);
   sv_Settle(10);
   CHECK(sst_BLKS_slab.used == 0U);
   sv_Disconnect();
}

static void sv_TestServerMalformedData(void)
{
   uint8_t u8ar_f[BLK_MAX_FRAME_LEN];

   printf("server: DATA chunk shorter than announced -> PROTOCOL_ERROR; bad hook writes\n");
   sv_Connect(247U);
   sv_PeerStartSend(su8ar_pattern, 1000U, 0U, false);  /* burst 0: manual */
   sv_Settle(5);
   CHECK(sst_peer.b_txStarted);
   (void)gu16_BLK_EncodeDataHeader(u8ar_f, sizeof(u8ar_f), sst_peer.u8_txId, 0U, 10U);
   sv_PeerWrite(u8ar_f, BLK_DATA_HDR_LEN + 10U);
   CHECK(sb_RunUntil(sb_PeerTxDone, 1000));
   CHECK(sst_peer.i_txDone == PEER_ABORT_BASE + eBS_PROTOCOL_ERROR);
   CHECK(si_devRxDone == eBS_PROTOCOL_ERROR);

   // Malformed writes are rejected by the hook with an ATT error
   u8ar_f[0] = 50U;
   CHECK(gt_BLKS_DataWriteHook(&sst_conn, &sst_dataAttr, u8ar_f, 10U, 0U, 0U)
      == BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN));
   CHECK(gt_BLKS_DataWriteHook(&sst_conn, &sst_dataAttr, u8ar_f, 10U, 5U, 0U)
      == BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET));
   CHECK(gt_BLKS_DataWriteHook(&sst_conn, &sst_dataAttr, u8ar_f, 10U, 0U, BT_GATT_WRITE_FLAG_PREPARE)
      == BT_GATT_ERR(BT_ATT_ERR_WRITE_NOT_PERMITTED));
   sv_Disconnect();
}
#endif // BLK_ENABLE_SERVER

/******************************************************************************/
/*  Both roles                                                                */
/******************************************************************************/
static void sv_TestShortMessages(void)
{
   uint8_t u8ar_f[BLK_MAX_FRAME_LEN];
   uint16_t u16_len;

   printf("short messages on both channels, size limits\n");
   sv_Connect(247U);
   (void)u8ar_f; (void)u16_len;

#if BLK_ENABLE_CLIENT
   // Device client -> peer server: Write Without Response on DATA
   CHECK(gu16_BLKC_GetMaxShortPayload() == BLK_MAX_SHORT_PAYLOAD);
   CHECK(gi_BLKC_SendShort(0x05U, su8ar_pattern, 242U, K_MSEC(10)) == 0);
   CHECK(sb_RunUntil(sb_PeerShortWrite, 1000));
   CHECK(sst_peer.u8_shortType == 0x05U && sst_peer.u8_shortLen == 242U);
   CHECK(memcmp(sst_peer.u8ar_short, su8ar_pattern, 242U) == 0);
   CHECK(gi_BLKC_SendShort(0x05U, su8ar_pattern, 243U, K_MSEC(10)) == -EMSGSIZE);
   CHECK(gi_BLKC_SendShort(eBFT_START, su8ar_pattern, 1U, K_MSEC(10)) == -EINVAL);

   // Peer server -> device client: CTRL notification
   u16_len = gu16_BLK_EncodeShort(u8ar_f, sizeof(u8ar_f), 0x08U, su8ar_pattern, 60U);
   sv_PeerNotify(u8ar_f, u16_len);
   CHECK(sb_RunUntil(sb_DevCliShort, 1000));
   CHECK(su8_devShortType == 0x08U && su8_devShortLen == 60U);
#endif // BLK_ENABLE_CLIENT

#if BLK_ENABLE_SERVER
   // Device server -> peer client: CTRL notification
   CHECK(gu16_BLKS_GetMaxShortPayload() == BLK_MAX_SHORT_PAYLOAD);
   CHECK(gi_BLKS_SendShort(0x06U, su8ar_pattern, 100U, K_MSEC(10)) == 0);
   CHECK(sb_RunUntil(sb_PeerShortNotify, 1000));
   CHECK(sst_peer.u8_shortType == 0x06U && sst_peer.u8_shortLen == 100U);
   sb_peerSubscribed = false;
   CHECK(gi_BLKS_SendShort(0x06U, su8ar_pattern, 1U, K_MSEC(10)) == -EACCES);
   sb_peerSubscribed = true;

   // Peer client -> device server: write on DATA
   u16_len = gu16_BLK_EncodeShort(u8ar_f, sizeof(u8ar_f), 0x07U, su8ar_pattern, 100U);
   sv_PeerWrite(u8ar_f, u16_len);
   CHECK(sb_RunUntil(sb_DevSrvShort, 1000));
   CHECK(su8_devShortType == 0x07U && su8_devShortLen == 100U);
#endif // BLK_ENABLE_SERVER
   sv_Disconnect();
}

#if BLK_ENABLE_SERVER && BLK_ENABLE_CLIENT
static void sv_TestBidirectional(void)
{
   printf("both roles on one link: simultaneous transfers in both directions\n");
   sv_Connect(247U);
   CHECK(gi_BLKC_SendBuffer(0x20U, su8ar_pattern, 60000U) == 0);
   sv_PeerStartSend(&su8ar_pattern[1000], 50000U, 4U, false);
   CHECK(sb_RunUntil(sb_BothDone, 60000));
   CHECK(si_devTxDone == eBS_OK);
   CHECK(si_devRxDone == eBS_OK);
   CHECK(memcmp(sst_peer.u8ar_rx, su8ar_pattern, 60000U) == 0);
   CHECK(memcmp(su8ar_devRx, &su8ar_pattern[1000], 50000U) == 0);

   // Disconnect with transfers running in both directions
   si_devTxDone = STATUS_NONE;
   si_devRxDone = STATUS_NONE;
   CHECK(gi_BLKC_SendBuffer(0x31U, su8ar_pattern, 100000U) == 0);
   sv_PeerStartSend(su8ar_pattern, 100000U, 4U, false);
   sv_Settle(30);
   sv_Disconnect();
   CHECK(si_devTxDone == eBS_DISCONNECTED);
   CHECK(si_devRxDone == eBS_DISCONNECTED);
}
#endif // BLK_ENABLE_SERVER && BLK_ENABLE_CLIENT

int main(int argc, char **argv)
{
   uint32_t i;

   gb_simVerbose = (argc > 1) && (strcmp(argv[1], "-v") == 0);

   for (i = 0U; i < MAX_OBJ; i++)
   {
      su8ar_pattern[i] = (uint8_t)((i * 31U) ^ (i >> 8));
   }

#if BLK_ENABLE_SERVER
   {
      BlkSrvCfg_T st_srv = { 0 };

      CHECK(gi_BLKS_Init(&st_srv) == -EINVAL);
      st_srv.stpt_ctrlAttr = &sst_ctrlAttr;
      st_srv.fpt_onRxStart = si_DevRxStart;
      st_srv.fpt_onRxData = si_DevRxData;
      st_srv.fpt_onRxDone = sv_DevRxDone;
      st_srv.fpt_onRxShort = sv_DevSrvShort;
      CHECK(gi_BLKS_Init(&st_srv) == 0);
      CHECK(gi_BLKS_Init(&st_srv) == -EALREADY);
   }
#endif // BLK_ENABLE_SERVER
#if BLK_ENABLE_CLIENT
   {
      BlkCliCfg_T st_cli = { 0 };

      CHECK(gi_BLKC_Attach(&sst_conn) == -EPERM);
      st_cli.fpt_onReady = sv_DevReady;
      st_cli.fpt_onTxDone = sv_DevTxDone;
      st_cli.fpt_onRxShort = sv_DevCliShort;
      CHECK(gi_BLKC_Init(&st_cli) == 0);
      CHECK(gi_BLKC_Init(&st_cli) == -EALREADY);
   }
#endif // BLK_ENABLE_CLIENT

   printf("roles: server %d, client %d\n", BLK_ENABLE_SERVER, BLK_ENABLE_CLIENT);

#if BLK_ENABLE_CLIENT
   sv_TestClientSizes();
   sv_TestClientLoss();
   sv_TestClientTimeout();
   sv_TestClientWindowStall();
   sv_TestClientSmallMtu();
   sv_TestClientAbortAndDisconnect();
   sv_TestClientAttach();
#endif // BLK_ENABLE_CLIENT
#if BLK_ENABLE_SERVER
   sv_TestServerSizes();
   sv_TestServerOverflow();
   sv_TestServerCrcAndReject();
   sv_TestServerAbortAndStale();
   sv_TestServerMalformedData();
#endif // BLK_ENABLE_SERVER
   sv_TestShortMessages();
#if BLK_ENABLE_SERVER && BLK_ENABLE_CLIENT
   sv_TestBidirectional();
#endif // BLK_ENABLE_SERVER && BLK_ENABLE_CLIENT

   if (si_failures == 0)
   {
      printf("All BulkXfer engine tests passed\n");
      return 0;
   }

   printf("%d check(s) failed\n", si_failures);
   return 1;
}
