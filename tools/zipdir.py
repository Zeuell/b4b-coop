#!/usr/bin/env python3
"""Deterministic zip writer for launch/package.sh on hosts without Info-ZIP (Git Bash on Windows).

    tools/zipdir.py <out.zip>  < list of entry paths (relative to the cwd, directories end in /, sorted)

Mirrors `TZ=UTC zip -qX -@`: entries in the given order, deflate, fixed 2020-01-01 00:00:00 timestamps, unix
permissions 755 for directories and 644 for files, no extra fields. The DLL hashes inside (what SHA256SUMS and the
updater check) don't depend on the zipper; the zip's own hash differs from Info-ZIP's, so a Windows-made zip is not
byte-identical to CI's.
"""
import os, sys, zipfile

out = sys.argv[1]
names = [l.rstrip("\r\n") for l in sys.stdin if l.strip()]
with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED, compresslevel=6) as z:
    for n in names:
        p = n.replace("\\", "/")
        is_dir = os.path.isdir(p)
        zi = zipfile.ZipInfo(p.rstrip("/") + ("/" if is_dir else ""), date_time=(2020, 1, 1, 0, 0, 0))
        zi.create_system = 3   # unix, like Info-ZIP on Linux
        if is_dir:
            zi.external_attr = (0o40755 << 16) | 0x10
            z.writestr(zi, b"")
        else:
            zi.external_attr = 0o100644 << 16
            zi.compress_type = zipfile.ZIP_DEFLATED
            with open(p, "rb") as f: z.writestr(zi, f.read())
