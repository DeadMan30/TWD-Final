# Contributing

TWD-Final is a clean-room decompilation of *The Walking Dead: The Final Season* (PC) with a native
Android runtime: the game's code rewritten as readable C and C++, one verified function at a time.

<!-- status:start -->
The game doesn't run on TWD-Final yet: 33 of 28021 functions are verified (0.12%) and 36 replaced (0.13%). On [our native runtime](runtime/) the executable's boot gets as far as reached_loop, with 556 of 556 imports implemented.
<!-- status:end -->

The goal is a complete native Android source port: every function rewritten and verified against the
original executable, running on our own Android runtime ([docs/OVERVIEW.md](docs/OVERVIEW.md)).

## Getting started

1. Read [README.md](README.md) and [docs/OVERVIEW.md](docs/OVERVIEW.md) for the layout and goals.
2. Place your own legal PC copy of the game in `game/` (it is git-ignored; never force-add game
   files).
3. Install the local git hooks (`tools/install_hooks.sh`) so accidental game files or stale
   generated blocks are caught before commit.

## Decompilation workflow

1. **Branch** from `main`: one function or tightly coupled cluster per branch.
2. **Rewrite and verify**: write readable C/C++ in `game/<system>/` (no `FUN_`, `DAT_`, or
   `undefined8` names left behind) and register the hook in `game/hooks.csv`.
3. **Update symbol tables**:
   - Record the function in `symbols/functions.csv` (`address,size,name,system,status,notes`).
   - Record verification runs in `symbols/verification.csv` and independent reviews in
     `symbols/reviews.csv`.
4. **Keep generated progress untouched in feature branches**: `STATUS.md`, `CONTRIBUTING.md`, and
   `docs/OVERVIEW.md` have generated blocks updated on `main` via
   `python3 tools/progress.py --update-readme`. GitHub Pages regenerates the live badges, treemap,
   and `progress.html` automatically on every push to `main`.

A function's status in `symbols/functions.csv` goes through three levels:

| Status | Means |
|---|---|
| `replaced` | registered in `game/hooks.csv` and builds |
| `edge-verified` | passes differential tests on generated edge cases (and any recorded live captures) against the original |
| `verified` | also matches original x86-64 machine bytes (`byte_exact=yes` in `symbols/matching.csv`) and has an approving review in `symbols/reviews.csv` |

## Rules

- **Never commit** game executables (`.exe`, `.dll`), game archives (`.ttarch2`), extracted assets,
  decompiler projects (`.gpr`, `.rep/`, `.gzf`), or raw decompiler dumps.
- `runtime/` stays game-agnostic (Android NDK / Win32-compatibility / graphics / audio / input).
  Game-specific code belongs in `game/<system>/`.
- Never type progress counts or percentages by hand; `tools/progress.py` generates them from
  `symbols/`.

## Naming

- Systems are directories under `game/` (e.g. `engine`, `render`, `audio`, `input`, `fs`,
  `scripting`).
- `symbols/functions.csv` name: lowercase `snake_case` starting with the system, e.g.
  `engine_main_loop`.
- Replacement symbol: `twd_` + name (`extern "C"`), e.g. `twd_engine_main_loop`.
- Addresses: `0x` and lowercase hex digits, ascending in every CSV (`WalkingDead4.exe` image base
  `0x140000000`).

## License

This project is licensed under **GPL-2.0** ([LICENSE](LICENSE)). By contributing, you license your
work under the same terms. New source files should start with
`SPDX-License-Identifier: GPL-2.0-only`.
