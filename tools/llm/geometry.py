"""geometry.py -- deterministic fit/curve helpers for dot-first LLM authoring.

All functions are pure-python, dependency-free, and operate on plain lists of
(x, y) tuples in source-image pixel coordinates.

Authored dots must sit INSIDE the source strokes (see imgscan.py -- verify
with in_stroke()). These helpers only *connect* dots into curves; they never
invent geometry on their own.

Key doctrine (hard-won):
  * smooth strands  -> 'catmull' chains (curve passes through every dot)
  * zigzag detail (fingers, teeth, toe caps) -> 'seg' chains (polyline).
    Any smoothing pass (Chaikin, over-length catmull spans) eats valleys and
    turns fingers into nubs.
  * long spans between dots make catmull wobble; keep dots 10-40 px apart on
    curves, denser in tight bends, sparser on straights.
"""
import math

Pt = tuple  # (x, y) float/int


# ---------------------------------------------------------------- sampling
def arclen_resample(pts, step=8.0, closed=False):
    """Resample a polyline to roughly uniform spacing (for tessellation)."""
    if len(pts) < 2:
        return list(pts)
    src = list(pts) + ([pts[0]] if closed else [])
    out = [src[0]]
    acc = 0.0
    px, py = src[0]
    for q in src[1:]:
        qx, qy = q
        d = math.hypot(qx - px, qy - py)
        while acc + d >= step and d > 0:
            t = (step - acc) / d
            nx, ny = px + t * (qx - px), py + t * (qy - py)
            out.append((nx, ny))
            px, py, d = nx, ny, math.hypot(qx - nx, qy - ny)
            acc = 0.0
        acc += d
        px, py = qx, qy
    out.append(src[-1])
    if closed:
        out = out[:-1]
    return out


def catmull_eval(p0, p1, p2, p3, t):
    """Uniform Catmull-Rom (matches rasterizer.f conversion: cp = p+(n-p)/6)."""
    t2 = t * t
    t3 = t2 * t
    x = 0.5 * ((2 * p1[0]) + (-p0[0] + p2[0]) * t +
               (2 * p0[0] - 5 * p1[0] + 4 * p2[0] - p3[0]) * t2 +
               (-p0[0] + 3 * p1[0] - 3 * p2[0] + p3[0]) * t3)
    y = 0.5 * ((2 * p1[1]) + (-p0[1] + p2[1]) * t +
               (2 * p0[1] - 5 * p1[1] + 4 * p2[1] - p3[1]) * t2 +
               (-p0[1] + 3 * p1[1] - 3 * p2[1] + p3[1]) * t3)
    return (x, y)


def tessellate(pts, smooth=True, closed=False, step=6.0):
    """Turn authored dots into a dense point list matching what a renderer
    will draw.  smooth=True -> catmull through all dots; False -> polyline."""
    pts = list(pts)
    if len(pts) < 2:
        return pts
    if not smooth:
        return arclen_resample(pts, step, closed)
    # extend ends so the first/last dot get a sensible tangent
    if closed:
        ext = [pts[-1]] + pts + [pts[0], pts[1]]
        rng = range(1, len(pts) + 1)
    else:
        ext = [pts[0]] + pts + [pts[-1]]
        rng = range(1, len(pts))
    out = []
    for i in rng:
        p0, p1, p2, p3 = ext[i - 1], ext[i], ext[i + 1], ext[i + 2]
        seglen = math.hypot(p2[0] - p1[0], p2[1] - p1[1])
        n = max(1, int(seglen / step))
        for k in range(n):
            out.append(catmull_eval(p0, p1, p2, p3, k / n))
    out.append(pts[0] if closed else pts[-1])
    if closed:
        out = out[:-1]
    return out


def chaikin(pts, passes=1, closed=False):
    """Corner-cutting smoothing. WARNING: eats zigzag valleys -- never use
    on finger/tooth detail (use smooth=False at the stroke level instead)."""
    cur = list(pts)
    for _ in range(passes):
        if len(cur) < 3:
            break
        nxt = [] if closed else [cur[0]]
        n = len(cur) if closed else len(cur) - 1
        for i in range(n):
            a, b = cur[i], cur[(i + 1) % len(cur)]
            nxt.append((0.75 * a[0] + 0.25 * b[0], 0.75 * a[1] + 0.25 * b[1]))
            nxt.append((0.25 * a[0] + 0.75 * b[0], 0.25 * a[1] + 0.75 * b[1]))
        if not closed:
            nxt.append(cur[-1])
        cur = nxt
    return cur


# ---------------------------------------------------------------- cleanup
def dp_simplify(pts, eps=1.5):
    """Ramer-Douglas-Peucker: drop dots that barely change the line."""
    if len(pts) < 3:
        return list(pts)
    (x1, y1), (x2, y2) = pts[0], pts[-1]
    dmax, idx = -1.0, 0
    dx, dy = x2 - x1, y2 - y1
    denom = math.hypot(dx, dy) or 1e-9
    for i in range(1, len(pts) - 1):
        d = abs(dy * pts[i][0] - dx * pts[i][1] + x2 * y1 - y2 * x1) / denom
        if d > dmax:
            dmax, idx = d, i
    if dmax <= eps:
        return [pts[0], pts[-1]]
    return dp_simplify(pts[:idx + 1], eps)[:-1] + dp_simplify(pts[idx:], eps)


def trim_ends(pts, r=6.0):
    """Snip r px off both ends of an open chain (so it marries the vertices it
    meets instead of overshooting them; the junction is drawn by the loop)."""
    pts = list(pts)
    if len(pts) < 2:
        return pts
    def cut(seq, r):
        acc = 0.0
        for i in range(1, len(seq)):
            acc += math.hypot(seq[i][0] - seq[i - 1][0], seq[i][1] - seq[i - 1][1])
            if acc >= r:
                return seq[i:]
        return seq[-1:]
    return list(reversed(cut(list(reversed(cut(pts, r))), r)))


# ---------------------------------------------------------------- measurement
def nearest_dist(pts, x, y):
    """Distance from (x, y) to a polyline (sampled segments)."""
    best = 1e18
    for i in range(1, len(pts)):
        ax, ay = pts[i - 1]
        bx, by = pts[i]
        dx, dy = bx - ax, by - ay
        L2 = dx * dx + dy * dy
        t = 0.0 if L2 == 0 else max(0.0, min(1.0, ((x - ax) * dx + (y - ay) * dy) / L2))
        d = math.hypot(x - (ax + t * dx), y - (ay + t * dy))
        if d < best:
            best = d
    return best


def chain(*items, bridge=8.0):
    """Concatenate dot-runs into one ordered list.

    items: point lists, single points, or ('rev', list) to reverse a run.
    If consecutive runs gap more than `bridge` px, a bridge point is inserted
    at the far run's start so the joint stays visible as one line.
    """
    out = []
    for it in items:
        rev = False
        if isinstance(it, tuple) and len(it) == 2 and it[0] == 'rev':
            rev, it = True, it[1]
        if isinstance(it, tuple) and len(it) == 2 and isinstance(it[0], (int, float)):
            run = [(it[0], it[1])]          # bare point
        else:
            run = list(it)
        if rev:
            run = run[::-1]
        if not run:
            continue
        if out:
            gap = math.hypot(run[0][0] - out[-1][0], run[0][1] - out[-1][1])
            if gap > bridge:
                out.append(run[0])          # explicit bridge, never a silent jump
        # avoid duplicate joint points
        if out and math.hypot(run[0][0] - out[-1][0], run[0][1] - out[-1][1]) < 0.5:
            run = run[1:]
        out.extend(run)
    return out
