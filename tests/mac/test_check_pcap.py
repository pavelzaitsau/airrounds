from pathlib import Path
import struct
import subprocess
import sys
from tempfile import TemporaryDirectory
import unittest
import zlib

from tests.fixtures.pcap_samples import (BEACON, CLIENT, CLIENT_TWO, SUPPORTED_RATES,
                                         eapol_capture, eapol_frame, header, record, samples)


TOOL = Path(__file__).resolve().parents[2] / "tools/mac/check_pcap.py"
ETHERNET_FRAME = bytes.fromhex("02000000000102000000000288b5")


class CheckPcapTest(unittest.TestCase):
    def setUp(self):
        directory = TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.capture = Path(directory.name) / "test.pcap"
        self.receipt = self.capture.with_suffix(".pcap.ok")

    def run_check(self, *options):
        return subprocess.run(
            [sys.executable, str(TOOL), str(self.capture), *options],
            capture_output=True,
            text=True,
            check=False,
        )

    def check_file(self, data, label, *options, code=1):
        self.capture.write_bytes(data)
        result = self.run_check(*options)
        self.assertEqual(result.returncode, code, result.stderr)
        output = result.stdout if code == 0 else result.stderr
        self.assertTrue(output.startswith(label + ":"), result)
        return output

    def write_receipt(self, data):
        self.receipt.write_bytes(
            struct.pack("<8sQI", b"ARPCAP\x01\x00", len(data), zlib.crc32(data))
        )
        return ("--receipt", str(self.receipt))

    def test_four_sample_files(self):
        for label, data in samples().items():
            with self.subTest(label=label):
                self.check_file(data, label, code=0 if label == "valid" else 1)

    def test_classic_byte_orders_and_resolution(self):
        for order in ("<", ">"):
            for nano in (False, True):
                with self.subTest(order=order, nano=nano):
                    fraction = (1_000_000_000 if nano else 1_000_000) - 1
                    self.check_file(
                        header(order=order, nano=nano) + record(order=order, fraction=fraction),
                        "valid", code=0,
                    )

    def test_cut_headers_and_later_record(self):
        valid = samples()["valid"]
        for data in (b"", valid[:3], valid[:20], valid[:39], valid + record()[:-1]):
            with self.subTest(length=len(data)):
                self.check_file(data, "truncated")

    def test_invalid_lengths_and_timestamp(self):
        for data in (
            header(),
            header(snaplen=0) + record(),
            header(snaplen=1) + record(),
            header() + record(fraction=1_000_000),
            header(nano=True) + record(fraction=1_000_000_000),
            header() + record(frame=b""),
        ):
            with self.subTest(data=data.hex()):
                self.check_file(data, "damaged")

    def test_unsupported_magic_version_and_link_type(self):
        for data in (
            bytes.fromhex("0a0d0d0a") + bytes(24),
            b"TEST" + bytes(24),
            header(minor=3) + record(),
            header(link_type=127) + record(),
            header(link_type=0x24000069) + record(),
        ):
            with self.subTest(data=data.hex()):
                self.check_file(data, "unsupported")

    def test_receipt_checks_size_and_content(self):
        valid = header(link_type=1) + record(frame=ETHERNET_FRAME)
        twice = valid + record(frame=ETHERNET_FRAME)
        options = self.write_receipt(twice)
        self.check_file(twice, "valid", *options, code=0)
        self.check_file(valid, "truncated", *options)
        self.check_file(header(link_type=1), "truncated", *options)
        self.check_file(twice[:-1] + b"\x01", "damaged", *options)
        self.check_file(twice + record(frame=ETHERNET_FRAME), "damaged", *options)
        self.receipt.write_bytes(self.receipt.read_bytes() + b"x")
        self.check_file(twice, "damaged", *options)

    def test_access_point_and_exact_ssid_bytes(self):
        frame = BEACON[:36] + b"\x00\x03A\x00\xff" + SUPPORTED_RATES
        client = bytes.fromhex("020000000002")
        probe_response = b"\x50" + frame[1:4] + client + frame[10:]
        options = ("--bssid", "02:00:00:00:00:01")
        output = self.check_file(header() + record(probe_response), "valid", *options, code=0)
        self.assertIn("SSID hex 4100ff", output)
        directed_beacon = frame[:4] + client + frame[10:]
        self.check_file(header() + record(directed_beacon), "not observed", *options, code=3)
        self.check_file(header() + record(), "not observed", "--bssid", "02:00:00:00:00:02",
                        code=3)

    def test_hidden_and_incomplete_access_point_records(self):
        options = ("--bssid", "02:00:00:00:00:01")
        for ssid in (b"\x00\x00", b"\x00\x03\x00\x00\x00"):
            hidden = record(BEACON[:36] + ssid + SUPPORTED_RATES)
            output = self.check_file(header() + hidden, "valid", *options, code=0)
            self.assertIn("SSID unknown", output)
            output = self.check_file(header() + hidden + record(), "valid", *options, code=0)
            self.assertIn("SSID hex 416972526f756e6473", output)
        for packet in (
            record(BEACON[:-1]),
            record(BEACON, original=len(BEACON) + 1),
            record(BEACON[:36]),
            record(BEACON + b"\x00\x00"),
            record(BEACON[:10] + bytes(6) + BEACON[16:]),
        ):
            self.check_file(header() + packet, "not observed", *options, code=3)
        self.check_file(samples()["valid"] + record()[:-1], "truncated", *options)

    def test_access_point_requires_ssid_and_supported_rates(self):
        fixed = BEACON[:36]
        ssid = b"\x00\x01A"
        options = ("--bssid", "02:00:00:00:00:01")
        for elements in (ssid, SUPPORTED_RATES + ssid, ssid + b"\x01\x00"):
            with self.subTest(elements=elements.hex()):
                self.check_file(header() + record(fixed + elements), "not observed",
                                *options, code=3)

    def check_message(self, data, message, *options, code=1):
        self.capture.write_bytes(data)
        result = self.run_check(*options)
        self.assertEqual((result.returncode, result.stderr.strip()), (code, message), result)

    def test_exact_header_and_receipt_messages(self):
        self.check_message(header(snaplen=0) + record(), "damaged: snapshot length is zero")
        self.check_message(header(major=3) + record(), "unsupported: PCAP version 3.4")
        self.check_message(header(minor=3) + record(), "unsupported: PCAP version 2.3")
        data = samples()["valid"]
        options = self.write_receipt(data)
        receipt = self.receipt.read_bytes()
        self.receipt.write_bytes(b"ARPCAP\x02\x00" + receipt[8:])
        self.check_message(data, "damaged: receipt magic", *options)

    def test_access_point_header_filters(self):
        options = ("--bssid", "02:00:00:00:00:01")
        other = bytes.fromhex("020000000002")
        frames = {
            "transmitter differs": BEACON[:10] + other + BEACON[16:],
            "address 3 differs": BEACON[:16] + other + BEACON[22:],
            "fragment number": BEACON[:22] + b"\x01" + BEACON[23:],
        }
        # To DS, From DS, More Fragments, Protected, and +HTC/Order change the frame layout.
        for flag in (0x01, 0x02, 0x04, 0x40, 0x80):
            frames[f"flags {flag:#04x}"] = BEACON[:1] + bytes([flag]) + BEACON[2:]
        for name, frame in frames.items():
            with self.subTest(name):
                self.check_message(header() + record(frame), "not observed: BSSID 02:00:00:00:00:01",
                                   *options, code=3)

    def test_access_point_element_filters(self):
        options = ("--bssid", "02:00:00:00:00:01")
        fixed = BEACON[:36]
        frames = {
            "SSID longer than 32 bytes": fixed + b"\x00\x21" + b"A" * 33 + SUPPORTED_RATES,
            "more than 8 rates": fixed + b"\x00\x01A\x01\x09" + bytes(9),
            "one trailing byte": BEACON + b"\xdd",
        }
        for name, frame in frames.items():
            with self.subTest(name):
                self.check_message(header() + record(frame), "not observed: BSSID 02:00:00:00:00:01",
                                   *options, code=3)
        largest = fixed + b"\x00\x20" + b"A" * 32 + b"\x01\x08" + bytes(8)
        output = self.check_file(header() + record(largest), "valid", *options, code=0)
        self.assertIn("SSID hex " + "41" * 32, output)

    def test_empty_last_element_is_complete(self):
        output = self.check_file(header() + record(BEACON + b"\xdd\x00"), "valid",
                                 "--bssid", "02:00:00:00:00:01", code=0)
        self.assertIn("SSID hex 416972526f756e6473", output)

    def test_receipt_is_named_only_when_checked(self):
        data = samples()["valid"]
        self.assertNotIn("receipt", self.check_file(data, "valid", code=0))
        output = self.check_file(data, "valid", *self.write_receipt(data), code=0)
        self.assertTrue(output.rstrip().endswith(", receipt matches"), output)

    def test_first_visible_ssid_wins(self):
        options = ("--bssid", "02:00:00:00:00:01")
        hidden = record(BEACON[:36] + b"\x00\x00" + SUPPORTED_RATES)
        renamed = record(BEACON[:36] + b"\x00\x01B" + SUPPORTED_RATES)
        output = self.check_file(header() + record() + hidden + renamed, "valid", *options, code=0)
        self.assertIn("SSID hex 416972526f756e6473", output)

    def test_snapshot_shortening_preserves_file_integrity(self):
        self.check_file(header(snaplen=32) + record(BEACON[:32], original=len(BEACON)),
                        "valid", code=0)

    def test_bssid_requires_wifi_link_type(self):
        data = header(link_type=1) + record(frame=ETHERNET_FRAME)
        self.check_file(data, "unsupported", "--bssid", "02:00:00:00:00:01")

    def test_record_size_limit(self):
        limit = 256 * 1024
        data = header(link_type=1, snaplen=limit + 1)
        self.check_file(data + record(bytes(limit)), "valid", code=0)
        self.check_file(data + record(bytes(limit + 1)), "unsupported")

    def test_missing_file_and_bad_argument(self):
        result = self.run_check()
        self.assertEqual(result.returncode, 1)
        self.assertTrue(result.stderr.startswith("read error:"), result)
        result = self.run_check("--bssid", "invalid")
        self.assertEqual(result.returncode, 2)
        self.assertIn("six hex bytes", result.stderr)

    def test_bssid_rejects_group_and_zero_addresses(self):
        self.capture.write_bytes(samples()["valid"])
        for address in ("01:00:00:00:00:01", "ff:ff:ff:ff:ff:ff", "00:00:00:00:00:00"):
            with self.subTest(address=address):
                result = self.run_check("--bssid", address)
                self.assertEqual(result.returncode, 2, result.stderr)
                self.assertIn("nonzero unicast", result.stderr)

    def run_eapol(self, data, code):
        self.capture.write_bytes(data)
        result = self.run_check("--eapol", "--bssid", "02:00:00:00:00:01")
        self.assertEqual(result.returncode, code, result.stderr)
        return result

    def test_eapol_complete_handshake_succeeds(self):
        result = self.run_eapol(eapol_capture([1, 2, 3, 4]), 0)
        self.assertTrue(result.stdout.startswith("EAPOL:"), result.stdout)
        self.assertIn("client 1: messages 1, 2, 3, 4; pairs M1+M2, M1+M4, M2+M3, M3+M4",
                      result.stdout)

    def test_eapol_crackable_pairs_each_succeed(self):
        # Every valid hashcat MESSAGE_PAIR from one client is a success: an ANonce frame (1 or 3)
        # with a SNonce-and-MIC frame (2 or 4).
        for messages, pair in (([1, 2], "M1+M2"), ([1, 4], "M1+M4"),
                               ([2, 3], "M2+M3"), ([3, 4], "M3+M4")):
            with self.subTest(pair=pair):
                result = self.run_eapol(eapol_capture(messages), 0)
                self.assertIn(f"pairs {pair}", result.stdout)

    def test_eapol_lone_message_has_no_pair(self):
        # A single message for the target holds no pair, so the capture is not crackable.
        result = self.run_eapol(eapol_capture([1]), 3)
        self.assertIn("no crackable pair", result.stderr)
        self.assertNotIn("pairs", result.stdout)

    def test_eapol_two_frames_of_one_role_are_no_pair(self):
        # M1+M3 are two ANonce frames; M2+M4 are two SNonce-and-MIC frames. Neither is a pair.
        for messages in ([1, 3], [2, 4]):
            with self.subTest(messages=messages):
                result = self.run_eapol(eapol_capture(messages), 3)
                self.assertIn("no crackable pair", result.stderr)
                self.assertNotIn("pairs", result.stdout)

    def test_eapol_repeated_message_is_deduplicated(self):
        # messages is a set, so a repeated record collapses: two M2 hold no pair, M1 then two M2 do.
        lone = self.run_eapol(eapol_capture([2, 2]), 3)
        self.assertIn("client 1: messages 2\n", lone.stdout + "\n")
        self.assertIn("no crackable pair", lone.stderr)
        paired = self.run_eapol(eapol_capture([1, 2, 2]), 0)
        self.assertIn("client 1: messages 1, 2; pairs M1+M2", paired.stdout)

    def test_eapol_reports_every_pair_in_order(self):
        # A client holding messages 2, 3, 4 yields two overlapping pairs, listed in message order.
        result = self.run_eapol(eapol_capture([2, 3, 4]), 0)
        self.assertIn("client 1: messages 2, 3, 4; pairs M2+M3, M3+M4", result.stdout)

    def test_eapol_cross_client_pair_is_not_crackable(self):
        # Message 3 to one client and message 4 from another are not a pair from one client.
        result = self.run_eapol(eapol_capture([3, 4], clients=[CLIENT, CLIENT_TWO]), 3)
        self.assertIn("no crackable pair", result.stderr)
        self.assertNotIn("pairs", result.stdout)

    def test_eapol_pairs_are_grouped_per_client(self):
        # One client holds a pair, another a lone message; only the first reports a pair.
        result = self.run_eapol(eapol_capture([1, 2, 3], clients=[CLIENT, CLIENT, CLIENT_TWO]), 0)
        self.assertIn("client 1: messages 1, 2; pairs M1+M2", result.stdout)
        self.assertIn("client 2: messages 3", result.stdout)
        client_two_line = next(line for line in result.stdout.splitlines() if line.startswith("client 2:"))
        self.assertNotIn("pairs", client_two_line)

    def test_eapol_rejects_a_non_eapol_record(self):
        result = self.run_eapol(header() + record(frame=BEACON), 1)
        self.assertIn("not an EAPOL-Key frame", result.stderr)

    def test_eapol_output_holds_no_client_mac(self):
        result = self.run_eapol(eapol_capture([1, 2, 3, 4]), 0)
        for text in (result.stdout, result.stderr):
            self.assertNotIn(CLIENT.hex(), text)
            self.assertNotIn(CLIENT.hex(":"), text)

    def test_eapol_needs_a_bssid(self):
        self.capture.write_bytes(eapol_capture([1, 2, 3, 4]))
        result = self.run_check("--eapol")
        self.assertEqual(result.returncode, 2, result.stderr)
        self.assertIn("--eapol needs --bssid", result.stderr)
