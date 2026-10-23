#!/usr/bin/env python3
"""b4bmod: the b4bcoop mod maker's kit, one command from the game's files to an installed add-on.

  setup                                   get .NET 10 + UAssetAPI (no admin), build the tools, show what's missing
  status                                  what is set up: tools, game folder, AES key, Blender
  config [<key> [<value>]]                show or set game, blender, aes_key (built in; only to override)

  find <regex>                            search every game asset path (offline, from the pak indexes)
  extract <asset | folder/*>... [--regex R] [-o DIR]
                                          copy game files out of the paks (default into the extract folder, which the
                                          other commands read); e.g. /Game/Characters/Heroes/Walker/*
  info <asset>...                         texture format/size, material parameters, what a package references
  tree <asset>                            mesh -> material instances -> textures
  export <asset> <out.png> [--mip N]      texture -> PNG
  texture <asset> <in.png> -o <moddir> [--resize] [--quality fast|balanced|best]
                                          your PNG -> the game's texture (same format, full mip chain)
  mi <asset> [list]                       material instance parameters
  mi <asset> set <param> <value> [set <param> <value>...] [parent <path>] -o <moddir>
  dt <asset> [<row>] [--names]            a data table's rows and fields (cards, difficulties, ...); --names: row
                                          names only; a composite table lists the tables it is made of
  rename <asset> </Game/new/Path/Name> -o <moddir> [--ref </Game/old>=</Game/new>]...
                                          a copy of an asset as a NEW package (adds, replaces nothing); --ref points
                                          its references at other renamed copies (MI -> textures, mesh -> MIs)
  mesh info <asset>                       materials (slots), LODs, bones of a skeletal mesh
  mesh export <asset> <out.glb> [--lod N] the game's mesh with its skeleton, to open in Blender
  mesh import <template asset> <model.fbx|.glb|.gltf> -o <moddir> [--lods N] [--material NAME=SLOT]...
                                          your model (rigged to the template's skeleton) -> the game's mesh
  mesh edit <asset> -o <moddir> [--inflate CM] [--scale-section L:S:F] [--material L:S:M]
  survivor <model> --outfit <3P outfit SKM> [--fp <FP arms SKM>|none] -o <moddir> [--slot MAT=SLOT]... [--tex MAT=PREFIX]...
         (--fp default: the outfit's own FP_ arms from the same folder; none = the game's arms in first person)
         [--as <name>]                    --as: ADD an outfit instead of replacing the template (new packages under
                                          /Game/b4bcoop/outfits/<name>/; players wear it with /model <name>)
  weapon <model> --fp-mesh <FP SKM | code like AR02> -o <moddir> [--slot MAT=SLOT]... [--tex MAT=PREFIX]...
         [--forward +x] [--up +z] [--part REGEX=BONE]...   the 3P/static/magazine meshes are found in the FP mesh's
         folder (override: --3p-mesh <SKM>, --static <SM>..., --mag-static <SM>, each also `none`; --no-infer)
         [--as <name>]                    --as: ADD a weapon look instead of replacing the weapon (new packages under
                                          /Game/b4bcoop/weapons/<name>/; players put it on their weapon with /model <name>)
                                          your model (FBX, glTF, OBJ, .blend; rigged or not) fitted onto the game's
                                          meshes in Blender, with LODs and textures; then packed into <moddir>.pak.
                                          --pak NAME.pak, --title/--author/--version/--description, --zip, --install,
                                          --no-pack; all other options: b4bmod model help. Guide: docs/meshes.md
  convert <mod.pak> --as <name> [-o <moddir>] [--title T] [--only N,...] [--list]
                                          a survivor REPLACEMENT mod (a pak that overrides an outfit) -> an ADDED
                                          outfit any survivor can wear (/model <name>); the pak is only read. Several
                                          outfits in the mod: <name>_01, _02 ... (--only picks, --list shows them).
                                          Packs <moddir>.pak (default <moddir> = <name>); --pak, --zip, --install,
                                          --author/--version/--description as for survivor. --eyes game (default):
                                          eyes drawn with a clothing/skin material get the game's eye material, the
                                          iris cut out of their texture (--eyes keep: as the mod has them)
  model <b4bmodel.py arguments>           the model pipeline as is (e.g. model textures <manifest.json> ...)
  pack <moddir> [-o NAME.pak] [--title T] [--author A] [--version V] [--category C] [--description D] [--zip]
                                          <moddir> -> one add-on .pak (+ a zip for players with --zip)
  install <addon.pak> | uninstall <name>  copy into / remove from <game>/b4bcoop-addons (restart the game)
  check [<addon.pak> | <addons folder>]   an add-on's info, or what the game will load (default: the game's folder)

<asset> is a game path like /Game/Characters/Heroes/Walker/Meshes/Elite/Elite_00/3P_Walker_Elite_00_SKM (from `find`)
or a .uasset file. <moddir> is your mod's folder: the edited files go to <moddir>/Gobi/Content/..., like the game's.
Options anywhere: --aes-key <key>, --game <Back 4 Blood folder>, --src <extract folder>.
Guide: README.md next to this file. Never share extracted game files; share add-ons you made.
"""
import hashlib, io, json, os, re, shutil, subprocess, sys, urllib.request, zipfile

KIT = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(KIT) if os.path.isdir(os.path.join(os.path.dirname(KIT), "native")) else None
WIN = os.name == "nt"
if WIN:
    DATA = os.environ.get("B4B_MODKIT_DATA") or os.path.join(os.environ.get("LOCALAPPDATA", os.path.expanduser("~")), "b4b-coop")
else:
    DATA = os.environ.get("B4B_MODKIT_DATA") or os.path.join(os.path.expanduser("~"), ".local", "share", "b4b-coop")
# third-party pieces `setup` fetches: <kit>/deps, or the repo's vendor/ (developers already have them there)
DEPS = os.environ.get("B4B_MODKIT_DEPS") or (os.path.join(REPO, "vendor") if REPO else os.path.join(KIT, "deps"))
CONFIG = os.path.join(DATA, "b4bmod.ini")

UASSETAPI_COMMIT = "3228c1e86261aa08131f7ec0ff1a395f5d0b2a84"   # MIT, github.com/atenfyr/UAssetAPI (tested)
UASSETAPI_ZIP_SHA256 = "3c044cc871c41e877f76ce42ced31f0d700ae9febe959d35b03a41ded8c4e92f"
DOTNET_CHANNEL = "10.0"
PROJECTS = {"b4bmod": os.path.join(KIT, "dotnet", "b4bmod"), "pakx": os.path.join(KIT, "dotnet", "pakx")}
KEY_RX = re.compile(r"^(0x)?[0-9a-fA-F]{64}$")
# The pak index AES key: the same for every copy of this game build (the public community key; `status` checks it).
AES_KEY = "0x0208250257E8EA16828509DEBF23D703A5B509FE4F15F33F11BEE4BAB1F97CFD"


def die(msg, code=1):
    print(f"b4bmod: {msg}", file=sys.stderr)
    raise SystemExit(code)


# ---- config -----------------------------------------------------------------------------------------------------

def read_config():
    cfg = {}
    if os.path.exists(CONFIG):
        for line in open(CONFIG, encoding="utf-8"):
            if "=" in line and not line.lstrip().startswith((";", "#")):
                k, v = line.split("=", 1)
                cfg[k.strip().lower()] = v.strip()
    return cfg


def write_config(cfg):
    os.makedirs(DATA, exist_ok=True)
    with open(CONFIG, "w", encoding="utf-8") as f:
        f.write("; b4bmod settings (b4bmod config <key> <value>)\n")
        for k in sorted(cfg):
            f.write(f"{k}={cfg[k]}\n")


OPTS = {}   # --aes-key, --game, --src given on the command line


def aes_key(required=True):
    k = OPTS.get("--aes-key") or os.environ.get("B4B_AES_KEY") or read_config().get("aes_key") or AES_KEY
    if k and not KEY_RX.match(k.strip()):
        die("the AES key must be 64 hex digits, optionally with 0x in front")
    return k.strip() if k else None


# ---- game folder ------------------------------------------------------------------------------------------------

def steam_roots():
    roots = []
    if WIN:
        try:
            import winreg
            with winreg.OpenKey(winreg.HKEY_CURRENT_USER, r"Software\Valve\Steam") as k:
                roots.append(winreg.QueryValueEx(k, "SteamPath")[0])
        except OSError:
            pass
        roots.append(r"C:\Program Files (x86)\Steam")
    else:
        h = os.path.expanduser("~")
        roots += [os.path.join(h, ".local/share/Steam"), os.path.join(h, ".steam/steam"),
                  os.path.join(h, ".var/app/com.valvesoftware.Steam/.local/share/Steam")]
    return roots


def find_game():
    g = OPTS.get("--game") or os.environ.get("B4B_GAME") or read_config().get("game")
    if g:
        return g
    libs = []
    for r in steam_roots():
        vdf = os.path.join(r, "steamapps", "libraryfolders.vdf")
        libs.append(r)
        if os.path.exists(vdf):
            libs += [p.replace("\\\\", "\\") for p in re.findall(r'"path"\s+"([^"]+)"', open(vdf, encoding="utf-8", errors="replace").read())]
    for lib in libs:
        g = os.path.join(lib, "steamapps", "common", "Back 4 Blood")
        if os.path.isdir(os.path.join(g, "Gobi", "Content", "Paks")):
            return g
    return None


def game_dir():
    g = find_game()
    if not g:
        die("Back 4 Blood not found. Tell b4bmod where it is (the folder with Back4Blood.exe and Gobi in it; in Steam:\n"
            "  right-click Back 4 Blood > Manage > Browse local files):  b4bmod config game \"<that folder>\"")
    if not os.path.isdir(os.path.join(g, "Gobi", "Content", "Paks")):
        die(f"{g}: no Gobi/Content/Paks in it; that is not the Back 4 Blood folder")
    return g


def paks_dir():
    return os.path.join(game_dir(), "Gobi", "Content", "Paks")


def src_dir():
    return OPTS.get("--src") or os.environ.get("B4B_EXTRACT") or os.path.join(DATA, "extract")


# ---- .NET and the tools -----------------------------------------------------------------------------------------

def dotnet_exe():
    """(path, env) of a dotnet with a 10.x SDK: the kit's own, else one on PATH; None if there is none."""
    own = os.path.join(DEPS, "dotnet", "dotnet.exe" if WIN else "dotnet")
    base = dict(os.environ, DOTNET_CLI_TELEMETRY_OPTOUT="1", DOTNET_NOLOGO="1", DOTNET_SKIP_FIRST_TIME_EXPERIENCE="1")
    if os.path.exists(own):
        return own, dict(base, DOTNET_ROOT=os.path.dirname(own))
    sysd = shutil.which("dotnet")
    if sysd:
        try:
            sdks = subprocess.run([sysd, "--list-sdks"], capture_output=True, text=True, timeout=60).stdout
            if re.search(r"^10\.", sdks, re.M):
                return sysd, base
        except (OSError, subprocess.SubprocessError):
            pass
    return None, base


def fetch(url):
    with urllib.request.urlopen(url, timeout=300) as r:
        return r.read()


def install_dotnet():
    d = os.path.join(DEPS, "dotnet")
    os.makedirs(DEPS, exist_ok=True)
    print(f"installing the .NET {DOTNET_CHANNEL} SDK into {d} (Microsoft's dotnet-install script, no admin) ...")
    if WIN:
        script = os.path.join(DEPS, "dotnet-install.ps1")
        open(script, "wb").write(fetch("https://dot.net/v1/dotnet-install.ps1"))
        subprocess.run(["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", script,
                        "-Channel", DOTNET_CHANNEL, "-InstallDir", d], check=True)
    else:
        script = os.path.join(DEPS, "dotnet-install.sh")
        open(script, "wb").write(fetch("https://dot.net/v1/dotnet-install.sh"))
        subprocess.run(["bash", script, "--channel", DOTNET_CHANNEL, "--install-dir", d], check=True)


def install_uassetapi():
    d = os.path.join(DEPS, "UAssetAPI")
    url = f"https://github.com/atenfyr/UAssetAPI/archive/{UASSETAPI_COMMIT}.zip"
    print(f"fetching UAssetAPI {UASSETAPI_COMMIT[:7]} (MIT) from {url} ...")
    data = fetch(url)
    if hashlib.sha256(data).hexdigest() != UASSETAPI_ZIP_SHA256:
        die("the UAssetAPI download does not have the expected SHA-256; not using it. Please report this.")
    top = f"UAssetAPI-{UASSETAPI_COMMIT}/"
    with zipfile.ZipFile(io.BytesIO(data)) as z:
        for m in z.infolist():
            if not m.filename.startswith(top) or m.is_dir():
                continue
            rel = m.filename[len(top):]
            if "/" in rel and not rel.startswith("UAssetAPI/"):
                continue   # tests, benchmarks and docs are not needed (top-level files are: README.md is packed)
            out = os.path.join(d, *rel.split("/"))
            os.makedirs(os.path.dirname(out), exist_ok=True)
            with open(out, "wb") as f:
                f.write(z.read(m))
    open(os.path.join(d, "COMMIT"), "w").write(UASSETAPI_COMMIT + "\n")


def tool_dll(name):
    return os.path.join(PROJECTS[name], "bin", "Release", "net10.0", name + ".dll")


def build(name, force=False):
    proj, dll = PROJECTS[name], tool_dll(name)
    srcs = [os.path.join(proj, f) for f in os.listdir(proj) if f.endswith((".cs", ".csproj"))]
    if not force and os.path.exists(dll) and max(map(os.path.getmtime, srcs)) <= os.path.getmtime(dll):
        return
    dn, env = dotnet_exe()
    if not dn:
        die("no .NET 10 SDK: run  b4bmod setup")
    if name == "b4bmod" and not os.path.exists(os.path.join(DEPS, "UAssetAPI", "UAssetAPI", "UAssetAPI.csproj")):
        die("UAssetAPI is missing: run  b4bmod setup")
    print(f"building {name} (first run downloads its NuGet packages) ...", file=sys.stderr)
    r = subprocess.run([dn, "build", "-c", "Release", "-v", "q", "-nologo", f"-p:B4BDeps={DEPS}", proj],
                       env=env, capture_output=True, text=True)
    if r.returncode:
        sys.stderr.write(r.stdout + r.stderr)
        die(f"building {name} failed")


def run_dotnet_tool(name, args, capture=False):
    build(name)
    dn, env = dotnet_exe()
    if capture:
        return subprocess.run([dn, tool_dll(name)] + args, env=env, capture_output=True, text=True)
    return subprocess.run([dn, tool_dll(name)] + args, env=env).returncode


def pakx(args, capture=False):
    base = ["--paks", paks_dir(), "--aes", aes_key()]
    r = run_dotnet_tool("pakx", [args[0]] + base + args[1:], capture)
    if capture and r.returncode:
        die((r.stderr or r.stdout).strip().removeprefix("pakx: "))
    return r


def tool_env():
    """What the mesh tools and b4bmodel need from b4bmod's settings: Blender, game folder, AES key."""
    env = dict(os.environ)
    bl = blender_exe()
    if bl:
        env["B4B_BLENDER"] = bl
    if OPTS.get("--game"):
        env["B4B_GAME"] = OPTS["--game"]
    if OPTS.get("--aes-key"):
        env["B4B_AES_KEY"] = OPTS["--aes-key"]
    return env


def python_tool(script, args):
    return subprocess.run([sys.executable, script] + args, env=tool_env()).returncode


# ---- assets -----------------------------------------------------------------------------------------------------

def unmangle(a):
    """Git Bash on Windows turns /Game/... into C:/Program Files/Git/Game/...: undo that."""
    m = re.match(r"^[A-Za-z]:[/\\].*?[/\\]Git[/\\](Game|Engine)[/\\](.*)$", a)
    return f"/{m.group(1)}/{m.group(2)}" if m else a


def pak_path(asset):
    """/Game/X/Y[.Y] -> Gobi/Content/X/Y ; /Engine/X -> Engine/Content/X."""
    for root, pre in (("/Game/", "Gobi/Content/"), ("/Engine/", "Engine/Content/")):
        if asset.startswith(root):
            p = asset[len(root):]
            dot = p.rfind(".")
            if dot > p.rfind("/"):
                p = p[:dot]
            return pre + p
    return None


def asset_regex(a):
    """/Game/... package or glob (* matches across folders) -> regex over pak paths, every file of the package(s)."""
    p = pak_path(a)
    if p is None:
        die(f"{a}: expected a /Game/... path (see `b4bmod find`) or --regex")
    rx = "^" + "".join(".*" if c == "*" else "." if c == "?" else re.escape(c) for c in p)
    return rx if p.endswith("*") else rx + r"\.[^/.]+$"


def extract(patterns, out=None, quiet=False):
    out = out or src_dir()
    args = ["extract", "--out", out]
    rx = "|".join(f"(?:{p})" for p in patterns)
    r = pakx(args + [rx], capture=True)
    if not quiet or not r.stderr.startswith("0 files"):
        sys.stderr.write(r.stderr)
    return r


def asset_file(asset):
    p = pak_path(asset)
    return os.path.join(src_dir(), *(p + ".uasset").split("/")) if p else asset


def ensure(asset, folder=False):
    """Extract a /Game/ package (or its whole folder) if it isn't in the extract folder yet."""
    p = pak_path(asset)
    if not p or os.path.exists(asset_file(asset)):
        return
    print(f"extracting {asset}{' and its folder' if folder else ''} ...", file=sys.stderr)
    extract([asset_regex(asset.rsplit("/", 1)[0] + "/*" if folder else asset)], quiet=True)
    if not os.path.exists(asset_file(asset)):
        die(f"{asset}: not in the game's paks (check the path with  b4bmod find <part of the name>)")


# ---- commands ---------------------------------------------------------------------------------------------------

def cmd_setup(a):
    if not dotnet_exe()[0]:
        install_dotnet()
    if not os.path.exists(os.path.join(DEPS, "UAssetAPI", "UAssetAPI", "UAssetAPI.csproj")):
        install_uassetapi()
    for n in PROJECTS:
        build(n, force=True)
    print("tools built.\n")
    return cmd_status([])


def cmd_status(a):
    ok = True
    print(f"python      {sys.version.split()[0]} ({sys.executable})")
    dn = dotnet_exe()[0]
    print(f".NET SDK    {dn or 'MISSING: run  b4bmod setup'}")
    ua = os.path.join(DEPS, "UAssetAPI", "UAssetAPI", "UAssetAPI.csproj")
    print(f"UAssetAPI   {os.path.dirname(os.path.dirname(ua)) if os.path.exists(ua) else 'MISSING: run  b4bmod setup'}")
    for n in PROJECTS:
        print(f"{n:<11} {'built' if os.path.exists(tool_dll(n)) else 'not built yet: run  b4bmod setup'}")
    ok &= bool(dn) and os.path.exists(ua)
    g = find_game()
    print(f"game        {g or 'NOT FOUND: b4bmod config game <Back 4 Blood folder>'}")
    k = aes_key(required=False)
    src = ("--aes-key" if OPTS.get("--aes-key") else "B4B_AES_KEY" if os.environ.get("B4B_AES_KEY")
           else CONFIG if read_config().get("aes_key") else "built in")
    if g and dn and os.path.exists(tool_dll("pakx")):
        r = run_dotnet_tool("pakx", ["key", "--paks", os.path.join(g, "Gobi", "Content", "Paks"), "--aes", k], capture=True)
        print(f"AES key     {'OK (checked against the paks)' if r.returncode == 0 else 'WRONG: ' + (r.stderr or r.stdout).strip()} (from {src})")
        ok &= r.returncode == 0
    else:
        print(f"AES key     set (from {src}), not checked yet")
    bl = blender_exe()
    print(f"Blender     {bl or 'not found (only for meshes): install Blender, or b4bmod config blender <path to blender>'}")
    print(f"extract to  {src_dir()}")
    print(f"settings    {CONFIG}")
    print("\nready." if ok else "\nnot ready yet: fix the lines above.")
    return 0 if ok else 1


def cmd_config(a):
    cfg = read_config()
    if not a:
        print(f"{CONFIG}:")
        for k in ("game", "blender", "aes_key"):
            print(f"  {k}={cfg.get(k, '')}")
        return 0
    k = a[0].lower()
    if k not in ("aes_key", "game", "blender"):
        die("keys: game, blender, aes_key")
    if len(a) == 1:
        print(cfg.get(k, ""))
        return 0
    v = a[1].strip()
    if v in ("", "-", "none"):
        cfg.pop(k, None)
    elif k == "aes_key":
        if not KEY_RX.match(v):
            die("the AES key must be 64 hex digits, optionally with 0x in front")
        v = v if v.lower().startswith("0x") else "0x" + v
        OPTS["--aes-key"] = v
        r = pakx(["key"], capture=True)
        print(r.stdout.strip())
    elif k == "game":
        v = os.path.abspath(v)
        if not os.path.isdir(os.path.join(v, "Gobi", "Content", "Paks")):
            die(f"{v}: no Gobi/Content/Paks in it; give the folder with Back4Blood.exe and Gobi")
    elif not os.path.exists(v):
        die(f"{v}: not found")
    if v not in ("", "-", "none"):
        cfg[k] = v
    write_config(cfg)
    print(f"saved in {CONFIG}")
    return 0


def listing():
    """Every file in the paks (path, pak), cached per game version (pak names and sizes)."""
    p = paks_dir()
    sig = hashlib.sha1("".join(f"{n}:{os.path.getsize(os.path.join(p, n))};" for n in sorted(os.listdir(p))
                               if n.endswith(".pak")).encode()).hexdigest()[:12]
    cache = os.path.join(DATA, "listing", f"files-{sig}.tsv")
    if not os.path.exists(cache):
        r = pakx(["list"], capture=True)
        os.makedirs(os.path.dirname(cache), exist_ok=True)
        with open(cache, "w", encoding="utf-8") as f:
            f.write(r.stdout)
        print(f"(indexed {r.stdout.count(chr(10))} files into {cache})", file=sys.stderr)
    for line in open(cache, encoding="utf-8"):
        path, _, pak = line.rstrip("\n").split("\t")
        yield path, pak


def cmd_find(a):
    if len(a) != 1:
        die("usage: b4bmod find <regex>   e.g. b4bmod find \"Heroes/Walker/.*_SKM$\"", 2)
    rx = re.compile(a[0], re.I)
    n = 0
    for path, pak in listing():
        if not path.endswith((".uasset", ".umap")):
            continue
        base = path.rsplit(".", 1)[0]
        game = "/Game/" + base[len("Gobi/Content/"):] if base.startswith("Gobi/Content/") else \
               "/Engine/" + base[len("Engine/Content/"):] if base.startswith("Engine/Content/") else base
        if rx.search(game):
            print(f"{game}\t{pak}")
            n += 1
    print(f"{n} assets", file=sys.stderr)
    return 0


def take(a, flag):
    if flag in a:
        i = a.index(flag)
        if i + 1 >= len(a):
            die(f"{flag} needs a value", 2)
        v = a[i + 1]
        del a[i:i + 2]
        return v
    return None


def cmd_extract(a):
    out = take(a, "-o") or take(a, "--out")
    pats = []
    while "--regex" in a:
        pats.append(take(a, "--regex"))
    pats += [asset_regex(x) for x in a]
    if not pats:
        die("usage: b4bmod extract </Game/... asset or folder/*>... [--regex R] [-o DIR]", 2)
    extract(pats, out)
    return 0


def extract_refs(a):
    """`tree` of an asset, after extracting what it references (materials, textures, parents), a level per round."""
    for _ in range(8):
        r = run_dotnet_tool("b4bmod", ["tree"] + a + ["--src", src_dir()], capture=True)
        missing = sorted(set(re.findall(r"(/(?:Game|Engine)/\S+)  \(not extracted\)", r.stdout)))
        if r.returncode or not missing:
            break
        print(f"extracting {len(missing)} referenced asset(s) ...", file=sys.stderr)
        extract([asset_regex(m) for m in missing], quiet=True)
    return r


def cmd_dotnet(cmd, a):
    """info/tree/export/texture/mi/...: the .NET tool, after extracting the assets it names."""
    for x in (a[:1] if cmd in ("rename", "mi") else a):   # rename, mi set: new paths are not in the game
        if x.startswith(("/Game/", "/Engine/")):
            ensure(x, folder=(cmd == "tree"))
            if cmd != "info":
                break
    if cmd == "tree":
        r = extract_refs(a)
        sys.stdout.write(r.stdout)
        sys.stderr.write(r.stderr)
        return r.returncode
    return run_dotnet_tool("b4bmod", [cmd] + a + ["--src", src_dir()])


def blender_exe():
    b = read_config().get("blender") or os.environ.get("BLENDER") or shutil.which("blender")
    if not b and WIN:
        import glob
        c = sorted(glob.glob(r"C:\Program Files\Blender Foundation\Blender*\blender.exe"))
        b = c[-1] if c else None
    return b


MODEL_EXTS = (".fbx", ".glb", ".gltf", ".obj", ".dae", ".blend", ".vrm")


def model_file(model):
    """A model file the mesh tools read: glTF as is; FBX/OBJ/DAE/.blend need Blender (they convert it themselves)."""
    ext = os.path.splitext(model)[1].lower()
    if ext not in MODEL_EXTS:
        die(f"{model}: give a .fbx, .glb, .gltf, .vrm, .obj, .dae or .blend file (other formats: open it in Blender, export FBX or glTF)")
    if not os.path.exists(model):
        die(f"{model}: not found")
    if ext not in (".glb", ".gltf") and not blender_exe():
        die(f"{ext} needs Blender to read it (or export glTF from your 3D tool): install Blender (blender.org), or\n"
            "  b4bmod config blender <path to blender(.exe)>")
    return os.path.abspath(model)


def moddir_file(moddir, asset):
    p = pak_path(asset)
    if not p:
        die(f"{asset}: give the template as a /Game/... path, so the output lands at the same game path")
    out = os.path.join(moddir, *(p + ".uasset").split("/"))
    os.makedirs(os.path.dirname(out), exist_ok=True)
    return out


def cmd_mesh(a):
    if not a or a[0] not in ("info", "export", "import", "edit"):
        die("usage: b4bmod mesh info|export|import|edit ...  (b4bmod help)", 2)
    sub, a = a[0], a[1:]
    skm, skmgltf = os.path.join(KIT, "skm.py"), os.path.join(KIT, "skmgltf.py")
    if sub == "info":
        for x in a:
            ensure(x)
        return python_tool(skm, ["info"] + [asset_file(x) for x in a])
    if sub == "export":
        if len(a) < 2:
            die("usage: b4bmod mesh export <asset> <out.glb> [--lod N]", 2)
        ensure(a[0])
        return python_tool(skmgltf, ["export", asset_file(a[0])] + a[1:])
    moddir = take(a, "-o") or take(a, "--out")
    if not moddir:
        die(f"mesh {sub} needs -o <moddir>", 2)
    if sub == "import":
        if len(a) < 2:
            die("usage: b4bmod mesh import <template asset> <model.fbx|.glb> -o <moddir> [--lods N] [--material NAME=SLOT]", 2)
        ensure(a[0])
        out = moddir_file(moddir, a[0])
        extra = a[2:]
        if "--lods" not in extra:
            # as many LODs as the template: lower graphics settings draw LOD1+ (copies of the imported mesh)
            info = subprocess.run([sys.executable, skm, "info", asset_file(a[0])], capture_output=True, text=True).stdout
            m = re.search(r"\blods (\d+)", info)
            if m:
                extra += ["--lods", m.group(1)]
        rc = python_tool(skmgltf, ["import", asset_file(a[0]), model_file(a[1]), out] + extra)
        if rc == 0:
            print(f"-> {out} (+ .uexp). Next: b4bmod pack {moddir}")
        return rc
    if not a:
        die("usage: b4bmod mesh edit <asset> -o <moddir> [--inflate CM] ...", 2)
    ensure(a[0])
    return python_tool(skm, ["edit", asset_file(a[0]), moddir_file(moddir, a[0])] + a[1:])


TEMPLATE_FLAGS = {"survivor": ("--outfit", "--fp"), "weapon": ("--fp-mesh", "--3p-mesh", "--static", "--mag-static")}


def game_name(pak_file_path):
    """Gobi/Content/X/Y.uasset -> /Game/X/Y (None for other files)."""
    if not pak_file_path.startswith("Gobi/Content/") or not pak_file_path.endswith(".uasset"):
        return None
    return "/Game/" + pak_file_path[len("Gobi/Content/"):-len(".uasset")]


def weapon_code_mesh(code):
    """A weapon code (AR02, hg01) -> its first-person mesh /Game/[TUxx/]Items/Weapons/<Class>/<Code>/Meshes/<Code>_SKM."""
    rx = re.compile(r"^/Game/(TU[0-9]+/)?Items/Weapons/[^/]+/" + re.escape(code) + r"/Meshes/" + re.escape(code) + "_SKM$",
                    re.I)
    hits = sorted({g for g in (game_name(p) for p, _ in listing()) if g and rx.match(g)})
    if len(hits) != 1:
        die(f"--fp-mesh {code}: {'no weapon with that code' if not hits else 'several: ' + ', '.join(hits)} "
            f"(b4bmod find \"Weapons/.*/Meshes/.*_SKM$\" lists them; give the full path)", 2)
    return hits[0]


def infer_weapon_meshes(a):
    """Fill in the weapon's other meshes from the first-person mesh's folder: the 3P skeletal mesh (3P_<Code>_SKM), the
    static meshes other players and world pickups show (3P_<Code>_SM, <Code>_Pickup_SM) and the dropped empty magazine
    (the folder's *Empty*_SM). Only flags the modder didn't give; `--3p-mesh none`, `--static none`, `--mag-static none`
    leave one out, --no-infer turns it off."""
    i = a.index("--fp-mesh")
    fp = a[i + 1]
    if "/" not in fp and not fp.endswith(".uasset"):
        fp = a[i + 1] = weapon_code_mesh(fp)
        print(f"weapon: --fp-mesh {fp}", file=sys.stderr)
    if "--no-infer" in a:
        a.remove("--no-infer")
        return
    if not fp.startswith("/Game/"):
        return   # a .uasset file: no folder in the game to look in
    folder, name = fp.rsplit("/", 1)
    code = re.sub(r"_SKM$", "", name, flags=re.I)
    norm = lambda s: s.lower().replace("_", "")
    names = sorted(g.rsplit("/", 1)[1] for g in (game_name(p) for p, _ in listing())
                   if g and g.rsplit("/", 1)[0].lower() == folder.lower())
    c = norm(code)
    found = {
        "--3p-mesh": [n for n in names if norm(n) == f"3p{c}skm"],
        "--static": [n for n in names if norm(n) in (f"3p{c}sm", f"{c}picksm", f"{c}pickupsm")],
        "--mag-static": [n for n in names if n.lower().endswith("_sm") and "empty" in n.lower()],
    }
    if len(found["--3p-mesh"]) > 1 or len(found["--mag-static"]) > 1:
        die(f"{folder}: more than one candidate ({found}); give --3p-mesh / --mag-static yourself", 2)
    inferred = []
    for flag, hits in found.items():
        if flag in a:
            continue
        for n in hits:
            a += [flag, f"{folder}/{n}"]
            inferred.append(f"{flag} {folder}/{n}")
    # `none` removes a flag (the modder wants that mesh left as the game's)
    j = 0
    while j < len(a) - 1:
        if a[j] in ("--3p-mesh", "--static", "--mag-static") and a[j + 1].lower() == "none":
            del a[j:j + 2]
        else:
            j += 1
    used = {x.rsplit("/", 1)[-1] for x in a}
    other = [n for n in names if n.lower().endswith(("_sm", "_skm")) and n not in used and n != name]
    for x in inferred:
        print(f"weapon: {x}", file=sys.stderr)
    if other:
        print(f"weapon: not replaced (the game's own look stays): {', '.join(other)}", file=sys.stderr)


def infer_fp_arms(a):
    """survivor without --fp: the first-person arms of the same outfit (its folder, 3P_ -> FP_), so the model shows in
    first person too; `--fp none` keeps the game's arms."""
    if "--fp" in a:
        i = a.index("--fp")
        if i + 1 < len(a) and a[i + 1].lower() == "none":
            del a[i:i + 2]
            a += ["--fp", "none"]
            print("note: --fp none: only the third-person outfit is replaced; in first person you keep the original arms",
                  file=sys.stderr)
        return
    tp = a[a.index("--outfit") + 1]
    folder, name = tp.rsplit("/", 1) if "/" in tp else ("", tp)
    cand = f"{folder}/{re.sub(r'^3P_', 'FP_', name, flags=re.I)}" if re.match(r"^3P_", name, re.I) else None
    if cand and cand.lower().endswith(".uasset"):
        found = os.path.exists(cand)
    elif cand and cand.startswith("/Game/"):
        want = cand.lower()
        found = any((g or "").lower() == want for g in (game_name(p) for p, _ in listing()))
        if found:
            cand = next(g for g in (game_name(p) for p, _ in listing()) if (g or "").lower() == want)
    else:
        found = False
    if found:
        a += ["--fp", cand]
        print(f"survivor: first-person arms --fp {cand} (the outfit's own; --fp none keeps the game's arms)",
              file=sys.stderr)
    else:
        print(f"warning: no first-person arms found for {tp} ({cand or 'not a 3P_ mesh'} doesn't exist): only the "
              f"third-person outfit is replaced, in first person you keep the original arms (--fp <FP arms SKM>)",
              file=sys.stderr)


def cmd_model(kind, a):
    """survivor / weapon: extract the templates and what they reference, run b4bmodel.py, pack, install."""
    pak, install = take(a, "--pak"), "--install" in a
    nopack, zipit = "--no-pack" in a, "--zip" in a
    a = [x for x in a if x not in ("--install", "--no-pack", "--zip")]
    meta = {k: take(a, "--" + k) for k in ("title", "author", "version", "category", "description")}
    moddir = take(a, "-o") or take(a, "--out")
    if not a or a[0].startswith("-") or not moddir:
        die(f"usage: b4bmod {kind} <model> {'--outfit <3P SKM> [--fp <FP SKM>]' if kind == 'survivor' else '--fp-mesh <SKM or code>'} "
            "-o <moddir> [options]  (b4bmod help)", 2)
    if install and nopack:
        die("--install needs the pack step (drop --no-pack)", 2)
    model_file(a[0])
    need = {"survivor": "--outfit", "weapon": "--fp-mesh"}[kind]
    if need not in a:
        die(f"b4bmod {kind} needs {need} <game mesh> (see docs/meshes.md)", 2)
    if kind == "weapon":
        infer_weapon_meshes(a)
    if kind == "survivor":
        infer_fp_arms(a)
    if "--as" in a and meta["title"] and "--as-title" not in a:
        a += ["--as-title", meta["title"]]   # the outfit's name in /model list
    templates = [a[i + 1] for i, x in enumerate(a[:-1]) if x in TEMPLATE_FLAGS[kind]]
    for t in templates:
        if t.startswith(("/Game/", "/Engine/")):
            ensure(t)
            if "--as" in a:   # --as copies the folder's packages the meshes import (e.g. the weapon's skeleton LMG01_SK)
                extract([asset_regex(t.rsplit("/", 1)[0] + "/*")], quiet=True)
            r = extract_refs([t])   # its materials, their parents and textures: b4bmodel reads them all
            if r.returncode:
                sys.stderr.write(r.stdout + r.stderr)
                die(f"{t}: could not list what it references")
    rc = python_tool(os.path.join(KIT, "b4bmodel.py"), [kind] + a + ["-o", moddir, "--src", src_dir()])
    if rc or nopack:
        return rc
    out = pak or os.path.abspath(moddir).rstrip("/\\") + ".pak"
    meta["category"] = meta["category"] or {"survivor": "survivors", "weapon": "weapons"}[kind]
    args = ["pack", moddir, "-o", out] + [x for k, v in meta.items() if v for x in ("--" + k, v)]
    rc = python_tool(os.path.join(KIT, "addon.py"), args + (["--zip"] if zipit else []))
    if rc or not install:
        if rc == 0:
            print(f"next: b4bmod install {out}")
        return rc
    return cmd_install([out])


def cmd_convert(a):
    """convert: a survivor replacement pak -> an added outfit (modkit/convert.py), packed like `survivor --as`."""
    pak, install = take(a, "--pak"), "--install" in a
    zipit, list_only = "--zip" in a, "--list" in a
    a = [x for x in a if x not in ("--install", "--zip", "--list")]
    meta = {k: take(a, "--" + k) for k in ("title", "author", "version", "description")}
    name, only, moddir = take(a, "--as"), take(a, "--only"), take(a, "-o") or take(a, "--out")
    eyes = take(a, "--eyes") or "game"
    if eyes not in ("game", "keep"): die("--eyes game|keep")
    if len(a) != 1 or not a[0].lower().endswith(".pak") or not (name or list_only):
        die("usage: b4bmod convert <mod.pak> --as <name> [-o <moddir>] [--title T] [--only N,...] [--list]", 2)
    src_pak = os.path.abspath(a[0])
    if not os.path.isfile(src_pak):
        die(f"{src_pak}: not found")
    name = (name or "outfit").lower()
    moddir = os.path.abspath(moddir or name)
    st = os.stat(src_pak)
    work = os.path.join(DATA, "convert", hashlib.sha1(f"{src_pak}|{st.st_size}|{st.st_mtime_ns}".encode()).hexdigest()[:12])
    stage = os.path.join(work, "pak")
    if not os.path.isdir(os.path.join(stage, "Gobi")):
        shutil.rmtree(work, ignore_errors=True)
        print(f"reading {os.path.basename(src_pak)} ...", file=sys.stderr)
        r = run_dotnet_tool("pakx", ["extract", "--pak", src_pak, "--aes", aes_key(), "--out", stage, "."], capture=True)
        if r.returncode:
            shutil.rmtree(work, ignore_errors=True)
            die((r.stderr or r.stdout).strip().removeprefix("pakx: "))
        sys.stderr.write(r.stderr)
    retail = {g for g in (game_name(p) for p, _ in listing()) if g}
    low = {g.lower(): g for g in retail}

    def retail_file(pkg):
        g = low.get((pkg or "").lower())
        if not g:
            return None
        f = asset_file(g)
        if not os.path.exists(f):
            extract([asset_regex(g)], quiet=True)
        return f if os.path.exists(f) else None

    sys.path.insert(0, KIT)
    import convert
    lines = convert.convert(stage, name, moddir, src_dir(), retail, retail_file, title=meta["title"], only=only,
                            list_only=list_only, eyes=eyes)
    if list_only:
        return 0
    out = pak or moddir.rstrip("/\\") + ".pak"
    meta["category"] = "survivors"
    args = ["pack", moddir, "-o", out] + [x for k, v in meta.items() if v for x in ("--" + k, v)]
    rc = python_tool(os.path.join(KIT, "addon.py"), args + (["--zip"] if zipit else []))
    if rc:
        return rc
    print(f"in game: {', '.join('/model ' + l.split('=', 1)[1].split('|', 1)[0] for l in lines)}"
          f" (any survivor; players without this add-on see your survivor)")
    return cmd_install([out]) if install else 0


def addons_dir():
    return os.path.join(game_dir(), "b4bcoop-addons")


def cmd_install(a):
    if len(a) != 1 or not a[0].lower().endswith(".pak"):
        die("usage: b4bmod install <addon.pak>", 2)
    d = addons_dir()
    os.makedirs(d, exist_ok=True)
    dst = os.path.join(d, os.path.basename(a[0]))
    try:
        shutil.copyfile(a[0], dst)
    except PermissionError:
        die(f"can't write {dst}: close the game first")
    print(f"installed {dst}\nstart (or restart) the game; the chat command /addons lists it.")
    return 0


def cmd_uninstall(a):
    if len(a) != 1:
        die("usage: b4bmod uninstall <name>", 2)
    n = a[0] if a[0].lower().endswith(".pak") else a[0] + ".pak"
    p = os.path.join(addons_dir(), os.path.basename(n))
    if not os.path.exists(p):
        die(f"{p}: not installed")
    try:
        os.remove(p)
    except PermissionError:
        die(f"can't remove {p}: close the game first")
    print(f"removed {p}")
    return 0


def cmd_check(a):
    t = a[0] if a else addons_dir()
    if os.path.isfile(t):
        return python_tool(os.path.join(KIT, "addon.py"), ["info", t])
    if not os.path.isdir(t):
        die(f"{t}: no such add-on or folder (nothing installed yet?)")
    return python_tool(os.path.join(KIT, "addon.py"), ["check", t])


def main(argv):
    a = [unmangle(x) for x in argv]
    for flag in ("--aes-key", "--game"):
        v = take(a, flag)
        if v:
            OPTS[flag] = v
    v = take(a, "--src")
    if v:
        OPTS["--src"] = v
    if not a or a[0] in ("-h", "--help", "help"):
        print(__doc__)
        return 0
    cmd, rest = a[0], a[1:]
    if cmd in ("setup", "status", "config", "find", "extract", "mesh", "install", "uninstall", "check"):
        return {"setup": cmd_setup, "status": cmd_status, "config": cmd_config, "find": cmd_find,
                "extract": cmd_extract, "mesh": cmd_mesh, "install": cmd_install, "uninstall": cmd_uninstall,
                "check": cmd_check}[cmd](rest)
    if cmd in ("survivor", "weapon"):
        return cmd_model(cmd, rest)
    if cmd == "convert":
        return cmd_convert(rest)
    if cmd == "model":   # the lower-level b4bmodel.py as is (e.g. `b4bmod model textures <manifest> ...`)
        help_ = not rest or rest[0] in ("-h", "--help", "help")
        return python_tool(os.path.join(KIT, "b4bmodel.py"), rest + ([] if help_ else ["--src", src_dir()]))
    if cmd == "pack":
        return python_tool(os.path.join(KIT, "addon.py"), ["pack"] + rest)
    if cmd == "pak":   # advanced: plain mod pak for the dev build's modpaks= (no addoninfo)
        return python_tool(os.path.join(KIT, "b4bpak.py"), ["pack"] + rest)
    if cmd in ("info", "tree", "export", "texture", "mi", "dt", "deps", "props", "texcheck", "rename"):
        return cmd_dotnet(cmd, rest)
    die(f"unknown command {cmd!r} (b4bmod help)", 2)


if __name__ == "__main__":
    try:
        sys.exit(main(sys.argv[1:]))
    except KeyboardInterrupt:
        sys.exit(130)
