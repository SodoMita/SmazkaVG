# AGENTS.md

## Before changing semantics

Ask for clarification when a request can alter any of:

- cell topology or incidence;
- property constant/variable typing;
- solver class or failure level;
- absolute Z semantics;
- canonical syntax;
- rendering dispatch.

Do not preserve accidental behavior merely because it already exists. No format contract is frozen yet.

## Non-negotiable invariants

1. Source record order has no meaning.
2. Every reference resolves after the whole file is parsed.
3. Every cell has explicit `exists` and a unique absolute integral `z`.
4. Every variable has a seed and finite bounds.
5. Every render-affecting variable is constrained or canonically tie-broken.
6. psolve is mandatory.
7. Rendering consumes only the immutable solved scene.
8. Constants and resolved types select specialized painting kernels.
9. Failure symbols always include a named status.
10. Unsupported semantics fail; they are never silently dropped.

## Canonical compact syntax

```text
+V point
=point.exists:bool 1
=point.z:int 10
?point.xy:vec2 12 10 0 100 0 100

!infeasible pin_x point.xy.x = 20
?degraded@10 prefer_y point.xy.y ~= 40
~optimal@20 leftmost point.xy.x min
```

ASCII is canonical.

## Development loop

```sh
git submodule update --init --recursive
make clean
make
make test

SAN='-O1 -g -Wall -Wextra -fsanitize=address,undefined -fno-omit-frame-pointer'
make clean
make CFLAGS="$SAN"
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
  bash tests/run_core_tests.sh
```

## Required tests for semantic changes

- source-record permutation invariance;
- canonical resolved byte invariance;
- LP and, where relevant, MIP/QP/PGS execution;
- duplicate/missing Z rejection;
- named failure-status reporting;
- constant and fully constrained variable equivalence;
- solved-scene-only rendering;
- sanitizer pass.

## Architecture and references

- [`spec/FORMAT.md`](spec/FORMAT.md)
- [`spec/ARCHITECTURE.md`](spec/ARCHITECTURE.md)
- [`docs/REFERENCES.md`](docs/REFERENCES.md)
