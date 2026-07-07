# Sirion GPU (SystemVerilog + Verilator + C++). Works from native MSYS2 UCRT64
# or WSL/Linux — host gotchas (silent-g++ PATH trap, broken Perl verilator
# wrapper) are documented in docs/BUILDING.md; the scripts handle them.

CXX                   ?= g++
VERILATOR             ?= verilator_bin.exe
export VERILATOR_ROOT ?= C:/msys64/ucrt64/share/verilator
# Prefer python3 (WSL/Ubuntu) but fall back to python (MSYS2).
PYTHON                ?= $(shell command -v python3 2>/dev/null || command -v python 2>/dev/null || echo python3)

# RTL in explicit compile order — package first (never globbed).
RTL = rtl/sirion_pkg.sv \
      rtl/common/counter.sv \
      rtl/common/sync_fifo.sv \
      rtl/compute/alu.sv \
      rtl/compute/regfile.sv \
      rtl/compute/decode.sv \
      rtl/compute/scoreboard.sv \
      rtl/compute/cu_core.sv \
      rtl/compute/fp_alu.sv \
      rtl/compute/sfu.sv \
      rtl/cache/l1_cache.sv \
      rtl/cache/l1_dcache.sv \
      rtl/cache/l2_cache.sv \
      rtl/scheduler/warp_sched.sv \
      rtl/scheduler/cta_dispatch.sv \
      rtl/gpu_top.sv \
      rtl/graphics/raster.sv \
      rtl/graphics/gfx_seq.sv \
      rtl/gpu_gfx_top.sv

# -Wno-MULTITOP: these are independent leaf modules; no integrating top exists until M3.
LINT_WAIVERS = -Wall -Wno-UNUSED -Wno-DECLFILENAME -Wno-IMPORTSTAR \
               -Wno-WIDTHEXPAND -Wno-WIDTHTRUNC -Wno-PINCONNECTEMPTY -Wno-MULTITOP \
               -Wno-VARHIDDEN   # function-local params (min/max/cmp) share short names by design

.PHONY: all sim test hl render png lint waves wavetext analysis clean help

all: sim ## build + run the RTL test suite (default)

sim: ## Verilate + run all self-checking RTL unit tests
	bash sim/run_rtl_tests.sh

test: ## assemble kernels + run golden-ISS self-checking tests
	bash sim/run_iss_tests.sh

hl: ## compile high-level kernels (.k -> .s -> .gpubin) and run them on the ISS
	bash sim/run_hl_tests.sh

runner: ## build the CLI kernel runner on the golden ISS (build/sirion_run.exe)
	$(CXX) -std=c++17 -O2 -Isim/iss tools/sirion_run.cpp -o build/sirion_run.exe
	@echo "built build/sirion_run.exe — see docs/PROGRAMMING.md"

RUNNER_RTL_SRCS = rtl/sirion_pkg.sv rtl/compute/alu.sv rtl/compute/fp_alu.sv rtl/compute/sfu.sv \
    rtl/compute/regfile.sv rtl/compute/decode.sv rtl/scheduler/warp_sched.sv \
    rtl/cache/l1_dcache.sv rtl/cache/l2_cache.sv rtl/compute/cu_core.sv \
    rtl/scheduler/cta_dispatch.sv rtl/gpu_top.sv
runner-rtl: ## build the CLI kernel runner on the REAL RTL GPU (build/sirion_run_rtl.exe)
	$(VERILATOR) --cc --exe --trace --assert -j 0 --unroll-count 1024 $(LINT_WAIVERS) \
	  --top-module gpu_top --Mdir obj_dir/runner -o sirion_run_rtl \
	  --CFLAGS "-std=c++17 -DSIRION_RTL" $(RUNNER_RTL_SRCS) \
	  "$(abspath tools/sirion_run.cpp)"
	$(MAKE) -j $(shell nproc 2>/dev/null || echo 8) -C obj_dir/runner -f Vgpu_top.mk
	cp obj_dir/runner/sirion_run_rtl* build/ 2>/dev/null || true
	@echo "built build/sirion_run_rtl.exe — see docs/PROGRAMMING.md"

render: ## software-render the demo scene, preview it, and write a viewable PNG
	$(CXX) $(CXXFLAGS) -Isim/gfx sim/tests/render_demo.cpp -o build/render_demo.exe
	./build/render_demo.exe build/render.ppm
	$(PYTHON) scripts/ppm_tool.py build/render.ppm --png build/render.png

png: ## convert every build/*.ppm to a PNG (opens in any image viewer)
	@for p in build/*.ppm; do \
	   [ -e "$$p" ] || { echo "no build/*.ppm — run 'make sim' or 'make render' first"; break; }; \
	   $(PYTHON) scripts/ppm_tool.py "$$p" --no-preview --png "$${p%.ppm}.png"; \
	 done

lint: ## Verilator lint-only over all RTL (package first)
	$(VERILATOR) --lint-only $(LINT_WAIVERS) $(RTL)

waves: ## open the newest waveform in GTKWave (if installed)
	@vcd=$$(ls -t build/*.vcd 2>/dev/null | head -1); \
	 if [ -n "$$vcd" ]; then echo "opening $$vcd"; gtkwave "$$vcd" >/dev/null 2>&1 & \
	 else echo "no VCD yet — run 'make sim' first"; fi

wavetext: ## render the newest waveform as a terminal table (no GUI; needs no X/GTKWave)
	@vcd=$$(ls -t build/*.vcd 2>/dev/null | head -1); \
	 if [ -n "$$vcd" ]; then $(PYTHON) scripts/vcd2txt.py "$$vcd"; \
	 else echo "no VCD yet — run 'make sim' first"; fi

analysis: ## run Python analysis/plot scripts (later milestones)
	@echo "analysis scripts arrive in later milestones"

clean: ## remove build artifacts
	rm -rf obj_dir sim/obj_* build/*.vcd build/*.log build/*.exe
	@echo cleaned

help: ## list targets
	@grep -E '^[a-zA-Z_-]+:.*?## .*$$' $(MAKEFILE_LIST) \
	 | awk 'BEGIN{FS=":.*?## "}{printf "  \033[36m%-10s\033[0m %s\n", $$1, $$2}'
