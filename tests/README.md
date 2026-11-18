# Host tests

This directory holds component and interface tests. See the
[test fixtures](fixtures/README.md) for cases and synthetic input generators.

The [toolchain examples](toolchain) check the cross-compilers and packaging.
See the [T0 build commands](../docs/t0.md).

## Mac PCAP checks

`make test` runs the Mac checks and verifies a capture with a receipt from T2.
Run the Mac tests on their own from the repository root.

```sh
python3 -m unittest -v tests.mac.test_check_pcap
```

See the [Mac tool guide](../tools/mac/README.md) for the T3 samples and
the independent `tcpdump` check.

## SD writes and PCAP finalization

Run the host tests from the repository root.

```sh
make test
make asan
make tsan
```

Use `make check` to include formatting and static analysis. See
[quality checks](../docs/quality.md) for tools, profiles, and exclusions.

The tests cover a failed open, short and zero writes, and reported card
errors. They check that overflow keeps the blocks in order when the writer
is busy. A host file size limit also forces a real `write` failure.

The PCAP tests cover close, read, and receipt errors. They also check an
interrupted process, changed bytes, and truncation at a record boundary.
`tcpdump` reads the synthetic capture as a separate check.

To compile the SD example, use the [T0 ESP32 settings](../docs/t0.md).
Build `tests/toolchain/esp32/airrounds_storage_smoke` with a separate output
directory and the flag `--library "$PWD/firmware/esp32/storage"`. The example
uses synthetic data. Physical card checks belong to the bench test.

For the finalization example, use the same build settings with
`tests/toolchain/esp32/airrounds_pcap_smoke`. It uses a synthetic Ethernet frame
to exercise both finalization and recovery.
