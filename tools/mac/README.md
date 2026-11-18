# Mac PCAP checker

Use Python 3.8 or later to check a capture after transfer to the Mac.
Run the commands from the repository root.

```sh
python3 tools/mac/check_pcap.py path/to/capture.pcap
```

The checker reads classic PCAP 2.4 with Ethernet or raw 802.11 records.
It supports both byte orders and microsecond or nanosecond timestamps.
It checks the file header and walks every record. Each record can hold up
to 256 KiB. PCAPNG, Radiotap, and extra link type flags report `unsupported`.

## Check a receipt or access point

Use the published `.pcap.ok` file from T2 to check the byte count and CRC32.

```sh
python3 tools/mac/check_pcap.py path/to/capture.pcap \
  --receipt path/to/capture.pcap.ok
```

A receipt lets the checker detect changed bytes and missing whole records.
Without a receipt, `valid` means the file and record headers pass the
structural checks.

Use `--bssid` with a nonzero unicast MAC address to check a raw 802.11 capture.
The checker looks for a complete 2.4 GHz beacon or probe response without a
trailing frame checksum. The SSID and supported rates fields come first.
The checker reports the first visible SSID as hex. A hidden SSID appears as
`unknown`.

```sh
python3 tools/mac/check_pcap.py path/to/capture.pcap \
  --bssid 02:00:00:00:00:01
```

| Result | Exit code | Meaning |
| --- | --- | --- |
| `valid` | 0 | All requested checks passed |
| `truncated` | 1 | A header or record ends early, or the receipt expects more bytes |
| `damaged` | 1 | An invalid field, empty capture, or receipt mismatch |
| `unsupported` | 1 | The checker cannot handle the format, link type, or record size |
| `read error` | 1 | The checker could not open or read an input file |
| Usage error | 2 | The command has an invalid or missing argument |
| `not observed` | 3 | The capture passed the file checks, with no usable record for the requested BSSID |

## Reproduce T3 on a Mac

Generate the four synthetic files in a new directory.

```sh
sample_dir="$(mktemp -d)"
python3 tests/fixtures/pcap_samples.py "$sample_dir"
for sample in valid truncated damaged unsupported; do
  python3 tools/mac/check_pcap.py "$sample_dir/$sample.pcap"
done
tcpdump -nn -e -r "$sample_dir/valid.pcap"
```

The checker reports the result named by each file. On macOS arm64,
`tcpdump` 4.99.1 opened the valid file and showed one beacon. The beacon
has SSID `AirRounds`, BSSID `02:00:00:00:00:01`, and channel 6.
