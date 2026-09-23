# BulkXfer — API Reference

Reliable, bidirectional bulk data transfer over two BLE GATT characteristics
(Zephyr / NCS). Protocol version **1**. Serves **one connection at a time**.

For the design rationale, protocol walkthrough and throughput tuning, see
[README.md](README.md). This file is the exact API contract.

| Header | Contents | Zephyr dependency |
|---|---|---|
| [BulkXfer.h](BulkXfer.h) | Engine API (lifecycle, send, abort, queries, GATT hook) | Yes |
| [BulkXfer_Types.h](BulkXfer_Types.h) | Callback types, `BlkSource_T`, `BlkCfg_T`, `BlkCaps_T` | Yes (`bt_gatt_attr`) |
| [BulkXfer_Frame.h](BulkXfer_Frame.h) | Wire format, enums, frame codec | No (host-testable) |
| [BulkXfer_Config.h](BulkXfer_Config.h) | Compile-time tunables | No |

Applications include only `BulkXfer.h`; it pulls in the other three.

**Dependencies:** [`GATT_CB`](../GATT_CB) (generic write callback + `GATTCustomWriteCb_F`),
[`AppLog`](../AppLog) (`AppLog.h`, logging macros), Kconfig `CONFIG_CRC=y`.
Optional Kconfig used by link auto-tuning: `CONFIG_BT_USER_PHY_UPDATE`,
`CONFIG_BT_USER_DATA_LEN_UPDATE`, `CONFIG_BT_GATT_CLIENT`.

**Naming convention:** `g` = global, then return type (`i` int, `v` void, `b` bool,
`u16` uint16_t, `t` ssize_t), then `_BLK_`. Types end in `_T` (struct), `_E` (enum), `_F` (function pointer).

---

## 1. Quick start

```c
#include "BulkXfer.h"

static int on_rx_data(uint8_t type, uint32_t off, const uint8_t *d, uint16_t n)
{
   return flash_write(off, d, n);            /* non-zero aborts with SINK_ERROR */
}
static void on_tx_done(uint8_t type, BlkStatus_E st) { /* free / start next */ }

void app_init(void)
{
   BlkCfg_T cfg = {
      .stpt_txAttr    = bt_gatt_find_by_uuid(svc.attrs, svc.attr_count, TX_UUID),
      .fpt_onRxData   = on_rx_data,
      .fpt_onTxDone   = on_tx_done,
      .b_autoTuneLink = true,
   };
   gi_BLK_Init(&cfg);
}

/* bt_conn_cb */
static void connected(struct bt_conn *c, uint8_t err)  { if (!err) gv_BLK_OnConnected(c); }
static void disconnected(struct bt_conn *c, uint8_t r) { gv_BLK_OnDisconnected(c); }

/* Generated service .c: custom write hook of the RX characteristic */
static ssize_t st_OnBulkRx(struct bt_conn *c, const struct bt_gatt_attr *a,
   const void *b, uint16_t l, uint16_t o, uint8_t f)
{
   return gt_BLK_RxWriteHook(c, a, b, l, o, f);
}

/* Send */
gi_BLK_SendBuffer(0x01, log_buf, log_len);
```

### Required GATT service

| Characteristic | Properties | Notes |
|---|---|---|
| RX | Write + Write Without Response, variable length, buffer ≥ `BLK_MAX_FRAME_LEN` (244) | Custom write hook forwards to `gt_BLK_RxWriteHook()` |
| TX | Notify + CCC | Its value attribute goes in `BlkCfg_T.stpt_txAttr` |
| Caps (optional) | Read, 4 bytes | Serve a `BlkCaps_T` filled by `gv_BLK_GetCaps()` |

---

## 2. Engine API — `BulkXfer.h`

### Lifecycle

#### `int gi_BLK_Init(const BlkCfg_T *stpt_cfg)`
Copies the configuration and starts the engine thread. Call once, before any other API.

| Return | Meaning |
|---|---|
| `0` | Success |
| `-EINVAL` | `stpt_cfg` or `stpt_cfg->stpt_txAttr` is `NULL` |
| `-EALREADY` | Already initialised |

#### `void gv_BLK_OnConnected(struct bt_conn *stpt_conn)`
Binds the framework to a connection (takes a `bt_conn_ref`). Call from
`bt_conn_cb.connected` only when `err == 0`. Ignored (with a warning) if not
initialised or a connection is already bound. When `b_autoTuneLink` is true, it
requests 2M PHY, maximum data length and (if `CONFIG_BT_GATT_CLIENT`) an ATT MTU exchange.

#### `void gv_BLK_OnDisconnected(struct bt_conn *stpt_conn)`
Releases the bound connection, stops all timers, restores TX credits, and fails
any running transfer with `eBS_DISCONNECTED`. The failure is reported
asynchronously through `fpt_onTxDone` / `fpt_onRxDone` on the engine thread.
Calls for a connection other than the bound one are ignored.

### Sending

#### `int gi_BLK_Send(uint8_t u8_appType, const BlkSource_T *stpt_source, uint32_t u32_totalLen)`
Starts an **asynchronous** multi-frame transfer to the peer. Completion comes
through `fpt_onTxDone`.

- Computes the object's CRC-32 **in the caller's thread** by reading the whole source once (64-byte reads). Long objects make this call slow.
- `stpt_source` is copied. The data it reads must stay readable and **unchanged** until `fpt_onTxDone`, because retransmits re-read the source.
- `u32_totalLen == 0` is allowed.
- Chunk size is fixed at call time: `min(ATT_MTU − 3, BLK_MAX_FRAME_LEN) − 4`.

| Return | Meaning |
|---|---|
| `0` | Transfer queued |
| `-EPERM` | Not initialised |
| `-EINVAL` | `stpt_source` / `fpt_read` is `NULL`, or `u8_appType > 0xEF` |
| `-EIO` | Source `fpt_read` returned non-zero during the CRC pass |
| `-ENOTCONN` | No bound connection |
| `-EBUSY` | An outgoing transfer is already running (see `gb_BLK_IsTxBusy()`) |
| `-EACCES` | Peer is not subscribed to TX notifications |
| `-EMSGSIZE` | MTU too small for a DATA frame with ≥ 1 data byte |

#### `int gi_BLK_SendBuffer(uint8_t u8_appType, const void *vpt_data, uint32_t u32_totalLen)`
`gi_BLK_Send()` with a built-in RAM source. The buffer must remain valid and
unchanged until `fpt_onTxDone`. Returns `-EINVAL` if `vpt_data` is `NULL` and
`u32_totalLen != 0`. Otherwise it returns the same codes as `gi_BLK_Send()`.

#### `int gi_BLK_SendShort(uint8_t u8_appType, const void *vpt_data, uint8_t u8_len, k_timeout_t t_timeout)`
Sends one `[len][type][data]` frame **synchronously**, with no ACK and no retransmit.
Can be used while a multi-frame transfer is running. `t_timeout` limits the wait for a TX credit.
Maximum `u8_len` is `gu16_BLK_GetMaxShortPayload()`.

| Return | Meaning |
|---|---|
| `0` | Frame handed to the host |
| `-EPERM` | Not initialised |
| `-EINVAL` | `u8_appType > 0xEF`, or `vpt_data == NULL` with `u8_len != 0` |
| `-ENOTCONN` / `-EACCES` | No connection / peer not subscribed |
| `-EMSGSIZE` | Payload does not fit the current MTU |
| `-EAGAIN` | No TX credit within `t_timeout` |
| other `< 0` | Error from `bt_gatt_notify_cb()` |

#### `void gv_BLK_AbortTx(void)` / `void gv_BLK_AbortRx(void)`
Request cancellation of the outgoing / incoming transfer. Asynchronous and safe
from any context. The peer receives an ABORT. Completion is reported as
`eBS_ABORTED` through `fpt_onTxDone` / `fpt_onRxDone`.

### Queries

| Function | Returns |
|---|---|
| `bool gb_BLK_IsTxBusy(void)` | `true` while `gi_BLK_Send()` would return `-EBUSY` |
| `uint16_t gu16_BLK_GetMaxShortPayload(void)` | Largest `gi_BLK_SendShort()` payload on the current link; `0` with no connection |
| `void gv_BLK_GetCaps(BlkCaps_T *stpt_caps)` | Fills the Caps record (asserts `stpt_caps != NULL`) |

### GATT hook

#### `ssize_t gt_BLK_RxWriteHook(struct bt_conn *stpt_connHandle, const struct bt_gatt_attr *stpt_attr, const void *vpt_buf, uint16_t u16_length, uint16_t u16_offset, uint8_t u8_flags)`
Matches `GATTCustomWriteCb_F` from `GATT_CB`. Runs in the BLE RX thread and
**never blocks**. It validates the frame, copies it into a memory-slab block and
queues it for the engine. It reads the frame from `vpt_buf` / `u16_length`. It
ignores the copy that `gt_GATT_GenericWrite` places in the descriptor buffer.

| Return | Condition |
|---|---|
| `0` | Frame queued, or write came from an unbound connection / before init (silently ignored) |
| `BT_GATT_ERR(BT_ATT_ERR_WRITE_NOT_PERMITTED)` | Prepare (long) write |
| `BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET)` | `u16_offset != 0` |
| `BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN)` | Length < 2, > `BLK_MAX_FRAME_LEN`, or `len byte + 2 != u16_length` |
| `BT_GATT_ERR(BT_ATT_ERR_INSUFFICIENT_RESOURCES)` | RX pool full; the engine will send a NACK (`eBS_NO_RESOURCES`) |

### Constant

| Macro | Value |
|---|---|
| `BLK_PROTOCOL_VERSION` | `1` |

---

## 3. Types — `BulkXfer_Types.h`

### Threading contract
**Every callback runs on the BulkXfer engine thread**, one at a time, never in
BLE stack context. Callbacks may call the BulkXfer API, for example to call
`gi_BLK_Send()` from `fpt_onTxDone`. A slow callback throttles the link, which
is safe: the receiver simply ACKs later. Callbacks run on the engine thread's stack
(`BLK_THREAD_STACK_SIZE`).

### `BlkCfg_T`, passed to `gi_BLK_Init()` and copied

| Field | Type | Req. | Meaning |
|---|---|---|---|
| `stpt_txAttr` | `const struct bt_gatt_attr *` | **Yes** | TX characteristic value attribute (`bt_gatt_find_by_uuid`) |
| `fpt_onRxStart` | `BlkRxStart_F` | No | Accept or reject incoming transfers. `NULL` accepts every transfer |
| `fpt_onRxData` | `BlkRxData_F` | To receive | `NULL` makes every incoming transfer `REJECTED` |
| `fpt_onRxDone` | `BlkRxDone_F` | No | Incoming transfer result |
| `fpt_onRxShort` | `BlkRxShort_F` | No | Single-frame messages. `NULL` drops them |
| `fpt_onTxDone` | `BlkTxDone_F` | No | Outgoing transfer result |
| `b_autoTuneLink` | `bool` | — | Request 2M PHY / max DLE / MTU exchange on connect |

### Callback signatures

```c
/* Sender data provider: copy u16_len bytes at u32_offset into u8pt_buf.
 * May be called again for the same range (retransmit).
 * Return 0, or negative errno -> transfer ends with eBS_SOURCE_ERROR. */
typedef int  (*BlkSourceRead_F)(void *vpt_ctx, uint32_t u32_offset,
                                uint8_t *u8pt_buf, uint16_t u16_len);

/* START received. Return 0 to accept, non-zero to reject (peer: REJECTED). */
typedef int  (*BlkRxStart_F)(uint8_t u8_appType, uint32_t u32_totalLen);

/* In-order, contiguous, never-duplicated chunk. Data valid only during the call.
 * Return 0 to continue, non-zero -> eBS_SINK_ERROR. */
typedef int  (*BlkRxData_F)(uint8_t u8_appType, uint32_t u32_offset,
                            const uint8_t *u8pt_data, uint16_t u16_len);

/* Incoming transfer finished. Only eBS_OK means the delivered data is
 * complete and CRC-verified; otherwise discard what fpt_onRxData wrote. */
typedef void (*BlkRxDone_F)(uint8_t u8_appType, BlkStatus_E e_status,
                            uint32_t u32_totalLen);

/* Single-frame message (type 0x00..0xEF). Data valid only during the call. */
typedef void (*BlkRxShort_F)(uint8_t u8_appType, const uint8_t *u8pt_data,
                             uint8_t u8_len);

/* Outgoing transfer finished (receiver's END status, or a local error). */
typedef void (*BlkTxDone_F)(uint8_t u8_appType, BlkStatus_E e_status);
```

### `BlkSource_T`
| Field | Meaning |
|---|---|
| `BlkSourceRead_F fpt_read` | Data provider. Must not be `NULL` |
| `void *vpt_ctx` | Passed back to `fpt_read` unchanged |

### `BlkCaps_T` (`__packed`, 4 bytes)
| Byte | Field | Value |
|---|---|---|
| 0 | `u8_protocolVersion` | `BLK_PROTOCOL_VERSION` |
| 1 | `u8_maxFrameLen` | `BLK_MAX_FRAME_LEN` (clamped to 255) |
| 2 | `u8_window` | `BLK_WINDOW_DEFAULT` |
| 3 | `u8_reserved` | `0` |

---

## 4. Wire format & codec — `BulkXfer_Frame.h`

One frame per GATT write / notification. Multi-byte fields are little-endian.

```
+--------+---------+----------------------+
| len(1) | type(1) | payload (len bytes)  |     len + 2 == ATT value length
+--------+---------+----------------------+
```

Types `0x00–0xEF` are application single-frame messages. Types `0xF0–0xFF` are reserved by the framework.

### `BlkFrameType_E`
| Value | Name | Direction | Payload (bytes) |
|---|---|---|---|
| `0xF0` | `eBFT_START` | S→R | xferId(1) appType(1) totalLen(4) chunkSize(1) window(1) crc32(4) = 12 |
| `0xF1` | `eBFT_DATA` | S→R | xferId(1) seq(1) data(1..chunkSize) |
| `0xF2` | `eBFT_ACK` | R→S | xferId(1) nextExpectedSeq(1) window(1) = 3 (cumulative) |
| `0xF3` | `eBFT_NACK` | R→S | xferId(1) nextExpectedSeq(1) reason(1) = 3 (Go-Back-N) |
| `0xF4` | `eBFT_END` | R→S | xferId(1) status(1) = 2 |
| `0xF5` | `eBFT_ABORT` | either | xferId(1) reason(1) dir(1) = 3 |

CRC is CRC-32/IEEE (`crc32_ieee`) over all object bytes. `seq` = absolute frame index mod 256.

### `BlkStatus_E`
Used for callback results, the END status, and the ABORT / NACK reason.

| Value | Name | Meaning |
|---|---|---|
| `0x00` | `eBS_OK` | Complete, CRC matched |
| `0x01` | `eBS_CRC_ERROR` | All data received, CRC mismatch |
| `0x02` | `eBS_TIMEOUT` | Peer stopped responding |
| `0x03` | `eBS_ABORTED` | Aborted by the local application |
| `0x04` | `eBS_REMOTE_ABORTED` | Aborted by the peer |
| `0x05` | `eBS_REJECTED` | Receiver refused the transfer |
| `0x06` | `eBS_DISCONNECTED` | Link lost |
| `0x07` | `eBS_SOURCE_ERROR` | Sender's `fpt_read` failed |
| `0x08` | `eBS_SINK_ERROR` | Receiver's `fpt_onRxData` failed |
| `0x09` | `eBS_PROTOCOL_ERROR` | Malformed / unexpected frame |
| `0x0A` | `eBS_NO_RESOURCES` | RX queue overflow (NACK reason) |
| `0x0B` | `eBS_OUT_OF_ORDER` | Sequence gap (NACK reason) |

### `BlkAbortDir_E`
| Value | Name |
|---|---|
| `0x00` | `eBAD_BY_SENDER`: sender cancels its outgoing transfer |
| `0x01` | `eBAD_BY_RECEIVER`: receiver cancels an incoming transfer |

### Size constants
| Macro | Value (default config) | Meaning |
|---|---|---|
| `BLK_FRAME_HDR_LEN` | 2 | len + type |
| `BLK_DATA_HDR_LEN` | 4 | len + type + xferId + seq |
| `BLK_MIN_FRAME_LEN` | 5 | Smallest usable DATA frame |
| `BLK_MAX_SHORT_PAYLOAD` | 242 | `BLK_MAX_FRAME_LEN − 2` |
| `BLK_MAX_CHUNK_LEN` | 240 | `BLK_MAX_FRAME_LEN − 4` |
| `BLK_APP_TYPE_MAX` | `0xEF` | Highest application type |
| `BLK_START_PAYLOAD_LEN` / `ACK` / `NACK` / `END` / `ABORT` | 12 / 3 / 3 / 2 / 3 | Fixed control payloads |
| `BLK_CTRL_FRAME_MAX_LEN` | 14 | Largest control frame (START) |

### `BlkFrame_T`: decoded frame (no copy)
Common fields are `u8_type`, `u8_payloadLen` and `u8pt_payload`. Pointers reference the parsed buffer.
Framework frames fill one member of `u_body`. Fields are declared widest-first to avoid
padding, so their order is not the wire order. Access them by name only: do not use positional
initializers, `memcpy` from the wire, or read `u8_xferId` through a different member than the one
matching `u8_type`.

| Member | Fields (declaration order) |
|---|---|
| `st_start` | `u32_totalLen, u32_crc32, u8_xferId, u8_appType, u8_chunkSize, u8_window` |
| `st_data` | `u8pt_data, u8_xferId, u8_seq, u8_dataLen` |
| `st_ack` | `u8_xferId, u8_seq, u8_window` |
| `st_nack` | `u8_xferId, u8_seq, u8_reason` |
| `st_end` | `u8_xferId, u8_status` |
| `st_abort` | `u8_xferId, u8_reason, u8_dir` |

### Codec functions
These functions are pure and have no Zephyr dependency, so they suit host tests and alternate centrals.

#### `int gi_BLK_FrameParse(const uint8_t *u8pt_buf, uint16_t u16_len, BlkFrame_T *stpt_frame)`
Validates the frame (header present, `len + 2 == u16_len`, exact payload size per control type, DATA ≥ 1 data byte) and decodes it.
Returns `0`, `-EINVAL` (malformed / `NULL` args), or `-ENOTSUP` (unknown type `0xF6–0xFF`).

#### Encoders
Each encoder returns the **total frame length**, or **`0`** for a `NULL` buffer, a buffer that is too small, or invalid arguments.

| Function | Notes |
|---|---|
| `gu16_BLK_EncodeShort(buf, bufLen, appType, data, dataLen)` | `appType ≤ 0xEF`, `dataLen ≤ 255`, `data` may be `NULL` if `dataLen == 0` |
| `gu16_BLK_EncodeStart(buf, bufLen, xferId, appType, totalLen, chunkSize, window, crc32)` | 14-byte frame |
| `gu16_BLK_EncodeDataHeader(buf, bufLen, xferId, seq, dataLen)` | Writes the 4-byte header only. The caller puts `dataLen` (1..253) bytes at `buf + BLK_DATA_HDR_LEN`, and `bufLen` must fit header + data |
| `gu16_BLK_EncodeAck(buf, bufLen, xferId, seq, window)` | `seq` = next expected |
| `gu16_BLK_EncodeNack(buf, bufLen, xferId, seq, reason)` | `reason` = `BlkStatus_E` |
| `gu16_BLK_EncodeEnd(buf, bufLen, xferId, status)` | `status` = `BlkStatus_E` |
| `gu16_BLK_EncodeAbort(buf, bufLen, xferId, reason, dir)` | `dir` = `BlkAbortDir_E` |

All parameters are `uint8_t` except `buf` (`uint8_t *`), `bufLen`/`dataLen` (`uint16_t`) and `totalLen`/`crc32` (`uint32_t`).

---

## 5. Configuration — `BulkXfer_Config.h`

Every macro is `#ifndef`-guarded. Override from the application's CMake, e.g.
`zephyr_compile_definitions(BLK_WINDOW_DEFAULT=32)`.

| Macro | Default | Constraint / effect |
|---|---|---|
| `BLK_MAX_FRAME_LEN` | 244 | 20..257 (`#error` otherwise). RX char buffer must be ≥ this. Frame on a link = `min(ATT_MTU − 3, this)` |
| `BLK_WINDOW_DEFAULT` | 16 | 2..128 (`#error` otherwise). Effective window = min of both peers |
| `BLK_RX_POOL_DEPTH` | window + 4 | Queued RX frames; ≈ `BLK_MAX_FRAME_LEN + 12` B RAM each |
| `BLK_TX_INFLIGHT_MAX` | 6 | Must stay below `CONFIG_BT_ATT_TX_COUNT` |
| `BLK_TX_ACK_TIMEOUT_MS` | 1000 | Sender: no ACK progress → Go-Back-N / resend START |
| `BLK_TX_MAX_RETRIES` | 5 | Consecutive ACK timeouts → `eBS_TIMEOUT` |
| `BLK_RX_ACK_DELAY_MS` | 20 | Receiver: max delay before ACK when < window/2 frames pending |
| `BLK_RX_IDLE_TIMEOUT_MS` | 5000 | Receiver: silence → `eBS_TIMEOUT` |
| `BLK_CTRL_TX_TIMEOUT_MS` | 200 | Max wait for a TX credit for ACK/NACK/END/ABORT |
| `BLK_NOTIFY_RETRY_MS` | 5 | Back-off after `-ENOMEM` / `-EAGAIN` from the host |
| `BLK_THREAD_STACK_SIZE` | 2048 | Engine thread stack (application callbacks run on it) |
| `BLK_THREAD_PRIORITY` | 5 | Engine thread priority (preemptible) |

---

## 6. Behavioural rules (for code that uses BulkXfer)

1. Only **one outgoing and one incoming** multi-frame transfer at a time. The two can run concurrently, and short messages can be sent alongside them.
2. `gi_BLK_Send*` returns once the transfer is queued. The data source / buffer must stay alive and unchanged until `fpt_onTxDone`.
3. Treat data from `fpt_onRxData` as provisional until `fpt_onRxDone(..., eBS_OK, ...)`.
4. Pointers passed to `fpt_onRxData` / `fpt_onRxShort` are valid only during the call.
5. Short messages have no delivery guarantee (no ACK, no retransmit).
6. The central should write RX frames with **Write Without Response**, one frame per write, and must not use long (prepared) writes.
7. The peer must enable TX notifications (CCC) before any send succeeds (`-EACCES` otherwise).
8. Never call the API from ISR context except `gv_BLK_AbortTx/Rx` (atomic bit + wake). The other functions take a mutex.
