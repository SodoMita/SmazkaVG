#!/usr/bin/env bash
# SmazkaVG test suite
# Builds everything, runs the security/regression/feature checks and the
# binary round-trip tests.  Exit code 0 = all green.
set -u
cd "$(dirname "$0")/.."
ROOT="$PWD"
BUILD="$ROOT/build"
mkdir -p "$BUILD" "$BUILD/t"

PASS=0; FAIL=0
ok()   { PASS=$((PASS+1)); echo "  PASS: $1"; }
bad()  { FAIL=$((FAIL+1)); echo "  FAIL: $1"; }
check() { if eval "$2"; then ok "$1"; else bad "$1"; fi; }

echo "== build =="
cc -O2 -Wall -Wextra -o "$BUILD/smazka-raster" src/rasterizer.c -lm || { bad "rasterizer build"; exit 1; }
cc -O2 -Wall -Wextra -o "$BUILD/resolver-test" -DSMZ_STANDALONE src/resolver.c -lm || { bad "resolver build"; exit 1; }
cc -O2 -Wall -Wextra -o "$BUILD/smazka-golf" tools/smazka-golf.c -lm || { bad "golf build"; exit 1; }
cc -O2 -Wall -Wextra -o "$BUILD/smazka-bin" tools/smazka-bin.c || { bad "bin build"; exit 1; }
ok "all targets build"

echo "== parser hardening (no crashes) =="
printf 'v 0 0 0\nv 999999 100 0\ne 0 0 1\n' > "$BUILD/t/evil1.smazka"
printf 'v 0 0 0\nv 1 100 0\ne 0 0 1\n' > "$BUILD/t/evil2.smazka"
python3 - "$BUILD/t/evil2.smazka" <<'EOF'
import sys
with open(sys.argv[1], 'a') as f:
    for i in range(300):
        f.write(f'c {i} min_dist 0 1 5.0\n')
EOF
printf 'v -5 0 0\nv 0 0 0\nv 1 100 0\ne 0 0 1\n' > "$BUILD/t/evil3.smazka"
printf 'v 0 0 0\nv 1 100 0\ne 0 0 999999\n' > "$BUILD/t/evil4.smazka"
for t in evil1 evil2 evil3 evil4; do
    "$BUILD/smazka-raster" "$BUILD/t/$t.smazka" 128 128 >/dev/null 2>&1
    rc=$?
    check "$t: exits 0 (no segfault)" "[ $rc -eq 0 ]"
done

echo "== examples render cleanly =="
for f in examples/*.smazka; do
    base=$(basename "$f" .smazka)
    "$BUILD/smazka-raster" "$f" 256 256 >"$BUILD/t/$base.log" 2>&1
    rc=$?
    check "$base renders (rc=0)" "[ $rc -eq 0 ]"
    check "$base no warnings" "! grep -q 'smazka: warning:' '$BUILD/t/$base.log'"
done

echo "== resolver self-test =="
"$BUILD/resolver-test" > "$BUILD/t/resolver.out" 2>&1
rc=$?
check "resolver self-test passes" "[ $rc -eq 0 ]"
check "resolver reports all tests" "grep -q 'ALL RESOLVER TESTS PASSED' '$BUILD/t/resolver.out'"

echo "== solver self-test (psolve backend) =="
if [ -d "$ROOT/third_party/psolve" ]; then
    make solver-test > "$BUILD/t/solver-build.log" 2>&1
    rc=$?
    if [ $rc -eq 0 ]; then
        ok "solver-test builds (psolve submodule)"
    else
        bad "solver-test builds (psolve submodule)"; tail -5 "$BUILD/t/solver-build.log"
    fi
    if [ -x "$BUILD/solver-test" ]; then
        "$BUILD/solver-test" > "$BUILD/t/solver.out" 2>&1
        rc=$?
        check "solver self-test passes (10 tests incl. LP/QP)" "[ $rc -eq 0 ]"
        check "solver tests LP bbox (test6)" "grep -q 'PASS test6' '$BUILD/t/solver.out'"
        check "solver tests LP min_dist SLP (test8)" "grep -q 'PASS test8' '$BUILD/t/solver.out'"
        check "solver tests QP fair_blend (test9)" "grep -q 'PASS test9' '$BUILD/t/solver.out'"
        check "solver tests QP min_stretch (test10)" "grep -q 'PASS test10' '$BUILD/t/solver.out'"
    fi
else
    echo "  SKIP: psolve submodule not checked out (git submodule update --init)"
fi

echo "== golf dialect =="
"$BUILD/smazka-golf" examples/golf_face.sg "$BUILD/t/golf.smazka"
check "golf expands" "[ -s '$BUILD/t/golf.smazka' ]"
"$BUILD/smazka-raster" "$BUILD/t/golf.smazka" 256 256 >/dev/null 2>&1
check "golf output renders" "[ $? -eq 0 ]"

echo "== binary round-trip =="
python3 - "$BUILD" "$ROOT" <<'EOF'
import re, subprocess, sys, os
build, root = sys.argv[1], sys.argv[2]
def is_hex6(t): return len(t)==6 and all(c in '0123456789abcdefABCDEF' for c in t)
def is_hex8(t): return len(t)==8 and all(c in '0123456789abcdefABCDEF' for c in t)
def norm_color(t):
    v = int(t,16)
    if len(t)==6: v = (v<<8)|0xFF
    return f"{v:08X}"
def norm(p):
    lines=[]
    for ln in open(p):
        ln = ln.strip()
        for i,ch in enumerate(ln):
            if ch=='#': ln=ln[:i]; break
        ln = ln.strip()
        if not ln: continue
        parts = ln.split(); out=[parts[0]]
        for i,t in enumerate(parts[1:], start=1):
            is_col=False
            if parts[0]=='s' and i==3 and (is_hex6(t) or is_hex8(t)): is_col=True
            elif parts[0]=='f' and i==len(parts)-1 and (is_hex6(t) or is_hex8(t)): is_col=True
            elif parts[0]=='r' and i==7 and (is_hex6(t) or is_hex8(t)): is_col=True
            elif parts[0]=='z' and i in (7,8) and (is_hex6(t) or is_hex8(t)): is_col=True
            elif parts[0]=='p' and len(parts)>=3:
                if parts[2]=='diffusion' and parts[i-1] in ('L','R') and (is_hex6(t) or is_hex8(t)): is_col=True
                elif parts[2]=='solid_fill' and i==len(parts)-1 and (is_hex6(t) or is_hex8(t)): is_col=True
            if is_col: out.append(norm_color(t))
            elif re.fullmatch(r'-?\d+\.?\d*', t): out.append(f"{float(t):.4f}")
            else: out.append(t)
        lines.append(' '.join(out))
    return sorted(lines)
failures=0
for f in ['golf_face','eyelash_v1.2','triangle_v1.2','curves_v1.3']:
    src=f'{root}/examples/{f}.smazka'; binp=f'{build}/t/{f}.smvg'; rtp=f'{build}/t/{f}.rt.smazka'
    subprocess.run([f'{build}/smazka-bin','enc',src,binp], capture_output=True)
    subprocess.run([f'{build}/smazka-bin','dec',binp,rtp], capture_output=True)
    if norm(src)==norm(rtp):
        print(f"  PASS: {f} round-trip ({os.path.getsize(src)}B -> {os.path.getsize(binp)}B)")
    else:
        failures+=1; print(f"  FAIL: {f} round-trip")
sys.exit(1 if failures else 0)
EOF
rc=$?
check "all binary round-trips" "[ $rc -eq 0 ]"

echo
echo "== result: $PASS passed, $FAIL failed =="
[ "$FAIL" -eq 0 ]
