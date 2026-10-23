# b4bcoop mod maker's kit (b4bmod)

Make add-ons for Back 4 Blood with the [b4bcoop](https://github.com/actuallydan/b4b-coop) mod: new textures and
material colours, your own models (FBX, glTF/glb, VRM, OBJ, DAE or .blend) for survivors and weapons, packed as one
add-on file that players drop into their game folder (or get from the add-on shop in the game's `~` window). No
Unreal editor needed.

This kit is **for mod makers only**. Players don't need it: they install the b4bcoop mod and the add-ons you share.

The kit is scripts and source code only. It contains no game files and no third-party programs. `b4bmod setup`
downloads the open-source pieces from their official sources; the rest you get yourself, as described below.

- [Install (Windows)](#install-windows) · [Install (Linux / Steam Deck)](#install-linux--steam-deck)
- [What you need, and where each piece comes from](#what-you-need-and-where-each-piece-comes-from)
- [The AES key](#the-aes-key)
- [From an FBX to an add-on](#from-an-fbx-to-an-add-on) · [Commands](#commands)
- [Where files go](#where-files-go) · [Troubleshooting](#troubleshooting) · [Tested on](#tested-on)

Guides: [docs/textures.md](docs/textures.md) (textures, materials) · [docs/meshes.md](docs/meshes.md) (models,
Blender) · [docs/addons.md](docs/addons.md) (packing, testing, sharing).

## Install (Windows)
1. **Python 3.10 or newer** from [python.org](https://www.python.org/downloads/windows/) (the installer, not the
   Microsoft Store version). In the installer, tick **Add python.exe to PATH**.
2. Extract this zip to a folder you can write to, e.g. `C:\b4bmod` (not inside `Program Files`).
3. Open a command prompt in that folder: in File Explorer, click the address bar, type `cmd`, press Enter.
4. Run:
   ```
   b4bmod setup
   ```
   It downloads the .NET 10 SDK (if you don't have it) and UAssetAPI into the kit's `deps` folder, builds the tools,
   and lists what is still missing. Nothing is installed system-wide and no admin rights are needed. Takes a few
   minutes the first time (about 250 MB).
5. `b4bmod status` should end with `ready.`

b4bmod finds Back 4 Blood in your Steam libraries. If not: `b4bmod config game "D:\SteamLibrary\steamapps\common\Back 4 Blood"`
(the folder with `Back4Blood.exe` and `Gobi` in it).

Use the Command Prompt or PowerShell (in PowerShell type `.\b4bmod.cmd` instead of `b4bmod`). Put paths with spaces and
search patterns in double quotes.

## Install (Linux / Steam Deck)
Python 3.10+ (preinstalled on most distributions and SteamOS), `bash` and `curl` (for Microsoft's .NET install
script). Extract the zip, then in a terminal in that folder:
```
./b4bmod.sh setup
./b4bmod.sh status
```
The game is found in `~/.local/share/Steam`, `~/.steam/steam` and Flatpak Steam, including extra Steam libraries.
Everywhere in the guides, `b4bmod` means `./b4bmod.sh` on Linux.

## What you need, and where each piece comes from
| Piece | What for | License | How you get it |
|---|---|---|---|
| Python 3.10+ | runs b4bmod | PSF | python.org (Windows), your distribution (Linux) |
| .NET 10 SDK | builds and runs the two C# tools (`dotnet\b4bmod`: textures, materials; `dotnet\pakx`: reading the game's paks) | MIT (Microsoft) | `b4bmod setup` uses yours if `dotnet --list-sdks` shows a 10.x SDK; otherwise it runs Microsoft's official install script (`https://dot.net/v1/dotnet-install.ps1` / `.sh`) into `deps\dotnet`. Manual: `winget install Microsoft.DotNet.SDK.10` or [dotnet.microsoft.com](https://dotnet.microsoft.com/download/dotnet/10.0) |
| UAssetAPI | reads and writes the game's cooked asset files | MIT | `b4bmod setup` downloads the tested version (commit `3228c1e`) from GitHub, checks its SHA-256, and unpacks it to `deps\UAssetAPI`. Manual: download `https://github.com/atenfyr/UAssetAPI/archive/3228c1e86261aa08131f7ec0ff1a395f5d0b2a84.zip` and extract it so that `deps\UAssetAPI\UAssetAPI\UAssetAPI.csproj` exists |
| CUE4Parse | reads the game's `.pak` files (the same library FModel uses) | Apache-2.0 | NuGet package `CUE4Parse` 1.2.2.202609: `dotnet` downloads it on the first build, into your NuGet cache (`%USERPROFILE%\.nuget\packages`). It brings **OodleSharp** (MIT), an open-source Oodle decoder |
| BCnEncoder.Net, StbImageSharp, StbImageWriteSharp | texture compression, PNG read/write | MIT / Unlicense | NuGet, same as above |
| Oodle | the game's files are Oodle-compressed | | **Not needed**: CUE4Parse decompresses with OodleSharp (open source, above). The kit uses no proprietary Oodle library |
| The AES key | the game's pak index is encrypted | (a number) | built into b4bmod: [The AES key](#the-aes-key) |
| Blender 4.2+ (tested 5.1) | models: fitting your model onto the game's meshes, FBX/OBJ/.blend import (b4bmod runs it in the background) | GPL | [blender.org/download](https://www.blender.org/download/) (Windows installer; Linux tarball or your distribution). Only for models. b4bmod finds it on PATH or in `C:\Program Files\Blender Foundation\`; else `b4bmod config blender "<path to blender.exe>"`. **No add-ons or extensions** needed (FBX/glTF/OBJ are built in). MPFB/MakeHuman is not needed: it only made our test character. DAE needs Blender 4.x |
| Back 4 Blood (Steam) | the game files everything starts from | | read only; b4bmod writes into the game folder only for `install` / `uninstall` (`b4bcoop-addons`) |
| The b4bcoop mod | loads add-ons in the game | | the player zip from the b4bcoop releases, to test your add-on |

## The AES key
The file list inside Back 4 Blood's `.pak` files is encrypted with AES-256. The key is the same for every copy of the
game (it is built into the game; one key per game build), and it is publicly known: UE modders collect such keys in
community AES key lists for FModel and UModel. b4bmod has it built in:
```
0x0208250257E8EA16828509DEBF23D703A5B509FE4F15F33F11BEE4BAB1F97CFD
```
`b4bmod status` decrypts every pak index with it and compares the result with the checksum stored in the pak. If a game
update ever changes the key, status says `WRONG`; then set the new one with `b4bmod config aes_key 0x<64 hex digits>`,
the environment variable `B4B_AES_KEY`, or `--aes-key <key>` on any command.

## From an FBX to an add-on
A survivor outfit (details and a weapon: [docs/meshes.md](docs/meshes.md)):
```
b4bmod find "Heroes/Mom/Meshes/Elite/.*_SKM$"                                          1. pick the outfit to replace
b4bmod mesh info /Game/TU11/Characters/Heroes/Mom/Meshes/Elite/Elite_04/3P_Mom_Elite_04_SKM         2. its slots
b4bmod survivor mymodel.fbx --outfit /Game/TU11/Characters/Heroes/Mom/Meshes/Elite/Elite_04/3P_Mom_Elite_04_SKM
    -o mymod --title "My survivor" --zip --install                                                          3.
```
(one command on one line.) Step 3 extracts what it needs from the game, fits your model (FBX, glTF, VRM, OBJ ...;
rigged with Mixamo, Rigify, VRoid, UE4 names ... or not rigged) onto the game's skeleton in Blender, puts your
materials on the outfit's slots (printed; `--slot <material>=<slot>` to choose), makes the first-person arms from the
outfit's own `FP_` mesh (`--fp none`: the game's arms), the LODs (a model denser than the survivor's is reduced to its
budget; `--keep-density`) and the textures,
and writes `mymod.pak` (the add-on), `mymod.zip` (to share) and installs it. Start the game and wear the outfit. Weapons: `b4bmod weapon mygun.fbx --fp-mesh ...`.
Add `--as <name>` to add an outfit instead of replacing Mom's (any survivor can wear it: **Wear** in the `~` window's
Add-ons tab, the game's customization screen, or `/model <name>`).

## Commands
`b4bmod help` prints all of them. The main ones:

| Command | What it does |
|---|---|
| `setup`, `status`, `config [key [value]]` | get and check the tools; settings `game`, `blender`, `aes_key` (built in; override only) |
| `find "<regex>"` | search the game's asset paths (`/Game/...`) |
| `extract <asset or "folder/*">` | copy game files out of the paks (the other commands do this for what they need) |
| `info`, `tree <asset>` | what an asset is; mesh → materials → textures |
| `dt <asset> [<row>] [--names]` | a data table's rows and fields (cards, difficulties, spawn weights); `--names` lists row names only |
| `export <texture> <out.png>`, `texture <texture> <in.png> -o <moddir>` | texture to PNG and back |
| `mi <material instance> [set <param> <value>...] -o <moddir>` | material parameters |
| `rename <asset> </Game/new/path> -o <moddir> [--ref old=new]...` | a copy of an asset under a new path (textures, materials, meshes); experimental, see docs/textures.md |
| `survivor <model> --outfit ... [--fp ...\|none] -o <moddir>`, `weapon <model> --fp-mesh <FP mesh or code> -o <moddir>` | your model, fitted in Blender, to a survivor outfit or a weapon, packed (`--install` installs it) |
| `survivor ... --as <name>` | the same, as an **added** outfit any survivor can wear (customization screen, `~` window, `/model <name>`; nothing of the game replaced; docs/meshes.md "Add an outfit") |
| `convert <mod.pak> --as <name> [--title T]` | a survivor **replacement** mod (someone's pak that overrides an outfit) to an added outfit any survivor can wear; the pak is only read (docs/meshes.md "Turn a replacement mod into an outfit") |
| `mesh info / export / import / edit` | skeletal meshes: slots and LODs; to glTF; your own fitted FBX/glTF back; quick edits |
| `pack <moddir> -o <name>.pak [--title ...] [--zip]` | the add-on |
| `install <pak>`, `uninstall <name>`, `check [<pak>]` | into / out of `<game>\b4bcoop-addons`; what the game will load |

`<asset>` is a path from `find`, like `/Game/Characters/Heroes/Walker/Meshes/Elite/Elite_00/3P_Walker_Elite_00_SKM`.
`<moddir>` is your mod's working folder (any name); its `Gobi\Content\...` part is what `pack` puts in the add-on.

## Where files go
- The kit folder: the scripts, `deps\` (.NET, UAssetAPI) and the tool builds (`dotnet\*\bin`).
- Your data, Windows `%LOCALAPPDATA%\b4b-coop\`, Linux `~/.local/share/b4b-coop/` (`B4B_MODKIT_DATA` to move it):
  `b4bmod.ini` (settings), `extract\` (game files you extracted: a survivor's folder is 0.3-2 GB), `listing\`
  (the `find` index), `tmp\`.
- Your mod folders and add-ons: wherever you run the commands (use `-o`).
- Extracted game files are for making your mod on your PC. Share only add-ons (and only the files you changed).

## Troubleshooting
- `'py' is not recognized` / `'python' is not recognized`: install Python from python.org with "Add python.exe to
  PATH", then open a new command prompt.
- `wrong AES key`: see [The AES key](#the-aes-key).
- `Back 4 Blood not found`: `b4bmod config game "<game folder>"`.
- `building b4bmod failed`: the lines above it are the compiler's; the first build needs internet access for NuGet.
  `b4bmod setup` rebuilds everything.
- Git Bash turns `/Game/...` into `C:/Program Files/Git/Game/...`; b4bmod undoes that, but the Command Prompt or
  PowerShell avoid it.
- `can't write ...: close the game first`: the game keeps its add-ons open while it runs.
- A survivor model comes out wrong (bones not found, stretched, wrong textures): [docs/meshes.md "Survivor
  troubleshooting"](docs/meshes.md#survivor-troubleshooting) explains each message.
- Your add-on doesn't show: the `~` window's Add-ons tab and `/addons` in the game's chat list it, `/addons` with the
  reason if it didn't load; `b4bmod check` shows the same offline. The b4bcoop mod must be installed.

## Tested on
- **Linux** (Arch, Python 3.14, .NET SDK 10.0.401, Blender 5.1): everything, from a fresh unzip of this kit
  (`setup` downloading .NET and UAssetAPI) through `find`, `extract`, `tree`, `export`, `texture`, `mi`, `mesh
  export/import` (FBX and glTF), `survivor` and `weapon` (FBX), `pack`, `install`, `check`. Extracted files are byte-identical to what the running
  game reads (801/801 files of a survivor).
- **Windows** (Windows 11, Python 3.14, Git Bash): `setup` (the PowerShell .NET install and both .NET tools), `status`, `find`, `extract`, `info`, `props`, `dt`. Earlier, under Wine with Windows Python 3.12: `b4bmod.cmd`, `status`,
  `config`, `mesh info/export/import`, `pack`, `install`, `check`, `uninstall` (Windows paths, `%LOCALAPPDATA%`);
  the add-on it packed is byte-identical to the Linux one. Not run on Windows: `setup` (PowerShell .NET install)
  and the two .NET tools (`find`, `extract`, `info`, `tree`, `export`, `texture`, `mi`), because .NET 10 doesn't
  start under Wine; they are plain .NET code with nothing Linux-specific. Reports welcome.

Unofficial; not affiliated with Turtle Rock Studios or Warner Bros. Games. The kit is MIT licensed (LICENSE); the
pieces it downloads keep their own licenses.
