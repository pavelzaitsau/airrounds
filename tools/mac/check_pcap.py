import argparse
from pathlib import Path
import re
import struct
import sys
import zlib


FORMATS = {
    b"\xd4\xc3\xb2\xa1": ("<", 1_000_000),
    b"\xa1\xb2\xc3\xd4": (">", 1_000_000),
    b"\x4d\x3c\xb2\xa1": ("<", 1_000_000_000),
    b"\xa1\xb2\x3c\x4d": (">", 1_000_000_000),
}
LINK_TYPES = {1: "Ethernet", 105: "raw 802.11"}
MAX_RECORD_SIZE = 256 * 1024
RECEIPT = struct.Struct("<8sQI")
RECEIPT_MAGIC = b"ARPCAP\x01\x00"


class CheckError(Exception):
    pass


def read_exact(stream, size, field):
    data = stream.read(size)
    if len(data) != size:
        raise CheckError(f"truncated: {field}")
    return data


def observed_ssid(frame, bssid):
    # Beacon and probe response share a 24-byte header and 12 fixed body bytes.
    if len(frame) < 38 or frame[0] not in (0x80, 0x50) or frame[10:22] != bssid * 2:
        return None
    # Skip protected, fragmented, DS, and optional-header forms.
    if frame[1] & 0xC7 or frame[22] & 0x0F:
        return None

    ssid_id, ssid_size = frame[36:38]
    offset = 38 + ssid_size
    if ssid_id != 0 or ssid_size > 32 or len(frame) - offset < 2:
        return None
    # On 2.4 GHz the Supported Rates element follows the SSID.
    if frame[offset] != 1 or not 1 <= frame[offset + 1] <= 8:
        return None
    ssid = frame[38:offset]

    while offset < len(frame):
        if len(frame) - offset < 2:
            return None
        element_id, size = frame[offset:offset + 2]
        offset += 2
        end = offset + size
        if end > len(frame) or element_id == 0:
            return None
        offset = end
    return ssid if any(ssid) else b""


def check_records(stream, bssid, verify_receipt):
    magic = read_exact(stream, 4, "file magic")
    if magic not in FORMATS:
        raise CheckError("unsupported: expected classic PCAP")
    order, fractions_per_second = FORMATS[magic]
    header = read_exact(stream, 20, "file header")
    major, minor, snaplen, link_type = struct.unpack(order + "HH8xII", header)
    if (major, minor) != (2, 4):
        raise CheckError(f"unsupported: PCAP version {major}.{minor}")
    if link_type not in LINK_TYPES:
        raise CheckError(f"unsupported: link type or flags 0x{link_type:08x}")
    if bssid is not None and link_type != 105:
        raise CheckError("unsupported: a BSSID check requires raw 802.11")
    if snaplen == 0:
        raise CheckError("damaged: snapshot length is zero")

    size = len(magic) + len(header)
    crc = zlib.crc32(magic + header) if verify_receipt else 0
    record_header = struct.Struct(order + "IIII")
    count = 0
    ssid = None
    while record := stream.read(record_header.size):
        count += 1
        if len(record) != record_header.size:
            raise CheckError(f"truncated: record {count} header")
        _, fraction, captured, original = record_header.unpack(record)
        if captured == 0 or captured > snaplen or captured > original:
            raise CheckError(f"damaged: record {count} length")
        if fraction >= fractions_per_second:
            raise CheckError(f"damaged: record {count} timestamp fraction")
        if captured > MAX_RECORD_SIZE:
            raise CheckError(f"unsupported: record {count} exceeds {MAX_RECORD_SIZE} bytes")

        frame = read_exact(stream, captured, f"record {count} data")
        size += len(record) + len(frame)
        if verify_receipt:
            crc = zlib.crc32(frame, zlib.crc32(record, crc))
        if bssid is not None and not ssid and captured == original:
            candidate = observed_ssid(frame, bssid)
            if candidate is not None:
                ssid = candidate

    return LINK_TYPES[link_type], count, size, crc, ssid


def check_receipt(path, size, crc):
    with path.open("rb") as stream:
        data = stream.read(RECEIPT.size + 1)
    if len(data) != RECEIPT.size:
        raise CheckError("damaged: receipt size")
    magic, expected_size, expected_crc = RECEIPT.unpack(data)
    if magic != RECEIPT_MAGIC:
        raise CheckError("damaged: receipt magic")
    if size < expected_size:
        raise CheckError("truncated: capture is shorter than the receipt size")
    if size != expected_size or crc != expected_crc:
        raise CheckError("damaged: capture size or checksum differs from the receipt")


def parse_bssid(value):
    if re.fullmatch(r"[0-9a-fA-F]{2}(?::[0-9a-fA-F]{2}){5}", value) is None:
        raise argparse.ArgumentTypeError("enter six hex bytes separated by colons")
    address = bytes.fromhex(value.replace(":", ""))
    if address[0] & 1 or not any(address):
        raise argparse.ArgumentTypeError("enter a nonzero unicast address")
    return address


def main():
    parser = argparse.ArgumentParser(description="Check a saved AirRounds PCAP.")
    parser.add_argument("capture", type=Path, help="classic PCAP file")
    parser.add_argument("--receipt", type=Path, help="matching T2 .pcap.ok file")
    parser.add_argument("--bssid", type=parse_bssid, help="access point MAC address to check")
    args = parser.parse_args()

    try:
        with args.capture.open("rb") as stream:
            link_type, count, size, crc, ssid = check_records(
                stream, args.bssid, args.receipt is not None
            )
        if args.receipt is not None:
            check_receipt(args.receipt, size, crc)
        if count == 0:
            raise CheckError("damaged: capture has no records")
    except CheckError as error:
        print(error, file=sys.stderr)
        return 1
    except OSError as error:
        print(f"read error: {error}", file=sys.stderr)
        return 1

    context = ""
    if args.bssid is not None:
        address = args.bssid.hex(":")
        if ssid is None:
            print(f"not observed: BSSID {address}", file=sys.stderr)
            return 3
        ssid_text = f"hex {ssid.hex()}" if ssid else "unknown"
        context = f", BSSID {address}, SSID {ssid_text}"

    receipt = ", receipt matches" if args.receipt is not None else ""
    print(f"valid: {count} record(s), {size} bytes, {link_type}{context}{receipt}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
