# Contributing to AirRounds

AirRounds accepts a contribution only under the MIT No Attribution license
(MIT-0), and only with a Developer Certificate of Origin sign-off on every
commit. Read [acceptable use](docs/acceptable-use.md) first: a contribution that
adds a transmit, deauthentication, or decryption path is out of scope and is
declined. A contribution does not make you an intended operator of AirRounds;
see [acceptable use](docs/acceptable-use.md#intended-operator).

## License of a contribution

When you open a pull request, you license every change in it to the project
under MIT-0. The MIT-0 text is in [LICENSES/MIT-0.txt](LICENSES/MIT-0.txt), with
your name as the copyright holder.

MIT-0 lets the copyright holder ship your change under the project license, the
PolyForm Noncommercial License 1.0.0, and under any other license later, such as
a commercial or an open-source one. Without that grant, one contribution under
other terms blocks every later change of license.

The SPDX identifier at the top of a source file names the license under which
the project distributes that file. It does not name the license of your
contribution, which is always MIT-0.

Some files are excluded, because their own licenses decide the terms of a
change:

| Files | Rule |
| --- | --- |
| `.agents/skills/harness-writing/`, `.agents/skills/libfuzzer/`, `.agents/skills/sharp-edges/` | The Trail of Bits skills stay unmodified under CC-BY-SA-4.0. The project declines changes to them. |
| `.agents/skills/demo-video/licenses/` and `.agents/skills/demo-video/NOTICE` | These texts stay under the voice model owners' terms. Do not edit them. |
| The glyph data in `docs/ui-ux/prototype-font.js` | The glyphs stay under the Adafruit BSD license in [NOTICE](NOTICE). Do not change the license header. |

To add third-party code, follow
[adding third-party code](docs/acceptable-use.md#adding-third-party-code). That
code keeps its upstream license, which [NOTICE](NOTICE) records.

## Sign-off

Sign off every commit:

```bash
git commit -s
```

The `-s` flag adds a `Signed-off-by:` line with the name and email from your Git
configuration. Use your real name. A pull request with an unsigned commit is not
merged until every commit carries a sign-off.

In this project, the sign-off states two things:

1. You certify the change under the
   [Developer Certificate of Origin 1.1](https://developercertificate.org/):
   you wrote it, or you otherwise have the right to submit it.
1. You license the change to the project under MIT-0, as
   [License of a contribution](#license-of-a-contribution) describes. This
   holds whatever SPDX identifier the changed file carries.

## Before you open a pull request

1. Run `make check`. The [quality checks](docs/quality.md) describe what it runs.
1. Add an SPDX identifier to every new source file, as
   [SPDX identifiers](docs/acceptable-use.md#spdx-identifiers) shows.
1. Name the branch as [AGENTS.md](AGENTS.md) describes.
