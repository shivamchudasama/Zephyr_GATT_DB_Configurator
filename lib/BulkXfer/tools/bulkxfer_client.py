#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""
BulkXfer reference central (PC side) for testing the nRF54 echo peripheral.

Implements the central half of the BulkXfer protocol on top of `bleak`:
  - sends objects with START / DATA / windowed ACK / Go-Back-N / CRC-32
  - receives objects the same way and acknowledges them
  - sends and receives single-frame ("short") messages

Usage:
    pip install bleak
    python bulkxfer_client.py ping
    python bulkxfer_client.py echo 100000          # send 100 kB, get it back, verify
    python bulkxfer_client.py echo 20000 --address AA:BB:CC:DD:EE:FF
"""

import argparse
import asyncio
import os
import struct
import sys
import time
import zlib

from bleak import BleakClient, BleakScanner

RX_UUID = "b1c00002-2f5b-4e6a-9c1d-7a3e5f8b0c21"   # central -> device (write)
TX_UUID = "b1c00003-2f5b-4e6a-9c1d-7a3e5f8b0c21"   # device -> central (notify)
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
RX_ACK_DELAY = 0.02


def frame(ftype: int, payload: bytes) -> bytes:
    return bytes([len(payload), ftype]) + payload


def seq_to_abs(seq: int, base: int) -> int:
    return base + ((seq - base) & 0xFF)


class BulkXferCentral:
    def __init__(self, client: BleakClient):
        self.client = client
        self.frame_cap = min(client.mtu_size - 3, MAX_FRAME)
        self.ctrl_q: asyncio.Queue = asyncio.Queue()    # ACK/NACK/END/ABORT for our TX
        self.rx_q: asyncio.Queue = asyncio.Queue()      # START/DATA/ABORT for our RX
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
        elif ftype in (T_ACK, T_NACK, T_END):
            self.ctrl_q.put_nowait(data)
        elif ftype == T_ABORT:
            (self.ctrl_q if data[4] == ABORT_BY_RECEIVER else self.rx_q).put_nowait(data)
        else:
            self.rx_q.put_nowait(data)

    async def write(self, data: bytes):
        await self.client.write_gatt_char(RX_UUID, data, response=False)

    # ---- short messages ----------------------------------------------------
    async def send_short(self, app_type: int, payload: bytes):
        assert len(payload) <= self.frame_cap - 2
        await self.write(frame(app_type, payload))

    # ---- central -> device transfer ---------------------------------------
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
                return "ABORTED_BY_DEVICE:" + STATUS.get(f[3], hex(f[3]))

    # ---- device -> central transfer ---------------------------------------
    async def receive(self, timeout: float = 30.0):
        """Wait for one incoming transfer. Returns (app_type, bytes, status)."""
        f = await asyncio.wait_for(self._next_rx(T_START), timeout)
        xid, app_type, total, chunk, win, crc_exp = struct.unpack("<BBIBBI", f[2:14])
        win = min(win, WINDOW)
        frames = (total + chunk - 1) // chunk
        buf = bytearray()
        nxt = since_ack = 0
        nack_sent = False

        async def ack():
            nonlocal since_ack
            since_ack = 0
            await self.write(frame(T_ACK, bytes([xid, nxt & 0xFF, win])))

        if frames == 0:
            status = 0 if zlib.crc32(b"") == crc_exp else 1
            await self.write(frame(T_END, bytes([xid, status])))
            return app_type, bytes(buf), STATUS[status]
        await ack()

        while True:
            try:
                f = await asyncio.wait_for(self.rx_q.get(), RX_ACK_DELAY)
            except asyncio.TimeoutError:
                if since_ack:
                    await ack()                          # delayed ACK
                continue
            if f[1] == T_ABORT and f[2] == xid:
                return app_type, bytes(buf), "ABORTED_BY_DEVICE"
            if f[1] == T_START and f[2] == xid:
                await ack()                              # repeated START
                continue
            if f[1] != T_DATA or f[2] != xid:
                continue
            diff = (f[3] - nxt) & 0xFF
            if diff == 0:
                buf += f[4:]
                nxt += 1
                since_ack += 1
                nack_sent = False
                if nxt == frames:
                    status = 0 if zlib.crc32(bytes(buf)) == crc_exp else 1
                    await self.write(frame(T_END, bytes([xid, status])))
                    return app_type, bytes(buf), STATUS[status]
                if since_ack >= max(1, win // 2):
                    await ack()
            elif diff < 128:
                if not nack_sent:
                    nack_sent = True
                    await self.write(frame(T_NACK, bytes([xid, nxt & 0xFF, 11])))
            else:
                await ack()                              # duplicate

    async def _next_rx(self, ftype: int) -> bytes:
        while True:
            f = await self.rx_q.get()
            if f[1] == ftype:
                return f


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
    ap.add_argument("command", choices=["ping", "echo", "caps"])
    ap.add_argument("size", nargs="?", type=int, default=20000)
    ap.add_argument("--address")
    ap.add_argument("--name", default="BulkXfer")
    args = ap.parse_args()

    target = await find_device(args.address, args.name)
    async with BleakClient(target) as client:
        central = BulkXferCentral(client)
        await client.start_notify(TX_UUID, central.on_notify)
        print(f"connected, ATT MTU {client.mtu_size} -> {central.frame_cap} B frames, "
              f"{central.frame_cap - 4} B per DATA frame")

        if args.command == "caps":
            ver, max_frame, window, _ = await client.read_gatt_char(CAPS_UUID)
            print(f"protocol v{ver}, max frame {max_frame} B, window {window}")

        elif args.command == "ping":
            t0 = time.perf_counter()
            await central.send_short(0x01, b"ping")
            app_type, payload = await asyncio.wait_for(central.short_q.get(), 5.0)
            print(f"echo type 0x{app_type:02x} {payload!r} in {(time.perf_counter() - t0) * 1e3:.1f} ms")

        else:
            data = os.urandom(args.size)
            t0 = time.perf_counter()
            status = await central.send(0x10, data)
            dt = time.perf_counter() - t0
            print(f"central -> device: {len(data)} B, {status}, {len(data) * 8 / dt / 1000:.0f} kbit/s")
            if status != "OK":
                sys.exit(1)

            t0 = time.perf_counter()
            app_type, echo, status = await central.receive()
            dt = time.perf_counter() - t0
            print(f"device -> central: {len(echo)} B, {status}, {len(echo) * 8 / dt / 1000:.0f} kbit/s")
            ok = status == "OK" and echo == data and app_type == 0x10
            print("echo verified" if ok else "ECHO MISMATCH")
            sys.exit(0 if ok else 1)


if __name__ == "__main__":
    asyncio.run(main())
