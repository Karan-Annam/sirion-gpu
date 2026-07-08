# Tooling

## Build and verification

- Verilator 5.x (5.040 on MSYS2, 5.020 on WSL, both exercised by the suite)
- g++ / C++17 for the golden ISS, the RTL testbenches, and the CLI runners
- Python for the assembler and compiler (`asm.py`, `sirc.py`) and small
  tools, never for anything that needs to be a reference result
- GNU Make and bash, `run_all.sh` runs the ISS suite, the compiler suite,
  and all 79 RTL tests
- SVA compiled into the RTL under `--assert`, VCD tracing on by default

Host quirks (the MSYS2 PATH trap, Verilator version differences, Windows SDK
macro collisions) are in [docs/BUILDING.md](docs/BUILDING.md).

## AI use

I wrote the RTL with AI assistance across all the milestones, then debugged
it myself and read through it to modify things as issues came up against
the golden ISS. This is the biggest of my four projects, and the
documentation went through the same process.

Ask about the reconvergence stack, the barrel pipeline, the L1/L2 coherence
story, or anything else here.
