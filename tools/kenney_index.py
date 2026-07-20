#!/usr/bin/env python3
"""Build the Kenney sprite library: copy individual sprite PNGs (no
spritesheets/previews/vectors) from a Kenney "All-in-1" style tree into
assets/kenney/lib/ and write index.tsv mapping each file to a
human/LLM-readable description and its pixel size.

  python3 tools/kenney_index.py ~/outside/kenney

index.tsv columns:  path <TAB> description <TAB> WxH <TAB> bytes
Description is built from the path (generic components like PNG/Default
dropped) plus the humanized filename, e.g.
  ui/UI Pack - Sci-fi/Blue/bar_round_gloss_large_m.png
  -> "UI Assets, UI Pack - Sci-fi, Blue, Bar Round Gloss Large M"
"""
import os
import re
import shutil
import struct
import sys

EXCLUDE = re.compile(
    r"(spritesheet|tilesheet|tilemap|packed|preview|sample|vector|/Tiled/|overview"
    r"|/Planets/)", re.I)
DROP_COMPONENTS = {"png", "default", "default size"}
# Files this big are textures/backgrounds (skyboxes, light masks, parchment),
# not composable sprites — and they were half the library's bytes.
MAX_BYTES = 20000


def png_size(path):
    with open(path, "rb") as f:
        head = f.read(24)
    if len(head) < 24 or head[:8] != b"\x89PNG\r\n\x1a\n":
        return None
    w, h = struct.unpack(">II", head[16:24])
    return w, h


def humanize(stem):
    s = re.sub(r"[_\-]+", " ", stem)
    s = re.sub(r"(?<=[a-z])(?=[A-Z])", " ", s)      # camelCase
    s = re.sub(r"(?<=[a-zA-Z])(?=\d)", " ", s)      # letter->digit
    return " ".join(w if w.isupper() else w.capitalize() for w in s.split())


def main():
    src_root = os.path.expanduser(sys.argv[1] if len(sys.argv) > 1
                                  else "~/outside/kenney")
    here = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    lib = os.path.join(here, "assets", "kenney", "lib")

    sections = [("2D assets", "2d", "2D Assets"),
                ("UI assets", "ui", "UI Assets")]
    rows = []
    total = 0
    for src_name, dst_name, label in sections:
        top = os.path.join(src_root, src_name)
        for dirpath, dirs, files in os.walk(top):
            dirs.sort()
            for f in sorted(files):
                if not f.lower().endswith(".png"):
                    continue
                src = os.path.join(dirpath, f)
                if EXCLUDE.search(src):
                    continue
                nbytes = os.path.getsize(src)
                if nbytes >= MAX_BYTES:
                    continue
                size = png_size(src)
                if not size:
                    continue
                rel_parts = os.path.relpath(src, top).split(os.sep)
                kept = [p for p in rel_parts[:-1]
                        if p.lower() not in DROP_COMPONENTS]
                dst_rel = os.path.join(dst_name, *kept, f)
                dst = os.path.join(lib, dst_rel)
                os.makedirs(os.path.dirname(dst), exist_ok=True)
                shutil.copy2(src, dst)
                desc = ", ".join([label] + kept + [humanize(f[:-4])])
                total += nbytes
                rows.append((dst_rel, desc, "%dx%d" % size, nbytes))

    rows.sort()
    with open(os.path.join(lib, "index.tsv"), "w") as f:
        f.write("path\tdescription\tsize\tbytes\n")
        for r in rows:
            f.write("%s\t%s\t%s\t%d\n" % r)
    print("%d sprites, %.1f MB -> %s" % (len(rows), total / 1e6, lib))


if __name__ == "__main__":
    main()
