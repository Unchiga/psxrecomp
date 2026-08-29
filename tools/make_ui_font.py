#!/usr/bin/env python3
"""Regenerate runtime/third_party/fonts/psx_ui_font_data.h.

The runtime UI (F10 menu, and anything else that draws real text) rasterises
Inter with stb_truetype, and draws its icons from Material Symbols through the
same path.  Both ship upstream as ONE variable font, so this script does three
things the runtime cannot do at build time:

  1. downloads Inter[opsz,wght].ttf and MaterialSymbolsOutlined from
     google/fonts and google/material-design-icons (SIL Open Font License and
     Apache 2.0 respectively),
  2. pins the variation axes, producing static instances -- Inter Regular (400)
     and SemiBold (600), and Material Symbols at wght 400 -- because
     stb_truetype has no `gvar` support and would otherwise always render the
     default master,
  3. subsets each instance to the glyphs the UI can actually draw and emits
     them as C arrays.

Subsetting is what makes this affordable to embed: the three upstream faces are
11 MB together, the three subsets are around 30 KB.  Embedding rather than
shipping loose .ttf files is deliberate -- the menu is the player's recovery
path for a bad setting, so it must not be able to lose its font to a missing
file.

Run:  python tools/make_ui_font.py            (needs `pip install fonttools`)

The generated header is COMMITTED.  Nothing in the build runs this.
"""

import io
import os
import sys
import urllib.request

SRC = "https://github.com/google/fonts/raw/main/ofl/inter/Inter%5Bopsz%2Cwght%5D.ttf"
LIC = "https://github.com/google/fonts/raw/main/ofl/inter/OFL.txt"
ICON_SRC = ("https://github.com/google/material-design-icons/raw/master/"
            "variablefont/MaterialSymbolsOutlined%5BFILL%2CGRAD%2Copsz%2Cwght%5D.ttf")
ICON_LIC = ("https://github.com/google/material-design-icons/raw/master/"
            "LICENSE")

HERE = os.path.dirname(os.path.abspath(__file__))
OUT_DIR = os.path.join(HERE, "..", "runtime", "third_party", "fonts")
OUT_H = os.path.join(OUT_DIR, "psx_ui_font_data.h")
OUT_LIC = os.path.join(OUT_DIR, "Inter-OFL.txt")
OUT_ICON_LIC = os.path.join(OUT_DIR, "MaterialSymbols-LICENSE.txt")

# ASCII 32..126 plus the handful of non-ASCII marks the UI draws.  The
# disclosure triangles are NOT here: Inter has no U+25B8/U+25BE, and a
# subset silently drops what the face does not contain, so the UI
# rasterises those two as filled triangles instead (vm_tri).
CHARS = "".join(chr(c) for c in range(0x20, 0x7F)) + "\u00d7\u2022\u2013\u2014\u2018\u2019\u201c\u201d\u2026\u2190\u2191\u2192\u2193\u2713"

# opsz is Inter's optical-size axis.  Pinned at 14, the size the UI text
# actually lands near on a 1080p window -- leaving it free would keep `gvar`
# alive, which stb_truetype ignores.
INSTANCES = [("REGULAR", {"wght": 400, "opsz": 14}),
             ("SEMIBOLD", {"wght": 600, "opsz": 14})]

# Icons the menu draws, by Material Symbols codepoint.  Keep this list in step
# with UF_ICON_CP in runtime/src/psx_ui_font.c: a codepoint present in one and
# not the other is an icon that silently does not draw.
ICONS = {
    0xE2C7: "folder",           0xE417: "visibility",
    0xEF5B: "monitor",          0xE050: "volume_up",
    0xE6EC: "sports_esports",   0xEA0B: "bolt",
    0xE87B: "extension",        0xE429: "tune",
}
# opsz 24 is the size class Material draws at for a UI row; FILL 0 / GRAD 0 is
# the plain outlined style the reference uses.
ICON_AXES = {"wght": 400, "FILL": 0, "GRAD": 0, "opsz": 24}


def fetch(url):
    with urllib.request.urlopen(url, timeout=120) as r:
        return r.read()


def emit_array(fh, name, blob):
    fh.write("static const unsigned char %s[%d] = {\n" % (name, len(blob)))
    for i in range(0, len(blob), 16):
        fh.write("    " + ",".join("0x%02x" % b for b in blob[i:i + 16]) + ",\n")
    fh.write("};\n\n")


def main():
    try:
        from fontTools.ttLib import TTFont
        from fontTools.varLib import instancer
        from fontTools import subset
    except ImportError:
        sys.exit("needs fonttools:  pip install fonttools")

    print("downloading Inter...")
    src = fetch(SRC)
    print("downloading OFL.txt...")
    lic = fetch(LIC)
    os.makedirs(OUT_DIR, exist_ok=True)
    with open(OUT_LIC, "wb") as f:
        f.write(lic)

    print("downloading Material Symbols...")
    isrc = fetch(ICON_SRC)
    print("downloading Material Symbols LICENSE...")
    with open(OUT_ICON_LIC, "wb") as f:
        f.write(fetch(ICON_LIC))

    blobs = []
    for name, axes in INSTANCES:
        font = TTFont(io.BytesIO(src))
        instancer.instantiateVariableFont(font, axes, inplace=True,
                                          updateFontNames=False)
        opts = subset.Options()
        opts.layout_features = []          # no GSUB/GPOS: stb_truetype ignores both
        opts.name_IDs = [1, 2, 6]
        opts.notdef_outline = True
        opts.recalc_bounds = True
        opts.drop_tables += ["DSIG", "GDEF", "GSUB", "GPOS", "MVAR", "STAT",
                             "HVAR", "VVAR", "fvar", "gvar", "avar"]
        sub = subset.Subsetter(options=opts)
        sub.populate(text=CHARS)
        sub.subset(font)
        buf = io.BytesIO()
        font.save(buf)
        blob = buf.getvalue()
        print("  %-8s %6d bytes" % (name, len(blob)))
        blobs.append((name, blob))

    font = TTFont(io.BytesIO(isrc))
    instancer.instantiateVariableFont(font, ICON_AXES, inplace=True,
                                      updateFontNames=False)
    opts = subset.Options()
    opts.layout_features = []
    opts.name_IDs = [1, 2, 6]
    opts.notdef_outline = True
    opts.recalc_bounds = True
    opts.drop_tables += ["DSIG", "GDEF", "GSUB", "GPOS", "MVAR", "STAT",
                         "HVAR", "VVAR", "fvar", "gvar", "avar"]
    sub_ = subset.Subsetter(options=opts)
    sub_.populate(unicodes=sorted(ICONS))
    sub_.subset(font)
    buf = io.BytesIO()
    font.save(buf)
    blob = buf.getvalue()
    print("  %-8s %6d bytes  (%s)"
          % ("ICONS", len(blob), ", ".join(sorted(ICONS.values()))))
    blobs.append(("ICONS", blob))

    with open(OUT_H, "w", newline="\n") as f:
        f.write("/* GENERATED by tools/make_ui_font.py -- do not edit by hand.\n"
                " *\n"
                " * Inter, subset to the UI's glyph set and instanced to two static\n"
                " * weights.  Copyright (c) The Inter Project Authors; licensed under the\n"
                " * SIL Open Font License 1.1 -- the full text ships beside this file as\n"
                " * Inter-OFL.txt.\n"
                " *\n"
                " * Material Symbols Outlined, subset to the eight icons the menu draws.\n"
                " * Copyright (c) Google Inc; licensed under Apache 2.0 -- the full text\n"
                " * ships beside this file as MaterialSymbols-LICENSE.txt.\n"
                " *\n"
                " * Both are reproduced in the top-level NOTICE.\n"
                " */\n"
                "#ifndef PSX_UI_FONT_DATA_H\n#define PSX_UI_FONT_DATA_H\n\n")
        for name, blob in blobs:
            emit_array(f, "PSX_UI_FONT_" + name + "_TTF", blob)
        f.write("#endif /* PSX_UI_FONT_DATA_H */\n")
    print("wrote", os.path.normpath(OUT_H))


if __name__ == "__main__":
    main()
