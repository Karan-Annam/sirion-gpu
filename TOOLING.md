# Tooling

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

## How much of this is AI, honestly

A lot of the first-draft code, and essentially all of the documentation,
including this file, WALKTHROUGH.md, and the README. Claude Code built most
of it from a spec and milestone plan I wrote, and I reviewed, edited, and
debugged from there rather than accepting it as-is. This is the largest of
my four projects and it shows in how much documentation exists; all four
went through the same process, worth saying directly rather than leaving you
to guess why the writing reads consistently across my repos.

Where I actually did the work myself was the hardware debugging, once things
were running against the golden ISS and started disagreeing with it in ways
that mattered. There's a long list of real bugs the tests caught along the
way; the full list is in [docs/WALKTHROUGH.md](docs/WALKTHROUGH.md) and it's
genuinely the most interesting part of the project to read. The one I'd
point to first: once the design went multi-CU with a shared L2, the atomic
histogram test started coming out with every count exactly doubled. The L2
was re-sampling a port's request signal on the same cycle it acknowledged
it, so a single atomic op got issued twice, invisibly, since ordinary loads
and stores don't have an exact-count invariant to catch that against. Fixed
by deasserting the request combinationally on ack. That bug only shows up
because atomics are the one operation where "ran twice" is externally
observable, everything else would have looked fine.

Ask me about the reconvergence stack, the barrel pipeline, the L1/L2
coherence story, or any of the other bugs in the walkthrough, happy to talk
through all of it.
