# BulkXfer — reliable bulk data transfer over BLE GATT (Zephyr / nRF54)

BulkXfer moves objects of any size (logs, files, images, firmware chunks) between two BLE
devices. The **sender is always the GATT client** and the **receiver always hosts the GATT
server**:

| Role | Does | API |
|---|---|---|
| **Client** (TX) | Writes frames into the peer's **DATA** characteristic with Write Without Response | [BulkXfer_Client.h](BulkXfer_Client.h) |
| **Server** (RX) | Hosts DATA + **CTRL**. Answers with ACK / NACK / END notifications on CTRL | [BulkXfer_Server.h](BulkXfer_Server.h) |

A device that must both send and receive runs both roles. The Server sits on top of the
[`GATT_CB`](../GATT_CB) generic callbacks without modifying them.

- **Frame format:** `len(1) + type(1) + payload(≤242)`, one frame per GATT write or notification.
- **Reliability:** windowed cumulative ACKs, Go-Back-N retransmit, CRC-32 over the whole object.
- **Throughput:** the Client keeps the controller queue full with Write Without Response. CTRL carries only about one small notification per half window.
- **Streaming:** data is pulled from a source callback and pushed to a sink callback, so objects never have to fit in RAM.

The exact API contract is in [API_REFERENCE.md](API_REFERENCE.md).

## Why the sender is the GATT client

Protocol v1 sent device → central data as notifications. Version 2 does not, for these reasons:

- **Symmetry between devices.** Any two BulkXfer devices can exchange data in either
  direction: whichever side sends attaches as a client to the other side's service. There is
  no longer one special "central" side that has to receive notifications.
- **The receiver owns its buffer.** The data lands in the receiver's own GATT database, the
  write hook runs on the receiver's BLE RX thread, and the receiver decides how deep its
  queue is (`BLK_RX_POOL_DEPTH`).
- **Same air-time cost.** Write Without Response and notifications are both unacknowledged
  ATT PDUs with a 3-byte header, so throughput is unchanged. The credit scheme simply moves
  from `bt_gatt_notify_cb()` to `bt_gatt_write_without_response_cb()`, which has the same
  completion callback.
- **Small, rare control traffic.** Notifications are still the natural way for a server to
  talk back, but CTRL carries only ≤ 14-byte ACK / NACK / END frames (about one per 8 DATA
  frames at the default window) and short messages.

The cost is that the sender must be able to act as a GATT client: it needs
`CONFIG_BT_GATT_CLIENT` and discovers the service once per connection. A PC tool built on
`bleak` can send to a device but cannot receive from one, because bleak has no GATT server.

## MTU: why 244?

| Quantity | Value | Why |
|---|---|---|
| LL payload (with Data Length Extension) | 251 B | Controller maximum |
| ATT_MTU | **247** | 251 − 4 (L2CAP header) |
| Write / notification payload = frame | **244** | 247 − 3 (ATT opcode + handle) |
| Short-message payload | 242 | 244 − `len` − `type` |
| DATA chunk per frame | 240 | 244 − `len` − `type` − `xferId` − `seq` |

ATT_MTU can be negotiated up to 517 (512-byte attribute values). Above 247, though, every
frame is split across several LL packets. That gains little throughput, uses more RAM, and
cannot be described by a 1-byte length field. **247 / 244 is the right target.**

The frame size is chosen **per connection** as `min(ATT_MTU − 3, 244)`. iOS typically
negotiates MTU 185, which gives 182-byte frames; a link that never exchanges MTU gives
20-byte frames. The chunk size is fixed in the START frame, so both sides always agree. With
`b_autoTuneLink`, the Client exchanges MTU **before** discovery, so the chunk size is final
by the time it reports ready.

## Protocol

Types `0x00–0xEF` are **application types**. A message that fits in one frame is sent as
`[len][appType][data]`, with no handshake and no ACK, in either direction: client → server on
DATA, server → client on CTRL. Types `0xF0–0xFF` are reserved for the framework:

| Type | Frame | Characteristic | Payload |
|---|---|---|---|
| `0xF0` | START | DATA (client → server) | xferId, appType, totalLen(LE32), chunkSize, window, crc32(LE32) |
| `0xF1` | DATA | DATA | xferId, seq (frame index mod 256), data |
| `0xF2` | ACK | CTRL (server → client) | xferId, nextExpectedSeq, window (cumulative) |
| `0xF3` | NACK | CTRL | xferId, nextExpectedSeq, reason → client goes back |
| `0xF4` | END | CTRL | xferId, status (OK / CRC_ERROR / …) |
| `0xF5` | ABORT | DATA if sent by the client, CTRL if sent by the server | xferId, reason, direction (0 = by sender, 1 = by receiver) |

```
client (GATT client)                              server (GATT server)
  attach: [MTU exchange] → discover DATA/CTRL → subscribe CTRL
  START(id, type, len, chunk, W, crc)  ─WwR DATA─►  fpt_onRxStart() may reject → ABORT
                        ◄─CTRL ntf─ ACK(seq 0, W')   window = min(W, W')
  DATA 0 … DATA W-1                    ─WwR DATA─►  fpt_onRxData(offset, chunk) in order
                        ◄─CTRL ntf─ ACK(next)        every W/2 frames, or after 20 ms idle
  DATA …   (gap seen / RX pool full)   ─WwR DATA─►
                        ◄─CTRL ntf─ NACK(next)       client rewinds to `next` (Go-Back-N)
  DATA last                            ─WwR DATA─►  CRC check
                        ◄─CTRL ntf─ END(status)      both sides report the result
```

- The BLE link layer already acknowledges and orders packets. Loss can only happen when the
  receiving application drops a frame, for example when its RX queue is full. The NACK covers
  that immediately; the ACK timeout (1 s, 5 retries) covers a stalled peer.
- The client keeps **no retransmit buffer**. A resend re-reads the source at
  `frameIndex × chunkSize`, so the source must stay readable until `fpt_onTxDone`.
- The window can be at most 128, which keeps the 8-bit sequence number unambiguous. The
  engine tracks 32-bit absolute frame indices internally.
- Each characteristic fixes the direction of its frames. A frame that arrives on the wrong
  characteristic is dropped.
- The server ignores START from a client that is not subscribed to CTRL, since no answer
  could reach it.

## Integration

### Server (receiving device)

1. **Declare the service** in the GATT Configurator (generic-callback mode) with the UUIDs from
   [BulkXfer_Uuid.h](BulkXfer_Uuid.h):
   - **DATA:** Write + Write Without Response, variable length, length **244**. Set the custom write hook to e.g. `st_OnBulkData`.
   - **CTRL:** Notify (with CCC).
   - **Caps (optional):** Read, 4 bytes.

2. **Forward the hook** in the generated service `.c`:
   ```c
   static ssize_t st_OnBulkData(struct bt_conn *c, const struct bt_gatt_attr *a,
      const void *b, uint16_t l, uint16_t o, uint8_t f)
   {
      return gt_BLKS_DataWriteHook(c, a, b, l, o, f);
   }
   ```
   `gt_GATT_GenericWrite` still copies each frame into the descriptor buffer. BulkXfer
   ignores that copy and `u16_actualLen`, and uses the hook's `vpt_buf` / `u16_length` instead.

3. **Initialise and wire the connection callbacks:**
   ```c
   BlkSrvCfg_T cfg = {
      .stpt_ctrlAttr  = bt_gatt_find_by_uuid(svc.attrs, svc.attr_count, BT_UUID_BLK_CTRL),
      .fpt_onRxStart  = on_rx_start,   // optional: accept / reject by type & size
      .fpt_onRxData   = on_rx_data,    // in-order chunks, e.g. straight to flash
      .fpt_onRxDone   = on_rx_done,
      .fpt_onRxShort  = on_rx_short,
      .b_autoTuneLink = true,          // request 2M PHY, DLE 251, MTU exchange
   };
   gi_BLKS_Init(&cfg);
   // bt_conn_cb: connected    -> gv_BLKS_OnConnected(conn)
   //             disconnected -> gv_BLK_OnDisconnected(conn)
   ```

### Client (sending device)

```c
BlkCliCfg_T cfg = {                    // NULL UUIDs: BulkXfer_Uuid.h defaults
   .fpt_onReady    = on_ready,         // attach result
   .fpt_onTxDone   = on_tx_done,
   .fpt_onRxShort  = on_server_short,
   .b_autoTuneLink = true,
};
gi_BLKC_Init(&cfg);
// bt_conn_cb: connected    -> gi_BLKC_Attach(conn)   (either link role)
//             disconnected -> gv_BLK_OnDisconnected(conn)

// once on_ready(conn, 0) has run:
gi_BLKC_SendBuffer(MY_TYPE_LOG, buf, len);                   // RAM object
gi_BLKC_Send(MY_TYPE_FILE, &(BlkSource_T){ read_fn, ctx }, len);  // streamed
gi_BLKC_SendShort(MY_TYPE_CMD, data, n, K_MSEC(50));         // single frame
```

`gi_BLKC_Send()` reads the whole source once to compute the CRC, so start large transfers
from a work item rather than from a BulkXfer callback.

All callbacks run on the BulkXfer engine thread, never in BLE stack context. They may call
the API, for example to start the next transfer from `fpt_onTxDone`.

### Choosing roles at build time

`BLK_ENABLE_SERVER` (default 1) and `BLK_ENABLE_CLIENT` (default: `CONFIG_BT_GATT_CLIENT`)
select what is compiled. A receive-only device sets
`zephyr_compile_definitions(BLK_ENABLE_CLIENT=0)`, and a send-only device sets
`BLK_ENABLE_SERVER=0` and does not need `GATT_CB`.

### Threading model

| Context | Work |
|---|---|
| BLE RX thread | Server DATA hook and Client CTRL notify callback: check the frame, copy it into a slab block, queue it (never block). Client discovery callbacks post their result through atomics |
| BLE TX completion | Returns a write / notify credit |
| Timer ISR | Sets an event bit |
| Engine thread | Everything else: both state machines, ACK/NACK, sending, application callbacks |

Each role has its own credit semaphore. The Client allows `BLK_CLI_WRITE_INFLIGHT_MAX`
(default 6) DATA writes in flight, and the Server `BLK_SRV_NOTIFY_INFLIGHT_MAX` (default 2)
CTRL notifications. Their sum must stay below `CONFIG_BT_ATT_TX_COUNT` (checked at build
time), so the host never blocks on buffer allocation. Credits are restored on disconnect,
because the host does not call completion callbacks for PDUs lost with the link.

## Configuration

`BulkXfer_Config.h` holds the defaults (window 16, RX pool 20, timeouts, stack size, role
selection). Override any of them with compile definitions.

The `prj.conf` files of the examples are a throughput baseline for NCS 3.4.1. Their key
settings:

- `CONFIG_BT_L2CAP_TX_MTU=247`
- `BT_BUF_ACL_{RX,TX}_SIZE=251`, `BT_CTLR_DATA_LENGTH_MAX=251`
- `BT_USER_{PHY,DATA_LEN}_UPDATE=y`
- `BT_ATT_TX_COUNT=10`, `BT_CONN_TX_MAX=10`, `BT_BUF_ACL_TX_COUNT=10`
- `BT_BUF_EVT_RX_COUNT=11` (must exceed the ACL TX count)
- `BT_CTLR_SDC_MAX_CONN_EVENT_LEN_DEFAULT=4000000`
- a 7.5–15 ms connection interval
- `CONFIG_CRC=y`
- `CONFIG_BT_GATT_CLIENT=y` on the sender

## Examples and tests

| Path | What |
|---|---|
| `examples/bulk_server/` | nRF54L15 DK peripheral, Server role. Receives any transfer, reports status and throughput back as a short message, and echoes shorts |
| `examples/bulk_client/` | nRF54L15 DK central, Client role. Scans for the service, attaches, and sends 100 kB every 2 s, logging both sides' throughput |
| `tools/bulkxfer_client.py` | PC GATT client (`bleak`): `send <bytes>` with throughput, `ping`, `caps` |
| `tests/test_frame.c` | Host unit tests of the frame codec |
| `tests/test_engine.c` | Host tests of the **real engine** (both roles) against a simulated link and a scripted peer |

`tests/test_engine.c` covers these scenarios:
- Client: sizes 0 B to 100 kB, frame loss (NACK), silent server (timeout), window stall, MTU 23 with sequence wrap, local abort, disconnect with writes in flight (credit restore), send before attach
- Client attach: missing service or CTRL, subscribe failure, disconnect during discovery, MTU exchange first
- Server: sizes, RX pool overflow (NACK), CRC error, reject, unsubscribed client, local abort, disconnect, stale frames after reconnect, malformed DATA, wrong-channel frames
- Both roles: short messages on both channels, and simultaneous transfers in both directions on one link

It is built three times: both roles, server only, and client only.

```bash
# Build the samples (NCS 3.4.1)
west build -b nrf54l15dk/nrf54l15/cpuapp lib/BulkXfer/examples/bulk_server
west build -b nrf54l15dk/nrf54l15/cpuapp lib/BulkXfer/examples/bulk_client

# Host tests (any C99 compiler)
cd lib/BulkXfer/tests
gcc -std=c99 -Wall -Wextra -I.. -o test_frame test_frame.c ../BulkXfer_Frame.c && ./test_frame
F="-std=gnu99 -Wall -Wextra -Werror -Wno-missing-field-initializers -Ishim -I.. -DCONFIG_BT_GATT_CLIENT -DBLK_RX_POOL_DEPTH=6"
gcc $F -o test_engine test_engine.c ../BulkXfer_Frame.c && ./test_engine
gcc $F -DBLK_ENABLE_CLIENT=0 -o test_server test_engine.c ../BulkXfer_Frame.c && ./test_server
gcc $F -DBLK_ENABLE_SERVER=0 -o test_client test_engine.c ../BulkXfer_Frame.c && ./test_client

# On hardware: two DKs (bulk_client -> bulk_server), or a PC as the client
python tools/bulkxfer_client.py send 100000
```
