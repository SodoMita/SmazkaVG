#!/usr/bin/env python3
"""audit.py - constraints that SHOW garbage instead of hiding it.

Runs the C expander to obtain canonical geometry (with `# |` record
names intact), then checks, per stroke:

  stray   -- % of the stroke's ink that lands where the SOURCE image
             has no ink within --tol px (the metric law, per stroke)
  hidden  -- stroke fully swallowed by a face that paints after it
             (fps<3 rule: it draws, then dies silently)
  dup     -- two strokes tracing the same curve (line-doubling)
  degen   -- zero-length / duplicate points / sw<=0
  join?   -- endpoint lands 0.75..3.5px from another vertex: an
             intended seam that isn't one

Two outputs: a text report (CI-able: exit code = #errors with
--strict), and OVERLAY.smazka - the format displaying its own
diagnostics: ghosted artwork, garbage in red, dups magenta, hidden
blue, suspect joins orange. Render it and LOOK:

    PYTHONPATH=tools python3 -m llm.audit drawing.smazka \
        --src original.jpg --overlay overlay.smazka
    bin/smazka-raster overlay.smazka 1350 2268 --out overlay

The design rule (vs the resolver path): assertions must REPORT and
PAINT mistakes, never repair them. An assertion that fixes geometry
hides the authoring error that produced it.
"""

import argparse
import os
import subprocess
import sys
from dataclasses import dataclass, field

import numpy as np

_sys_dir = os.path.dirname(os.path.abspath(__file__))
if _sys_dir not in sys.path:
    sys.path.insert(0, _sys_dir)

from imgscan import Source  # noqa: E402

TOOLS_BIN = os.path.join(_sys_dir, "..", "..", "build", "smazka-raster")


# ---------------- xpanded-form parsing ----------------

@dataclass
class Edge:
    eid: str
    a: int
    b: int
    kind: str                     # seg quad cubic rational catmull
    ctrl: tuple = ()              # control points (x,y pairs, flat)
    w: float = 1.0


@dataclass
class Stroke:
    sid: str
    edge: str
    color: str
    w: float
    order: int                    # paint order in doc
    record: str = "?"             # authoring record name via `# |`
    pts: np.ndarray = None        # sampled polyline (filled later)


@dataclass
class Face:
    fid: str
    edges: list
    fill: str
    order: int
    record: str = "?"


@dataclass
class Doc:
    verts: dict = field(default_factory=dict)      # vid(int) -> (x,y)
    edges: dict = field(default_factory=dict)      # eid(str) -> Edge
    strokes: list = field(default_factory=list)    # Stroke, doc order
    faces: list = field(default_factory=list)      # Face, doc order


def xpand_text(path, bin_path=TOOLS_BIN):
    r = subprocess.run([bin_path, path, "--xpand", "-"],
                       capture_output=True, text=True)
    if r.returncode != 0:
        raise SystemExit(f"xpand failed:\n{r.stderr}")
    return r.stdout


def parse_xpanded(text):
    doc = Doc()
    record = "?"
    order = 0
    for ln in text.splitlines():
        if ln.startswith("# |"):
            parts = ln[3:].split()
            if len(parts) >= 2 and parts[0] in ("fobj", "path"):
                record = parts[1]
            else:
                record = parts[0] if parts else "?"
            continue
        if not ln or ln.startswith("#"):
            continue
        t = ln.split()
        if t[0] == "v":
            doc.verts[int(t[1])] = (float(t[2]), float(t[3]))
        elif t[0] == "e":
            kind = t[4].split("=", 1)[-1] if len(t) > 4 else "seg"
            ctrl = tuple(float(v) for v in t[5:])
            doc.edges[t[1]] = Edge(t[1], int(t[2]), int(t[3]), kind, ctrl)
        elif t[0] == "s":
            if len(t) > 2 and t[2] == "group_id":
                continue                      # group membership marker
            doc.strokes.append(Stroke(t[1], t[2], t[3], float(t[4]), order, record))
            order += 1
        elif t[0] == "f":
            frec = [x for x in t[2:] if not x.startswith("|")]
            holes = t[t.index("|") + 1:] if "|" in t else []
            fill = next((x for x in reversed(t[2:]) if all(
                c in "0123456789abcdefABCDEF" for c in x) and len(x) in (6, 8)), "")
            edges = [x for x in frec if x != fill]
            doc.faces.append(Face(t[1], edges, fill or "000000", order, record))
            order += 1
    return doc


# ---------------- curve sampling (matches rasterizer.c) ----------------

def _catmull_neighbors(doc, e):
    ea, eb = e.a, e.b
    pa = (doc.verts[ea][0], doc.verts[ea][1])
    pb = (doc.verts[eb][0], doc.verts[eb][1])
    ta = tb = None
    for eid in sorted(doc.edges):
        if eid == e.eid:
            continue
        o = doc.edges[eid]
        if ta is None and o.b == ea:
            va = doc.verts[o.a]
            ta = (pa[0] - va[0], pa[1] - va[1])
        if tb is None and o.a == eb:
            vb = doc.verts[o.b]
            tb = (vb[0] - pb[0], vb[1] - pb[1])
    if ta is None:
        ta = (pb[0] - pa[0], pb[1] - pa[1])
    if tb is None:
        tb = (pb[0] - pa[0], pb[1] - pa[1])
    return np.array(pa), np.array(pb), np.array(ta), np.array(tb)


def sample_edge(doc, e, n=None):
    pa = np.array(doc.verts[e.a], float)
    pb = np.array(doc.verts[e.b], float)
    if e.kind == "seg":
        n = n or max(3, int(np.hypot(*(pb - pa)) / 3) + 1)
        t = np.linspace(0, 1, n)[:, None]
        return pa * (1 - t) + pb * t
    n = n or 64
    t = np.linspace(0, 1, n)[:, None]
    if e.kind == "quad":
        c = np.array(e.ctrl[:2])
        return (1 - t) ** 2 * pa + 2 * (1 - t) * t * c + t ** 2 * pb
    if e.kind == "cubic":
        c1 = np.array(e.ctrl[:2])
        c2 = np.array(e.ctrl[2:4])
        return ((1 - t) ** 3 * pa + 3 * (1 - t) ** 2 * t * c1
                + 3 * (1 - t) * t ** 2 * c2 + t ** 3 * pb)
    if e.kind == "rational":
        c = np.array(e.ctrl[:2])
        w = e.ctrl[2] if len(e.ctrl) > 2 else 1.0
        num = (1 - t) ** 2 * pa + 2 * w * (1 - t) * t * c + t ** 2 * pb
        den = (1 - t) ** 2 + 2 * w * (1 - t) * t + t ** 2
        return num / den
    if e.kind == "catmull":
        p0, p1, ta, tb = _catmull_neighbors(doc, e)
        t2, t3 = t * t, t * t * t
        return ((2 * t3 - 3 * t2 + 1) * p0 + (t3 - 2 * t2 + t) * ta
                + (-2 * t3 + 3 * t2) * p1 + (t3 - t2) * tb)
    raise ValueError(e.kind)


def sample_stroke(doc, s):
    pts = sample_edge(doc, doc.edges[s.edge])
    s.pts = pts
    return pts


# ---------------- findings ----------------

@dataclass
class Finding:
    kind: str                     # stray hidden dup degen join
    msg: str
    strokes: list = field(default_factory=list)   # Stroke objs
    at: tuple = None              # optional (x,y) marker
    severity: str = "warn"


def check_degenerates(doc):
    out = []
    for s in doc.strokes:
        p = s.pts
        if float(np.hypot(*(p[-1] - p[0]))) < 0.5:
            out.append(Finding("degen", f"{s.sid} ({s.record}): zero-length "
                               f"at ({p[0][0]:.0f},{p[0][1]:.0f})", [s],
                               tuple(p[0]), "error"))
        dups = int((np.hypot(*np.diff(p, axis=0).T) < 0.01).sum())
        if dups > 2:
            out.append(Finding("degen", f"{s.sid} ({s.record}): {dups} "
                               "duplicate consecutive points", [s], tuple(p[0])))
        if s.w <= 0:
            out.append(Finding("degen", f"{s.sid} ({s.record}): sw={s.w}",
                               [s], tuple(p[0]), "error"))
    return out


def _face_polygon(doc, f):
    pts = None
    for eid in f.edges:
        e = doc.edges.get(eid)
        if e is None:
            return None
        ep = sample_edge(doc, e, 32)
        if pts is None:
            pts = ep
        else:
            d0 = np.hypot(*(pts[-1] - ep[0]))
            d1 = np.hypot(*(pts[-1] - ep[-1]))
            pts = np.vstack([pts, ep if d0 <= d1 else ep[::-1]])
    return pts


def check_hidden(doc, pad=8):
    """Stroke fully covered by a later-painting opaque face -> invisible.

    Face masks are rasterized in the stroke's bbox crop; coverage is
    accepted only when the stroke sits >= w/2+1 inside the fill.
    """
    from PIL import Image, ImageDraw
    from scipy.ndimage import distance_transform_edt

    out = []
    faces = []
    for f in doc.faces:
        if len(f.fill) == 8 and int(f.fill[6:8], 16) < 0xF0:
            continue                      # translucent: doesn't fully hide
        poly = _face_polygon(doc, f)
        if poly is None:
            continue
        xs, ys = poly[:, 0], poly[:, 1]
        faces.append((f, poly, (xs.min(), ys.min(), xs.max(), ys.max()), f.order))
    for s in doc.strokes:
        p = s.pts
        sx0, sy0 = p[:, 0].min() - s.w - pad, p[:, 1].min() - s.w - pad
        sx1, sy1 = p[:, 0].max() + s.w + pad, p[:, 1].max() + s.w + pad
        for f, poly, bb, order in faces:
            if order <= s.order:
                continue
            if bb[0] > sx0 or bb[1] > sy0 or bb[2] < sx1 or bb[3] < sy1:
                continue
            w = int(sx1 - sx0) + 2
            h = int(sy1 - sy0) + 2
            if w * h > 4_000_000:
                continue
            im = Image.new("L", (w, h), 0)
            ImageDraw.Draw(im).polygon(
                [(x - sx0, y - sy0) for x, y in poly], fill=255)
            mask = np.array(im) > 0
            edt = distance_transform_edt(mask)
            ix = np.clip((p[:, 0] - sx0).astype(int), 0, w - 1)
            iy = np.clip((p[:, 1] - sy0).astype(int), 0, h - 1)
            d = edt[iy, ix]
            if (d > s.w / 2 + 1).all():
                out.append(Finding(
                    "hidden", f"{s.sid} ({s.record}): fully inside face "
                    f"{f.fid} ({f.record}) which paints over it - dead ink",
                    [s], tuple(p[0])))
                break
    return out


def check_duplicates(doc, mean_tol=1.5, max_tol=4.0, min_len=20.0):
    out = []
    n = 48
    res = []
    for s in doc.strokes:
        p = s.pts
        L = float(np.hypot(*np.diff(p, axis=0).T).sum())
        res.append((s, _resample(p, n), L, (p[:, 0].min(), p[:, 1].min(),
                                            p[:, 0].max(), p[:, 1].max())))
    done = set()
    for i in range(len(res)):
        si, pi, li, bi = res[i]
        if li < min_len:
            continue
        for j in range(i + 1, len(res)):
            sj, pj, lj, bj = res[j]
            if lj < min_len or (si.sid, sj.sid) in done:
                continue
            if bi[0] > bj[2] + 4 or bj[0] > bi[2] + 4 or \
               bi[1] > bj[3] + 4 or bj[1] > bi[3] + 4:
                continue
            if not (0.5 < li / lj < 2.0):
                continue
            d = np.hypot(*(pi - pj).T)
            dr = np.hypot(*(pi - pj[::-1]).T)
            m, mx = (d.mean(), d.max()) if d.mean() <= dr.mean() \
                else (dr.mean(), dr.max())
            if m < mean_tol and mx < max_tol:
                done.add((si.sid, sj.sid))
                out.append(Finding(
                    "dup", f"{si.sid} ({si.record}) ~= {sj.sid} ({sj.record}): "
                    f"same curve within {m:.2f}px - line-doubling",
                    [si, sj], tuple(pi[0])))
    return out


def _resample(p, n):
    d = np.hypot(*np.diff(p, axis=0).T)
    s = np.concatenate([[0], np.cumsum(d)])
    if s[-1] < 1e-9:
        return np.repeat(p[:1], n, axis=0)
    t = np.linspace(0, s[-1], n)
    return np.stack([np.interp(t, s, p[:, 0]), np.interp(t, s, p[:, 1])], 1)


def check_joins(doc, lo=0.75, hi=3.5):
    """Endpoints floating near another vertex: intended seams that aren't."""
    out = []
    vs = list(doc.verts.values())
    for s in doc.strokes:
        e = doc.edges[s.edge]
        for which, vid in (("start", e.a), ("end", e.b)):
            pt = doc.verts[vid]
            for x, y in vs:
                d = float(np.hypot(pt[0] - x, pt[1] - y))
                if lo < d <= hi:
                    out.append(Finding(
                        "join", f"{s.sid} ({s.record}) {which} sits {d:.1f}px "
                        f"from vertex ({x:.0f},{y:.0f}) - seam that isn't",
                        [s], pt))
                    break
    return out


def check_stray(doc, src, tol):
    """The metric law, per stroke: is there source ink under this paint?"""
    from PIL import Image, ImageDraw
    from scipy.ndimage import distance_transform_edt

    sm = np.array(src.im) < src.thr
    dist = distance_transform_edt(~sm)
    H, W = sm.shape
    out = []
    for s in doc.strokes:
        p = s.pts
        x0 = max(int(p[:, 0].min() - s.w - tol), 0)
        y0 = max(int(p[:, 1].min() - s.w - tol), 0)
        x1 = min(int(p[:, 0].max() + s.w + tol) + 2, W)
        y1 = min(int(p[:, 1].max() + s.w + tol) + 2, H)
        im = Image.new("L", (x1 - x0, y1 - y0), 0)
        dr = ImageDraw.Draw(im)
        dr.line([(x - x0, y - y0) for x, y in p],
                fill=255, width=max(1, int(round(s.w))), joint="curve")
        ink = np.array(im) > 0
        if not ink.any():
            continue
        d = dist[y0:y1, x0:x1][ink]
        stray = float((d > tol).mean())
        stray_px = int((d > tol).sum())
        if stray > 0.4 and stray_px > 40:
            out.append(Finding(
                "stray", f"{s.sid} ({s.record}): {stray * 100:.0f}% of "
                f"{ink.sum()} ink px lands off-source (tol={tol})",
                [s], tuple(p[0])))
    return out


# ---------------- overlay: paint the findings ----------------

OVERLAY_COLORS = {
    "stray": "E02020",
    "dup": "E000E0",
    "hidden": "2060E0",
    "join": "FF9000",
    "degen": "FF9000",
}


def write_overlay(doc, findings, path, ghost="D8D8D8"):
    """Re-emit the artwork ghosted, with flagged strokes painted loud."""
    lines = ["# audit overlay - the format displaying its own garbage",
             "# {doctype smazka overlay}", ""]
    nv = 0
    ne = 0

    def emit_v(x, y):
        nonlocal nv
        nv += 1
        lines.append(f"v {nv} {x:.2f} {y:.2f}")
        return nv

    for s in doc.strokes:                        # ghost: thin silver
        prev = None
        for x, y in s.pts[::max(1, len(s.pts) // 40)]:
            vid = emit_v(x, y)
            if prev is not None:
                ne += 1
                lines.append(f"e {ne} {prev} {vid}")
                lines.append(f"s g{ne} {ne} {ghost} 1.8")
            prev = vid
    flagged = {}
    for fd in findings:
        for s in fd.strokes:
            flagged.setdefault(s.sid, (s, fd.kind))
    lines.append("")
    for s, kind in flagged.values():
        col = OVERLAY_COLORS[kind]
        prev = None
        for x, y in s.pts[::max(1, len(s.pts) // 48)]:
            vid = emit_v(x, y)
            if prev is not None:
                ne += 1
                lines.append(f"e {ne} {prev} {vid}")
                lines.append(f"s f{ne} {ne} {col} {s.w + 2.5:.1f}")
            prev = vid
    with open(path, "w") as fh:
        fh.write("\n".join(lines) + "\n")


# ---------------- driver ----------------

def main(argv=None):
    ap = argparse.ArgumentParser(
        description="audit a smazka doc: show the garbage, don't hide it")
    ap.add_argument("doc")
    ap.add_argument("--src", help="source raster for the stray-ink check")
    ap.add_argument("--raster", default=TOOLS_BIN, help="smazka-raster binary")
    ap.add_argument("--tol", type=float, default=6.0)
    ap.add_argument("--overlay", help="write findings as overlay .smazka")
    ap.add_argument("--report", help="write text report here (default stdout)")
    ap.add_argument("--strict", action="store_true",
                    help="exit code = number of errors (for CI)")
    ap.add_argument("--top", type=int, default=40,
                    help="max findings per kind in the report")
    a = ap.parse_args(argv)

    doc = parse_xpanded(xpand_text(a.doc, a.raster))
    for s in doc.strokes:
        sample_stroke(doc, s)

    findings = []
    findings += check_degenerates(doc)
    findings += check_hidden(doc)
    findings += check_duplicates(doc)
    findings += check_joins(doc)
    if a.src:
        src = Source(a.src)
        findings += check_stray(doc, src, a.tol)

    by_kind = {}
    for fd in findings:
        by_kind.setdefault(fd.kind, []).append(fd)

    summ = ", ".join(f"{k}:{len(v)}" for k, v in sorted(by_kind.items()))
    out = [f"audit: {a.doc}  —  {len(doc.strokes)} strokes, "
           f"{len(doc.faces)} faces, {len(doc.edges)} edges",
           f"findings: {summ or 'none'}", ""]
    hints = {
        "stray": "ink with no source under it. Re-anchor to runs/probes "
                 "or retire the stroke.",
        "hidden": "dead ink under a later fill (fps<3). Retire it, or "
                  "move it to a record that paints after the face.",
        "dup": "two strokes on one curve. Keep one; width belongs to "
               "the object, not to repetition.",
        "join": "endpoint floats near a vertex. If a seam was meant, "
                "share exact coordinates (or use use|rev / useg|revg).",
        "degen": "broken geometry. Fix or delete.",
    }
    for kind in ("stray", "hidden", "dup", "join", "degen"):
        fds = by_kind.get(kind)
        if not fds:
            continue
        out.append(f"== {kind} ({len(fds)}) — {hints[kind]}")
        for fd in fds[:a.top]:
            out.append(f"  {fd.msg}")
        if len(fds) > a.top:
            out.append(f"  ... and {len(fds) - a.top} more")
        out.append("")
    report = "\n".join(out)

    if a.report:
        with open(a.report, "w") as fh:
            fh.write(report + "\n")
    print(report)

    if a.overlay:
        write_overlay(doc, findings, a.overlay)
        print(f"overlay: {a.overlay} — render it and LOOK at the garbage")
    if a.strict:
        return sum(1 for f in findings if f.severity == "error")
    return 0


if __name__ == "__main__":
    sys.exit(main())
