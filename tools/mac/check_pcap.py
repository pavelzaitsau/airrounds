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
# The hashcat MESSAGE_PAIR combinations an attacker can place: one ANonce frame (message 1 or 3)
# with one SNonce-and-MIC frame (message 2 or 4) from the same client. M1+M2 is also the active
# mode's crackable set (docs/t12-architecture.md); the rest are pairs the passive mode may catch.
# The check reports any of them as a success.
CRACKABLE_PAIRS = ((1, 2), (1, 4), (2, 3), (3, 4))


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
    if frame[0] == 0x80 and frame[4:10] != b"\xff" * 6:
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


def eapol_header_len(frame):
    # IEEE 802.11 MAC header: 24, plus 2 for a QoS data subtype, plus 4 for HT Control.
    length = 24
    if frame[0] & 0x80:
        length += 2
        if frame[1] & 0x80:
            length += 4
    return length


def eapol_message(frame, bssid):
    """The (client, message) of an EAPOL-Key handshake frame for bssid, or None when the frame is
    not a placeable pairwise EAPOL-Key frame for that BSSID. The BSSID and client come from the DS
    bits, the same map as the firmware; nothing is decrypted."""
    if len(frame) < 24 or frame[0] & 0x0C != 0x08 or frame[1] & 0x40:
        return None # Not a plaintext data frame.
    ds = frame[1] & 0x03
    if ds == 0x01:   # ToDS: client to AP (messages 2 and 4).
        ap, client = frame[4:10], frame[10:16]
    elif ds == 0x02: # FromDS: AP to client (messages 1 and 3).
        ap, client = frame[10:16], frame[4:10]
    elif ds == 0x00: # Same BSS.
        ap, client = frame[16:22], frame[10:16]
    else:            # 4-address WDS has no single BSSID.
        return None
    if ap != bssid:
        return None
    header = eapol_header_len(frame)
    if len(frame) < header + 10:
        return None
    if frame[header:header + 6] != b"\xaa\xaa\x03\x00\x00\x00" or frame[header + 6:header + 8] != b"\x88\x8e":
        return None
    if frame[header + 9] != 3: # EAPOL packet type 3 is EAPOL-Key.
        return None
    offset = header + 8
    if len(frame) < offset + 7:
        return None
    key_info = (frame[offset + 5] << 8) | frame[offset + 6]
    if not key_info & 0x0008: # Key Type clear: a group rekey, not a pairwise handshake message.
        return None
    ack, mic, secure = bool(key_info & 0x0080), bool(key_info & 0x0100), bool(key_info & 0x0200)
    message = {(True, False, False): 1, (False, True, False): 2,
               (True, True, True): 3, (False, True, True): 4}.get((ack, mic, secure))
    if message is None:
        return None
    return bytes(client), message


def check_records(stream, bssid, verify_receipt, eapol=False):
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
    clients = [] # (client_address, {messages}) in order of first appearance, for the EAPOL check.
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
        if eapol:
            # Every record must be a placeable EAPOL-Key frame for the target BSSID; the firmware
            # writes nothing else, so a failure here points at a writer or a mode defect.
            placed = eapol_message(frame, bssid)
            if placed is None:
                raise CheckError(f"unsupported: record {count} is not an EAPOL-Key frame for the BSSID")
            client, message = placed
            entry = next((messages for address, messages in clients if address == client), None)
            if entry is None:
                clients.append((client, {message}))
            else:
                entry.add(message)
        elif bssid is not None and not ssid and captured == original:
            candidate = observed_ssid(frame, bssid)
            if candidate is not None:
                ssid = candidate

    return LINK_TYPES[link_type], count, size, crc, ssid, clients


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


def report_eapol(bssid, count, size, link_type, clients, has_receipt):
    """Print the handshake messages and the crackable message pairs per client, then return the exit
    code: 0 when one client holds a valid hashcat message pair (M1+M2, M1+M4, M2+M3, or M3+M4), 3
    for the target with no such pair. Clients are numbered in order of first appearance; no client MAC
    address is printed. The check reads no nonce or MIC, so a reported pair means the two messages
    are present, not that the capture is proven crackable."""
    address = bssid.hex(":")
    receipt = ", receipt matches" if has_receipt else ""
    print(f"EAPOL: {count} record(s), {size} bytes, {link_type}, BSSID {address}{receipt}")
    crackable = False
    for index, (_client, messages) in enumerate(clients, start=1):
        line = f"client {index}: messages " + ", ".join(str(message) for message in sorted(messages))
        pairs = [f"M{a}+M{b}" for a, b in CRACKABLE_PAIRS if a in messages and b in messages]
        if pairs:
            line += "; pairs " + ", ".join(pairs)
            crackable = True
        print(line)
    if crackable:
        return 0
    print(f"no crackable pair: BSSID {address}", file=sys.stderr)
    return 3


def main():
    parser = argparse.ArgumentParser(description="Check a saved AirRounds PCAP.")
    parser.add_argument("capture", type=Path, help="classic PCAP file")
    parser.add_argument("--receipt", type=Path, help="matching T2 .pcap.ok file")
    parser.add_argument("--bssid", type=parse_bssid, help="access point MAC address to check")
    parser.add_argument("--eapol", action="store_true",
                        help="check EAPOL-Key handshake frames for --bssid")
    args = parser.parse_args()
    if args.eapol and args.bssid is None:
        parser.error("--eapol needs --bssid")

    try:
        with args.capture.open("rb") as stream:
            link_type, count, size, crc, ssid, clients = check_records(
                stream, args.bssid, args.receipt is not None, args.eapol
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

    if args.eapol:
        return report_eapol(args.bssid, count, size, link_type, clients,
                            args.receipt is not None)

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
