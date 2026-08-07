"""author.py -- dot-first stroke registry that emits SmazkaVG + preview SVG.

Mental model (this is the contract with the LLM author):
  * st(name, dots)      -- a named open/closed stroke, dots in source-image px
  * obj(name, loops, inner=[stroke names])  -- a closed, white-filled object.
      loops[0] is the OUTER loop (one closed single-line outline).
      inner: registry strokes drawn on top of the object (details).
  * Objects are drawn in obj() call order (painter z-order). Overlaps are
    resolved by z-order ALONE -- never by interleaved paths.
  * A seam between two adjacent objects (wrist, ankle, band edge) must be the
    SAME coordinates in both loops: build the run once, reuse it in each
    (copy_span/span_x/span_y). Identical coordinates = zero-width seam.

Edit-friendliness doctrine (do NOT violate without being asked):
  * uniform stroke widths, round caps/joins; NO tapered ribbons
  * every object = ONE closed single-line loop (plus inner detail strokes)
  * smooth=True only for smooth strands; fingers/zigzag detail = smooth=False
"""
from . import geometry


class Stroke:
    __slots__ = ('name', 'dots', 'w', 'smooth', 'closed', 'trim', 'color')

    def __init__(self, name, dots, w=2.4, smooth=True, closed=False,
                 trim=0.0, color='000000'):
        self.name = name
        self.dots = list(dots)
        self.w = float(w)
        self.smooth = smooth
        self.closed = closed
        self.trim = float(trim)
        self.color = color


class Obj:
    __slots__ = ('name', 'loops', 'inner', 'fill', 'stroke_w', 'smooth')

    def __init__(self, name, loops, inner, fill, stroke_w, smooth):
        self.name = name
        self.loops = loops
        self.inner = inner
        self.fill = fill
        self.stroke_w = stroke_w
        self.smooth = smooth


class Doc:
    def __init__(self, W, H):
        self.W, self.H = int(W), int(H)
        self.strokes = {}
        self.retired = set()
        self.objects = []
        self.warnings = []

    # ------------------------------------------------------------- strokes
    def st(self, name, dots, w=2.4, smooth=True, closed=False, trim=0.0,
           color='000000'):
        if name in self.strokes:
            self.warnings.append(f'DUP stroke name {name} (overwritten)')
        self.strokes[name] = Stroke(name, dots, w, smooth, closed, trim, color)
        return name

    def retire(self, *names):
        """Keep a stroke's dots (chains/spans can still reference them) but
        never draw it directly -- its line is now carried by an object loop."""
        self.retired.update(names)

    def copy_span(self, name, i0=0, i1=None, rev=False):
        """Copy an exact coordinate run of a stroke -> the SAME coordinates can
        be used in two adjacent object loops: a zero-width butt seam."""
        src = self.strokes[name].dots
        seg = list(src[i0:(len(src) if i1 is None else i1 + 1)])
        return seg[::-1] if rev else seg

    def span_x(self, name, x0, x1):
        return [p for p in self.strokes[name].dots if x0 <= p[0] <= x1]

    def span_y(self, name, y0, y1):
        return [p for p in self.strokes[name].dots if y0 <= p[1] <= y1]

    def chain(self, *items, bridge=8.0):
        """Like geometry.chain but accepts stroke names / ('rev', name)."""
        resolved = []
        for it in items:
            if isinstance(it, str):
                resolved.append(list(self.strokes[it].dots))
            elif isinstance(it, tuple) and len(it) == 2 and it[0] == 'rev':
                nm = it[1]
                run = list(self.strokes[nm].dots) if isinstance(nm, str) else list(nm)
                resolved.append(('rev', run))
            else:
                resolved.append(it)
        return geometry.chain(*resolved, bridge=bridge)

    # ------------------------------------------------------------- objects
    def obj(self, name, loops=(), inner=(), fill='FFFFFF', stroke_w=3.0,
            smooth=True):
        """loops items: (dots, w|None, smooth|None) | (dots, w) | dots."""
        norm = []
        for lp in loops:
            if isinstance(lp, tuple) and len(lp) == 3:
                norm.append((list(lp[0]), lp[1], lp[2]))
            elif isinstance(lp, tuple) and len(lp) == 2 and \
                    isinstance(lp[1], (int, float)):
                norm.append((list(lp[0]), lp[1], None))
            else:
                norm.append((list(lp), None, None))
        self.objects.append(Obj(name, norm, list(inner), fill, stroke_w, smooth))
        return name

    # ------------------------------------------------------------- checks
    def validate(self, verbose=True):
        placed = set()
        for o in self.objects:
            placed.update(o.inner)
        unplaced = [n for n in self.strokes
                    if n not in placed and n not in self.retired]
        missing = [n for o in self.objects for n in o.inner
                   if n not in self.strokes]
        if unplaced:
            self.warnings.append(
                f'UNPLACED={unplaced} (never drawn; retire() if intended)')
        if missing:
            self.warnings.append(f'MISSING-IN-REGISTRY={missing}')
        for n, s in self.strokes.items():
            if len(s.dots) < 2 and n not in self.retired:
                self.warnings.append(f'DEGENERATE {n}: <2 dots')
        if verbose:
            for w in self.warnings:
                print('  CHECK', w)
        return self.warnings

    def _draw_items(self):
        """Flatten to one draw list across objects (z-order = object order)."""
        return [it for o in self.objects for it in self._draw_items_for(o)]

    # ------------------------------------------------------------- emit smazka
    def emit_smazka(self, path=None, tess_step=8.0, header=None):
        """Serialize to Line-ASM.

        Faces: tessellated closed seg-edge loops with FFFFFFFF fill + uniform
        per-edge round-cap strokes (a stroke record references ONE edge, so a
        loop of N edges gets N stroke records -- that is the uniform-width idiom).
        Open chains: private vertex chains of catmull (smooth) or seg (zigzag)
        edges. Vertices are NEVER shared between chains: the rasterizer derives
        catmull tangents from any edge touching a vertex (SPEC 7.x), so sharing
        a vertex across strands would make tangents hop across lines.
        """
        L = [header or '# SmazkaVG v1.5 -- authored via tools/llm (dot-first pipeline)']
        vid = eid = fid = sid = 0
        body = []

        def new_vert(x, y):
            nonlocal vid
            body.append(f'v {vid} {x:.2f} {y:.2f}')
            vid += 1
            return vid - 1

        def new_edge(v0, v1, smooth):
            nonlocal eid
            body.append(f'e {eid} {v0} {v1} type={"catmull" if smooth else "seg"}')
            eid += 1
            return eid - 1

        def stroke_edge(e, w, color='000000FF'):
            nonlocal sid
            body.append(f's {sid} {e} {color} {w:.2f} {w:.2f} cap=round')
            sid += 1

        for kind, dots, w, smooth, closed, fill in self._draw_items():
            if len(dots) < 2:
                continue
            if kind == 'face':
                tess = geometry.tessellate(dots, smooth=smooth, closed=True,
                                           step=tess_step)
                if len(tess) < 3:
                    continue
                vs = [new_vert(x, y) for x, y in tess]
                es = [new_edge(vs[i], vs[(i + 1) % len(vs)], False)
                      for i in range(len(vs))]
                body.append(f'f {fid} ' + ' '.join(map(str, es))
                            + ' ' + (fill or 'FFFFFF'))
                fid += 1
                for e in es:
                    stroke_edge(e, w)
            else:
                seq = dots + ([dots[0]] if closed else [])
                vs = [new_vert(x, y) for x, y in seq]
                for i in range(len(vs) - 1):
                    stroke_edge(new_edge(vs[i], vs[i + 1], smooth), w)

        out = '\n'.join(L + body) + '\n'
        if path:
            with open(path, 'w') as fh:
                fh.write(out)
        return out

    # ------------------------------------------------------------- emit preview svg
    def emit_svg(self, path=None, tess_step=4.0):
        """WYSIWYG preview: the same geometry the verify metric scores.
        Uniform-width round-cap round-join paths; object loops white-filled;
        z-order = obj() call order. Iterate THIS against the source image."""
        P = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{self.W}" '
             f'height="{self.H}" viewBox="0 0 {self.W} {self.H}">',
             f'<rect width="{self.W}" height="{self.H}" fill="white"/>']

        def d_of(tess, closed):
            if len(tess) < 2:
                return None
            d = [f'M {tess[0][0]:.2f} {tess[0][1]:.2f}']
            d += [f'L {x:.2f} {y:.2f}' for x, y in tess[1:]]
            if closed:
                d.append('Z')
            return ' '.join(d)

        for o in self.objects:
            P.append(f'<g id="{o.name}">')
            for kind, dots, w, smooth, closed, fill in self._draw_items_for(o):
                tess = geometry.tessellate(dots, smooth=smooth, closed=closed,
                                           step=tess_step)
                d = d_of(tess, closed)
                if not d:
                    continue
                if kind == 'face':
                    P.append(f'<path d="{d}" fill="#{fill}" stroke="#000" '
                             f'stroke-width="{w}" stroke-linejoin="round" '
                             f'stroke-linecap="round"/>')
                else:
                    P.append(f'<path d="{d}" fill="none" stroke="#000" '
                             f'stroke-width="{w}" stroke-linejoin="round" '
                             f'stroke-linecap="round"/>')
            P.append('</g>')
        P.append('</svg>')
        out = '\n'.join(P)
        if path:
            with open(path, 'w') as fh:
                fh.write(out)
        return out

    def _draw_items_for(self, o):
        items = []
        if o.loops:
            dots, w, sm = o.loops[0]
            sm = o.smooth if sm is None else sm
            w = o.stroke_w if w is None else w
            items.append(('face', dots, w, sm, True, o.fill))
            for d2, w2, sm2 in o.loops[1:]:
                w2 = o.stroke_w if w2 is None else w2
                sm2 = o.smooth if sm2 is None else sm2
                items.append(('line', d2, w2, sm2, False, None))
        for name in o.inner:
            s = self.strokes.get(name)
            if s is None or name in self.retired:
                continue
            em = geometry.trim_ends(s.dots, s.trim) if s.trim else s.dots
            items.append(('line', em, s.w, s.smooth, s.closed, None))
        return items
