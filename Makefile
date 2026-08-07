# SmazkaVG constraint-first build
# psolve is mandatory: git submodule update --init --recursive

CC       ?= cc
CFLAGS   ?= -O2 -Wall -Wextra
BUILD    := build
PSOLVE   := third_party/psolve
PSOLVE_LIB := $(BUILD)/libpsolve.a

all: smazka

$(BUILD):
	mkdir -p $@

$(PSOLVE_LIB): $(BUILD)
	@if [ ! -f $(PSOLVE)/src/solver.h ]; then \
		echo "mandatory psolve submodule missing; run: git submodule update --init --recursive"; \
		exit 1; \
	fi
	$(MAKE) -C $(PSOLVE) lib
	cp $(PSOLVE)/libpsolve.a $@

smazka: $(PSOLVE_LIB) src/smazka.c src/smazka.h tools/smazka.c
	$(CC) $(CFLAGS) -I$(PSOLVE)/src -Isrc \
	    -o $(BUILD)/smazka tools/smazka.c src/smazka.c $(PSOLVE_LIB) -lm

test: all
	bash tests/run_core_tests.sh

asan:
	$(MAKE) clean
	$(MAKE) CFLAGS='-O1 -g -Wall -Wextra -fsanitize=address,undefined -fno-omit-frame-pointer'
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
	UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
	  bash tests/run_core_tests.sh

clean:
	rm -rf $(BUILD)

.PHONY: all smazka test asan clean
