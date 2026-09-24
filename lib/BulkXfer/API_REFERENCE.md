# BulkXfer — API Reference

Reliable bulk data transfer over BLE GATT (Zephyr / NCS). Protocol version **2**.

The **sender is always the GATT client** and the **receiver always hosts the GATT server**:

| Role | Header | Direction | Transport |
|---|---|---|---|
| **Client** (TX) | [BulkXfer_Client.h](BulkXfer_Client.h) | this device → peer | Writes START / DATA into the peer's **DATA** characteristic with **Write Without Response**. Receives ACK / NACK / END as **CTRL** notifications |
| **Server** (RX) | [BulkXfer_Server.h](BulkXfer_Server.h) | peer → this device | Hosts DATA + CTRL. Receives frames through the DATA write hook and answers on CTRL |

A device that needs both directions runs both roles. Each role binds **one connection
at a time**, and the two roles may use the same link or different links.

For design rationale, the protocol walkthrough and throughput tuning, see
[README.md](README.md). This file is the exact API contract.

| Header | Contents | Zephyr dependency |
|---|---|---|
| [BulkXfer.h](BulkXfer.h) | Umbrella: includes the headers below for the enabled roles, plus `gv_BLK_OnDisconnected()` | Yes |
| [BulkXfer_Server.h](BulkXfer_Server.h) | Server API, `BlkSrvCfg_T` | Yes |
| [BulkXfer_Client.h](BulkXfer_Client.h) | Client API, `BlkCliCfg_T`, `BlkCliReady_F` | Yes |
| [BulkXfer_Types.h](BulkXfer_Types.h) | Shared callback types, `BlkSource_T`, `BlkCaps_T`, `BLK_PROTOCOL_VERSION` | Yes (`gatt.h`) |
| [BulkXfer_Uuid.h](BulkXfer_Uuid.h) | Default service / characteristic UUIDs | Yes (`uuid.h`) |
| [BulkXfer_Frame.h](BulkXfer_Frame.h) | Wire format, enums, frame codec | No (host-testable) |
| [BulkXfer_Config.h](BulkXfer_Config.h) | Compile-time tunables and role selection | No |
| `BulkXfer_Core_Priv.h` | Internal interface between the engine core and the roles | Not for applications |

Applications include only `BulkXfer.h`.

**Dependencies:**
- [`AppLog`](../AppLog) (`AppLog.h`, logging macros) and Kconfig `CONFIG_CRC=y`.
- Server: [`GATT_CB`](../GATT_CB), whose generic write callback and `GATTCustomWriteCb_F` drive the DATA hook.
- Client: Kconfig `CONFIG_BT_GATT_CLIENT=y`.
- Optional link tuning: `CONFIG_BT_USER_PHY_UPDATE`, `CONFIG_BT_USER_DATA_LEN_UPDATE`.

**Naming convention:** `g` = global, then return type (`i` int, `v` void, `b` bool,
`u16` uint16_t, `t` ssize_t), then the module: `_BLKS_` Server, `_BLKC_` Client, `_BLK_` shared.
Types end in `_T` (struct), `_E` (enum), `_F` (function pointer).

---

## 1. Quick start

### Server (receives)

```c
#include "BulkXfer.h"

static int on_rx_data(uint8_t type, uint32_t off, const uint8_t *d, uint16_t n)
{
   return flash_write(off, d, n);            /* non-zero aborts with SINK_ERROR */
}

void app_init(void)
{
   BlkSrvCfg_T cfg = {
      .stpt_ctrlAttr  = bt_gatt_find_by_uuid(svc.attrs, svc.attr_count, BT_UUID_BLK_CTRL),
      .fpt_onRxData   = on_rx_data,
      .fpt_onRxDone   = on_rx_done,
      .b_autoTuneLink = true,
   };
   gi_BLKS_Init(&cfg);
}

/* bt_conn_cb */
static void connected(struct bt_conn *c, uint8_t err)  { if (!err) gv_BLKS_OnConnected(c); }
static void disconnected(struct bt_conn *c, uint8_t r) { gv_BLK_OnDisconnected(c); }

/* Generated service .c: custom write hook of the DATA characteristic */
static ssize_t st_OnBulkData(struct bt_conn *c, const struct bt_gatt_attr *a,
   const void *b, uint16_t l, uint16_t o, uint8_t f)
{
   return gt_BLKS_DataWriteHook(c, a, b, l, o, f);
}
```

### Client (sends)

```c
static void on_ready(struct bt_conn *c, int status) { if (status == 0) start_sending(); }
static void on_tx_done(uint8_t type, BlkStatus_E st) { /* free / start next */ }

void app_init(void)
{
   BlkCliCfg_T cfg = {                       /* NULL UUIDs = BulkXfer_Uuid.h defaults */
      .fpt_onReady    = on_ready,
      .fpt_onTxDone   = on_tx_done,
      .b_autoTuneLink = true,
   };
   gi_BLKC_Init(&cfg);
}

static void connected(struct bt_conn *c, uint8_t err)  { if (!err) gi_BLKC_Attach(c); }
static void disconnected(struct bt_conn *c, uint8_t r) { gv_BLK_OnDisconnected(c); }

gi_BLKC_SendBuffer(0x10, log_buf, log_len);  /* once on_ready reported 0 */
```

### GATT service (hosted by the Server)

| Characteristic | UUID (`BulkXfer_Uuid.h`) | Properties | Notes |
|---|---|---|---|
| Service | `BT_UUID_BLK_SVC` `B1C00001-2F5B-4E6A-9C1D-7A3E5F8B0C21` | — | |
| DATA | `BT_UUID_BLK_DATA` `B1C00002-…` | Write Without Response (+ Write), variable length, buffer ≥ `BLK_MAX_FRAME_LEN` (244) | Custom write hook forwards to `gt_BLKS_DataWriteHook()` |
| CTRL | `BT_UUID_BLK_CTRL` `B1C00003-…` | Notify + CCC | Its value attribute goes in `BlkSrvCfg_T.stpt_ctrlAttr` |
| Caps (optional) | `BT_UUID_BLK_CAPS` `B1C00004-…` | Read, 4 bytes | Serve a `BlkCaps_T` filled by `gv_BLKS_GetCaps()` |

Each `BT_UUID_BLK_*` macro has a `BT_UUID_BLK_*_VAL` twin (the encoded bytes), usable in
`BT_UUID_INIT_128()` and advertising data.

---

## 2. Shared API — `BulkXfer.h`

#### `void gv_BLK_OnDisconnected(struct bt_conn *stpt_conn)`
Forwards the disconnect to every enabled role (`gv_BLKS_OnDisconnected()`,
`gv_BLKC_OnDisconnected()`). Each role ignores connections it is not bound to. Call it from
`bt_conn_cb.disconnected`.

---

## 3. Server API — `BulkXfer_Server.h`

Compiled when `BLK_ENABLE_SERVER` is 1.

### Lifecycle

#### `int gi_BLKS_Init(const BlkSrvCfg_T *stpt_cfg)`
Copies the configuration and starts the engine thread (shared with the Client). Call once, before any other Server API.

| Return | Meaning |
|---|---|
| `0` | Success |
| `-EINVAL` | `stpt_cfg` or `stpt_cfg->stpt_ctrlAttr` is `NULL` |
| `-EALREADY` | Already initialised |

#### `void gv_BLKS_OnConnected(struct bt_conn *stpt_conn)`
Binds the Server to a connection (takes a `bt_conn_ref`). Call it from `bt_conn_cb.connected`
only when `err == 0`. The call is ignored (with a warning) if the Server is not initialised
or a connection is already bound. When `b_autoTuneLink` is true, it requests 2M PHY, maximum
data length and, with `CONFIG_BT_GATT_CLIENT`, an ATT MTU exchange.

#### `void gv_BLKS_OnDisconnected(struct bt_conn *stpt_conn)`
Releases the bound connection, stops the Server timers, restores its credits, and fails a
running transfer with `eBS_DISCONNECTED`. The failure is reported asynchronously through
`fpt_onRxDone` on the engine thread. Calls for other connections are ignored.
`gv_BLK_OnDisconnected()` calls it.

### Server → client

#### `int gi_BLKS_SendShort(uint8_t u8_appType, const void *vpt_data, uint8_t u8_len, k_timeout_t t_timeout)`
Notifies one `[len][type][data]` frame on CTRL **synchronously**, with no ACK and no
retransmit. It can be used while a transfer is running. `t_timeout` limits the wait for a
credit. The maximum `u8_len` is `gu16_BLKS_GetMaxShortPayload()`.

| Return | Meaning |
|---|---|
| `0` | Frame handed to the host |
| `-EPERM` | Not initialised |
| `-EINVAL` | `u8_appType > 0xEF`, or `vpt_data == NULL` with `u8_len != 0` |
| `-ENOTCONN` | No bound connection |
| `-EACCES` | Client not subscribed to CTRL |
| `-EMSGSIZE` | Payload does not fit the current MTU |
| `-EAGAIN` | No credit within `t_timeout` |
| other `< 0` | Error from `bt_gatt_notify_cb()` |

#### `void gv_BLKS_AbortRx(void)`
Requests cancellation of the incoming transfer. It is asynchronous and safe from any context.
The client receives ABORT (by receiver), and the local result is `eBS_ABORTED` through `fpt_onRxDone`.

### Queries

| Function | Returns |
|---|---|
| `bool gb_BLKS_IsRxBusy(void)` | `true` between an accepted START and `fpt_onRxDone` |
| `uint16_t gu16_BLKS_GetMaxShortPayload(void)` | Largest `gi_BLKS_SendShort()` payload on the current link; `0` with no connection |
| `void gv_BLKS_GetCaps(BlkCaps_T *stpt_caps)` | Fills the Caps record (asserts `stpt_caps != NULL`) |

### GATT hook

#### `ssize_t gt_BLKS_DataWriteHook(struct bt_conn *stpt_connHandle, const struct bt_gatt_attr *stpt_attr, const void *vpt_buf, uint16_t u16_length, uint16_t u16_offset, uint8_t u8_flags)`
Matches `GATTCustomWriteCb_F` from `GATT_CB`. It runs in the BLE RX thread and **never
blocks**: it checks the frame length, copies the frame into a memory-slab block and queues it
for the engine. It reads the frame from `vpt_buf` / `u16_length`, and ignores the copy that
`gt_GATT_GenericWrite` places in the descriptor buffer.

| Return | Condition |
|---|---|
| `0` | Frame queued, or write came from an unbound connection / before init (silently ignored) |
| `BT_GATT_ERR(BT_ATT_ERR_WRITE_NOT_PERMITTED)` | Prepare (long) write |
| `BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET)` | `u16_offset != 0` |
| `BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN)` | Length < 2, > `BLK_MAX_FRAME_LEN`, or `len byte + 2 != u16_length` |
| `BT_GATT_ERR(BT_ATT_ERR_INSUFFICIENT_RESOURCES)` | RX pool full; the engine sends a NACK (`eBS_NO_RESOURCES`) |

The engine accepts on DATA only application shorts, START, DATA and ABORT (by sender).
ACK / NACK / END / ABORT (by receiver) written to DATA are dropped with a warning.

A **START is ignored** (no ACK, no ABORT, `fpt_onRxStart` not called) while the client is not
subscribed to CTRL, because the answer could not reach it. The client's ACK timeout then ends the attempt.

### `BlkSrvCfg_T`: passed to `gi_BLKS_Init()` and copied

| Field | Type | Req. | Meaning |
|---|---|---|---|
| `stpt_ctrlAttr` | `const struct bt_gatt_attr *` | **Yes** | CTRL value attribute (`bt_gatt_find_by_uuid(..., BT_UUID_BLK_CTRL)`) |
| `fpt_onRxStart` | `BlkRxStart_F` | No | Accept or reject incoming transfers. `NULL` accepts every transfer |
| `fpt_onRxData` | `BlkRxData_F` | To receive | `NULL` makes every incoming transfer `REJECTED` |
| `fpt_onRxDone` | `BlkRxDone_F` | No | Incoming transfer result |
| `fpt_onRxShort` | `BlkRxShort_F` | No | Client → server short messages. `NULL` drops them |
| `b_autoTuneLink` | `bool` | — | Request 2M PHY / max DLE / MTU exchange on connect |

---

## 4. Client API — `BulkXfer_Client.h`

Compiled when `BLK_ENABLE_CLIENT` is 1 (default: `CONFIG_BT_GATT_CLIENT` is set). The GATT
client role is independent of the link role, so a peripheral can be the Client.

### Lifecycle

#### `int gi_BLKC_Init(const BlkCliCfg_T *stpt_cfg)`
Copies the configuration, fills `NULL` UUIDs with the defaults, and starts the engine thread.

| Return | Meaning |
|---|---|
| `0` | Success |
| `-EINVAL` | `stpt_cfg` is `NULL` |
| `-EALREADY` | Already initialised |

#### `int gi_BLKC_Attach(struct bt_conn *stpt_conn)`
Binds the Client to a connection (takes a `bt_conn_ref`) and starts the attach sequence
**asynchronously**:

1. With `b_autoTuneLink` set: request 2M PHY and max DLE, then run an ATT MTU exchange. Discovery starts from its callback, so the chunk size is final once the Client is ready.
2. Discover the primary service, then the DATA characteristic (must have Write Without Response) and the CTRL characteristic (must have Notify), then CTRL's CCC.
3. Subscribe to CTRL. The subscription is volatile, so it is dropped at disconnect even when bonded.

The result arrives through `fpt_onReady` on the engine thread. A failed attach releases the
binding, so `gi_BLKC_Attach()` may be called again.

| Return | Meaning |
|---|---|
| `0` | Attach started |
| `-EPERM` | Not initialised |
| `-EINVAL` | `stpt_conn` is `NULL` |
| `-EALREADY` | This connection is already attached or attaching |
| `-EBUSY` | Another connection is bound |
| other `< 0` | `bt_gatt_discover()` failed to start (binding released) |

#### `void gv_BLKC_OnDisconnected(struct bt_conn *stpt_conn)`
Releases the bound connection, stops the Client timers and restores its credits. A running
transfer fails with `eBS_DISCONNECTED` through `fpt_onTxDone`. An attach in progress reports
`fpt_onReady(conn, -ENOTCONN)`. Both are reported asynchronously on the engine thread. Calls
for other connections are ignored. `gv_BLK_OnDisconnected()` calls it.

### Sending

#### `int gi_BLKC_Send(uint8_t u8_appType, const BlkSource_T *stpt_source, uint32_t u32_totalLen)`
Starts an **asynchronous** multi-frame transfer to the server. Completion comes through `fpt_onTxDone`.

- Computes the object's CRC-32 **in the caller's thread** by reading the whole source once (64-byte reads). Long objects make this call slow, so avoid calling it from a BulkXfer callback for large sources.
- `stpt_source` is copied. The data it reads must stay readable and **unchanged** until `fpt_onTxDone`, because retransmits re-read the source.
- `u32_totalLen == 0` is allowed.
- The chunk size is fixed at call time: `min(ATT_MTU − 3, BLK_MAX_FRAME_LEN) − 4`.

| Return | Meaning |
|---|---|
| `0` | Transfer queued |
| `-EPERM` | Not initialised |
| `-EINVAL` | `stpt_source` / `fpt_read` is `NULL`, or `u8_appType > 0xEF` |
| `-EIO` | Source `fpt_read` returned non-zero during the CRC pass |
| `-ENOTCONN` | No attached connection |
| `-EAGAIN` | Attach not finished (`fpt_onReady` not yet reported 0) |
| `-EBUSY` | An outgoing transfer is already running (see `gb_BLKC_IsTxBusy()`) |
| `-EMSGSIZE` | MTU too small for a DATA frame with ≥ 1 data byte |

#### `int gi_BLKC_SendBuffer(uint8_t u8_appType, const void *vpt_data, uint32_t u32_totalLen)`
Calls `gi_BLKC_Send()` with a built-in RAM source. The buffer must remain valid and unchanged
until `fpt_onTxDone`. It returns `-EINVAL` if `vpt_data` is `NULL` and `u32_totalLen != 0`;
otherwise it returns the same codes as `gi_BLKC_Send()`.

#### `int gi_BLKC_SendShort(uint8_t u8_appType, const void *vpt_data, uint8_t u8_len, k_timeout_t t_timeout)`
Writes one `[len][type][data]` frame to DATA **synchronously**, with no ACK and no
retransmit. It can be used while a transfer is running. `t_timeout` limits the wait for a
credit. The maximum `u8_len` is `gu16_BLKC_GetMaxShortPayload()`.

| Return | Meaning |
|---|---|
| `0` | Frame handed to the host |
| `-EPERM` | Not initialised |
| `-EINVAL` | `u8_appType > 0xEF`, or `vpt_data == NULL` with `u8_len != 0` |
| `-ENOTCONN` | No attached connection |
| `-EAGAIN` | Attach not finished, or no credit within `t_timeout` |
| `-EMSGSIZE` | Payload does not fit the current MTU |
| other `< 0` | Error from `bt_gatt_write_without_response_cb()` |

#### `void gv_BLKC_AbortTx(void)`
Requests cancellation of the outgoing transfer. It is asynchronous and safe from any context.
The server receives ABORT (by sender) if START was already sent. The local result is `eBS_ABORTED` through `fpt_onTxDone`.

### Queries

| Function | Returns |
|---|---|
| `bool gb_BLKC_IsReady(void)` | `true` from `fpt_onReady(…, 0)` until the disconnect |
| `bool gb_BLKC_IsTxBusy(void)` | `true` while `gi_BLKC_Send()` would return `-EBUSY` |
| `uint16_t gu16_BLKC_GetMaxShortPayload(void)` | Largest `gi_BLKC_SendShort()` payload on the current link; `0` unless ready |

The engine accepts on CTRL only application shorts, ACK, NACK, END and ABORT (by receiver).
Other frames are dropped with a warning.

### `BlkCliCfg_T`: passed to `gi_BLKC_Init()` and copied

| Field | Type | Req. | Meaning |
|---|---|---|---|
| `stpt_svcUuid` | `const struct bt_uuid *` | No | Service to discover. `NULL` = `BT_UUID_BLK_SVC` |
| `stpt_dataUuid` | `const struct bt_uuid *` | No | DATA characteristic. `NULL` = `BT_UUID_BLK_DATA` |
| `stpt_ctrlUuid` | `const struct bt_uuid *` | No | CTRL characteristic. `NULL` = `BT_UUID_BLK_CTRL` |
| `fpt_onReady` | `BlkCliReady_F` | No | Attach result |
| `fpt_onTxDone` | `BlkTxDone_F` | No | Outgoing transfer result |
| `fpt_onRxShort` | `BlkRxShort_F` | No | Server → client short messages. `NULL` drops them |
| `b_autoTuneLink` | `bool` | — | Request 2M PHY / max DLE and exchange MTU before discovery |

The UUIDs are kept **by pointer** and must have static storage. `BT_UUID_DECLARE_128()` used
inside a function is a compound literal with automatic storage; use a file-scope
`static const struct bt_uuid_128 x = BT_UUID_INIT_128(...)` and pass `&x.uuid`.

### `BlkCliReady_F`

```c
/* Attach finished. stpt_conn is valid only during the call. */
typedef void (*BlkCliReady_F)(struct bt_conn *stpt_conn, int i_status);
```

| `i_status` | Meaning |
|---|---|
| `0` | Ready: DATA / CTRL found, subscribed to CTRL |
| `-ENOENT` | Service, DATA (with Write Without Response), CTRL (with Notify) or CTRL's CCC not found |
| `-EIO` | CCC write for the subscription failed |
| `-ENOTCONN` | Link lost before the attach finished |
| other `< 0` | A discovery step failed to start (`bt_gatt_discover()` error) |

Exactly one `fpt_onReady` call follows each successful `gi_BLKC_Attach()`.

---

## 5. Shared types — `BulkXfer_Types.h`

### Threading contract
**Every callback runs on the BulkXfer engine thread**, one at a time, never in BLE stack
context. Server and Client callbacks share that thread. Callbacks may call the BulkXfer API,
for example `gi_BLKC_Send()` from `fpt_onTxDone`. A slow callback throttles the link, which is
safe because the receiver simply ACKs later. Callbacks run on the engine thread's stack
(`BLK_THREAD_STACK_SIZE`).

### Callback signatures

```c
/* Sender data provider: copy u16_len bytes at u32_offset into u8pt_buf.
 * May be called again for the same range (retransmit).
 * Return 0, or negative errno -> transfer ends with eBS_SOURCE_ERROR. */
typedef int  (*BlkSourceRead_F)(void *vpt_ctx, uint32_t u32_offset,
                                uint8_t *u8pt_buf, uint16_t u16_len);

/* START received. Return 0 to accept, non-zero to reject (client: REJECTED). */
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

/* Outgoing transfer finished (server's END status, or a local error). */
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

### Constant
| Macro | Value |
|---|---|
| `BLK_PROTOCOL_VERSION` | `2` |

---

## 6. Wire format & codec — `BulkXfer_Frame.h`

One frame per GATT write or notification. Multi-byte fields are little-endian.

```
+--------+---------+----------------------+
| len(1) | type(1) | payload (len bytes)  |     len + 2 == ATT value length
+--------+---------+----------------------+
```

Types `0x00–0xEF` are application single-frame messages. Types `0xF0–0xFF` are reserved by the framework.

### `BlkFrameType_E`
| Value | Name | Carried on | Payload (bytes) |
|---|---|---|---|
| `0xF0` | `eBFT_START` | DATA (client → server) | xferId(1) appType(1) totalLen(4) chunkSize(1) window(1) crc32(4) = 12 |
| `0xF1` | `eBFT_DATA` | DATA | xferId(1) seq(1) data(1..chunkSize) |
| `0xF2` | `eBFT_ACK` | CTRL (server → client) | xferId(1) nextExpectedSeq(1) window(1) = 3 (cumulative) |
| `0xF3` | `eBFT_NACK` | CTRL | xferId(1) nextExpectedSeq(1) reason(1) = 3 (Go-Back-N) |
| `0xF4` | `eBFT_END` | CTRL | xferId(1) status(1) = 2 |
| `0xF5` | `eBFT_ABORT` | DATA if `dir` = by sender, CTRL if `dir` = by receiver | xferId(1) reason(1) dir(1) = 3 |

CRC is CRC-32/IEEE (`crc32_ieee`) over all object bytes. `seq` = absolute frame index mod 256.
The frame layout is the same as in protocol v1. Version 2 changes which characteristic carries each frame.

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
| `0x00` | `eBAD_BY_SENDER`: client cancels its outgoing transfer |
| `0x01` | `eBAD_BY_RECEIVER`: server cancels an incoming transfer |

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
The common fields are `u8_type`, `u8_payloadLen` and `u8pt_payload`. Pointers reference the
parsed buffer. Framework frames fill one member of `u_body`. Fields are declared widest-first
to avoid padding, so their order is not the wire order. Access them by name only: do not use
positional initializers or `memcpy` from the wire, and read `u8_xferId` only through the
member that matches `u8_type`.

| Member | Fields (declaration order) |
|---|---|
| `st_start` | `u32_totalLen, u32_crc32, u8_xferId, u8_appType, u8_chunkSize, u8_window` |
| `st_data` | `u8pt_data, u8_xferId, u8_seq, u8_dataLen` |
| `st_ack` | `u8_xferId, u8_seq, u8_window` |
| `st_nack` | `u8_xferId, u8_seq, u8_reason` |
| `st_end` | `u8_xferId, u8_status` |
| `st_abort` | `u8_xferId, u8_reason, u8_dir` |

### Codec functions
These functions are pure and have no Zephyr dependency, so they suit host tests and other client implementations.

#### `int gi_BLK_FrameParse(const uint8_t *u8pt_buf, uint16_t u16_len, BlkFrame_T *stpt_frame)`
Validates the frame (header present, `len + 2 == u16_len`, exact payload size per control type,
DATA ≥ 1 data byte) and decodes it. Returns `0`, `-EINVAL` (malformed / `NULL` args), or
`-ENOTSUP` (unknown type `0xF6–0xFF`).

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

## 7. Configuration — `BulkXfer_Config.h`

Every macro is `#ifndef`-guarded. Override it from the application's CMake, e.g.
`zephyr_compile_definitions(BLK_WINDOW_DEFAULT=32)`.

| Macro | Default | Constraint / effect |
|---|---|---|
| `BLK_ENABLE_SERVER` | 1 | Build the Server role |
| `BLK_ENABLE_CLIENT` | 1 if `CONFIG_BT_GATT_CLIENT`, else 0 | Build the Client role. At least one role must be enabled (`#error`) |
| `BLK_MAX_FRAME_LEN` | 244 | 20..257 (`#error` otherwise). The DATA char buffer must be ≥ this. Frame on a link = `min(ATT_MTU − 3, this)` |
| `BLK_WINDOW_DEFAULT` | 16 | 2..128 (`#error` otherwise). Effective window = min of both peers |
| `BLK_RX_POOL_DEPTH` | window + 4 | Server: queued DATA frames; ≈ `BLK_MAX_FRAME_LEN + 12` B RAM each |
| `BLK_CLI_CTRL_POOL_DEPTH` | 4 | Client: queued CTRL notifications; same RAM per entry |
| `BLK_SRV_NOTIFY_INFLIGHT_MAX` | 2 | Server: CTRL notifications in flight in the host |
| `BLK_CLI_WRITE_INFLIGHT_MAX` | 6 | Client: DATA writes in flight in the host. The sum over the enabled roles must stay below `CONFIG_BT_ATT_TX_COUNT` (`BUILD_ASSERT`) |
| `BLK_TX_ACK_TIMEOUT_MS` | 1000 | Client: no ACK progress → Go-Back-N / resend START |
| `BLK_TX_MAX_RETRIES` | 5 | Client: consecutive ACK timeouts → `eBS_TIMEOUT` |
| `BLK_RX_ACK_DELAY_MS` | 20 | Server: max delay before ACK when < window/2 frames pending |
| `BLK_RX_IDLE_TIMEOUT_MS` | 5000 | Server: silence → `eBS_TIMEOUT` |
| `BLK_CTRL_TX_TIMEOUT_MS` | 200 | Max wait for a credit for ACK/NACK/END/ABORT |
| `BLK_WRITE_RETRY_MS` | 5 | Client: back-off after `-ENOMEM` / `-EAGAIN` from the host |
| `BLK_THREAD_STACK_SIZE` | 2048 | Engine thread stack (application callbacks run on it) |
| `BLK_THREAD_PRIORITY` | 5 | Engine thread priority (preemptible) |

---

## 8. Behavioural rules (for code that uses BulkXfer)

1. Each role runs **one** multi-frame transfer at a time. A device with both roles can send and receive concurrently, and short messages can be sent alongside transfers.
2. The Client sends only after `fpt_onReady(…, 0)` (or `gb_BLKC_IsReady()`). Before that, `gi_BLKC_Send*` returns `-EAGAIN`.
3. `gi_BLKC_Send*` returns once the transfer is queued. The data source / buffer must stay alive and unchanged until `fpt_onTxDone`.
4. Treat data from `fpt_onRxData` as provisional until `fpt_onRxDone(..., eBS_OK, ...)`.
5. Pointers passed to `fpt_onRxData` / `fpt_onRxShort` are valid only during the call.
6. Short messages have no delivery guarantee (no ACK, no retransmit).
7. Frames on DATA must use **Write Without Response**, one frame per write. Long (prepared) writes are refused.
8. The server answers only a client that is subscribed to CTRL.
9. Call `gv_BLK_OnDisconnected()` (or the per-role variants) from `bt_conn_cb.disconnected` for every link a role may be bound to.
10. Never call the API from ISR context except `gv_BLKS_AbortRx()` / `gv_BLKC_AbortTx()` (atomic bit + wake). The other functions take a mutex.
