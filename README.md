# TWD-Final

*The Walking Dead: The Final Season (PC), rebuilt as readable code that runs natively on Android.*

**[Progress](https://deadman30.github.io/TWD-Final/progress.html)** ·
**[Status](STATUS.md)** ·
**[Overview](docs/OVERVIEW.md)** ·
[Discussions](https://github.com/DeadMan30/TWD-Final/discussions) ·
[Contributing](CONTRIBUTING.md)

## What this is

TWD-Final rebuilds *The Walking Dead: The Final Season* (`WalkingDead4.exe`, PC x86-64) so the
game runs natively on Android, prioritizing the critical boot, rendering, input, audio, and game
loop paths needed to reach playable gameplay. Third-party middleware (`openssl`, `bullet`, `curl`,
`jsoncpp`, `zlib`, `enlighten`, `breakpad`, MSVC `crt`, and internal Lua runtime internals)
is excluded from the decompilation denominator and swapped with native/upstream libraries or shims
in [runtime/](runtime/) rather than manually decompiled.

## Status

**The game is not playable on Android yet.** Runtime import coverage sits at **`439 / 556` PE
imports (`78.96%`, milestone `5 of 5: Audio & GameLoop`)** via the Vulkan/GLES D3D11 shim, input,
media, and filesystem layers in [runtime/](runtime/); see [BOOT_TRACE.md](docs/BOOT_TRACE.md) for
the main-menu critical path and [STATUS.md](STATUS.md) for live numbers (with middleware/CRT excluded
from the decompilation denominator).

[![functions verified](https://deadman30.github.io/TWD-Final/badge-verified.svg)](STATUS.md)
[![functions replaced](https://deadman30.github.io/TWD-Final/badge-replaced.svg)](STATUS.md)
[![code replaced](https://deadman30.github.io/TWD-Final/badge-code.svg)](STATUS.md)
[![replaced in 7 days](https://deadman30.github.io/TWD-Final/badge-week.svg)](STATUS.md)

[<img alt="Progress map of the game's code: green where a function is rewritten and verified, amber where replaced, grey where not yet started" src="https://deadman30.github.io/TWD-Final/progress.svg" width="100%">](https://deadman30.github.io/TWD-Final/progress.html)

**Details:** [full numbers](STATUS.md) · [live progress page](https://deadman30.github.io/TWD-Final/progress.html) · [how it works and layout](docs/OVERVIEW.md)

## Try it

Once the native Android runtime and initial engine layers are implemented, building will target
Android (Android NDK / CMake, Vulkan / OpenGL ES, AAudio) alongside a Windows/PC verification
harness, using assets from your own PC copy placed in `game/` (which is git-ignored). See
[docs/OVERVIEW.md](docs/OVERVIEW.md) for the planned architecture and repository layout.

## Help out

Everyone is welcome: map subsystems in Ghidra, rewrite and verify functions, or work on the native
Android runtime and platform abstraction layer. Read [CONTRIBUTING.md](CONTRIBUTING.md) before
opening a pull request.

## Legal

- **No game files.** This repository holds only newly written code, tools, symbol tables, and
  documentation; pre-commit hooks and CI checks refuse game binaries, archives, and extracted assets.
- **Your own copy.** You need your own legally obtained PC copy of *The Walking Dead: The Final
  Season* (`WalkingDead4.exe` and its `Archives/`). We never provide or link to game files.
- **Not affiliated** with, endorsed by, or sponsored by Skybound Games, Telltale Games, or AdHoc
  Studio. *The Walking Dead* and related names are trademarks of their respective owners.

Full text: [docs/LEGAL.md](docs/LEGAL.md).

## License

GPL-2.0 ([LICENSE](LICENSE)). None of this code grants any rights in the game or its assets.
