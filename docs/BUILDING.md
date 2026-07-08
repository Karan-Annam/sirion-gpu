# Building: environment notes and gotchas

Everything runs from either **native MSYS2 UCRT64** (Windows) or **WSL/Linux**;
the scripts auto-detect the host via `uname` and pick the right toolchain.
`bash run_all.sh` is the one-shot entry point (ISS tests + compiler tests + RTL
tests). This file collects the host quirks that cost me real debugging time, so
nobody has to rediscover them.

## Toolchains

- MSYS2: `verilator_bin.exe` 5.040 + UCRT64 g++ 15, `-std=c++17`, `python`
- WSL Ubuntu: distro `verilator` 5.020 + g++, `python3`

Both are exercised by the full suite; a couple of 5.020-vs-5.040 differences are
listed below.

## MSYS2 traps

1. **PATH ordering.** `/c/msys64/ucrt64/bin` MUST precede `/mingw64/bin`, or
   `g++` fails *silently* (exit 1, empty stderr) from mismatched
   libgmp/libmpfr DLLs. Symptom: `g++ --version` works but any real compile
   instantly dies with no message. The scripts export PATH first thing.
2. **The Perl `verilator` wrapper is broken** on some installs
   (`Can't locate Pod/Usage.pm`). Call `verilator_bin.exe` directly and set
   `VERILATOR_ROOT` to a Windows-style path.
3. **Windows SDK macros collide with innocent identifiers.** `far`, `near`,
   `OUT`, and `DrawState` are all macros on Windows; each one broke a build at
   some point (`Framebuffer::init(..., int32_t far)` was the first). If a
   perfectly reasonable name produces a baffling error on Windows only, suspect
   wingdi.h. (`inside` and `tri` are SystemVerilog keywords: same category of
   surprise, different tool.)

## Verilator notes

- **Nested `--Mdir` isn't auto-created**, `mkdir -p` first.
- **`undefined reference to sc_time_stamp()`** at link: Verilator's legacy
  trace path references it. Fixed with a forced-emit stub in
  `sim/rtl_driver.hpp` (`__attribute__((used))`; plain `inline` gets dropped
  because nothing calls it).
- **Multiple DUTs in one executable break tracing** if they all register with
  the global `VerilatedContext`. Each `Sim` owns a private context and
  constructs its DUT on it.
- **5.020 vs 5.040 differences** found the hard way: 5.020 rejects
  >64-iteration loops with delayed array assignments (the L2's 256-set reset;
  `--unroll-count 1024` fixes it), and the two versions path `--exe` sources
  differently in the generated sub-make. The scripts pass testbench paths as
  absolute (via `cygpath -m` on MSYS2) so both work.
- `--build -j 0` did **not** parallelize the sub-make once the model got large
  (9+ minutes); the scripts verilate first, then run `make -j $(nproc)`
  explicitly (~25 s).
- On WSL, heavy parallel builds on the 9p `/mnt/c` mount are flaky. Copy the
  tree to ext4 (`~/`) for a clean WSL run.

## Viewing output

- **Renders** are binary (P6) PPM. Valid PPM, but many viewers only accept the
  ASCII (P3) variant and call a good P6 file "not a PPM". `make png` converts
  every `build/*.ppm` to a PNG (Pillow if installed, otherwise a built-in
  stdlib zlib encoder, no dependencies). Terminal preview:
  `python scripts/ppm_tool.py build/render_tex_rtl.ppm`.
- **Waveforms**: `make wavetext` renders the newest VCD as a terminal table
  (no GTKWave/X needed). GTKWave opens VCDs with an empty pane until you drag
  signals in and Zoom-Fit, that's GTKWave, not the file.

## Conventions

SVA assertions are compiled in (`--assert`) and guarded by
`` `ifndef SYNTHESIS ``; waveform tracing is on (`--trace`). RTL is listed in
explicit compile order in the Makefile, the package first, never globbed.
