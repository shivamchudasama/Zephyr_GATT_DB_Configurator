#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""
BulkXfer reference GATT client (PC side) for testing examples/bulk_server.

Implements the Client role of BulkXfer protocol v2 on top of `bleak`:
  - writes START / DATA frames into the server's DATA characteristic with
    Write Without Response (windowed ACK, Go-Back-N, CRC-32)
  - receives ACK / NACK / END / ABORT and short messages as CTRL notifications
  - sends single-frame ("short") messages

The PC cannot receive bulk data: in v2 the receiver must host the GATT server,
and bleak has no GATT server. Use examples/bulk_client on a second board for
device -> device transfers.

Usage:
    pip install bleak
    python bulkxfer_client.py ping
    python bulkxfer_client.py send 100000
    python bulkxfer_client.py send 20000 --address AA:BB:CC:DD:EE:FF
"""

import argparse
import asyncio
import os
import struct
import sys
import time
import zlib

from bleak import BleakClient, BleakScanner

DATA_UUID = "b1c00002-2f5b-4e6a-9c1d-7a3e5f8b0c21"   # client -> server (write w/o response)
CTRL_UUID = "b1c00003-2f5b-4e6a-9c1d-7a3e5f8b0c21"   # server -> client (notify)
CAPS_UUID = "b1c00004-2f5b-4e6a-9c1d-7a3e5f8b0c21"

T_START, T_DATA, T_ACK, T_NACK, T_END, T_ABORT = 0xF0, 0xF1, 0xF2, 0xF3, 0xF4, 0xF5
ABORT_BY_SENDER, ABORT_BY_RECEIVER = 0, 1
STATUS = {0: "OK", 1: "CRC_ERROR", 2: "TIMEOUT", 3: "ABORTED", 4: "REMOTE_ABORTED",
          5: "REJECTED", 6: "DISCONNECTED", 7: "SOURCE_ERROR", 8: "SINK_ERROR",
          9: "PROTOCOL_ERROR", 10: "NO_RESOURCES", 11: "OUT_OF_ORDER"}
MAX_FRAME = 244
WINDOW = 16
ACK_TIMEOUT = 1.0
MAX_RETRIES = 5
APP_TYPE_RESULT, APP_TYPE_PING = 0x01, 0x02


def frame(ftype: int, payload: bytes) -> bytes:
    return bytes([len(payload), ftype]) + payload


def seq_to_abs(seq: int, base: int) -> int:
    return base + ((seq - base) & 0xFF)


class BulkXferClient:
    def __init__(self, client: BleakClient):
        self.client = client
        self.frame_cap = min(client.mtu_size - 3, MAX_FRAME)
        self.ctrl_q: asyncio.Queue = asyncio.Queue()    # ACK/NACK/END/ABORT
        self.short_q: asyncio.Queue = asyncio.Queue()
        self.xfer_id = 0

    # ---- plumbing ----------------------------------------------------------
    def on_notify(self, _handle, data: bytearray):
        data = bytes(data)
        if len(data) < 2 or data[0] + 2 != len(data):
            print(f"! malformed frame {data.hex()}")
            return
        ftype = data[1]
        if ftype <= 0xEF:
            self.short_q.put_nowait((ftype, data[2:]))
        elif ftype in (T_ACK, T_NACK, T_END) or (ftype == T_ABORT and data[4] == ABORT_BY_RECEIVER):
            self.ctrl_q.put_nowait(data)
        else:
            print(f"! frame 0x{ftype:02x} is not valid on CTRL")

    async def write(self, data: bytes):
        await self.client.write_gatt_char(DATA_UUID, data, response=False)

    # ---- short messages ----------------------------------------------------
    async def send_short(self, app_type: int, payload: bytes):
        assert len(payload) <= self.frame_cap - 2
        await self.write(frame(app_type, payload))

    # ---- client -> server transfer ----------------------------------------
    async def send(self, app_type: int, data: bytes) -> str:
        self.xfer_id = (self.xfer_id + 1) & 0xFF
        xid = self.xfer_id
        chunk = self.frame_cap - 4
        frames = (len(data) + chunk - 1) // chunk
        start = frame(T_START, struct.pack("<BBIBBI", xid, app_type, len(data), chunk,
                                           WINDOW, zlib.crc32(data)))
        window = WINDOW
        acked = nxt = 0
        started = False
        retries = 0
        await self.write(start)

        while True:
            if started:
                while nxt < frames and nxt - acked < window:
                    off = nxt * chunk
                    await self.write(frame(T_DATA, bytes([xid, nxt & 0xFF]) + data[off:off + chunk]))
                    nxt += 1
            try:
                f = await asyncio.wait_for(self.ctrl_q.get(), ACK_TIMEOUT)
            except asyncio.TimeoutError:
                retries += 1
                if retries > MAX_RETRIES:
                    await self.write(frame(T_ABORT, bytes([xid, 2, ABORT_BY_SENDER])))
                    return "TIMEOUT"
                if not started:
                    await self.write(start)
                nxt = acked                              # Go-Back-N
                continue
            if f[2] != xid:
                continue
            if f[1] == T_ACK:
                if not started:
                    if f[3] == 0:
                        started, window = True, max(1, min(window, f[4]))
                    continue
                a = seq_to_abs(f[3], acked)
                if acked < a <= nxt:
                    acked, retries = a, 0
            elif f[1] == T_NACK:
                a = seq_to_abs(f[3], acked)
                if a <= nxt:
                    acked = nxt = a
            elif f[1] == T_END:
                return STATUS.get(f[3], hex(f[3]))
            elif f[1] == T_ABORT:
                return "ABORTED_BY_SERVER:" + STATUS.get(f[3], hex(f[3]))


async def find_device(address, name):
    if address:
        return address
    print(f"scanning for '{name}' ...")
    dev = await BleakScanner.find_device_by_name(name, timeout=10.0)
    if dev is None:
        sys.exit(f"device '{name}' not found")
    return dev


async def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("command", choices=["ping", "send", "caps"])
    ap.add_argument("size", nargs="?", type=int, default=20000)
    ap.add_argument("--address")
    ap.add_argument("--name", default="BulkXfer")
    args = ap.parse_args()

    target = await find_device(args.address, args.name)
    async with BleakClient(target) as client:
        blk = BulkXferClient(client)
        await client.start_notify(CTRL_UUID, blk.on_notify)
        print(f"connected, ATT MTU {client.mtu_size} -> {blk.frame_cap} B frames, "
              f"{blk.frame_cap - 4} B per DATA frame")

        if args.command == "caps":
            ver, max_frame, window, _ = await client.read_gatt_char(CAPS_UUID)
            print(f"protocol v{ver}, max frame {max_frame} B, window {window}")

        elif args.command == "ping":
            t0 = time.perf_counter()
            await blk.send_short(APP_TYPE_PING, b"ping")
            app_type, payload = await asyncio.wait_for(blk.short_q.get(), 5.0)
            print(f"echo type 0x{app_type:02x} {payload!r} in {(time.perf_counter() - t0) * 1e3:.1f} ms")

        else:
            data = os.urandom(args.size)
            t0 = time.perf_counter()
            status = await blk.send(0x10, data)
            dt = time.perf_counter() - t0
            print(f"client -> server: {len(data)} B, {status}, {len(data) * 8 / dt / 1000:.0f} kbit/s")
            try:
                app_type, payload = await asyncio.wait_for(blk.short_q.get(), 2.0)
                if app_type == APP_TYPE_RESULT and len(payload) >= 9:
                    st, n, kbps = struct.unpack("<BII", payload[:9])
                    print(f"server reports: {STATUS.get(st, hex(st))}, {n} B, {kbps} kbit/s")
            except asyncio.TimeoutError:
                pass
            sys.exit(0 if status == "OK" else 1)


if __name__ == "__main__":
    asyncio.run(main())
