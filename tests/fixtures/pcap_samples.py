import argparse
from pathlib import Path
import struct


BSSID = bytes.fromhex("020000000001")
SUPPORTED_RATES = b"\x01\x01\x82"
BEACON = (
    bytes.fromhex("80000000ffffffffffff")
    + BSSID * 2
    + bytes.fromhex("0000")
    + struct.pack("<QHH", 0, 100, 1)
    + b"\x00\x09AirRounds" + SUPPORTED_RATES + b"\x03\x01\x06"
)


def record(frame=BEACON, *, order="<", fraction=123456, original=None):
    if original is None:
        original = len(frame)
    return struct.pack(order + "IIII", 1, fraction, len(frame), original) + frame


def header(*, order="<", nano=False, link_type=105, snaplen=65535, major=2, minor=4):
    magic = 0xA1B23C4D if nano else 0xA1B2C3D4
    return struct.pack(order + "IHHIIII", magic, major, minor, 0, 0, snaplen, link_type)


def samples():
    valid = header() + record()
    return {
        "valid": valid,
        "truncated": valid[:-1],
        "damaged": header() + record(original=len(BEACON) - 1),
        "unsupported": header(link_type=147) + record(),
    }


def main():
    parser = argparse.ArgumentParser(description="Write synthetic T3 samples.")
    parser.add_argument("directory", type=Path, help="existing output directory")
    args = parser.parse_args()
    for label, data in samples().items():
        with (args.directory / (label + ".pcap")).open("xb") as output:
            output.write(data)


if __name__ == "__main__":
    main()
