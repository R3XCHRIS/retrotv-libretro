#!/usr/bin/env python3
"""Embed a file in C as a NUL-terminated char array: embed.py in.html out.c name"""
import sys

src, out, name = sys.argv[1:4]
data = open(src, "rb").read()
rows = [", ".join("0x%02x" % b for b in data[i:i + 16]) for i in range(0, len(data), 16)]
with open(out, "w") as f:
    f.write("/* Generated from %s by scripts/embed.py (make src/%s.c). Don't edit. */\n" % (src, name))
    f.write('#include "%s.h"\n\n' % name)
    f.write("const char %s_page[] = {\n  %s,\n  0x00\n};\n" % (name, ",\n  ".join(rows)))
