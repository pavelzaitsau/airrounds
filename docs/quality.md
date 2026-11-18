# Code quality checks

Run the commands from the repository root. They do not flash a device.
Python 3 and Make run the checks; no CMake or Python packages are needed.
`tcpdump` reads the synthetic PCAP produced by the normal test run; macOS
provides it by default.

## Install the host tools

On macOS:

```sh
brew install llvm cppcheck
```

The runner finds Homebrew's keg-only LLVM on Apple Silicon and Intel Macs,
then falls back to PATH. Set `LLVM_BIN` to use another LLVM installation.
`CXX`, `CLANG_TIDY`, `CLANG_FORMAT`, and `CPPCHECK` can override individual
executables. `CXX` must support Clang's analyzer and sanitizer flags.

The initial verification used LLVM 23.1.5, Cppcheck 2.22.0, and macOS arm64.
Other versions must pass the checks before use; analyzer diagnostics can change.

## Commands

```sh
make check
make embedded
```

`make check` runs every host check below and stops with a nonzero status on
failure. It never reformats files. `make embedded` requires the pinned SDKs
from [T0](t0.md); it builds all ESP32 sketch directories and the Flipper app, verifies
the ESP32 image checksum and validation hash, and runs Cppcheck using their compilation databases.

| Command | Checks |
| --- | --- |
| `make format-check` | Formatting of project `.c`, `.cpp`, `.h`, and `.ino` files |
| `make format` | Applies `.clang-format` to those files |
| `make test` | All `tests/storage/*_test.cpp` with strict C++11 warnings |
| `make asan` | The same tests with AddressSanitizer and UndefinedBehaviorSanitizer |
| `make tsan` | The same tests with ThreadSanitizer, in separate executables |
| `make tidy` | Clang-Tidy and its Clang Analyzer checks on the host test translation units |
| `make cppcheck` | Host tests and included production headers, with native and 32-bit ESP32 profiles |
| `make analyze` | Standalone Clang Static Analyzer; findings fail the command |

The tests include the real storage headers. File-system tests use a fresh
temporary directory on each run. Sanitizers stop on the first detected error.
ASan/UBSan and TSan run separately because their runtimes cannot be combined.
The runner does not silently skip an unavailable tool or sanitizer.

## Worktrees and outputs

SDKs default to `.airrounds-tools/`. A worktree can reuse the installed SDKs:

```sh
AIRROUNDS_TOOLS=/path/to/primary/AirRounds/.airrounds-tools make embedded
```

Host executables, analysis output, and `compile_commands.json` go under
`build/quality/`. Embedded build intermediates go under `build/embedded/`;
`.bin` and `.fap` outputs go under `artifacts/quality/`. These are ignored by Git.
The Flipper SDK also uses its existing build cache under `UFBT_HOME`; do not
run another build of the same app against that shared cache concurrently.

## Analyzer policy

The checks retain C++11 and the existing enum representation. The
`performance-enum-size` suggestion is disabled because changing a public enum's
underlying type can change ABI. Host tests permit exceptions to escape `main`:
an allocation failure should fail the test rather than be swallowed. The immutable
PCAP fixture has a separate line-specific static-initialization exemption.

Cppcheck exclusions in project code are inline, next to each reviewed case:

- Test assertions deliberately compare statically determined results.
- Two test assignments are consumed through a template or callback that the
  analyzer does not follow.
- PCAP wrappers and fault-injection methods deliberately use static template
  dispatch to call or wrap nonvirtual file methods.
- Queue slots are initialized by `enqueue` before release publication and read
  only after acquire. The constructor deliberately avoids clearing every slot.

Missing system includes, including quoted standard includes inside external SDK
headers, are omitted from Cppcheck diagnostics because it uses library models
and compilation databases omit compiler-default include paths. Missing project
includes and syntax errors remain errors; cross-compilers also enforce SDK includes.
The embedded profile uses real SDK defines and include paths. It excludes only
`returnTempReference` and `duplInheritedMember` diagnostics in external Arduino
headers, and `comparePointers` at CMSIS linker-table boundaries. These SDK
exclusions do not apply to AirRounds files. Flipper analysis supplies the pinned
ARM GCC 12.3 macros and newlib `_ATTRIBUTE` alias; revisit them with an SDK upgrade.
Clang-Tidy checks portable storage through host tests; it does not emulate Xtensa.
The PCAP validator retains its size/CRC argument API with a line-specific
swappable-parameter exemption. Guarded read counts and bounded fixture sizes
use explicit casts; shift counts and masks use unsigned literals.

A 32-bit profile models integer widths, not radio, RTOS scheduling, SD timing,
power loss, or hardware interrupts. Host checks and embedded compilation do not
replace the [bench acceptance checks](plan.md#bench-checks-and-handoff).

## Project-local skills

`harness-writing`, `libfuzzer`, and `sharp-edges` are installed in
`.agents/skills/` and are available to Codex on the next turn. Their unmodified
upstream files, pinned revision, and license are recorded in
[the source manifest](../.agents/skills/trailofbits-source.json).
Use the fuzzing skills when parsers or additional fuzz targets are introduced;
installing a skill does not itself run a fuzz campaign.

See the [verification and review report](quality-review.md) for the initial results.
