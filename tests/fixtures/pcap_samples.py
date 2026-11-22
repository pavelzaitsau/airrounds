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


CLIENT = bytes.fromhex("112233445566")
CLIENT_TWO = bytes.fromhex("0a0b0c0d0e0f")
KEY_INFO = {1: 0x0088, 2: 0x0108, 3: 0x0388, 4: 0x0308}


def eapol_frame(message, *, bssid=BSSID, client=CLIENT, key_data=0):
    """A raw EAPOL-Key handshake frame without its FCS. Messages 1 and 3 go AP to client (FromDS),
    2 and 4 go client to AP (ToDS), the same DS map the firmware and the Mac check use."""
    from_ds = message in (1, 3)
    mac = bytearray(24)
    mac[0] = 0x08
    mac[1] = 0x02 if from_ds else 0x01
    mac[4:10] = client if from_ds else bssid   # Addr1
    mac[10:16] = bssid if from_ds else client  # Addr2
    mac[16:22] = bssid                         # Addr3
    key_info = KEY_INFO[message]
    body = bytearray(95 + key_data)
    body[0] = 2
    body[1] = (key_info >> 8) & 0xFF
    body[2] = key_info & 0xFF
    eapol = bytes([2, 3, (len(body) >> 8) & 0xFF, len(body) & 0xFF])
    return bytes(mac) + b"\xaa\xaa\x03\x00\x00\x00\x88\x8e" + eapol + bytes(body)


def eapol_capture(messages, *, clients=None):
    clients = clients if clients is not None else [CLIENT] * len(messages)
    data = header()
    for message, client in zip(messages, clients):
        data += record(frame=eapol_frame(message, client=client))
    return data


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
