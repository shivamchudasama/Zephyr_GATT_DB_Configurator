/**
 * @file          BulkXfer_Core.c
 * @brief         Engine core of the BLE bulk transfer (BulkXfer) framework,
 *                shared by the Server (RX) and Client (TX) roles.
 *
 *                Execution contexts:
 *                  - BLE RX thread : the Server DATA write hook and the Client
 *                                    CTRL notify callback only validate a
 *                                    frame, copy it into a slab block and
 *                                    queue it. They never block.
 *                  - BLE TX context: write / notify completions return credits.
 *                  - Timer ISR     : timer expiry sets an event bit.
 *                  - Engine thread : everything else. All session state is
 *                                    owned by this thread under gst_BLK_lock.
 *                Every producer "kicks" the engine through sst_BLK_wakeSem; the
 *                engine drains all pending work on each wake, so coalesced
 *                kicks never lose work.
 *
 * @date          24/09/2026
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
#include <zephyr/sys/util.h>
#include "BulkXfer.h"
#include "BulkXfer_Core_Priv.h"
#include "AppLog.h"

/******************************************************************************/
/*                                                                            */
/*                                  DEFINES                                   */
/*                                                                            */
/******************************************************************************/
#if defined(CONFIG_BT_ATT_TX_COUNT)
BUILD_ASSERT(((BLK_ENABLE_SERVER ? BLK_SRV_NOTIFY_INFLIGHT_MAX : 0U)
   + (BLK_ENABLE_CLIENT ? BLK_CLI_WRITE_INFLIGHT_MAX : 0U)) < CONFIG_BT_ATT_TX_COUNT,
   "BulkXfer in-flight credits must stay below CONFIG_BT_ATT_TX_COUNT");
#endif // CONFIG_BT_ATT_TX_COUNT

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
static void sv_EngineRunOnce(void);
static void sv_EngineThread(void *vpt_p1, void *vpt_p2, void *vpt_p3);

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
/**
 * @var           gt_blkThread
 * @brief         Engine thread; started by the first role init. K_THREAD_DEFINE
 *                gives the thread ID external linkage.
 */
K_THREAD_DEFINE(gt_blkThread, BLK_THREAD_STACK_SIZE, sv_EngineThread, NULL, NULL,
   NULL, BLK_THREAD_PRIORITY, 0, SYS_FOREVER_MS);

/**
 * @var           gst_BLK_lock
 * @brief         Protects all role state. Recursive, so application callbacks
 *                may call the public API.
 */
K_MUTEX_DEFINE(gst_BLK_lock);

/**
 * @var           gt_BLK_events
 * @brief         Pending BlkEvent_E bits.
 */
atomic_t gt_BLK_events = ATOMIC_INIT(0);

/******************************************************************************/
/*                                                                            */
/*                             PRIVATE VARIABLES                              */
/*                                                                            */
/******************************************************************************/
/**
 * @var           sst_BLK_wakeSem
 * @brief         Binary "work available" signal for the engine thread.
 */
static K_SEM_DEFINE(sst_BLK_wakeSem, 0, 1);

/**
 * @var           sb_BLK_engineStarted
 * @brief         Set once the engine thread has been started.
 */
static bool sb_BLK_engineStarted = false;

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
 * @private       sv_EngineRunOnce
 * @brief         One engine pass under gst_BLK_lock. Separated from the thread
 *                loop so that host tests can drive the engine step by step.
 * @return        void
 */
static void sv_EngineRunOnce(void)
{
   (void)k_mutex_lock(&gst_BLK_lock, K_FOREVER);

   // Pre: owed completions, events, then queued frames. Post: work that
   // depends on the frames just processed (overflow NACK, TX pump).
#if BLK_ENABLE_SERVER
   gv_BLKS_EnginePre();
#endif // BLK_ENABLE_SERVER
#if BLK_ENABLE_CLIENT
   gv_BLKC_EnginePre();
#endif // BLK_ENABLE_CLIENT
#if BLK_ENABLE_SERVER
   gv_BLKS_EnginePost();
#endif // BLK_ENABLE_SERVER
#if BLK_ENABLE_CLIENT
   gv_BLKC_EnginePost();
#endif // BLK_ENABLE_CLIENT

   k_mutex_unlock(&gst_BLK_lock);
}

/**
 * @private       sv_EngineThread
 * @brief         Engine main loop: wait for a kick, then run one pass.
 * @param[in]     vpt_p1 Unused.
 * @param[in]     vpt_p2 Unused.
 * @param[in]     vpt_p3 Unused.
 * @return        void
 */
static void sv_EngineThread(void *vpt_p1, void *vpt_p2, void *vpt_p3)
{
   ARG_UNUSED(vpt_p1);
   ARG_UNUSED(vpt_p2);
   ARG_UNUSED(vpt_p3);

   for (;;)
   {
      // Binary semaphore: kicks coalesce, which is safe because every pass
      // drains all pending work
      (void)k_sem_take(&sst_BLK_wakeSem, K_FOREVER);
      sv_EngineRunOnce();
   }
}

/******************************************************************************/
/*                                                                            */
/*                        PUBLIC FUNCTION DEFINITIONS                         */
/*                                                                            */
/******************************************************************************/
/**
 * @public        gv_BLK_EngineStart
 * @brief         Start the engine thread once (idempotent). Called by the role
 *                inits after their state is set up.
 * @return        void
 */
void gv_BLK_EngineStart(void)
{
   bool b_start = false;

   (void)k_mutex_lock(&gst_BLK_lock, K_FOREVER);
   b_start = !sb_BLK_engineStarted;
   sb_BLK_engineStarted = true;
   k_mutex_unlock(&gst_BLK_lock);

   // Check if this is the first role to start
   if (b_start)
   {
      k_thread_start(gt_blkThread);
   }
}

/**
 * @public        gv_BLK_Kick
 * @brief         Wake the engine thread (coalescing). Safe from any context.
 * @return        void
 */
void gv_BLK_Kick(void)
{
   k_sem_give(&sst_BLK_wakeSem);
}

/**
 * @public        gv_BLK_TuneLink
 * @brief         Request throughput-oriented link settings: 2M PHY and
 *                maximum data length, where the Kconfig allows it. May block
 *                in the BT stack; call without gst_BLK_lock.
 * @param[in]     stpt_conn Connection.
 * @return        void
 */
void gv_BLK_TuneLink(struct bt_conn *stpt_conn)
{
   ARG_UNUSED(stpt_conn);

#if defined(CONFIG_BT_USER_PHY_UPDATE)
   (void)bt_conn_le_phy_update(stpt_conn, BT_CONN_LE_PHY_PARAM_2M);
#endif // CONFIG_BT_USER_PHY_UPDATE
#if defined(CONFIG_BT_USER_DATA_LEN_UPDATE)
   (void)bt_conn_le_data_len_update(stpt_conn, BT_LE_DATA_LEN_PARAM_MAX);
#endif // CONFIG_BT_USER_DATA_LEN_UPDATE
}

/**
 * @public        gb_BLK_FrameLenValid
 * @brief         Cheap check done in BLE RX context before queueing a frame:
 *                header present, not oversized, len byte matches.
 * @param[in]     u8pt_buf Frame.
 * @param[in]     u16_len Frame length.
 * @return        true if the frame may be queued.
 */
bool gb_BLK_FrameLenValid(const uint8_t *u8pt_buf, uint16_t u16_len)
{
   return (u8pt_buf != NULL) && (u16_len >= BLK_FRAME_HDR_LEN)
      && (u16_len <= BLK_MAX_FRAME_LEN)
      && (((uint16_t)u8pt_buf[0] + BLK_FRAME_HDR_LEN) == u16_len);
}

/**
 * @public        gu16_BLK_FrameCapacity
 * @brief         Largest frame usable on the link: min(ATT_MTU - 3, max).
 *                Write Without Response and notifications share the 3-byte
 *                ATT header (opcode + handle).
 * @param[in]     stpt_conn Connection.
 * @return        Frame capacity in bytes.
 */
uint16_t gu16_BLK_FrameCapacity(struct bt_conn *stpt_conn)
{
   uint16_t u16_mtu = bt_gatt_get_mtu(stpt_conn);
   uint16_t u16_cap = (u16_mtu > 3U) ? (uint16_t)(u16_mtu - 3U) : 0U;

   return MIN(u16_cap, (uint16_t)BLK_MAX_FRAME_LEN);
}

/**
 * @public        gu32_BLK_FrameCount
 * @brief         Number of DATA frames needed for u32_totalLen bytes.
 * @param[in]     u32_totalLen Object size.
 * @param[in]     u8_chunkSize Bytes per frame (non-zero).
 * @return        ceil(u32_totalLen / u8_chunkSize), overflow-safe.
 */
uint32_t gu32_BLK_FrameCount(uint32_t u32_totalLen, uint8_t u8_chunkSize)
{
   // Avoids the overflow of (len + chunk - 1) / chunk near UINT32_MAX
   return (u32_totalLen / u8_chunkSize) + (((u32_totalLen % u8_chunkSize) != 0U) ? 1U : 0U);
}

/**
 * @public        gu32_BLK_SeqToAbs
 * @brief         Expand an 8-bit wire sequence number to an absolute frame
 *                index at or after u32_base (window <= 128 keeps this unique).
 * @param[in]     u8_seq Wire sequence number.
 * @param[in]     u32_base Reference absolute index.
 * @return        Absolute frame index.
 */
uint32_t gu32_BLK_SeqToAbs(uint8_t u8_seq, uint32_t u32_base)
{
   // 8-bit forward distance from base to seq. A seq behind base maps ~256
   // frames ahead; callers reject it with their range checks.
   return u32_base + (uint8_t)(u8_seq - (uint8_t)u32_base);
}

/**
 * @public        gv_BLK_OnDisconnected
 * @brief         Forward a disconnect to every enabled role. Each role ignores
 *                connections it is not bound to. Call from the application's
 *                bt_conn_cb.disconnected callback.
 * @param[in]     stpt_conn Connection that went down.
 * @return        void
 */
void gv_BLK_OnDisconnected(struct bt_conn *stpt_conn)
{
#if BLK_ENABLE_SERVER
   gv_BLKS_OnDisconnected(stpt_conn);
#endif // BLK_ENABLE_SERVER
#if BLK_ENABLE_CLIENT
   gv_BLKC_OnDisconnected(stpt_conn);
#endif // BLK_ENABLE_CLIENT
}
