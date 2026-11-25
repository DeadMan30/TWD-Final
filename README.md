# TWD-Final

*The Walking Dead: The Final Season (PC), rebuilt as readable code that runs natively on Android.*

**[Progress](https://deadman30.github.io/TWD-Final/progress.html)** ·
**[Status](STATUS.md)** ·
**[Overview](docs/OVERVIEW.md)** ·
[Discussions](https://github.com/DeadMan30/TWD-Final/discussions) ·
[Contributing](CONTRIBUTING.md)

## What this is

TWD-Final rewrites *The Walking Dead: The Final Season* (`WalkingDead4.exe`, PC x86-64) by hand as
readable C and C++ source, one function at a time, with the final goal of running the game
natively on Android. Every rewritten function is tested differentially against the original
executable to prove identical behavior. This repository contains no game files or proprietary
assets: you bring your own legally obtained PC copy of the game.

## Status

**The game is not playable on Android yet.** The repository, progress pipeline, and initial PE
function and import tables from `WalkingDead4.exe` are set up; Ghidra analysis, runtime bring-up,
and the first verified function replacements are next.

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
