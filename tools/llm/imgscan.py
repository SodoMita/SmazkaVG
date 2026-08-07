"""imgscan.py -- read ground truth out of the SOURCE raster.

An LLM cannot see pixels precisely; this module turns a crop of the source
image into text it *can* reason about: ASCII art, row/column band scans, and
verified dot candidates. Authoring rule #1: every dot must sit INSIDE a dark
run of the source image -- use probe()/dot_run() to check before you commit.

Uses Pillow only.
"""
from PIL import Image

_RAMP = " .:-=+*#%@"


class Source:
    def __init__(self, path, thr=128):
        self.im = Image.open(path).convert("L")
        self.W, self.H = self.im.size
        self.thr = thr
        self.px = self.im.load()

    # ---------------------------------------------------------- primitives
    def dark(self, x, y, thr=None):
        t = self.thr if thr is None else thr
        return 0 <= x < self.W and 0 <= y < self.H and self.px[x, y] < t

    def row_runs(self, y, x0, x1, thr=None):
        """Dark runs [(xs, xe), ...] along row y (inclusive)."""
        runs, s = [], None
        for x in range(x0, x1 + 1):
            if self.dark(x, y, thr):
                if s is None:
                    s = x
            elif s is not None:
                runs.append((s, x - 1)); s = None
        if s is not None:
            runs.append((s, x1))
        return runs

    def col_runs(self, x, y0, y1, thr=None):
        runs, s = [], None
        for y in range(y0, y1 + 1):
            if self.dark(x, y, thr):
                if s is None:
                    s = y
            elif s is not None:
                runs.append((s, y - 1)); s = None
        if s is not None:
            runs.append((s, y1))
        return runs

    # ---------------------------------------------------------- authoring aid
    def row_scan(self, y0, y1, x0, x1, step=1, thr=None):
        """For each row: center of every dark run -> dot candidates on strokes.
        Returns {y: [(xc, ws, we), ...]}. This is the main 'where do dots go'
        instrument: centers of dark runs are guaranteed inside the source."""
        res = {}
        for y in range(y0, y1 + 1, step):
            runs = self.row_runs(y, x0, x1, thr)
            res[y] = [((a + b) / 2.0, a, b) for a, b in runs]
        return res

    def col_scan(self, y0, y1, x0, x1, step=1, thr=None):
        res = {}
        for x in range(x0, x1 + 1, step):
            runs = self.col_runs(x, y0, y1, thr)
            res[x] = [((a + b) / 2.0, a, b) for a, b in runs]
        return res

    def probe(self, pts, thr=None):
        """Check authored dots: 'on' = inside dark, 'near' = within 2 px,
        'off' = in the white (BAD dot -- move it)."""
        out = []
        for (x, y) in pts:
            x, y = int(round(x)), int(round(y))
            if self.dark(x, y, thr):
                out.append(((x, y), 'on')); continue
            near = any(self.dark(x + dx, y + dy, thr)
                       for dy in (-2, -1, 0, 1, 2) for dx in (-2, -1, 0, 1, 2))
            out.append(((x, y), 'near' if near else 'off'))
        return out

    def dot_run(self, y, x0, x1, n=8, thr=None):
        """Pick n dots covering the longest dark run on row y (evenly spaced
        centers of sub-runs) -- a ready-made strand backbone."""
        runs = self.row_runs(y, x0, x1, thr)
        if not runs:
            return []
        a, b = max(runs, key=lambda r: r[1] - r[0])
        if n <= 1 or b - a < 4:
            return [((a + b) / 2.0, float(y))]
        return [(a + (b - a) * i / (n - 1), float(y)) for i in range(n)]

    # ---------------------------------------------------------- eyes
    def ascii(self, x0, y0, x1, y1, cw=2, ch=4, thr=None):
        """ASCII art of a region so an LLM can 'see' zoomed structure."""
        rows = []
        y = y0
        while y <= y1:
            line = []
            x = x0
            while x <= x1:
                tot, n = 0, 0
                for dy in range(ch):
                    for dx in range(cw):
                        xx, yy = x + dx, y + dy
                        if 0 <= xx < self.W and 0 <= yy < self.H:
                            tot += self.px[xx, yy]; n += 1
                l = tot / max(1, n)
                idx = min(len(_RAMP) - 1, max(0, int((255 - l) / 256 * len(_RAMP))))
                line.append(_RAMP[idx])
                x += cw
            rows.append("".join(line))
            y += ch
        return "\n".join(rows)

    def ascii_binary(self, x0, y0, x1, y1, thr=None):
        """1 char per pixel: '#' dark, '.' light, ' ' white. Best for fine
        detail like eyes/hands (regions up to ~120x60 chars)."""
        t = self.thr if thr is None else thr
        rows = []
        for y in range(y0, y1 + 1):
            line = []
            for x in range(x0, x1 + 1):
                v = self.px[x, y] if (0 <= x < self.W and 0 <= y < self.H) else 255
                line.append('#' if v < t else ('.' if v < 200 else ' '))
            rows.append("".join(line))
        return "\n".join(rows)

    def crop(self, x0, y0, x1, y1, path, scale=3):
        """Save a zoomed crop for visual inspection (pairs with renders)."""
        c = self.im.crop((x0, y0, x1, y1))
        if scale != 1:
            c = c.resize((c.width * scale, c.height * scale), Image.NEAREST)
        c.save(path)
        return path

    # ---------------------------------------------------------- fit aid
    def fit_report(self, pts, smooth=True, closed=False, thr=None):
        """How well does a dot chain track the source? Returns per-dot verdicts
        + max distance of tessellated curve to nearest dark pixel in a ±3px
        neighborhood (cheap local check; the global metric lives in verify.py)."""
        from . import geometry
        tess = geometry.tessellate(pts, smooth=smooth, closed=closed, step=3.0)
        bad = []
        for p in tess:
            x, y = int(round(p[0])), int(round(p[1]))
            ok = any(self.dark(x + dx, y + dy, thr)
                     for dy in range(-3, 4) for dx in range(-3, 4))
            if not ok:
                bad.append((x, y))
        return {'dots': self.probe(pts, thr), 'curve_off': bad,
                'n_off': len(bad), 'n_total': len(tess)}


if __name__ == "__main__":          # tiny CLI for quick probes
    import sys, json
    src = Source(sys.argv[1])
    cmd = sys.argv[2]
    a = [int(v) for v in sys.argv[3:]]
    if cmd == 'row':
        print(json.dumps(src.row_scan(a[0], a[1], a[2], a[3]).__str__()))
    elif cmd == 'ascii':
        print(src.ascii(a[0], a[1], a[2], a[3]))
    elif cmd == 'bin':
        print(src.ascii_binary(a[0], a[1], a[2], a[3]))
    elif cmd == 'crop':
        src.crop(a[0], a[1], a[2], a[3], sys.argv[7], int(sys.argv[8]) if len(sys.argv) > 8 else 3)
        print('saved', sys.argv[7])
