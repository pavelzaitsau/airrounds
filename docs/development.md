# Development

1. Record the source repositories, revisions, installed firmware, SDK versions,
   and dependencies. Reproduce the ESP32 firmware and Flipper app builds. Record
   the commands and artifact hashes.
2. Test SD writes and PCAP finalization on the host. Cover failed opens, short
   writes, and buffer overflow.
3. Run a passive session for one access point through the Flipper app. Transfer
   the file and check it on a Mac.
4. Add support for multiple access points. Check that the session ends when
   there are no observations.
5. Record hardware acceptance results separately from host checks.

Keep synthetic inputs in `tests/fixtures/`. Keep real captures and bench
results in the local `captures/` directory. Use
`git diff-tree --root --no-commit-id --check HEAD` to check the first commit.
Add the build and test commands after pinning the tools.
