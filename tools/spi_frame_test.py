#!/usr/bin/env python3
"""Reads frames from the STM32 node over SPI and checks them.

Runs on the Raspberry Pi (SPI enabled, python3-spidev installed), connected to the
Nucleo as in the README. The frame layout and the CRC are in
firmware/stm32-sensor-node/Src/frame.h and docs/decisions/0010-spi-link-design.md.

    python3 spi_frame_test.py poll --count 60 --period 1.0 --verbose
    python3 spi_frame_test.py gap
    python3 spi_frame_test.py speed

poll   N reads at a fixed period.
gap    reads with a shrinking pause between them (100 ms down to 0): shows whether the
       node is ready again in time.
speed  the same reads at several SPI clocks (100 kHz up to 6 MHz).

Each read is classified as:
  good     marker A5 in byte 0 and a valid CRC
  shifted  the frame starts one byte late (a leftover byte went out first)
  restart  the header repeats later in the frame (the DMA was restarted in the middle)
  bad      anything else (bits damaged on the wire, a dead node, ...)
"""
import argparse
import time

import spidev

FRAME_SIZE = 14
STATUS = {0: "OK", 1: "NO_READING", 2: "NACK", 3: "TIMEOUT", 4: "BUS", 5: "ID", 6: "NO_DATA"}


def crc16(data, crc=0xFFFF):
    """CRC-16/CCITT-FALSE: polynomial 0x1021, start value 0xFFFF, no reflection, no final XOR."""
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def classify(rx):
    if rx[0] == 0xA5 and crc16(rx[:12]) == (rx[12] | (rx[13] << 8)):
        return "good"
    if rx[1] == 0xA5:
        return "shifted"
    if rx[0] == 0xA5 and any(rx[k] == 0xA5 and rx[k + 1] == rx[1] and rx[k + 2] == rx[2]
                              for k in range(3, 12)):
        return "restart"
    return "bad"


def open_spi(speed_hz):
    spi = spidev.SpiDev()
    spi.open(0, 0)
    spi.mode = 0
    spi.max_speed_hz = speed_hz
    return spi


def run(spi, label, count, pause, verbose):
    good = shifted = bad = jumps = 0
    last = None
    statuses = {}
    for i in range(count):
        rx = spi.xfer2([0] * FRAME_SIZE)
        kind = classify(rx)
        text = " ".join("%02X" % b for b in rx)
        if kind == "good":
            good += 1
            seq = rx[2] | (rx[3] << 8)
            statuses[rx[1]] = statuses.get(rx[1], 0) + 1
            if last is not None and not 0 <= seq - last <= 3:
                jumps += 1
                print("%4d sequence jump %d -> %d" % (i, last, seq))
            last = seq
            if verbose:
                t = int.from_bytes(bytes(rx[4:8]), "little", signed=True) / 100.0
                p = int.from_bytes(bytes(rx[8:12]), "little") / 100.0
                print("%4d ok  seq=%d status=%s T=%.2f C P=%.2f hPa"
                      % (i, seq, STATUS.get(rx[1], rx[1]), t, p))
        elif kind == "shifted":
            shifted += 1
            print("%4d SHIFTED %s" % (i, text))
        else:
            bad += 1
            print("%4d %-7s %s" % (i, kind.upper(), text))
        if pause > 0:
            time.sleep(pause)
    names = ", ".join("%s:%d" % (STATUS.get(k, k), v) for k, v in sorted(statuses.items()))
    print("%-24s good %5d  shifted %4d  bad %4d  seq-jumps %3d  [%s]"
          % (label, good, shifted, bad, jumps, names))


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("poll", help="N reads at a fixed period")
    p.add_argument("--count", type=int, default=100)
    p.add_argument("--period", type=float, default=1.0)
    p.add_argument("--speed", type=int, default=1000000)
    p.add_argument("--verbose", action="store_true")
    g = sub.add_parser("gap", help="reads with a shrinking pause between them")
    g.add_argument("--count", type=int, default=200)
    g.add_argument("--speed", type=int, default=1000000)
    s = sub.add_parser("speed", help="the same reads at several SPI clocks")
    s.add_argument("--count", type=int, default=200)
    a = ap.parse_args()

    if a.cmd == "poll":
        spi = open_spi(a.speed)
        run(spi, "poll %d Hz" % a.speed, a.count, a.period, a.verbose)
    elif a.cmd == "gap":
        spi = open_spi(a.speed)
        for pause in (0.1, 0.01, 0.005, 0.002, 0.001, 0.0005, 0.0002, 0):
            run(spi, "gap %.1f ms" % (pause * 1000), a.count, pause, False)
    else:
        for speed in (100000, 500000, 1000000, 2000000, 4000000, 6000000):
            spi = open_spi(speed)
            run(spi, "speed %d Hz" % speed, a.count, 0.005, False)
            spi.close()


if __name__ == "__main__":
    main()
