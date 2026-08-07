# SmazkaVG build & test
# Targets:
#   make            build all binaries into build/
#   make test       run the full test suite (tests/run_tests.sh)
#   make clean      remove build artifacts

CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra
BUILD   := build

all: raster golf bin resolver-test

raster: src/rasterizer.c
	$(CC) $(CFLAGS) -o $(BUILD)/smazka-raster src/rasterizer.c -lm

golf: tools/smazka-golf.c
	$(CC) $(CFLAGS) -o $(BUILD)/smazka-golf tools/smazka-golf.c -lm

bin: tools/smazka-bin.c
	$(CC) $(CFLAGS) -o $(BUILD)/smazka-bin tools/smazka-bin.c

resolver-test: src/resolver.c
	$(CC) $(CFLAGS) -DSMZ_STANDALONE -o $(BUILD)/resolver-test src/resolver.c -lm

test: all
	@mkdir -p $(BUILD)
	bash tests/run_tests.sh

clean:
	rm -rf $(BUILD)

.PHONY: all raster golf bin resolver-test test clean
