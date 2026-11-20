# Code quality checks

Run the commands from the repository root. They do not flash a device.
Python 3 and Make run the checks; no CMake or Python packages are needed.
`tcpdump` reads the synthetic PCAP produced by the normal test run; macOS
provides it by default. Node.js runs the UI prototype tests.

## Install the host tools

On macOS:

```sh
brew install llvm cppcheck node
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
from [T0](t0.md); it builds the ESP32 examples, the UART firmware, and the Flipper app. It checks
each ESP32 image, runs Cppcheck with the target SDK settings, and checks the
[memory budgets](#memory-checks).

| Command | Checks |
| --- | --- |
| `make format-check` | Formatting of project `.c`, `.cpp`, `.h`, and `.ino` files |
| `make format` | Applies `.clang-format` to those files |
| `make test` | Every C++11 host test, the Python tests, the Node prototype tests, Mac PCAP checks, and the native Flipper UI emulator. It fails if a test file is out of reach. |
| `make asan` | The C++ host tests with AddressSanitizer, LeakSanitizer, and UndefinedBehaviorSanitizer |
| `make tsan` | The C++ host tests with ThreadSanitizer, in separate executables |
| `make tidy` | Clang-Tidy and its Clang Analyzer checks on the host test translation units |
| `make cppcheck` | Host tests and included production headers, with native and 32-bit ESP32 profiles |
| `make analyze` | Standalone Clang Static Analyzer; findings fail the command |

The tests include the real storage headers. File-system tests use a fresh
temporary directory on each run. Sanitizers stop on the first detected error.
ASan/UBSan and TSan run separately because their runtimes cannot be combined.
The runner does not silently skip an unavailable tool or sanitizer.

## Runaway tests

`tools/quality/check.py` starts every command in its own process group
under kernel limits. A test that hangs or loops cannot outlive the gate,
and one that writes without end cannot write a file past 1 GiB.

| Limit | Value | Effect |
| --- | --- | --- |
| Wall time per command | 180 s | The gate kills the whole process group, children included |
| CPU time per process | 300 s | The kernel stops the process, even after the gate exits |
| File size per file | 1 GiB | A write past the limit fails with `SIGXFSZ` |

The gate also stops on Ctrl-C, `SIGTERM`, `SIGHUP`, and `SIGQUIT`, and the
shell then sees 128 plus the signal number. A stopped command first gets
`SIGTERM` and 2 s to remove its temporary files, then `SIGKILL`. A signal
that the caller ignores, as `nohup` ignores `SIGHUP`, stays ignored. A
signal that arrives during a cleanup waits until the cleanup ends. A lower
limit that the shell already set stays in force.

When a command ends, normally or not, the gate also kills the rest of its
process group, so a background child cannot outlive its test. Every guard
owns a temporary directory, and a guard started below another one, as in
`tests/flipper/run_ui_emulator.py`, puts its directory inside the enclosing
one. Each child registers its process group in its guard's directory before
it runs. A guard that ends kills every group registered in its subtree,
repeats until no new group appears, and removes the subtree. A child that
starts after that fails instead of running unguarded.

Run test binaries outside the gate, for example in a mutation harness,
under the same limits: `ulimit -t 300 -f 1048576` in a bash shell, which
counts file size in 1,024-byte blocks. Without the limits, a mutant that
loops in a write keeps writing after its harness stops.

## Memory checks

The gates fail on leaks, on unbounded stack frames, and on stack or RAM use
over a budget. The table lists the budgets for the current source.

| Check | Gate | Budget | Current use |
| --- | --- | --- | --- |
| Heap leaks in host tests and the Flipper UI emulator | `make check` | None at exit | None |
| Worst AirRounds call path on the Flipper app thread | `make embedded` | 2,048 B, half of `stack_size` | 1,448 B |
| Global and static RAM of `airrounds_uart` | `make embedded` | 65,536 B | 52,160 B |
| Largest stack frame in `airrounds_uart` | `make embedded` | 1,024 B | 656 B in `storage::writePcapRecords` |
| Stack frame of `writeSessionJournal` | `make embedded` | 2,560 B | 2,512 B |

LeakSanitizer is off by default on macOS, and Apple clang does not support
it. The host gate and the emulator therefore use the Homebrew LLVM and set
`detect_leaks=1`. Before the ASan run, a canary program leaks 64 bytes. The
gate fails if LeakSanitizer does not report that leak.

The Flipper check compiles `airrounds.cpp` for ARM with `-fcallgraph-info=su`.
It adds the `airrounds_app` frame to the deepest path of every other function,
because the event loop can call any callback on that thread. A call cycle, a
dynamic frame, or an indirect call without listed targets fails the check.
List the targets of a new function-pointer call in `tools/quality/memory.py`.
The other half of the stack holds SDK, `printf`, and event-loop frames.

The pinned ESP32 compiler, GCC 8.4, has no call-graph output. The ESP32 check
therefore limits each frame from `-fstack-usage` and the global RAM reported
by `arduino-cli`. `writeSessionJournal` keeps its 2,304-byte journal text on
the 8,192-byte `airrounds-log` task stack and has its own cap. A new frame over
1,024 B needs a review and an entry in `tools/quality/memory.py`.

The checks count AirRounds frames only. SD, FATFS, Wi-Fi, and SDK frames are
precompiled and have no stack reports. The T6 bench recorded at least 2,212 B
free on the Flipper app stack. No measurement covers the ESP32 task stacks yet.

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
