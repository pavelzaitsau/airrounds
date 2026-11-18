# Host tests

This directory holds component and interface tests. See the
[test fixtures](fixtures/README.md) for cases and synthetic input generators.

The [toolchain examples](toolchain) check the cross-compilers and packaging.
See the [T0 build commands](../docs/t0.md).

## SD writes

Run both tests from the repository root.

```sh
build_dir="$(mktemp -d)"
for test in pending_write sd_writer; do
  c++ -std=c++11 -Wall -Wextra -Werror -pedantic -pthread \
    -I firmware/esp32/storage "tests/storage/${test}_test.cpp" \
    -o "$build_dir/${test}_test"
done
"$build_dir/pending_write_test"
"$build_dir/sd_writer_test" "$build_dir"
```

The tests cover a failed open, short and zero writes, and reported card
errors. They check that overflow keeps the blocks in order when the writer
is busy. A host file size limit also forces a real `write` failure.

To compile the SD example, use the [T0 ESP32 settings](../docs/t0.md).
Build `tests/toolchain/esp32/airrounds_storage_smoke` with a separate output
directory and the flag `--library "$PWD/firmware/esp32/storage"`. The example
uses synthetic data. Physical card checks belong to the bench test.
