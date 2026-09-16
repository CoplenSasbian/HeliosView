#!/usr/bin/env python3
"""One-shot refactor: rename the paint *module* to canvas.

Scope (agreed with the maintainer):
  * files:    heliosview_paint* -> heliosview_canvas*, HeliosViewCore/Paint.h -> Canvas.h
  * symbols:  heliosview_paint_engine_t -> heliosview_canvas_engine_t,
              heliosview_paint_feature_t -> heliosview_canvas_feature_t,
              heliosview_set_default_paint_engine -> ..._canvas_engine,
              heliosview_window_set_paint_engine / _paint_engine -> ..._canvas_engine,
              default_painter_state -> default_canvas_state
  * namespace: hv::paint -> hv::canvas
  * prose:    "paint"/"Painting" used as a verb or for the module -> drawing / canvas

Deliberately NOT renamed (they name a real concept, not the module):
  Painter, PainterState, PaintEngine, PaintFeature, heliosview_painter_*,
  and the past-tense verbs "painted"/"painting" inside sentences.
"""

from __future__ import annotations

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

SKIP_DIRS = {".git", ".vs", ".idea", "out", "cmake-build-debug", "third_party",
             "node_modules", "build"}
EXTS = {".h", ".hpp", ".cpp", ".c", ".cc", ".cmake", ".txt", ".md", ".yml",
        ".yaml", ".in", ".cmd", ".ps1", ".js", ".json"}

# --- file moves: old relative path -> new relative path -------------------
RENAMES = [
    ("include/HeliosView/heliosview_paint.h", "include/HeliosView/heliosview_canvas.h"),
    ("src/heliosview_paint_internal.h", "src/heliosview_canvas_internal.h"),
    ("src/heliosview_paint.cpp", "src/heliosview_canvas.cpp"),
    ("src/win32/heliosview_paint_gdiplus.cpp", "src/win32/heliosview_canvas_gdiplus.cpp"),
    ("src/win32/heliosview_paint_d2d.cpp", "src/win32/heliosview_canvas_d2d.cpp"),
    ("include/HeliosViewCore/Paint.h", "include/HeliosViewCore/Canvas.h"),
]

# NOTE: `\b` is useless around these tokens -- `_` is a word character, so
# `\bheliosview_paint\b` does NOT match `heliosview_paint_engine`. The custom
# boundary below only refuses an immediately adjacent letter/digit, which is
# what lexical correctness actually requires here.
def _tok(word: str) -> re.Pattern[str]:
    return re.compile(rf"(?<![A-Za-z0-9]){re.escape(word)}(?![A-Za-z0-9])")


# --- content rules, applied in order --------------------------------------
RULES = [
    # module / file names (guarded by the custom boundary, not \b)
    (_tok("heliosview_paint"), "heliosview_canvas"),
    (_tok("hv::paint"), "hv::canvas"),
    (re.compile(r"<HeliosViewCore/Paint\.h>"), "<HeliosViewCore/Canvas.h>"),
    (_tok("Paint.h"), "Canvas.h"),
    (_tok("HELIOSVIEW_HELIOSVIEW_PAINT_H"), "HELIOSVIEW_HELIOSVIEW_CANVAS_H"),
    (_tok("HELIOSVIEW_PAINT"), "HELIOSVIEW_CANVAS"),
    (_tok("default_painter_state"), "default_canvas_state"),
    (_tok("active_painter"), "canvas_session"),
    # prose: the module and the act of drawing
    (_tok("Paint API"), "Canvas API"),
    (_tok("paint layer"), "canvas layer"),
    (_tok("Paint layer"), "Canvas layer"),
    (_tok("paint core"), "canvas core"),
    (_tok("Paint core"), "Canvas core"),
    (_tok("painting"), "drawing"),
    (_tok("Painting"), "Drawing"),
    (_tok("painted"), "drawn"),
    (_tok("Painted"), "Drawn"),
    (_tok("paints"), "draws"),
    (_tok("paint"), "canvas"),
    (_tok("Paint"), "Canvas"),
]


def targets():
    for dirpath, dirnames, filenames in os.walk(ROOT):
        dirnames[:] = [d for d in dirnames if d not in SKIP_DIRS]
        for name in filenames:
            if os.path.splitext(name)[1].lower() in EXTS:
                yield os.path.join(dirpath, name)


def main() -> int:
    changed: list[str] = []
    for path in sorted(targets()):
        if os.path.basename(path) == os.path.basename(__file__):
            continue
        with open(path, "r", encoding="utf-8", newline="") as fh:
            text = fh.read()
        out = text
        for pattern, repl in RULES:
            out = pattern.sub(repl, out)
        if out != text:
            with open(path, "w", encoding="utf-8", newline="") as fh:
                fh.write(out)
            changed.append(os.path.relpath(path, ROOT))

    for old, new in RENAMES:
        src = os.path.join(ROOT, old)
        dst = os.path.join(ROOT, new)
        if os.path.exists(src):
            if os.path.exists(dst):
                print(f"! refusing to overwrite {new}", file=sys.stderr)
                return 1
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            os.replace(src, dst)
            changed.append(f"{old} -> {new}")

    for item in changed:
        print(item)
    print(f"-- {len(changed)} entries")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
