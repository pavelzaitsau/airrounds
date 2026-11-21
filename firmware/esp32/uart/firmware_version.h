// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

namespace airrounds {

// The release version of the ESP32 firmware. Keep it equal to fap_version in
// flipper/application.fam; tests/mac/test_version.py fails `make check` otherwise.
constexpr char firmwareVersion[] = "0.1";

} // namespace airrounds
