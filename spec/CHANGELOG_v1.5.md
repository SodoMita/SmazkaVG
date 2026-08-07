# SmazkaVG v1.5 — Changelog (from v1.4.1)

> Animation support: keyframes drive node transforms and the rasterizer
> renders frame sequences (PNG/BMP per frame + animated GIF).

---

## 1. Keyframe animation (`k` records)

**Problem:** the format had static documents and a resolver-side
`state_machine` model that computed blend weights, but no declarative way to
animate node transforms over time, and the rasterizer could only render a
single static frame.

**Fix:**
- New record `k <id> <node_id> <time> [tx=..] [ty=..] [rot=..] [sx=..]
  [sy=..] [skew=..]` — a *partial* pose at `time` seconds.
- Each of the six transform fields is animated **independently**,
  piecewise-linearly over the keyframes that set it; fields with no
  keyframes keep the node's base (`n`) value, so a keyframe only lists what
  changes.
- The interpolated pose is baked with the standard node transform semantics
  (§4.6): vertex content moves the vertex; edge content moves endpoints +
  control points (curves animate). To move a rigid shape, use one node per
  vertex with matching keyframes.
- `--loop` wraps time modulo the last keyframe time (closed cycles).

## 2. Frame-sequence rendering

**Problem:** no way to render motion.

**Fix:** `smazka-raster <in> [w] [h] --anim <fps> <frames> [--loop] [--out prefix]`
renders `<prefix>_000.png/.bmp …` at `t = i/fps`. Two details that matter:

- **Fixed camera**: the auto-fit view would follow the moving geometry and
  hide the motion, so the animation view is computed ONCE from the union of
  the base geometry and every keyframe pose (the animation's full bounding
  box) and frozen.
- **Animated GIF**: the PNG frames are assembled into `<prefix>.gif` when PIL
  is available (best-effort, fork/exec like the WebP path).

`--t <seconds>` renders a single frame at a time (useful for debugging or
still captures). Verified: a translation-only animation moves with uniform
per-frame steps; `--loop` produces the expected sawtooth; a jump-arc demo
(spin + rise + return) renders 24 frames + a 24-frame GIF with the apex
mid-animation.

## 3. Tooling

- **Binary container** (`tools/smazka-bin`): `k` records encode/decode with a
  field mask (tag 0x09); round-trip verified on the animation demo.
- **Golf dialect** (`tools/smazka-golf`): `K <node> <time> <tx> <ty> <rot>
  [sx] [sy] [skew]` expands to a keyframe record.

## 4. Docs & tests

- SPEC → v1.5: §4.9 keyframe record (binary layout + Line-ASM + semantics +
  rendering options), binary layout, ABNF.
- `examples/animation_demo.smazka`: spinning square on a jump arc (12 fps ×
  24 frames → `anim_demo.gif`).
- Tests 44 → 50: frame-sequence rendering (uniform steps, monotonic),
  loop wrapping, GIF assembly, keyframe round-trip through the binary
  container.

---

## Summary

| Aspect | v1.4.1 | v1.5 |
|---|---|---|
| Motion model | Static documents | **Keyframe records (`k`) → node transforms** |
| Rendering | Single frame | **`--anim <fps> <frames>` frame sequences + GIF** |
| Camera | Auto-fit | **Fixed animation-view (full bbox)** |
| Time | — | **`--t` single frame, `--loop` wrap** |
| Binary container | — | **`k` records (tag 0x09)** |
| Golf dialect | — | **`K` shorthand** |
| Tests | 44 | **50** |
