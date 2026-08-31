#!/usr/bin/env python3
import argparse
import hashlib
import re
import struct
import time
import zlib
from pathlib import Path

import serial

MAGIC = b"P25D"
DATA_TYPE = 0x01
HEADER_SIZE = 11
DEFAULT_CHUNK = 4096
JEDEC_SIZES = {
    "85-60-12": 256 * 1024,
    "85-60-13": 512 * 1024,
}


def crc32_update(crc, data):
    return zlib.crc32(data, crc) & 0xFFFFFFFF


def read_exact(ser, size, timeout):
    deadline = time.monotonic() + timeout
    data = bytearray()
    while len(data) < size:
        if time.monotonic() >= deadline:
            raise TimeoutError(f"timeout reading {size} bytes")
        part = ser.read(size - len(data))
        if part:
            data.extend(part)
    return bytes(data)


def read_frame(ser, timeout, size, chunk):
    window = bytearray()
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        byte = ser.read(1)
        if not byte:
            continue
        window.extend(byte)
        if len(window) > len(MAGIC):
            del window[:-len(MAGIC)]
        if bytes(window) == MAGIC:
            header = read_exact(ser, HEADER_SIZE, timeout)
            frame_type, seq, offset, length = struct.unpack("<BIIH", header)
            if length == 0 or length > chunk or offset + length > size:
                raise ValueError("invalid data frame geometry")
            payload = read_exact(ser, length, timeout)
            crc_bytes = read_exact(ser, 4, timeout)
            received_crc, = struct.unpack("<I", crc_bytes)
            actual_crc = crc32_update(0, header + payload)
            return frame_type, seq, offset, length, payload, received_crc, actual_crc
    raise TimeoutError("timeout waiting for data frame")


def wait_for_line(ser, pattern, timeout):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        line = ser.readline()
        if not line:
            continue
        text = line.decode("ascii", "replace").strip()
        print(f"< {text}")
        match = pattern.search(text)
        if match:
            return match
    raise TimeoutError("timeout waiting for device ready")


def open_serial(port, baud):
    ser = serial.Serial(port=port, baudrate=baud, timeout=0.25, write_timeout=2)
    ser.dtr = False
    ser.rts = True
    time.sleep(0.12)
    ser.rts = False
    return ser


def main():
    parser = argparse.ArgumentParser(description="Read a P25Q20H through the ESP32 dump firmware")
    parser.add_argument("port", nargs="?", default="COM20")
    parser.add_argument("outfile", nargs="?", default="p25q20h_dump.bin")
    parser.add_argument("-b", "--baud", type=int, default=921600)
    parser.add_argument("--ready-timeout", type=float, default=10)
    parser.add_argument("--frame-timeout", type=float, default=3)
    parser.add_argument("--retries", type=int, default=5)
    args = parser.parse_args()

    ready_pattern = re.compile(r"P25D READY id=([0-9A-Fa-f]{2})-([0-9A-Fa-f]{2})-([0-9A-Fa-f]{2}) size=(\d+) chunk=(\d+)")
    begin_pattern = re.compile(r"P25D BEGIN size=(\d+) chunk=(\d+)")
    end_pattern = re.compile(r"P25D END size=(\d+) crc32=([0-9A-Fa-f]{8})")

    output = Path(args.outfile)
    output.parent.mkdir(parents=True, exist_ok=True)
    ser = open_serial(args.port, args.baud)
    try:
        match = wait_for_line(ser, ready_pattern, args.ready_timeout)
        jedec = "-".join(match.group(i).upper() for i in range(1, 4))
        size = int(match.group(4))
        chunk = int(match.group(5))
        expected_size = JEDEC_SIZES.get(jedec)
        if expected_size is None:
            raise RuntimeError(f"unsupported JEDEC ID: {jedec}")
        if size != expected_size or chunk != DEFAULT_CHUNK:
            raise RuntimeError(f"unexpected device geometry: size={size}, chunk={chunk}")

        ser.write(b"GO\n")
        begin = wait_for_line(ser, begin_pattern, args.ready_timeout)
        if int(begin.group(1)) != size or int(begin.group(2)) != chunk:
            raise RuntimeError("device geometry changed during handshake")

        sha256 = hashlib.sha256()
        image_crc = 0
        with output.open("wb") as image:
            image.truncate(size)
            for seq, offset in enumerate(range(0, size, chunk)):
                length = min(chunk, size - offset)
                for attempt in range(1, args.retries + 1):
                    ser.write(f"GET {seq} {offset} {length}\n".encode("ascii"))
                    try:
                        frame_type, frame_seq, frame_offset, frame_length, payload, received_crc, actual_crc = read_frame(ser, args.frame_timeout, size, chunk)
                        valid = (
                            frame_type == DATA_TYPE
                            and frame_seq == seq
                            and frame_offset == offset
                            and frame_length == length
                            and received_crc == actual_crc
                        )
                    except (TimeoutError, ValueError, struct.error):
                        valid = False
                        payload = b""
                        actual_crc = 0
                        received_crc = 0
                    if valid:
                        image.seek(offset)
                        image.write(payload)
                        sha256.update(payload)
                        image_crc = crc32_update(image_crc, payload)
                        ser.write(f"ACK {seq}\n".encode("ascii"))
                        print(f"{offset + length:>6}/{size} bytes")
                        break
                    ser.write(f"NACK {seq}\n".encode("ascii"))
                    if attempt == args.retries:
                        raise RuntimeError(f"chunk {seq} failed after {args.retries} attempts")
                else:
                    raise RuntimeError(f"chunk {seq} failed")

        ser.write(b"FINISH\n")
        end_line = ser.readline().decode("ascii", "replace").strip()
        print(f"< {end_line}")
        end = end_pattern.fullmatch(end_line)
        if not end or int(end.group(1)) != size:
            raise RuntimeError("invalid device END response")
        device_crc = int(end.group(2), 16)
        if device_crc != image_crc:
            raise RuntimeError(f"whole-image CRC mismatch: device={device_crc:08X}, pc={image_crc:08X}")

        print(f"output: {output}")
        print(f"size: {output.stat().st_size}")
        print(f"JEDEC ID: {jedec}")
        print(f"CRC32: {image_crc:08X}")
        print(f"SHA256: {sha256.hexdigest()}")
    finally:
        ser.close()


if __name__ == "__main__":
    main()
