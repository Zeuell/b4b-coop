#!/usr/bin/env bash
# Build and run the unit tests on Linux with the pinned zig: the add-on pak checks (native/test/pakfmt_test.c) and the
# updater's (native/test/updcore_test.c), the latter against fixtures made here: a release-shaped zip (deflate), a
# stored zip, a zip with a "../" entry, an OpenSSL ed25519 key with the manifest and zip signed by
# tools/sign-release.sh (what release.yml runs), a second key for the shop catalog, and the committed public keys
# (docs/*-signing.pub.pem) to check native/src/signkeys.h against. CI runs this (ci.yml).
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)" root="$(cd "$(dirname "$0")/../.." && pwd)"
v="$root/vendor"
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
zig="$v/zig/zig"; [[ -x $zig.exe ]] && zig+=.exe
"$zig" cc -O2 -Wall -Wno-unused-function -I"$here/../src" -I"$v/monocypher/src" -I"$v/monocypher/src/optional" \
  -I"$v/zlib/contrib/puff" "$here/updcore_test.c" "$here/../src/updcore.c" "$v/monocypher/src/monocypher.c" \
  "$v/monocypher/src/optional/monocypher-ed25519.c" "$v/zlib/contrib/puff/puff.c" -o "$tmp/updcore_test"
python3 - "$tmp" <<'PY'
import sys, zipfile, os
d = sys.argv[1]
dll = b"MZ" + bytes((i * 7 + i // 256) & 0xFF for i in range(200000))   # compressible, not trivial
with zipfile.ZipFile(f"{d}/release.zip", "w", zipfile.ZIP_DEFLATED) as z:
    for n in ("Gobi/", "Gobi/Binaries/", "Gobi/Binaries/Win64/"): z.writestr(n, b"")
    z.writestr("Gobi/Binaries/Win64/X3DAudio1_7.dll", dll)
    z.writestr("Gobi/Binaries/Win64/b4bcoop.ini", b"; settings\r\n;host=0\r\n")
    z.writestr("b4bcoop-COMMANDS.txt", b"commands\r\n" * 500)
    z.writestr("b4bcoop-LICENSE.txt", b"MIT\r\n")
    z.writestr("b4bcoop-README.txt", b"readme\r\n" * 100)
    z.writestr("xinput1_3.dll", b"MZ" + os.urandom(3000))
with zipfile.ZipFile(f"{d}/evil.zip", "w", zipfile.ZIP_DEFLATED) as z:
    z.writestr("../evil.txt", b"x")
with zipfile.ZipFile(f"{d}/stored.zip", "w", zipfile.ZIP_STORED) as z:
    z.writestr("b4bcoop-README.txt", b"stored")
PY
sha256sum "$tmp/release.zip" | cut -d' ' -f1 > "$tmp/release.zip.sha256"
cp "$here/fixtures/release-latest.json" "$tmp/"
openssl genpkey -algorithm ed25519 -out "$tmp/key.pem" 2>/dev/null
"$root/tools/sign-release.sh" pubhex "$tmp/key.pem" > "$tmp/pub.hex"
"$root/tools/sign-release.sh" manifest "$tmp/release.zip" 0.6.2 2 > "$tmp/b4bcoop-update.txt"
printf '{"b4bcoop-shop": 1, "addons": [{"id": "casual_joe", "name": "Casual Joe", "license": "CC0-1.0", "size": 5, "sha256": "%s", "url": "https://github.com/o/r/releases/download/t/casual_joe.pak"}]}\n' \
  "$(printf hello | sha256sum | cut -d' ' -f1)" > "$tmp/catalog.json"   # the add-on shop's catalog (#36)
"$root/tools/sign-release.sh" sign "$tmp/key.pem" "$tmp/b4bcoop-update.txt" "$tmp/release.zip" >/dev/null
openssl genpkey -algorithm ed25519 -out "$tmp/shopkey.pem" 2>/dev/null   # the shop has its own key (signkeys.h)
"$root/tools/sign-release.sh" pubhex "$tmp/shopkey.pem" > "$tmp/shoppub.hex"
"$root/tools/sign-release.sh" sign "$tmp/shopkey.pem" "$tmp/catalog.json" >/dev/null
# the keys built into the agent (native/src/signkeys.h) must be the committed public keys
"$root/tools/sign-release.sh" pubhex "$root/docs/release-signing.pub.pem" > "$tmp/release-committed.hex"
"$root/tools/sign-release.sh" pubhex "$root/docs/shop-signing.pub.pem" > "$tmp/shop-committed.hex"
"$tmp/updcore_test" "$tmp"

# Add-on pak checks (native/test/pakfmt_test.c)
zig="$v/zig/zig"; [[ -x $zig.exe ]] && zig+=.exe
"$zig" cc -O2 -Wall -I"$here/../src" "$here/pakfmt_test.c" "$here/../src/pakfmt.c" -o "$tmp/pakfmt_test"
"$tmp/pakfmt_test"
