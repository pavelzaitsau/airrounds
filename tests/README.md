# Host tests

This directory holds component and interface tests. See the
[test fixtures](fixtures/README.md) for cases and synthetic input generators.

The [toolchain examples](toolchain) check the cross-compilers and packaging.
See the [T0 build commands](../docs/t0.md).

## SD block writes

Run the storage test from the repository root.

```sh
build_dir="$(mktemp -d)"
c++ -std=c++11 -Wall -Wextra -Werror -pedantic -I firmware/esp32/storage \
  tests/storage/pending_write_test.cpp -o "$build_dir/pending_write_test"
"$build_dir/pending_write_test"
```

To check the helper with `fs::File`, use the [T0 ESP32 settings](../docs/t0.md).
Build `tests/toolchain/esp32/airrounds_storage_smoke` with a separate output
directory and the flag `--library "$PWD/firmware/esp32/storage"`.
