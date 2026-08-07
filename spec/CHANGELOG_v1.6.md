# SmazkaVG v1.6 — Changelog (from v1.5)

> Combining the two animation mechanisms: the resolver's state machines now
> DRIVE the renderer's keyframe timelines via pose blending.

---

## 1. State machines drive keyframe poses

**Problem:** v1.5 had two independent animation systems that never talked to
each other: keyframe timelines (`k` records, renderer-side, piecewise-linear
node transform animation) and state machines (`a state_machine`, solver-side
blend weights). A document could have one or the other, but not both meaningfully.

**Fix:** `k` records gain an optional `st=<state>` tag defining that state's
**pose**; the state machine's per-frame activations become **blend weights**
over those poses. At frame time `t`:

```
value(node, field) = ( Σ_{active states that define it} w_s · pose_s ) / ( Σ same )
```

— renormalized over the defining states — with the global keyframe timeline
(`k` without `st=`) as fallback for fields no state pose covers, then the
node's base value. Verified: a 2-state machine blends tx=0→160 linearly over
1s (centroid error < 0.7 px); a 3-state idle→walk→jump chain moves the shape
right, dips for the walk bounce, and raises it at the jump.

## 2. Exclusive-chain weight model (was: activation model)

**Problem:** the v1.3.1 activation model kept the initial state permanently
active (baseline 1.0), so a single time-triggered transition saturated at
50/50 and idle never really ended.

**Fix:** exclusive-chain weights: `w_initial = max(0, 1 − Σ a_i)` and
`w_i = a_i · Π_{j>i} (1 − a_j)`, normalized. Later transitions fade earlier
ones, so sequential transitions blend cleanly (idle→walk at 50/50 mid-ramp,
full walk at t=1, walk→jump 50/50 at t=1.5, full jump at t=2). The resolver's
`resolve_state_machines` and its self-test (test 3: frame5 weights now
(0, 1/3, 2/3)) were updated to the same model.

## 3. Resolver `smazka_resolve_anim` + `smazka-solve --t`

**Problem:** the combination existed only in the renderer; the resolver
couldn't compute blended poses.

**Fix:** the resolver gains a keyframe model (`Document.keyframes`) and
`smazka_resolve_anim(doc, t, loop)` implementing the same exclusive-chain
weights + pose blending (pure math, no psolve; solver test 12 verifies tx at
t=0/0.5/1 = 0/80/160 exactly). `smazka-solve in.smazka out.smazka --t 0.5`
bakes the blended pose into the output document (nodes updated, `k`/`a
state_machine` lines stripped) so you can render a specific frame statically.

## 4. Tooling

- **Binary container**: `k` records encode the `st` group (zigzag); `a
  state_machine` records encode/decode with an explicit transition count
  (tag 0x23). Round-trip verified (9 a/k lines ↔ 9).
- **Golf dialect**: `K <node> <time> [st=<n>] <tx> <ty> <rot> [sx] [sy] [skew]`.

## 5. Docs & tests

- SPEC → v1.6 (§4.9 keyframe `st=`, §5.3.1 combined semantics + exclusive
  weights, binary layout).
- `examples/statemachine_demo.smazka`: idle→walk→jump ball with a halo
  (12 fps × 24 frames → `sm_demo.gif`).
- Tests 50 → 54: rasterizer state-machine blend (linear), resolver combined
  anim (test 12), binio `st=`/state_machine round-trip.

---

## Summary

| Aspect | v1.5 | v1.6 |
|---|---|---|
| State machines & keyframes | Independent | **State machine weights blend keyframe pose groups** |
| Weight model | Activation (initial always 1) | **Exclusive-chain (idle→walk→jump)** |
| Keyframes | Global timeline only | **+ `st=<state>` pose groups** |
| Resolver | Weights only | **+ `smazka_resolve_anim` (bakes poses)** |
| smazka-solve | Constraints only | **+ `--t <s>` frame baking** |
| Binary container | `k` (no st) | **`st` + state_machine (0x23)** |
| Tests | 50 | **54** |
