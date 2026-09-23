# BulkXfer — reliable bulk data transfer over BLE GATT (Zephyr / nRF54)

BulkXfer moves objects of any size (logs, files, images, firmware chunks) in
**both directions** over two GATT characteristics. It sits on top of the
[`GATT_CB`](../GATT_CB) generic callbacks without modifying them.

- **Frame format:** `len(1) + type(1) + payload(≤242)`, one frame per GATT write / notification.
- **Reliability:** windowed cumulative ACKs, Go-Back-N retransmit, CRC-32 over the whole object.
- **Throughput:** keeps the controller queue full with notifications (device → central). The central writes with Write Without Response.
- **Streaming:** data is pulled from a source callback and pushed to a sink callback, so objects never have to fit in RAM.

## MTU: why 244?

| Quantity | Value | Why |
|---|---|---|
| LL payload (with Data Length Extension) | 251 B | Controller maximum |
| ATT_MTU | **247** | 251 − 4 (L2CAP header) |
| Notification / write payload = frame | **244** | 247 − 3 (ATT opcode + handle) |
| Short-message payload | 242 | 244 − `len` − `type` |
| DATA chunk per frame | 240 | 244 − `len` − `type` − `xferId` − `seq` |

ATT_MTU can be negotiated up to 517 (512-byte attribute values). Above 247, though, every
frame is split across several LL packets. That gains little throughput, uses more RAM, and
cannot be described by a 1-byte length field. **247 / 244 is the right target.**

The frame size is chosen **per connection** as `min(ATT_MTU − 3, 244)`. iOS typically
negotiates MTU 185, which gives 182-byte frames; a central that never exchanges MTU gives
20-byte frames. The chunk size is fixed in the START frame, so both sides always agree.

## Protocol

Types `0x00–0xEF` are **application types**. A message that fits in one frame is sent as
`[len][appType][data]`, with no handshake and no ACK. Types `0xF0–0xFF` are reserved for the framework:

| Type | Frame | Direction | Payload |
|---|---|---|---|
| `0xF0` | START | sender → receiver | xferId, appType, totalLen(LE32), chunkSize, window, crc32(LE32) |
| `0xF1` | DATA | sender → receiver | xferId, seq (frame index mod 256), data |
| `0xF2` | ACK | receiver → sender | xferId, nextExpectedSeq, window (cumulative) |
| `0xF3` | NACK | receiver → sender | xferId, nextExpectedSeq, reason → sender goes back |
| `0xF4` | END | receiver → sender | xferId, status (OK / CRC_ERROR / …) |
| `0xF5` | ABORT | either | xferId, reason, direction (0 = by sender, 1 = by receiver) |

```
sender                                   receiver
  START(id, type, len, chunk, W, crc) ──►   fpt_onRxStart() may reject → ABORT
                        ◄── ACK(seq 0, W')   window = min(W, W')
  DATA 0 … DATA W-1                  ──►   fpt_onRxData(offset, chunk) in order
                        ◄── ACK(next)        every W/2 frames, or after 20 ms idle
  DATA …   (gap seen / RX pool full) ──►
                        ◄── NACK(next)       sender rewinds to `next` (Go-Back-N)
  DATA last                          ──►   CRC check
                        ◄── END(status)      both sides report the result
```

- The BLE link layer already acknowledges and orders packets. Loss can only happen when the
  receiving application drops a frame (RX queue full, OS buffer overflow on a phone). The
  NACK covers that immediately; the ACK timeout (1 s, 5 retries) covers a stalled peer.
- The sender keeps **no retransmit buffer**. A resend re-reads the source at
  `frameIndex × chunkSize`, so the source must stay readable until `fpt_onTxDone`.
- The window can be at most 128, which keeps the 8-bit sequence number unambiguous. The
  engine tracks 32-bit absolute frame indices internally.
- Each side can run one transfer per direction at the same time.

## Integration

1. **Declare the service** in the GATT Configurator (generic-callback mode):
   - **RX:** Write + Write Without Response, variable length, length **244**. Set the custom write hook to e.g. `st_OnBulkRx`.
   - **TX:** Notify (with CCC).
   - **Caps (optional):** Read, 4 bytes.

2. **Forward the hook** in the generated service `.c`:
   ```c
   static ssize_t st_OnBulkRx(struct bt_conn *c, const struct bt_gatt_attr *a,
      const void *b, uint16_t l, uint16_t o, uint8_t f)
   {
      return gt_BLK_RxWriteHook(c, a, b, l, o, f);
   }
   ```
   `gt_GATT_GenericWrite` still copies each frame into the descriptor buffer. BulkXfer
   ignores that copy and `u16_actualLen`, and uses the hook's `vpt_buf` / `u16_length` instead.

3. **Initialise and wire the connection callbacks:**
   ```c
   BlkCfg_T cfg = {
      .stpt_txAttr    = bt_gatt_find_by_uuid(svc.attrs, svc.attr_count, TX_UUID),
      .fpt_onRxStart  = on_rx_start,   // optional: accept / reject by type & size
      .fpt_onRxData   = on_rx_data,    // in-order chunks, e.g. straight to flash
      .fpt_onRxDone   = on_rx_done,
      .fpt_onRxShort  = on_rx_short,
      .fpt_onTxDone   = on_tx_done,
      .b_autoTuneLink = true,          // request 2M PHY, DLE 251, MTU exchange
   };
   gi_BLK_Init(&cfg);
   // bt_conn_cb: connected -> gv_BLK_OnConnected(conn)
   //             disconnected -> gv_BLK_OnDisconnected(conn)
   ```

4. **Send:**
   ```c
   gi_BLK_SendBuffer(MY_TYPE_LOG, buf, len);               // RAM object
   gi_BLK_Send(MY_TYPE_FILE, &(BlkSource_T){ read_fn, ctx }, len);  // streamed
   gi_BLK_SendShort(MY_TYPE_CMD, data, n, K_MSEC(50));     // single frame
   ```

All callbacks run on the BulkXfer engine thread, never in BLE stack context. They may call
the API, for example to start the next transfer from `fpt_onTxDone`.

### Threading model

| Context | Work |
|---|---|
| BLE RX thread | `gt_BLK_RxWriteHook` validates the frame, copies it into a slab block and queues it (never blocks) |
| BLE TX completion | returns a TX credit |
| Timer ISR | sets an event bit |
| Engine thread | everything else: state machines, ACK/NACK, sending, application callbacks |

The TX credit semaphore (`BLK_TX_INFLIGHT_MAX`, default 6) bounds the notifications queued
in the host. It must stay below `CONFIG_BT_ATT_TX_COUNT`, so `bt_gatt_notify_cb()` never
blocks on buffer allocation. Credits are restored on disconnect, because the host does not
call completion callbacks for notifications lost with the link.

## Configuration

`BulkXfer_Config.h` holds the defaults (window 16, RX pool 20, timeouts, stack size). Override any of them with compile definitions.

The `prj.conf` of [`examples/peripheral_echo`](examples/peripheral_echo/prj.conf) is a
verified throughput baseline for NCS 3.4.1. Its key settings:

- `CONFIG_BT_L2CAP_TX_MTU=247`
- `BT_BUF_ACL_{RX,TX}_SIZE=251`, `BT_CTLR_DATA_LENGTH_MAX=251`
- `BT_USER_{PHY,DATA_LEN}_UPDATE=y`
- `BT_ATT_TX_COUNT=10`, `BT_CONN_TX_MAX=10`, `BT_BUF_ACL_TX_COUNT=10`
- `BT_BUF_EVT_RX_COUNT=11` (must exceed the ACL TX count)
- `BT_CTLR_SDC_MAX_CONN_EVENT_LEN_DEFAULT=4000000`
- a 7.5–15 ms connection interval
- `CONFIG_CRC=y`

## Example and tests

| Path | What |
|---|---|
| `examples/peripheral_echo/` | nRF54L15 DK echo peripheral: echoes every transfer and short message back. |
| `tools/bulkxfer_client.py` | PC central (`bleak`): `ping`, `echo <bytes>` with verification and throughput, `caps`. |
| `tests/test_frame.c` | Host unit tests of the frame codec. |
| `tests/test_engine.c` | Host tests of the **real engine** against a simulated link and scripted central. |

`tests/test_engine.c` covers these scenarios:
- sizes 0 B to 100 kB
- MTU 23 and 247
- sequence wrap
- frame loss (NACK)
- RX pool overflow
- silent peer (timeout)
- CRC error
- reject
- local abort
- disconnect with notifications in flight
- stale frames after reconnect
- window stall
- malformed DATA

```bash
# Build the sample (NCS 3.4.1)
west build -b nrf54l15dk/nrf54l15/cpuapp lib/BulkXfer/examples/peripheral_echo

# Host tests (any C99 compiler)
cd lib/BulkXfer/tests
gcc -std=c99 -Wall -Wextra -I.. -o test_frame test_frame.c ../BulkXfer_Frame.c && ./test_frame
gcc -std=gnu99 -Wall -Wextra -Wno-missing-field-initializers -Ishim -I.. -DBLK_RX_POOL_DEPTH=6 \
    -o test_engine test_engine.c ../BulkXfer_Frame.c && ./test_engine

# On hardware
python tools/bulkxfer_client.py echo 100000
```
