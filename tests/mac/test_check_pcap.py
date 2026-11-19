from pathlib import Path
import struct
import subprocess
import sys
from tempfile import TemporaryDirectory
import unittest
import zlib

from tests.fixtures.pcap_samples import BEACON, SUPPORTED_RATES, header, record, samples


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
