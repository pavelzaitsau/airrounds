# AirRounds

## Scope and acceptable use

AirRounds is a personal, defensive Wi-Fi security tool for the author's
noncommercial research. The author runs it only on networks the author owns or
has written permission to test. It is not offered for use by anyone else. The
default build is receive-only and adds no transmit, deauthentication, or
decryption path. A separate, opt-in build adds the active mode, which transmits a
targeted deauthentication frame under
[active operation](docs/acceptable-use.md#active-operation); it derives no key.
Before you change behaviour or documentation, read
[acceptable use](docs/acceptable-use.md) and keep your change within its scope
and limits.

## Git branch names

Before creating or renaming a Git branch, read and follow the
[branch-names skill](.agents/skills/branch-names/SKILL.md).

Use the task type as the prefix in `<type>/[<ticket-id>-]<slug>`.
Examples include `feat/add-pcap-validator`, `fix/handle-short-writes`, and
`build/code-quality`. Do not use the `codex/` prefix in this repository.
