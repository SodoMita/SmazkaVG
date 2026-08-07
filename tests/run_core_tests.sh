#!/usr/bin/env bash
set -u
cd "$(dirname "$0")/.."
BUILD="$PWD/build"
T="$BUILD/core-tests"
mkdir -p "$T"
PASS=0
FAIL=0
ok() { PASS=$((PASS+1)); echo "  PASS: $1"; }
bad() { FAIL=$((FAIL+1)); echo "  FAIL: $1"; }
check() { if eval "$2"; then ok "$1"; else bad "$1"; fi; }

echo "== constraint-first core =="
"$BUILD/smazka" examples/triangle.smazka --resolved "$T/triangle.resolved" --svg "$T/triangle.svg" 100 100 2>"$T/triangle.log"
rc=$?
check "MIP-backed example solves" "[ $rc -eq 0 ] && grep -q 'solved by MIP' '$T/triangle.log'"
check "hard and soft rules resolve expected geometry" "grep -q '^=a.xy:vec2 10 20$' '$T/triangle.resolved'"
check "named degraded status reports residual" "grep -q '^status=degraded tier=20 rule=prefer_y residual=0$' '$T/triangle.log'"
check "resolved output contains constants only" "! grep -Eq '^[?!~]' '$T/triangle.resolved'"
python3 - "$T/triangle.svg" <<'PY'
import sys
s = open(sys.argv[1]).read()
assert s.index('<polygon') < s.index('id="ab"')
assert 'points="10.000000,20.000000 90.000000,20.000000 50.000000,80.000000"' in s
PY
check "absolute Z drives type-specific SVG painting" "[ $? -eq 0 ]"

python3 - examples/triangle.smazka "$T/permuted.smazka" <<'PY'
import random, sys
lines = [line for line in open(sys.argv[1]) if line.strip() and not line.lstrip().startswith('#')]
random.Random(788).shuffle(lines)
open(sys.argv[2], 'w').writelines(lines)
PY
"$BUILD/smazka" "$T/permuted.smazka" --resolved "$T/permuted.resolved" --svg "$T/permuted.svg" 100 100 2>"$T/permuted.log"
check "record permutation preserves solved document" "cmp -s '$T/triangle.resolved' '$T/permuted.resolved'"
check "record permutation preserves SVG bytes" "cmp -s '$T/triangle.svg' '$T/permuted.svg'"
"$BUILD/smazka" "$T/triangle.resolved" --resolved "$T/idempotent.resolved" --svg "$T/idempotent.svg" 100 100 2>"$T/idempotent.log"
check "immutable solved document is idempotent" "cmp -s '$T/triangle.resolved' '$T/idempotent.resolved'"
check "constant and fully solved variable scenes paint identically" "cmp -s '$T/triangle.svg' '$T/idempotent.svg'"

cat >"$T/lp.smazka" <<'EOF'
+V p
=p.exists:bool 1
=p.z:int 1
?p.xy:vec2 7 9 0 100 0 100
!infeasible pin_x p.xy.x = 25
?degraded@5 pin_y p.xy.y ~= 30
EOF
"$BUILD/smazka" "$T/lp.smazka" --resolved "$T/lp.resolved" --check 2>"$T/lp.log"
check "continuous properties use mandatory LP backend" "grep -q 'solved by LP' '$T/lp.log'"
check "LP lexicographic tiers resolve values" "grep -q '^=p.xy:vec2 25 30$' '$T/lp.resolved'"

cat >"$T/objective.smazka" <<'EOF'
+V p
=p.exists:bool 1
=p.z:int 1
?p.xy:vec2 50 9 0 100 0 100
~optimal@1 leftmost p.xy.x min
EOF
"$BUILD/smazka" "$T/objective.smazka" --resolved "$T/objective.resolved" --check 2>"$T/objective.log"
check "symbolic objective tier is solved before canonical seed tier" "grep -q '^=p.xy:vec2 0 9$' '$T/objective.resolved'"
check "named optimal status is reported" "grep -q '^status=optimal tier=1 rule=leftmost value=0$' '$T/objective.log'"

cat >"$T/duplicate-z.smazka" <<'EOF'
+V a
+V b
=a.exists:bool 1
=a.z:int 4
=a.xy:vec2 0 0
=b.exists:bool 1
=b.z:int 4
=b.xy:vec2 1 1
EOF
"$BUILD/smazka" "$T/duplicate-z.smazka" --check >"$T/dz.out" 2>"$T/dz.err"
rc=$?
check "duplicate absolute Z is rejected" "[ $rc -ne 0 ] && grep -q 'duplicate absolute z' '$T/dz.err'"
cat >"$T/missing-z.smazka" <<'EOF'
+V p
=p.exists:bool 1
=p.xy:vec2 0 0
EOF
"$BUILD/smazka" "$T/missing-z.smazka" --check >"$T/mz.out" 2>"$T/mz.err"
rc=$?
check "missing absolute Z is rejected" "[ $rc -ne 0 ] && grep -q 'requires z:int' '$T/mz.err'"

cat >"$T/hard-fail.smazka" <<'EOF'
+V p
=p.exists:bool 1
=p.z:int 1
=p.xy:vec2 2 3
!reject exact p.xy.x = 9
EOF
"$BUILD/smazka" "$T/hard-fail.smazka" --check >"$T/hf.out" 2>"$T/hf.err"
rc=$?
check "named hard failure is reported" "[ $rc -ne 0 ] && grep -q 'exact \[reject\]' '$T/hf.err'"

cat >"$T/bad-status.smazka" <<'EOF'
+V p
=p.exists:bool 1
=p.z:int 1
=p.xy:vec2 2 3
!maybe nope p.xy.x = 2
EOF
"$BUILD/smazka" "$T/bad-status.smazka" --check >"$T/bs.out" 2>"$T/bs.err"
rc=$?
check "invalid failure status is rejected" "[ $rc -ne 0 ] && grep -q 'status is not valid' '$T/bs.err'"

echo
echo "== core result: $PASS passed, $FAIL failed =="
[ "$FAIL" -eq 0 ]
