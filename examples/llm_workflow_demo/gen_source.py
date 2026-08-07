#!/usr/bin/env python3
"""gen_source.py -- draw the synthetic 'line art' this demo vectorizes.

Elements chosen to exercise the three authoring situations an LLM meets in
real line art: a long smooth strand, two closed blobs that overlap (z-order +
windowed scans), and a zigzag detail that smoothing would destroy.
"""
from PIL import Image, ImageDraw

W, H = 400, 300
im = Image.new('L', (W, H), 255)
d = ImageDraw.Draw(im)


def smooth_curve(pts, w=3, n=120):
    """Catmull-rom polyline through pts (draws like real hand line art)."""
    ext = [pts[0]] + list(pts) + [pts[-1]]
    out = []
    for i in range(1, len(pts)):
        p0, p1, p2, p3 = ext[i - 1], ext[i], ext[i + 1], ext[i + 2]
        for k in range(n // len(pts)):
            t = k / (n // len(pts))
            t2 = t * t
            t3 = t2 * t
            x = 0.5 * ((2 * p1[0]) + (-p0[0] + p2[0]) * t +
                       (2 * p0[0] - 5 * p1[0] + 4 * p2[0] - p3[0]) * t2 +
                       (-p0[0] + 3 * p1[0] - 3 * p2[0] + p3[0]) * t3)
            y = 0.5 * ((2 * p1[1]) + (-p0[1] + p2[1]) * t +
                       (2 * p0[1] - 5 * p1[1] + 4 * p2[1] - p3[1]) * t2 +
                       (-p0[1] + 3 * p1[1] - 3 * p2[1] + p3[1]) * t3)
            out.append((x, y))
    out.append(pts[-1])
    d.line(out, fill=0, width=w, joint='curve')


# 1. long smooth strand across the bottom
smooth_curve([(30, 240), (110, 210), (190, 250), (270, 205), (360, 235)], w=3)

# 2. two overlapping closed blobs; y-separated in the overlap columns so
#    windowed scans can attribute every run to exactly one blob.
bean1 = [(100, 120), (140, 85), (200, 88), (222, 120), (200, 160),
         (130, 158), (100, 120)]
bean2 = [(190, 100), (225, 68), (320, 72), (350, 105), (320, 138),
         (230, 132), (190, 100)]
smooth_curve(bean2, w=3)   # back
smooth_curve(bean1, w=3)   # front

# 3. zigzag detail inside bean1 (finger/teeth analogue; w=2)
d.line([(140, 130), (155, 110), (170, 130), (185, 110), (200, 130), (215, 112)],
       fill=0, width=2, joint='curve')

# 4. two short hatch accents inside bean2
d.line([(285, 95), (300, 123)], fill=0, width=2)
d.line([(305, 91), (320, 119)], fill=0, width=2)

im.save('source.png')
print('wrote source.png', im.size)
