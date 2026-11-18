# Quality verification and review

Verified on November 18, 2026, on macOS arm64. Base revision: `cc953a0` (including PCAP finalization).
Worktree branch: `codex/code-quality`.

## Scope and result

The three Trail of Bits skills are installed locally and pinned to revision
`149d7b21f4067765b82e67029730b42d228ef4b4`. Each installed file was compared
with a second download of that exact revision; upstream files are unmodified.
The source manifest and license are committed beside the skills.

All available host checks and all four embedded example builds passed.
A separate reviewer checked the changes and approved them after the reported
issues were fixed. When concurrent work added PCAP finalization, this branch was
rebased onto that commit and the new modules, tests, and sketch were included.
This work did not modify the source checkout.

## Verification

| Check | Result |
| --- | --- |
| `make format-check` | All twelve project C/C++/Arduino source and header files passed |
| `make test` | PCAP capture: 7 groups; pending writes: 4 groups; SD writer: 6 groups |
| `make asan` | The same 17 groups passed with ASan and UBSan |
| `make tsan` | The same 17 groups passed with TSan, including producer/consumer overlap |
| `make tidy` | All three host test translation units and included storage headers passed |
| `make cppcheck` | Native and 32-bit ESP32 profiles passed, including project-include checks |
| `tcpdump -nn -r` | Independently read the synthetic PCAP produced by the normal test |
| `make analyze` | All three translation units passed; findings are configured to fail the command |
| `make check` | All host checks above passed in one final run |
| `make embedded` | All three ESP32-S2 `.bin` files and the Flipper `.fap` built; image checksum/hash verification and all four SDK-based Cppcheck analyses passed |

Host tools: LLVM 23.1.5 and Cppcheck 2.22.0. Embedded tools are the versions
pinned in [T0](t0.md): Arduino ESP32 2.0.11, Xtensa GCC 8.4.0, esptool 4.5.1,
Flipper f7 SDK 1.5.1/API 88.2, and ARM GCC 12.3.1.

## Failure checks

The checks were also exercised on deliberately broken files in temporary
copies, without changing the worktree's production code:

- Formatting accepted a checkout beneath a directory named `build`, then
  rejected an unformatted source file.
- Clang-Tidy and Clang Static Analyzer rejected a null-pointer dereference.
- Cppcheck rejected an out-of-bounds array access.
- ASan rejected a heap-buffer overflow; UBSan rejected signed integer overflow.
- TSan rejected a deliberately unsynchronized shared counter.
- Existing storage tests rejected a mutation that stopped advancing the accepted
  write offset.
- The embedded runner rejected corrupted payload and validation-hash bytes.
  For both corruptions the pinned esptool itself returned success; the runner
  caught its explicit invalid-checksum/hash output.

The failure probes test that the gates can reject defects; they do not measure
exhaustive test coverage. Host/build logs and available probe logs are retained
locally under `artifacts/quality/logs/` and are ignored by Git.

## Findings resolved

- Added braces around the test sink's short `if` and moved its expected capture
  fixture from dynamic global initialization into the test that uses it.
- Kept enum sizes unchanged. Scoped test-main exception exemptions to those three
  lines, and Cppcheck assertion/assignment exemptions to inspected statements.
- Documented the queue invariant: enqueue initializes a slot before publishing
  it; acquire consumption precedes any read. The constructor need not clear it.
- Fixed format discovery to inspect repository-relative paths, and to reject
  empty discovery instead of invoking clang-format on stdin.
- Kept new PCAP byte counts and stream offsets explicit with checked/bounded
  casts, and used unsigned shift/mask literals. Static-dispatch wrappers and
  fault-injection methods have line-specific analyzer annotations.
- Added explicit esptool checksum/hash verification and enabled missing-project-
  include diagnostics. External SDK exclusions are documented and path-scoped.

The three original production headers were compared with `cc953a0` after
removing comments and whitespace: their token sequences are identical. The PCAP
headers add explicit casts and unsigned literals with equivalent arithmetic.
As an additional check, compiling the PCAP test translation unit at `-O2` against
the baseline and updated production headers produced identical assembly.
Storage status values, enum representations, memory orders, queue capacity,
file formats, I/O behavior, and retry semantics were not changed.

## Artifacts

These hashes identify this build; paths are relative to the repository root.

| Artifact | Bytes | SHA-256 |
| --- | --- | --- |
| `artifacts/quality/esp32/airrounds_pcap_smoke/airrounds_pcap_smoke.ino.bin` | 293,120 | `cdf168393d976e01af7dd1f38ac8d960dd92f898de7c196df8513ef982a10086` |
| `artifacts/quality/esp32/airrounds_smoke/airrounds_smoke.ino.bin` | 193,040 | `2581b2a83ec46c36371f9fbbd93a3ffb5877b611def3857645fc39c77974adf6` |
| `artifacts/quality/esp32/airrounds_storage_smoke/airrounds_storage_smoke.ino.bin` | 280,128 | `d55dfeaab437a9be02318455faec6f54dd7aa2e941400fa6ff31ae9550df7d43` |
| `artifacts/quality/flipper/airrounds_smoke.fap` | 924 | `0603df621a85d2e24286afef8dbb070d2fcdfd4934a626ca76715a781cbcbbfe` |

## Remaining hardware checks

No device was flashed and no physical radio/SD acceptance run was performed.
Timing, power-loss recovery, actual card failures, and radio behavior remain
bench checks under the [development plan](plan.md). The project currently has
storage code and toolchain examples; these results do not claim a complete
capture session is implemented or verified.
