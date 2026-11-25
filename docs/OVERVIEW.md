# Overview

The technical details behind [README.md](../README.md): what TWD-Final is, the target binaries,
and how the repository is organized for decompiling the PC version toward a native Android port.
Live numbers are on the [progress page](https://deadman30.github.io/TWD-Final/progress.html) and
in [STATUS.md](../STATUS.md).

## What this is

TWD-Final has two tracks built side by side:

- **A matching & functional decompilation.** `WalkingDead4.exe` (x86-64 Windows, Telltale Tool
  engine) is rewritten function by function into readable C and C++. A replacement earns
  `edge-verified` once it behaves identically on generated edge cases (and live captures), earns
  `verified` once it also matches the original x86-64 bytes (`byte_exact=yes` in
  `symbols/matching.csv`) and passes review, and compiles cleanly for **ARM64 / Android**.
- **A native Android runtime.** Platform APIs that the PC executable imports (`KERNEL32`, `USER32`,
  `d3d11`, `MFPlat`, `XINPUT9_1_0`, `DINPUT8`, `WS2_32`, plus audio/compression middleware) are
  mapped onto native Android equivalents (Android NDK, Vulkan / OpenGL ES, AAudio / OpenSL ES,
  GameActivity / touch & controller input, POSIX/Android file I/O).

## Layout

| Path | Purpose |
|---|---|
| `game/<system>/` | Decompiled replacement code, organized by engine/game system (local game files placed in `game/` are git-ignored). |
| `game/hooks.csv` | Hook registry mapping original virtual addresses to `twd_*` replacements. |
| `game/progress_map.csv` | Library display names and groupings for the treemap and progress page. |
| `runtime/` | Game-agnostic native Android runtime and platform abstraction layer. |
| `symbols/` | `ghidra_functions.csv` (all functions), `functions.csv` (replaced/verified), `families.csv`, `source_files.csv`, `imports.csv`, `verification.csv`, `matching.csv`, `reviews.csv`. |
| `tools/` | Progress calculation, SVG treemap/badge generator, HTML progress page generator, no-game-data checker, Ghidra scripts, and git hooks. |
| `docs/` | Project documentation and legal policy. |

## Progress tracking

Everything displayed in the README badges, treemap (`progress.svg`), `progress.html`,
`progress.json`, and `STATUS.md` is computed strictly from the CSV/JSON files under `symbols/`
(zero game data):

- **Functions & bytes:** `symbols/ghidra_functions.csv` provides the denominator;
  `symbols/functions.csv` and `symbols/families/` provide the replaced, edge-verified, and verified
  numerators.
- **Libraries & source files:** `symbols/source_files.csv` + `game/progress_map.csv` group code
  into engine vs. middleware blocks in the treemap.
- **Evidence & reviews:** `symbols/verification.csv`, `symbols/matching.csv`, and
  `symbols/reviews.csv` track test recordings, byte-exact matches, and independent reviews.
- **Imports:** `symbols/imports.csv` (and `symbols/boot.json` once the runtime harness is wired)
  track Win32/middleware imports resolved by the runtime.

## Target

PC release of *The Walking Dead: The Final Season* (`WalkingDead4.exe`, x86-64 PE, image base
`0x140000000`). Expected SHA-256 hashes of the target binaries:

<!-- target:start -->
```
487f873e9e869f8654db00a8ce2018c6930df7089e25190946dde40d7444f336  WalkingDead4.exe
d94745b9908ebb74b07f283e4c6621c43e37709b9fa9c949e64b1fa26db5c1f8  fmod64.dll
32f16998fddf75cebb051ea5a12d2bf2ee3f0f2ef4d5d9c2f03c85d4fc94e8c9  fmodstudio64.dll
df9b57b29df860d3d3248e2e7f1799cb414be48f09863678c3e6791c6915edc3  oo2core_5_win64.dll
a50a7f00bf8d28ab3ceb645344e86e68e3d2ec97935dcd94c0d8e31cc744b64a  TTMediaEngine64.dll
```
<!-- target:end -->
