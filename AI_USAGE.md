# AI usage notice

Most of this repository was written with an AI coding assistant (Anthropic's Claude, used
through Claude Code), working at the maintainer's direction on the maintainer's machine. That
includes:

- the C++ in `src/` (app setup, perf logger and profiler, guest critical-section spinning,
  in-process autoplay input), the recompiler configuration in `config/` (function boundaries,
  static initializers, jump-table overrides, hooks) and the CMake changes;
- the PowerShell and Python tooling in `tools/` (benchmarks, profiling, autoplay sessions and
  reports);
- the ReXGlue SDK change in `patches/` and its unit tests;
- the documentation, including `README.md`, `OPTIMIZATION.md` and these notices.

Reverse-engineering findings (crash causes, lock contention, GPU wait behaviour) came from the
assistant reading the generated code, disassembling the SDK binaries and profiling the running
game.

## How it was checked

Changes were verified by building, by the branch checks and SDK unit tests, and by running the
game: scripted benchmark runs (`tools/perf-run.ps1`), unattended autoplay sessions
(`tools/autoplay.ps1`) and screenshot review. Performance numbers in `OPTIMIZATION.md` come
from those runs on a single machine (RTX 5060 Laptop GPU, i7-13645HX), mostly in the opening
tutorial area.

## Limitations

- The code has not had line-by-line human review. Treat it like any unreviewed contribution.
- Gameplay testing covers the start of the game; missions, other areas and long play sessions
  are largely untested.
- Hardware other than the test machine has not been tried.

Bug reports and reviews are welcome.
