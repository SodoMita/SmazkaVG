#!/usr/bin/env bash
# Full demo of the LLM dot-first workflow:
#   source image -> imgscan measurements -> author.py -> .svg + .smazka
#   -> verify metrics & overlays -> C rasterizer crosscheck.
set -eu
cd "$(dirname "$0")"
python3 gen_source.py
python3 author_demo.py
echo
echo "outputs: source.png  preview.svg  preview_render.png  drawing.smazka(+png/svg)"
echo "         cmp_overlay.png (red=wrong ink, blue=missing)  cmp_sbs.jpg (side-by-side)"
