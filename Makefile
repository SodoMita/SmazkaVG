# SmazkaVG build & test
# Targets:
#   make            build all binaries into build/
#   make test       run the full test suite (tests/run_tests.sh)
#   make solver-test  build the resolver with the psolve LP/QP backend
#   make clean      remove build artifacts
#
# The psolve solver is a git submodule (third_party/psolve).  If it is not
# checked out, run:  git submodule update --init

CC       ?= cc
CFLAGS   ?= -O2 -Wall -Wextra
BUILD    := build
PSOLVE   := third_party/psolve
PSOLVE_LIB := $(BUILD)/libpsolve.a

all: raster golf bin resolver-test solve

$(BUILD):
	mkdir -p $@

raster: $(BUILD) src/rasterizer.c
	$(CC) $(CFLAGS) -o $(BUILD)/smazka-raster src/rasterizer.c -lm

golf: $(BUILD) tools/smazka-golf.c
	$(CC) $(CFLAGS) -o $(BUILD)/smazka-golf tools/smazka-golf.c -lm

bin: $(BUILD) tools/smazka-bin.c
	$(CC) $(CFLAGS) -o $(BUILD)/smazka-bin tools/smazka-bin.c

resolver-test: $(BUILD) src/resolver.c
	$(CC) $(CFLAGS) -DSMZ_STANDALONE -o $(BUILD)/resolver-test src/resolver.c -lm

# psolve-backed resolver (LP + convex QP via the submodule)
$(PSOLVE_LIB): $(BUILD)
	@if [ ! -f $(PSOLVE)/src/solver.h ]; then \
		echo "psolve submodule not checked out; run:  git submodule update --init"; \
		exit 1; \
	fi
	$(MAKE) -C $(PSOLVE) lib
	cp $(PSOLVE)/libpsolve.a $(BUILD)/libpsolve.a

solver-test: $(PSOLVE_LIB) src/resolver.c
	$(CC) $(CFLAGS) -DSMZ_STANDALONE -DSMZ_HAVE_PSOLVE -I$(PSOLVE)/src \
	    -o $(BUILD)/solver-test src/resolver.c $(PSOLVE_LIB) -lm

solve: $(PSOLVE_LIB) tools/smazka-solve.c
	$(CC) $(CFLAGS) -I$(PSOLVE)/src \
	    -o $(BUILD)/smazka-solve tools/smazka-solve.c $(PSOLVE_LIB) -lm

test: all
	@mkdir -p $(BUILD)
	bash tests/run_tests.sh

# LLM dot-first vectorization toolkit (python3 + Pillow + numpy, cairosvg
# optional but recommended). See AGENTS.md.
llm-test: $(BUILD)/smazka-raster
	@command -v python3 >/dev/null || { echo "python3 required"; exit 1; }
	python3 -m tools.llm.selftest

llm-demo: $(BUILD)/smazka-raster
	cd examples/llm_workflow_demo && ./run.sh

clean:
	rm -rf $(BUILD)

.PHONY: all raster golf bin resolver-test solver-test solve test llm-test llm-demo clean
