# Tooling

What actually went into building, verifying, and writing this up.

## Build and verification

- Verilator 5.x for RTL simulation (both 5.040 on MSYS2 and 5.020 on WSL
  Ubuntu are exercised by the suite)
- g++ / C++17 for the golden ISS, the RTL testbenches, and the CLI runners
- Python for the assembler (`scripts/asm.py`), the C-like compiler
  (`scripts/sirc.py`), and small tools like the PPM-to-PNG converter and the
  waveform-to-text viewer, never for anything that needs to be a reference
  result
- GNU Make and bash, with `run_all.sh` as the one command that runs the ISS
  suite, the compiler suite, and the full RTL suite (79 tests) in one shot
- SystemVerilog Assertions compiled into the RTL and checked under
  `--assert`, plus VCD waveform tracing on by default

Host-specific gotchas (the MSYS2 PATH trap, Verilator version differences,
Windows SDK macro collisions, viewing PPM renders and waveforms) are in
[docs/BUILDING.md](docs/BUILDING.md).

## AI-assisted coding

I used Claude Code as a coding tool throughout this project, the way a lot of
people now pair-program with an LLM. I wrote the spec and the milestone plan,
ran the build, and did the hardware debugging myself once things were running
against the golden ISS.

There's a long list of real bugs the tests caught along the way (the full
list is in [docs/WALKTHROUGH.md](docs/WALKTHROUGH.md), and it's genuinely the
most interesting part of the project to read). The one I'd point to first:
once the design went multi-CU with a shared L2, the atomic histogram test
started coming out with every count exactly doubled. The L2 was re-sampling a
port's request signal on the same cycle it acknowledged it, so a single
atomic op got issued twice, invisibly, since loads and stores don't have an
exact-count invariant to catch it against. Fixed by deasserting the request
combinationally on ack. That one only shows up because atomics are the one
operation where "ran twice" is externally observable.

Happy to talk through any part of this in more depth: the reconvergence
stack, the barrel pipeline, the L1/L2 coherence story, any of it.
