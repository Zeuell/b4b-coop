#!/usr/bin/env bash
# Fetch build/tooling dependencies into vendor/ (not committed): zig (cross-compiler), MinHook, Dear ImGui, Monocypher
# and zlib's puff (the in-game updater's signature check and inflate, #34), stb_image (the add-on shop's thumbnails,
# #36), embeddable Windows Python (runs inside the
# game's Proton prefix for memory tooling). Also creates the host .venv.
#   tools/fetch-deps.sh          everything (development)
#   tools/fetch-deps.sh --build  only what native/build.sh needs: zig + MinHook + ImGui + Monocypher + puff + stb (CI)
# zig, MinHook, ImGui, Monocypher and zlib are pinned (version + sha256, commit; ImGui, Monocypher and puff also the
# sha256 of the files the build compiles) so every build uses the same compiler and libraries.
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
v="$root/vendor"; mkdir -p "$v"
ZIG_VERSION=0.15.2
ZIG_SHA256=02aa270f183da276e5b5920b1dac44a63f1a49e55050ebde3aecc9eb82f93239   # zig-x86_64-linux-0.15.2.tar.xz
ZIG_SHA256_WIN=3a0ed1e8799a2f8ce2a6e6290a9ff22e6906f8227865911fb7ddedc3cc14cb0c   # zig-x86_64-windows-0.15.2.zip (Git Bash)
MINHOOK_COMMIT=8af6b4acae5a9388fd742b56fa79ece89d96f823                          # TsudaKageyu/minhook
IMGUI_COMMIT=f1cc2ae15e53a861a874c3034aae6798fde194ab                            # ocornut/imgui v1.92.9b (MIT)
IMGUI_SHA256=3104b07fe14084b78d567321434d7975616c82da26ecab54bb986cb826b928cf    # the sources below, concatenated
IMGUI_FILES="imgui.h imgui_internal.h imgui.cpp imgui_draw.cpp imgui_tables.cpp imgui_widgets.cpp imconfig.h imstb_rectpack.h
  imstb_textedit.h imstb_truetype.h backends/imgui_impl_dx12.h backends/imgui_impl_dx12.cpp backends/imgui_impl_win32.h
  backends/imgui_impl_win32.cpp"
MONOCYPHER_COMMIT=0d85f98c9d9b0227e42cf795cb527dff372b40a4                       # LoupVaillant/Monocypher 4.0.2 (CC0 or BSD-2)
MONOCYPHER_SHA256=afbfd61450ebe219cd9e01d3897f914b0c9351d0a0593bd64b3eb34b6127038a # the sources below, concatenated
MONOCYPHER_FILES="src/monocypher.c src/monocypher.h src/optional/monocypher-ed25519.c src/optional/monocypher-ed25519.h"
ZLIB_COMMIT=51b7f2abdade71cd9bb0e7a373ef2610ec6f9daf                             # madler/zlib v1.3.1 (zlib licence): contrib/puff
PUFF_SHA256=6da25c1bcc606585566da8d6af3c7209657ee4ec5c67f116e13e52197e86b1a5      # puff.c + puff.h, concatenated
STB_COMMIT=2c980bb59875b0d32144a71867fbdebb2f77cd20                              # nothings/stb (public domain / MIT): stb_image v2.30
STB_IMAGE_SHA256=594c2fe35d49488b4382dbfaec8f98366defca819d916ac95becf3e75f4200b3 # stb_image.h
build_only=0; [[ ${1:-} == --build ]] && build_only=1

# The same zig release for the host we run on: Linux (CI, dev machines) or Windows under Git Bash / MSYS (zig.exe;
# native/build.sh cross-compiles for Windows from either). The build output does not depend on the host.
case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) zig_host=windows zig_exe="$v/zig/zig.exe" ;; *) zig_host=linux zig_exe="$v/zig/zig" ;; esac
if [[ ! -x "$zig_exe" ]]; then
  tmp=$(mktemp -d)
  if [[ $zig_host == windows ]]; then
    curl -sSfLo "$tmp/zig.zip" "https://ziglang.org/download/$ZIG_VERSION/zig-x86_64-windows-$ZIG_VERSION.zip"
    echo "$ZIG_SHA256_WIN  $tmp/zig.zip" | sha256sum -c --quiet -
    unzip -q "$tmp/zig.zip" -d "$v" && rm -rf "$tmp"
    mv "$v/zig-x86_64-windows-$ZIG_VERSION" "$v/zig"
  else
    curl -sSfLo "$tmp/zig.tar.xz" "https://ziglang.org/download/$ZIG_VERSION/zig-x86_64-linux-$ZIG_VERSION.tar.xz"
    echo "$ZIG_SHA256  $tmp/zig.tar.xz" | sha256sum -c --quiet -
    tar xJf "$tmp/zig.tar.xz" -C "$v" && rm -rf "$tmp"
    mv "$v/zig-x86_64-linux-$ZIG_VERSION" "$v/zig"
  fi
fi
[[ $("$zig_exe" version) == "$ZIG_VERSION" ]] || { echo "vendor/zig is not zig $ZIG_VERSION" >&2; exit 1; }

if [[ ! -d "$v/minhook" ]]; then
  git init -q "$v/minhook"
  git -C "$v/minhook" fetch -q --depth 1 https://github.com/TsudaKageyu/minhook.git "$MINHOOK_COMMIT"
  git -C "$v/minhook" -c core.autocrlf=false -c core.eol=lf checkout -q FETCH_HEAD
fi
[[ $(git -C "$v/minhook" rev-parse HEAD) == "$MINHOOK_COMMIT" ]] || echo "warning: vendor/minhook is not at the pinned commit $MINHOOK_COMMIT" >&2

if [[ ! -d "$v/imgui" ]]; then   # the `~` overlay (native/src/overlay.cpp, #26)
  git init -q "$v/imgui"
  git -C "$v/imgui" fetch -q --depth 1 https://github.com/ocornut/imgui.git "$IMGUI_COMMIT"
  git -C "$v/imgui" -c core.autocrlf=false -c core.eol=lf checkout -q FETCH_HEAD
fi
[[ $(cd "$v/imgui" && cat $IMGUI_FILES | sha256sum | cut -d' ' -f1) == "$IMGUI_SHA256" ]] ||
  { echo "vendor/imgui does not match the pinned sources ($IMGUI_COMMIT)" >&2; exit 1; }

pinned_git() {   # pinned_git <dir> <url> <commit>
  if [[ ! -d "$v/$1" ]]; then
    git init -q "$v/$1"
    git -C "$v/$1" fetch -q --depth 1 "$2" "$3"
    git -C "$v/$1" -c core.autocrlf=false -c core.eol=lf checkout -q FETCH_HEAD
  fi
}
pinned_git monocypher https://github.com/LoupVaillant/Monocypher.git "$MONOCYPHER_COMMIT"   # updater: ed25519 check
[[ $(cd "$v/monocypher" && cat $MONOCYPHER_FILES | sha256sum | cut -d' ' -f1) == "$MONOCYPHER_SHA256" ]] ||
  { echo "vendor/monocypher does not match the pinned sources ($MONOCYPHER_COMMIT)" >&2; exit 1; }
pinned_git zlib https://github.com/madler/zlib.git "$ZLIB_COMMIT"                            # updater: inflate (puff)
[[ $(cd "$v/zlib/contrib/puff" && cat puff.c puff.h | sha256sum | cut -d' ' -f1) == "$PUFF_SHA256" ]] ||
  { echo "vendor/zlib does not match the pinned sources ($ZLIB_COMMIT)" >&2; exit 1; }

pinned_git stb https://github.com/nothings/stb.git "$STB_COMMIT"                              # shop: thumbnails
[[ $(sha256sum < "$v/stb/stb_image.h" | cut -d' ' -f1) == "$STB_IMAGE_SHA256" ]] ||
  { echo "vendor/stb does not match the pinned sources ($STB_COMMIT)" >&2; exit 1; }

if [[ $build_only == 0 ]]; then
  if [[ ! -x "$v/winpy/python.exe" ]]; then
    tmp=$(mktemp -d); curl -sSfLo "$tmp/py.zip" https://www.python.org/ftp/python/3.12.10/python-3.12.10-embed-amd64.zip
    mkdir -p "$v/winpy" && unzip -q "$tmp/py.zip" -d "$v/winpy" && rm -rf "$tmp"
  fi
  [[ -d "$root/.venv" ]] || { python3 -m venv "$root/.venv" && "$root/.venv/bin/pip" -q install capstone pefile; }
fi
echo "deps ready in $v"
