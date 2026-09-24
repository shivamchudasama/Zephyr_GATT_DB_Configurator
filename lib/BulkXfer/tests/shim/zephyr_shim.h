/**
 * @file          zephyr_shim.h
 * @brief         Minimal, single-threaded stand-in for the Zephyr kernel and
 *                Bluetooth APIs used by the BulkXfer core, Server and Client,
 *                so that the real engine can be exercised on a host PC by
 *                test_engine.c.
 *
 *                - Time is simulated (gi64_simNowMs); timers fire only when
 *                  the test calls gv_SimFireTimers().
 *                - A k_sem_take() that would block calls gv_SimOnBlock() so the
 *                  simulated link can complete notifications and free credits.
 *                - bt_gatt_notify_cb, bt_gatt_write_without_response_cb,
 *                  bt_gatt_discover, bt_gatt_subscribe, bt_gatt_exchange_mtu,
 *                  bt_gatt_is_subscribed and bt_gatt_get_mtu are implemented
 *                  by the test (simulated link and peer).
 *
 * @date          22/09/2026
 * @author        Shivam Chudasama
 * @copyright     Shivam Chudasama
 * @license       MIT
 */

/* SPDX-License-Identifier: MIT */

#ifndef _ZEPHYR_SHIM_H
#define _ZEPHYR_SHIM_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>

/* ---- errno values missing from some host C libraries (old MinGW) -------- */
#ifndef ENOTCONN
#define ENOTCONN              128
#endif // ENOTCONN
#ifndef EMSGSIZE
#define EMSGSIZE              122
#endif // EMSGSIZE
#ifndef EALREADY
#define EALREADY              120
#endif // EALREADY

/* ---- Toolchain / util ---------------------------------------------------- */
#define ARG_UNUSED(x)         (void)(x)
#define __packed              __attribute__((packed))
#define __ASSERT(c, msg)      do { if (!(c)) { printf("ASSERT: %s\n", msg); abort(); } } while (0)
#ifndef MIN
#define MIN(a, b)             (((a) < (b)) ? (a) : (b))
#endif // MIN
#ifndef MAX
#define MAX(a, b)             (((a) > (b)) ? (a) : (b))
#endif // MAX
#define CLAMP(v, lo, hi)      MIN(MAX((v), (lo)), (hi))
#define ARRAY_SIZE(a)         (sizeof(a) / sizeof((a)[0]))
/* C89-compatible stand-in for Zephyr's BUILD_ASSERT (no _Static_assert). */
#define SHIM_CAT_(a, b)       a##b
#define SHIM_CAT(a, b)        SHIM_CAT_(a, b)
#define BUILD_ASSERT(c, ...)  typedef char SHIM_CAT(shim_build_assert_, __LINE__)[(c) ? 1 : -1]

/* ---- Time ---------------------------------------------------------------- */
typedef struct { int64_t ms; } k_timeout_t;
#define K_NO_WAIT             ((k_timeout_t){ 0 })
#define K_FOREVER             ((k_timeout_t){ -1 })
#define K_MSEC(x)             ((k_timeout_t){ (x) })
#define SYS_FOREVER_MS        (-1)

extern int64_t gi64_simNowMs;
static inline int64_t k_uptime_get(void) { return gi64_simNowMs; }

/* ---- Logging (AppLog.h) -------------------------------------------------- */
extern bool gb_simVerbose;
#define SIM_LOG(lvl, fmt, ...) \
   do { if (gb_simVerbose) printf("[%7d] " lvl " %s: " fmt "\n", \
      (int)gi64_simNowMs, __func__, ##__VA_ARGS__); } while (0)
#define APP_LOG_ERR(fmt, ...) SIM_LOG("ERR", fmt, ##__VA_ARGS__)
#define APP_LOG_WRN(fmt, ...) SIM_LOG("WRN", fmt, ##__VA_ARGS__)
#define APP_LOG_INF(fmt, ...) SIM_LOG("INF", fmt, ##__VA_ARGS__)
#define APP_LOG_DBG(fmt, ...) SIM_LOG("DBG", fmt, ##__VA_ARGS__)

/* ---- Mutex (single-threaded: no-op) -------------------------------------- */
struct k_mutex { int i_unused; };
#define K_MUTEX_DEFINE(name)  struct k_mutex name = { 0 }
static inline int k_mutex_lock(struct k_mutex *m, k_timeout_t t) { (void)m; (void)t; return 0; }
static inline int k_mutex_unlock(struct k_mutex *m) { (void)m; return 0; }

/* ---- Semaphore ----------------------------------------------------------- */
struct k_sem { unsigned int count; unsigned int limit; };
#define K_SEM_DEFINE(name, init, lim) struct k_sem name = { (init), (lim) }
extern void gv_SimOnBlock(struct k_sem *stpt_sem);
static inline int k_sem_take(struct k_sem *s, k_timeout_t t)
{
   // A blocking take lets the simulated link make progress once
   if ((s->count == 0U) && (t.ms != 0))
   {
      gv_SimOnBlock(s);
   }
   if (s->count == 0U)
   {
      return -EBUSY;
   }
   s->count--;
   return 0;
}
static inline void k_sem_give(struct k_sem *s) { if (s->count < s->limit) { s->count++; } }
static inline void k_sem_reset(struct k_sem *s) { s->count = 0U; }

/* ---- FIFO ---------------------------------------------------------------- */
struct k_fifo { void *head; void *tail; };
#define K_FIFO_DEFINE(name)   struct k_fifo name = { NULL, NULL }
static inline void k_fifo_put(struct k_fifo *f, void *item)
{
   *(void **)item = NULL;
   if (f->tail != NULL) { *(void **)f->tail = item; } else { f->head = item; }
   f->tail = item;
}
static inline void *k_fifo_get(struct k_fifo *f, k_timeout_t t)
{
   void *item = f->head;
   (void)t;
   if (item != NULL)
   {
      f->head = *(void **)item;
      if (f->head == NULL) { f->tail = NULL; }
   }
   return item;
}

/* ---- Memory slab --------------------------------------------------------- */
struct k_mem_slab { size_t block; uint32_t num; uint8_t *buf; void *free; bool init; uint32_t used; };
#define K_MEM_SLAB_DEFINE_STATIC(name, bs, n, al) \
   static uint8_t name##_buf[(bs) * (n)] __attribute__((aligned(al))); \
   static struct k_mem_slab name = { (bs), (n), name##_buf, NULL, false, 0U }
static inline int k_mem_slab_alloc(struct k_mem_slab *s, void **mem, k_timeout_t t)
{
   uint32_t i;
   (void)t;
   if (!s->init)
   {
      for (i = 0U; i < s->num; i++) { void *b = &s->buf[i * s->block]; *(void **)b = s->free; s->free = b; }
      s->init = true;
   }
   if (s->free == NULL) { *mem = NULL; return -ENOMEM; }
   *mem = s->free;
   s->free = *(void **)s->free;
   s->used++;
   return 0;
}
static inline void k_mem_slab_free(struct k_mem_slab *s, void *mem)
{
   *(void **)mem = s->free;
   s->free = mem;
   s->used--;
}

/* ---- Timer --------------------------------------------------------------- */
struct k_timer { void (*expiry)(struct k_timer *); int64_t deadline; bool running; };
#define K_TIMER_DEFINE(name, exp, stop) struct k_timer name = { (exp), 0, false }
extern void gv_SimRegisterTimer(struct k_timer *t);
static inline void k_timer_start(struct k_timer *t, k_timeout_t d, k_timeout_t p)
{
   (void)p;
   gv_SimRegisterTimer(t);
   t->deadline = gi64_simNowMs + d.ms;
   t->running = true;
}
static inline void k_timer_stop(struct k_timer *t) { t->running = false; }
static inline uint32_t k_timer_remaining_get(struct k_timer *t)
{
   return (t->running && (t->deadline > gi64_simNowMs)) ? (uint32_t)(t->deadline - gi64_simNowMs) : 0U;
}

/* ---- Thread -------------------------------------------------------------- */
#define K_THREAD_DEFINE(name, ss, entry, p1, p2, p3, prio, opt, delay) \
   const int name = 0; \
   void (*const name##_entry)(void *, void *, void *) = (entry)
#define k_thread_start(t)     ((void)(t))

/* ---- Atomics (single-threaded) ------------------------------------------- */
typedef long atomic_t;
typedef long atomic_val_t;
typedef void *atomic_ptr_t;
#define ATOMIC_INIT(v)        (v)
#define ATOMIC_PTR_INIT(p)    (p)
static inline void atomic_set_bit(atomic_t *a, int b) { *a |= (1L << b); }
static inline bool atomic_test_and_clear_bit(atomic_t *a, int b)
{
   bool r = (*a & (1L << b)) != 0;
   *a &= ~(1L << b);
   return r;
}
static inline void atomic_clear_bit(atomic_t *a, int b) { *a &= ~(1L << b); }
static inline atomic_val_t atomic_get(const atomic_t *a) { return *a; }
static inline atomic_val_t atomic_set(atomic_t *a, atomic_val_t v) { atomic_val_t o = *a; *a = v; return o; }
static inline atomic_val_t atomic_inc(atomic_t *a) { return (*a)++; }
static inline atomic_val_t atomic_clear(atomic_t *a) { atomic_val_t o = *a; *a = 0; return o; }
static inline void *atomic_ptr_get(const atomic_ptr_t *p) { return *p; }
static inline void *atomic_ptr_set(atomic_ptr_t *p, void *v) { void *o = *p; *p = v; return o; }

/* ---- CRC ----------------------------------------------------------------- */
static inline uint32_t crc32_ieee_update(uint32_t crc, const uint8_t *data, size_t len)
{
   size_t i;
   int k;
   crc = ~crc;
   for (i = 0; i < len; i++)
   {
      crc ^= data[i];
      for (k = 0; k < 8; k++) { crc = (crc >> 1) ^ (0xEDB88320U & (0U - (crc & 1U))); }
   }
   return ~crc;
}

/* ---- Bluetooth: UUIDs --------------------------------------------------- */
#define BT_UUID_TYPE_16                    0
#define BT_UUID_TYPE_128                   2
struct bt_uuid { uint8_t type; };
struct bt_uuid_16 { struct bt_uuid uuid; uint16_t val; };
struct bt_uuid_128 { struct bt_uuid uuid; uint8_t val[16]; };
#define BT_UUID_128_ENCODE(w32, w1, w2, w3, w48)    (uint8_t)(w48), (uint8_t)((w48) >> 8), (uint8_t)((w48) >> 16), (uint8_t)((w48) >> 24),    (uint8_t)((w48) >> 32), (uint8_t)((w48) >> 40), (uint8_t)(w3), (uint8_t)((w3) >> 8),    (uint8_t)(w2), (uint8_t)((w2) >> 8), (uint8_t)(w1), (uint8_t)((w1) >> 8),    (uint8_t)(w32), (uint8_t)((w32) >> 8), (uint8_t)((w32) >> 16), (uint8_t)((w32) >> 24)
#define BT_UUID_INIT_128(...)              { { BT_UUID_TYPE_128 }, { __VA_ARGS__ } }
#define BT_UUID_DECLARE_16(v)    ((const struct bt_uuid *)(&(const struct bt_uuid_16){ { BT_UUID_TYPE_16 }, (v) }))
#define BT_UUID_DECLARE_128(...)    ((const struct bt_uuid *)(&(const struct bt_uuid_128){ { BT_UUID_TYPE_128 }, { __VA_ARGS__ } }))
#define BT_UUID_GATT_CCC                   BT_UUID_DECLARE_16(0x2902)
static inline int bt_uuid_cmp(const struct bt_uuid *u1, const struct bt_uuid *u2)
{
   if (u1->type != u2->type) { return (int)u1->type - (int)u2->type; }
   if (u1->type == BT_UUID_TYPE_16)
   {
      return (int)((const struct bt_uuid_16 *)u1)->val - (int)((const struct bt_uuid_16 *)u2)->val;
   }
   return memcmp(((const struct bt_uuid_128 *)u1)->val, ((const struct bt_uuid_128 *)u2)->val, 16);
}

/* ---- Bluetooth: connection and GATT server ------------------------------- */
struct bt_conn { int i_id; };
struct bt_gatt_attr { const void *uuid; void *user_data; uint16_t handle; };
typedef void (*bt_gatt_complete_func_t)(struct bt_conn *conn, void *user_data);
struct bt_gatt_notify_params
{
   const void *uuid;
   const struct bt_gatt_attr *attr;
   const void *data;
   uint16_t len;
   bt_gatt_complete_func_t func;
   void *user_data;
};
#define BT_GATT_CCC_NOTIFY                 0x0001
#define BT_GATT_WRITE_FLAG_PREPARE         0x01
#define BT_GATT_WRITE_FLAG_CMD             0x02
#define BT_GATT_ERR(e)                     (-(e))
#define BT_ATT_ERR_WRITE_NOT_PERMITTED     0x03
#define BT_ATT_ERR_INVALID_OFFSET          0x07
#define BT_ATT_ERR_INVALID_ATTRIBUTE_LEN   0x0d
#define BT_ATT_ERR_INSUFFICIENT_RESOURCES  0x11
static inline struct bt_conn *bt_conn_ref(struct bt_conn *c) { return c; }
static inline void bt_conn_unref(struct bt_conn *c) { (void)c; }
extern int bt_gatt_notify_cb(struct bt_conn *conn, struct bt_gatt_notify_params *params);
extern bool bt_gatt_is_subscribed(struct bt_conn *conn, const struct bt_gatt_attr *attr,
   uint16_t ccc_type);
extern uint16_t bt_gatt_get_mtu(struct bt_conn *conn);

/* ---- Bluetooth: GATT client ---------------------------------------------- */
#define BT_ATT_FIRST_ATTRIBUTE_HANDLE      0x0001
#define BT_ATT_LAST_ATTRIBUTE_HANDLE       0xffff
#define BT_GATT_ITER_STOP                  0
#define BT_GATT_ITER_CONTINUE              1
#define BT_GATT_CHRC_WRITE_WITHOUT_RESP    0x04
#define BT_GATT_CHRC_WRITE                 0x08
#define BT_GATT_CHRC_NOTIFY                0x10
#define BT_GATT_SUBSCRIBE_FLAG_VOLATILE    0
enum
{
   BT_GATT_DISCOVER_PRIMARY,
   BT_GATT_DISCOVER_SECONDARY,
   BT_GATT_DISCOVER_INCLUDE,
   BT_GATT_DISCOVER_CHARACTERISTIC,
   BT_GATT_DISCOVER_DESCRIPTOR,
};
struct bt_gatt_service_val { const struct bt_uuid *uuid; uint16_t end_handle; };
struct bt_gatt_chrc { const struct bt_uuid *uuid; uint16_t value_handle; uint8_t properties; };
struct bt_gatt_discover_params;
typedef uint8_t (*bt_gatt_discover_func_t)(struct bt_conn *conn, const struct bt_gatt_attr *attr,
   struct bt_gatt_discover_params *params);
struct bt_gatt_discover_params
{
   const struct bt_uuid *uuid;
   bt_gatt_discover_func_t func;
   uint16_t start_handle;
   uint16_t end_handle;
   uint8_t type;
};
struct bt_gatt_subscribe_params;
typedef uint8_t (*bt_gatt_notify_func_t)(struct bt_conn *conn,
   struct bt_gatt_subscribe_params *params, const void *data, uint16_t length);
typedef void (*bt_gatt_subscribe_func_t)(struct bt_conn *conn, uint8_t err,
   struct bt_gatt_subscribe_params *params);
struct bt_gatt_subscribe_params
{
   bt_gatt_notify_func_t notify;
   bt_gatt_subscribe_func_t subscribe;
   uint16_t value_handle;
   uint16_t ccc_handle;
   uint16_t value;
   atomic_t flags[1];
};
struct bt_gatt_exchange_params
{
   void (*func)(struct bt_conn *conn, uint8_t err, struct bt_gatt_exchange_params *params);
};
extern int bt_gatt_write_without_response_cb(struct bt_conn *conn, uint16_t handle,
   const void *data, uint16_t length, bool sign, bt_gatt_complete_func_t func, void *user_data);
extern int bt_gatt_discover(struct bt_conn *conn, struct bt_gatt_discover_params *params);
extern int bt_gatt_subscribe(struct bt_conn *conn, struct bt_gatt_subscribe_params *params);
extern int bt_gatt_exchange_mtu(struct bt_conn *conn, struct bt_gatt_exchange_params *params);

#endif // _ZEPHYR_SHIM_H
