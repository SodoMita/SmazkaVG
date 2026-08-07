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

echo "== legacy .smazka extension still accepted =="
cp examples/triangle_v1.2.smazkavg "$BUILD/t/legacy.smazka"
"$BUILD/smazka-raster" "$BUILD/t/legacy.smazka" 128 128 >/dev/null 2>&1
check "legacy .smazka input renders" "[ $? -eq 0 ]"

echo "== include (inc) inlining + unsafe records =="
mkdir -p "$BUILD/t/inc"
printf 'v 10 10 10\nv 11 50 10\ne 20 10 11\n' > "$BUILD/t/inc/part.smazkavg"
printf 'v 0 0 0\nv 1 100 0\ne 0 0 1\ninc part.smazkavg\nt 0 50 50 size=12 hi\nimg 1 10 10 40 40 logo.png\n' > "$BUILD/t/inc/main.smazkavg"
"$BUILD/smazka-raster" "$BUILD/t/inc/main.smazkavg" 128 128 >/dev/null 2>&1
python3 - "$BUILD/t/inc/main.smazkavg" <<'EOF'
import sys, re
# main(2 verts) + part(2 verts) with sparse ids 10,11 -> n_v = max_id+1 = 12
# we can't read n_v from the render easily; check the include rendered without
# parse errors by rendering again and scanning stderr for 'unknown'
import subprocess
r = subprocess.run(['./build/smazka-raster', sys.argv[1], '128', '128'],
                   capture_output=True, text=True, cwd='.')
err = r.stderr
assert 'unknown command' not in err, err
assert 'unsafe text record skipped' in err, "text record should warn"
assert 'unsafe raster record' in err, "img record should warn"
print("  PASS: include inlined (no parse errors), t/img warned+skipped")
EOF
check "include + unsafe records handled" "[ $? -eq 0 ]"
"$BUILD/smazka-raster" "$BUILD/t/inc/cyc_a.smazkavg" 64 64 >/dev/null 2>&1 || true
printf 'inc cyc_b.smazkavg\n' > "$BUILD/t/inc/cyc_a.smazkavg"
printf 'inc cyc_a.smazkavg\n' > "$BUILD/t/inc/cyc_b.smazkavg"
"$BUILD/smazka-raster" "$BUILD/t/inc/cyc_a.smazkavg" 64 64 2>&1 | grep -q "include depth"
check "include cycle guard" "[ $? -eq 0 ]"

echo "== smazka-sanitize (unsafe -> safe) =="
make sanitize > "$BUILD/t/sanitize-build.log" 2>&1
check "smazka-sanitize builds" "[ $? -eq 0 ]"
"$BUILD/smazka-sanitize" "$BUILD/t/inc/main.smazkavg" "$BUILD/t/inc/main.safe" 2>/dev/null
python3 - "$BUILD/t/inc/main.safe" <<'EOF'
import sys
lines = open(sys.argv[1]).read().splitlines()
data = [l for l in lines if l and not l.startswith('#')]
assert 'inc ' not in ' '.join(data), "include should be inlined"
assert not any(l.startswith(('t ', 'img ')) for l in data), "unsafe records should be stripped"
assert any(l.startswith('v ') for l in data) and any(l.startswith('e ') for l in data), "geometry preserved"
print("  PASS: sanitize inlines inc, strips t/img, preserves geometry")
EOF
check "sanitize produces a safe document" "[ $? -eq 0 ]"
"$BUILD/smazka-raster" "$BUILD/t/inc/main.safe" 128 128 2>&1 | grep -q "0 warnings"
check "sanitized document renders clean" "[ $? -eq 0 ]"

echo "== parser hardening (no crashes) =="
printf 'v 0 0 0\nv 999999 100 0\ne 0 0 1\n' > "$BUILD/t/evil1.smazkavg"
printf 'v 0 0 0\nv 1 100 0\ne 0 0 1\n' > "$BUILD/t/evil2.smazkavg"
python3 - "$BUILD/t/evil2.smazkavg" <<'EOF'
import sys
with open(sys.argv[1], 'a') as f:
    for i in range(300):
        f.write(f'c {i} min_dist 0 1 5.0\n')
EOF
printf 'v -5 0 0\nv 0 0 0\nv 1 100 0\ne 0 0 1\n' > "$BUILD/t/evil3.smazkavg"
printf 'v 0 0 0\nv 1 100 0\ne 0 0 999999\n' > "$BUILD/t/evil4.smazkavg"
for t in evil1 evil2 evil3 evil4; do
    "$BUILD/smazka-raster" "$BUILD/t/$t.smazkavg" 128 128 >/dev/null 2>&1
    rc=$?
    check "$t: exits 0 (no segfault)" "[ $rc -eq 0 ]"
done

echo "== examples render cleanly =="
for f in examples/*.smazka examples/*.smazkavg; do
    [ -f "$f" ] || continue
    base=$(basename "$f" .smazkavg); base=${base%.smazka}
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
        check "solver self-test passes (11 tests incl. LP/QP)" "[ $rc -eq 0 ]"
        check "solver tests LP bbox (test6)" "grep -q 'PASS test6' '$BUILD/t/solver.out'"
        check "solver tests LP min_dist SLP (test8)" "grep -q 'PASS test8' '$BUILD/t/solver.out'"
        check "solver tests QP fair_blend (test9)" "grep -q 'PASS test9' '$BUILD/t/solver.out'"
        check "solver tests QP min_stretch (test10)" "grep -q 'PASS test10' '$BUILD/t/solver.out'"
        check "solver tests QP rig equilibrium (test11)" "grep -q 'PASS test11' '$BUILD/t/solver.out'"
    fi
else
    echo "  SKIP: psolve submodule not checked out (git submodule update --init)"
fi

echo "== golf dialect =="
"$BUILD/smazka-golf" examples/golf_face.sg "$BUILD/t/golf.smazkavg"
check "golf expands" "[ -s '$BUILD/t/golf.smazkavg' ]"
"$BUILD/smazka-raster" "$BUILD/t/golf.smazkavg" 256 256 >/dev/null 2>&1
check "golf output renders" "[ $? -eq 0 ]"

echo "== PNG output =="
"$BUILD/smazka-raster" examples/triangle_v1.2.smazkavg 256 256 >/dev/null 2>&1
python3 - "$ROOT" <<'EOF'
import sys
from PIL import Image
import numpy as np
png = np.asarray(Image.open(sys.argv[1] + '/examples/triangle_v1.2.png').convert('RGB'))
bmp = np.asarray(Image.open(sys.argv[1] + '/examples/triangle_v1.2.bmp').convert('RGB'))
assert png.shape == bmp.shape == (256, 256, 3), png.shape
assert (png == bmp).all(), "PNG pixels differ from BMP"
print("  PASS: PNG decodes and matches BMP pixel-for-pixel")
EOF
check "PNG valid + matches BMP" "[ $? -eq 0 ]"

echo "== stroke caps & joins =="
cat > "$BUILD/t/caps.smazkavg" <<'EOF'
v 0 50 50
v 1 100 50
v 2 50 150
v 3 150 150
e 0 0 1
e 1 2 3
s 0 0 FF0000 20 20 cap=round
s 1 1 0000FF 20 20 cap=butt
EOF
cat > "$BUILD/t/join.smazkavg" <<'EOF'
v 0 100 100
v 1 200 100
v 2 100 200
e 0 0 1
e 1 0 2
s 0 0 000000 30 30
s 1 1 000000 30 30
EOF
"$BUILD/smazka-raster" "$BUILD/t/caps.smazkavg" 256 256 >/dev/null 2>&1
"$BUILD/smazka-raster" "$BUILD/t/join.smazkavg" 256 256 >/dev/null 2>&1
"$BUILD/smazka-raster" "$BUILD/t/join_butt.smazkavg" 256 256 >/dev/null 2>&1 2>/dev/null || true
sed 's/ 30 30$/ 30 30 cap=butt/' "$BUILD/t/join.smazkavg" > "$BUILD/t/joinb.smazkavg"
"$BUILD/smazka-raster" "$BUILD/t/joinb.smazkavg" 256 256 >/dev/null 2>&1
python3 - "$BUILD/t" <<'EOF'
import sys, os
from PIL import Image
import numpy as np
t = sys.argv[1]
im = np.asarray(Image.open(t + '/caps.bmp').convert('RGB')).astype(int)
red = (im[:,:,0]>150)&(im[:,:,1]<100)&(im[:,:,2]<100)
blue = (im[:,:,2]>150)&(im[:,:,0]<100)&(im[:,:,1]<100)
r = red[34:66, :]; b = blue[190:222, :]
# strip vertex markers
for m in (r, b):
    m[13:19, 47:53] = False
assert r[:, :50].sum() > 0, "round cap missing"
assert b[:, :50].sum() == 0, "butt cap not flat"
j1 = np.asarray(Image.open(t + '/join.bmp').convert('RGB')).astype(int)
j2 = np.asarray(Image.open(t + '/joinb.bmp').convert('RGB')).astype(int)
d1 = (j1[:,:,0]<120)&(j1[:,:,1]<120)&(j1[:,:,2]<120)
d2 = (j2[:,:,0]<120)&(j2[:,:,1]<120)&(j2[:,:,2]<120)
assert d1[:50,:50].sum() > 0, "round join notch not filled"
assert d2[:50,:50].sum() == 0, "butt join should leave notch empty"
print("  PASS: round/butt caps and round-vs-butt joins")
EOF
check "caps + joins render correctly" "[ $? -eq 0 ]"

echo "== diffusion (Poisson) =="
cat > "$BUILD/t/diff.smazkavg" <<'EOF'
v 0 200 50
v 1 200 350
e 0 0 1
p 0 diffusion 0 L FF0000 R 0000FF
EOF
"$BUILD/smazka-raster" "$BUILD/t/diff.smazkavg" 256 256 >/dev/null 2>&1
cp "$BUILD/t/diff.bmp" "$BUILD/t/diff_r1.bmp"
"$BUILD/smazka-raster" "$BUILD/t/diff.smazkavg" 256 256 >/dev/null 2>&1
cmp -s "$BUILD/t/diff.bmp" "$BUILD/t/diff_r1.bmp"
check "diffusion deterministic (byte-identical reruns)" "[ $? -eq 0 ]"
python3 - "$BUILD/t" <<'EOF'
import sys
from PIL import Image
import numpy as np
im = np.asarray(Image.open(sys.argv[1] + '/diff.bmp').convert('RGB')).astype(int)
row = im[128].astype(float)
# find the transition (red/blue boundary); sides are relative to travel direction
rb = row[:,0] - row[:,2]
line = int(np.argmax(np.abs(np.diff(rb))))
# near the curve (3-5 px): left side blue-dominant, right side red-dominant
left = row[line-5:line-3]; right = row[line+3:line+5]
assert left[:,0].mean() < 80 and left[:,2].mean() > 200, f"left side not blue {left.mean(axis=0)}"
assert right[:,0].mean() > 200 and right[:,2].mean() < 80, f"right side not red {right.mean(axis=0)}"
# smoothness away from the 6px line: no jumps > 12
mask = np.ones(len(row), bool); mask[max(line-6,0):line+6] = False
mx = np.abs(np.diff(row, axis=0)).max(axis=1)[mask[:-1]].max()
assert mx < 12, f"diffusion not smooth (max jump {mx})"
# far from the curve the field returns toward the white background
far = row[max(line-30,0):max(line-25,0)]
assert far.mean() > 190, f"diffusion did not fall off to background {far.mean()}"
print("  PASS: diffusion boundary colors + smooth harmonic gradient + falloff")
EOF
check "diffusion solves Poisson correctly" "[ $? -eq 0 ]"

echo "== animation: frame sequence =="
cat > "$BUILD/t/anim.smazkavg" <<'EOF'
v 0 0 0
v 1 40 0
v 2 40 40
v 3 0 40
e 0 0 1
e 1 1 2
e 2 2 3
e 3 3 0
f 0 0 1 2 3 66CCFF
n 0 content=0
n 1 content=1
n 2 content=2
n 3 content=3
k 0 0 0.0 tx=0
k 1 0 1.0 tx=160
k 2 1 0.0 tx=0
k 3 1 1.0 tx=160
k 4 2 0.0 tx=0
k 5 2 1.0 tx=160
k 6 3 0.0 tx=0
k 7 3 1.0 tx=160
EOF
"$BUILD/smazka-raster" "$BUILD/t/anim.smazkavg" 256 256 --anim 4 5 --out "$BUILD/t/animf" >/dev/null 2>&1
python3 - "$BUILD/t" <<'EOF'
import sys
from PIL import Image
import numpy as np
t = sys.argv[1]
def cx(i):
    im = np.asarray(Image.open(f"{t}/animf_{i:03d}.png").convert('RGB')).astype(int)
    fill = (np.abs(im[:,:,0]-0x66)<=8)&(np.abs(im[:,:,1]-0xCC)<=8)&(np.abs(im[:,:,2]-0xFF)<=8)
    ys,xs = np.where(fill)
    return xs.mean() if len(xs) else None
xs = [cx(i) for i in range(5)]
assert all(x is not None for x in xs), "fill missing in frames"
assert all(xs[i] < xs[i+1] for i in range(4)), f"motion not monotonic {xs}"
steps = [xs[i+1]-xs[i] for i in range(4)]
assert max(steps)-min(steps) < 3, f"interpolation not uniform {steps}"
print("  PASS: 5 frames, monotonic, uniform steps")
EOF
check "frame sequence renders with uniform motion" "[ $? -eq 0 ]"

echo "== animation: loop wrap =="
"$BUILD/smazka-raster" "$BUILD/t/anim.smazkavg" 256 256 --anim 4 8 --loop --out "$BUILD/t/animl" >/dev/null 2>&1
python3 - "$BUILD/t" <<'EOF'
import sys
from PIL import Image
import numpy as np
t = sys.argv[1]
def cx(i):
    im = np.asarray(Image.open(f"{t}/animl_{i:03d}.png").convert('RGB')).astype(int)
    fill = (np.abs(im[:,:,0]-0x66)<=8)&(np.abs(im[:,:,1]-0xCC)<=8)&(np.abs(im[:,:,2]-0xFF)<=8)
    ys,xs = np.where(fill)
    return xs.mean() if len(xs) else None
xs = [cx(i) for i in range(8)]
assert xs[4] < xs[3] - 30, f"time did not wrap (frame4={xs[4]} frame3={xs[3]})"
print("  PASS: loop wraps time (sawtooth)")
EOF
check "animation loop wraps" "[ $? -eq 0 ]"

echo "== animation: GIF assembly + keyframe round-trip =="
"$BUILD/smazka-raster" examples/animation_demo.smazkavg 320 240 --anim 12 12 --loop --out "$BUILD/t/ad" >/dev/null 2>&1
python3 - "$BUILD/t" <<'EOF'
import sys, os
from PIL import Image
g = Image.open(sys.argv[1] + '/ad.gif')
assert g.n_frames == 12, f"GIF frames {g.n_frames}"
print("  PASS: animated GIF has 12 frames")
EOF
check "animated GIF assembled" "[ $? -eq 0 ]"
"$BUILD/smazka-bin" enc examples/animation_demo.smazkavg "$BUILD/t/ad.smvg" 2>/dev/null
"$BUILD/smazka-bin" dec "$BUILD/t/ad.smvg" "$BUILD/t/ad.rt" 2>/dev/null
check "keyframes round-trip through binary container" "[ "$(grep -c '^k ' examples/animation_demo.smazkavg)" = "$(grep -c '^k ' "$BUILD/t/ad.rt")" ]"

echo "== face holes =="
"$BUILD/smazka-raster" examples/donut.smazkavg 256 256 >/dev/null 2>&1
python3 - "$ROOT" <<'EOF'
import sys
from PIL import Image
import numpy as np
im = np.asarray(Image.open(sys.argv[1] + '/examples/donut.png').convert('RGB')).astype(int)
fill = (np.abs(im[:,:,0]-0xFF)<=8)&(np.abs(im[:,:,1]-0xAA)<=8)&(np.abs(im[:,:,2]-0)<=8)
sc, ox, oy = 0.4333, 41.33, 63.0
def px(wx,wy): return (int(round(ox+wx*sc)), int(round(oy+wy*sc)))
hx,hy = px(200,160)
assert not fill[hy,hx], "hole center should be empty"
rx,ry = px(80,80)
assert fill[ry,rx], "ring should be filled"
fx,fy = px(400,300)
assert not fill[fy,fx], "outside should be empty"
print("  PASS: donut hole empty, ring filled, outside empty")
EOF
check "face holes render correctly" "[ $? -eq 0 ]"

echo "== node transforms =="
cat > "$BUILD/t/node.smazkavg" <<'EOF'
v 0 100 100
v 1 100 0
e 0 0 1
n 0 tx=0 ty=100 content=0
n 1 rot=1.5707963 content=1
EOF
"$BUILD/smazka-raster" "$BUILD/t/node.smazkavg" 256 256 >/dev/null 2>&1
python3 - "$BUILD/t/node.bmp" <<'EOF'
import sys
from PIL import Image
import numpy as np
im = np.asarray(Image.open(sys.argv[1]).convert('RGB')).astype(int)
marker = (im[:,:,0]>180)&(im[:,:,1]<100)&(im[:,:,2]<100)
ys,xs = np.where(marker)
pts = set(zip(xs.tolist(), ys.tolist()))
def near(pts, x, y, tol=4): return any(abs(a-x)<=tol and abs(b-y)<=tol for a,b in pts)
assert near(pts, 205, 206), "translated vertex missing"
assert near(pts, 50, 50), "rotated vertex missing"
print("  PASS: node translation + rotation applied")
EOF
check "node transforms applied" "[ $? -eq 0 ]"

echo "== digit-leading fill regression =="
printf 'v 0 0 0\nv 1 100 0\nv 2 50 86\ne 0 0 1\ne 1 1 2\ne 2 0 2\nf 0 0 1 2 88AA00\n' > "$BUILD/t/digitfill.smazkavg"
"$BUILD/smazka-raster" "$BUILD/t/digitfill.smazkavg" 128 128 >/dev/null 2>&1
python3 - "$BUILD/t/digitfill.bmp" <<'EOF'
import sys
from PIL import Image
import numpy as np
im = np.asarray(Image.open(sys.argv[1]).convert('RGB')).astype(int)
f = (np.abs(im[:,:,0]-0x88)<=8)&(np.abs(im[:,:,1]-0xAA)<=8)&(np.abs(im[:,:,2]-0)<=8)
assert f.sum() > 100, "digit-leading fill color not applied"
print("  PASS: fill color starting with digits parses (88AA00)")
EOF
check "digit-leading fill parses" "[ $? -eq 0 ]"

echo "== combined animation: state machine drives keyframe poses =="
cat > "$BUILD/t/sm.smazkavg" <<'EOF'
v 0 0 0
v 1 40 0
v 2 40 40
v 3 0 40
e 0 0 1
e 1 1 2
e 2 2 3
e 3 3 0
f 0 0 1 2 3 66CCFF
n 0 content=0
n 1 content=1
n 2 content=2
n 3 content=3
a 0 state_machine 0 0  1 time 1.0
k 0 0 0 st=0 tx=0
k 1 0 0 st=1 tx=160
k 2 1 0 st=0 tx=0
k 3 1 0 st=1 tx=160
k 4 2 0 st=0 tx=0
k 5 2 0 st=1 tx=160
k 6 3 0 st=0 tx=0
k 7 3 0 st=1 tx=160
EOF
"$BUILD/smazka-raster" "$BUILD/t/sm.smazkavg" 256 256 --anim 4 5 --out "$BUILD/t/smf" >/dev/null 2>&1
python3 - "$BUILD/t" <<'EOF'
import sys
from PIL import Image
import numpy as np
t = sys.argv[1]
def cx(i):
    im = np.asarray(Image.open(f"{t}/smf_{i:03d}.png").convert('RGB')).astype(int)
    fill = (np.abs(im[:,:,0]-0x66)<=8)&(np.abs(im[:,:,1]-0xCC)<=8)&(np.abs(im[:,:,2]-0xFF)<=8)
    ys,xs = np.where(fill)
    return xs.mean() if len(xs) else None
xs = [cx(i) for i in range(5)]
assert all(x is not None for x in xs)
steps = [xs[i+1]-xs[i] for i in range(4)]
assert max(steps)-min(steps) < 3, f"state-machine blend not linear {steps}"
print("  PASS: state machine blends pose0->pose1 linearly", [round(x,1) for x in xs])
EOF
check "state machine drives keyframe pose blending" "[ $? -eq 0 ]"

echo "== combined anim: bake a frame with smazka-solve --t =="
"$BUILD/smazka-solve" "$BUILD/t/sm.smazkavg" "$BUILD/t/baked.smazkavg" --t 0.5 2>/dev/null
python3 - "$BUILD/t/baked.smazkavg" <<'EOF'
import sys, re
txs = []
for ln in open(sys.argv[1]):
    m = re.search(r'n \d+ tx=([\d.-]+)', ln)
    if m: txs.append(float(m.group(1)))
assert len(txs) == 4, f"expected 4 baked nodes, got {len(txs)}"
assert all(abs(t - 80.0) < 0.1 for t in txs), f"baked tx should be 80 at t=0.5, got {txs}"
assert not any(ln.startswith(('k ', 'a ')) for ln in open(sys.argv[1])), "k/a lines should be stripped"
print("  PASS: smazka-solve --t 0.5 bakes tx=80 and strips k/a")
EOF
check "smazka-solve bakes a combined-animation frame" "[ $? -eq 0 ]"

echo "== combined anim: binary round-trip (st + state_machine) =="
"$BUILD/smazka-bin" enc "$BUILD/t/sm.smazkavg" "$BUILD/t/sm.smvg" 2>/dev/null
"$BUILD/smazka-bin" dec "$BUILD/t/sm.smvg" "$BUILD/t/sm.rt" 2>/dev/null
o=$(grep -cE '^(a|k) ' "$BUILD/t/sm.smazkavg"); r=$(grep -cE '^(a|k) ' "$BUILD/t/sm.rt")
check "state machine + st keyframes round-trip (${o}->${r})" "[ "$o" = "$r" ]"
grep -q "state_machine 0 0 1 time" "$BUILD/t/sm.rt"
check "state machine transitions survive round-trip" "[ $? -eq 0 ]"
grep -q "st=1" "$BUILD/t/sm.rt"
check "keyframe st groups survive round-trip" "[ $? -eq 0 ]"

echo "== smazka-solve pipeline =="
make solve > "$BUILD/t/solve-build.log" 2>&1
check "smazka-solve builds" "[ $? -eq 0 ]"
"$BUILD/smazka-solve" examples/solve_demo.smazkavg "$BUILD/t/solve_demo.out" 2>/dev/null
python3 - "$BUILD/t/solve_demo.out" <<'EOF'
import sys, re
verts = {}
for ln in open(sys.argv[1]):
    m = re.match(r'v (\d+) ([\d.-]+) ([\d.-]+)', ln)
    if m: verts[int(m.group(1))] = (float(m.group(2)), float(m.group(3)))
v0, v1, v2 = verts[0], verts[1], verts[2]
d = ((v1[0]-v0[0])**2 + (v1[1]-v0[1])**2) ** 0.5
assert d >= 49.99, f"min_dist not enforced (d={d})"
assert 50 <= v2[0] <= 200 and 150 <= v2[1] <= 400, "bbox_clamp not enforced"
assert abs(2*v0[0] + v1[1] - 80) < 0.1, f"linear_eq not enforced (2*{v0[0]}+{v1[1]}={2*v0[0]+v1[1]})"
print("  PASS: min_dist + bbox_clamp + linear_eq enforced by resolver")
EOF
check "solve pipeline enforces constraints" "[ $? -eq 0 ]"

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
    src=f'{root}/examples/{f}.smazkavg'; binp=f'{build}/t/{f}.smvg'; rtp=f'{build}/t/{f}.rt.smazkavg'
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
