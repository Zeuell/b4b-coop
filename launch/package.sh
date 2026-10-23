#!/usr/bin/env bash
# Build the player zip dist/b4bcoop-<version>.zip (VERSION) from the player build (native/build.sh --release: no
# command server, no test commands). Its layout mirrors the game folder, so installing = extracting it into the folder
# Steam's "Browse local files" opens (docs/investigations/launch.md):
#   xinput1_3.dll                         Windows: lets a normal Steam "Play" skip the EAC bootstrapper
#   Gobi/Binaries/Win64/X3DAudio1_7.dll   the mod (Windows and Linux/Steam Deck, no launch options)
#   Gobi/Binaries/Win64/b4bcoop.ini       settings (all optional: no ini = host by default, Steam joins only)
#   b4bcoop-README.txt, b4bcoop-COMMANDS.txt (docs/COMMANDS.md, CRLF), b4bcoop-LICENSE.txt
# Plus dist/SHA256SUMS and dist/b4bcoop-update.txt, the in-game updater's manifest (version, protocol, zip size and
# SHA-256; tools/sign-release.sh manifest), which release.yml signs with the release key (#34). The zip is
# reproducible. CI runs this on a v* tag (.github/workflows/release.yml).
# b4bcoop-README.txt mirrors README.md's player part (Install to Troubleshooting, Safety in short): keep them in sync.
set -euo pipefail
# Info-ZIP reads $ZIP and $ZIPOPT as default options; never let a caller's env change the archive.
unset ZIP ZIPOPT
root="$(cd "$(dirname "$0")/.." && pwd)"
version=$(sed -n 's/^version=//p' "$root/VERSION") protocol=$(sed -n 's/^protocol=//p' "$root/VERSION")
"$root/native/build.sh" --release >/dev/null
rel="$root/native/out/release"
dist="$root/dist"; out="$dist/b4bcoop"; zip="b4bcoop-$version.zip"
rm -rf "$out" "$dist/b4bcoop-legacy" "$dist"/b4bcoop*.zip "$dist"/*.sig "$dist/b4bcoop-update.txt"; mkdir -p "$out/Gobi/Binaries/Win64"
cp "$rel/xinput1_3.dll" "$out/"
cp "$rel/X3DAudio1_7.dll" "$out/Gobi/Binaries/Win64/"
cp "$root/LICENSE" "$out/b4bcoop-LICENSE.txt"
sed 's/$/\r/' "$root/docs/COMMANDS.md" > "$out/b4bcoop-COMMANDS.txt"
ini="$out/Gobi/Binaries/Win64/b4bcoop.ini"
cat > "$ini" <<'INI'
; b4bcoop settings. Everything here is optional: with no changes you host automatically in offline Fort Hope,
; and your Steam friends join you with "Join Game" in their Steam friends list.
; To turn a setting on, remove the ';' at the start of its line. Saved changes apply within a couple of
; seconds, also while you play (host, join, host_ip, netguard: restart the game).

; At the game start: ask "b4bcoop co-op or Online" (ask), or always start co-op (coop) or the online game (online).
; "Remember my choice" in that question writes this line. Hold Shift while the game starts to be asked anyway.
;launch=ask

; Co-op starts sign in Offline by themselves (no Sign in button, no Online/Offline question). 0 = the game's own.
;auto_signin=0

; Don't host: your offline game stays private.
;host=0

; Host only: allow 5 survivors (default 4).
;teamsize=5

; Flashlight toggle key (default L; off disables it).
;flashlight_key=L
; Your flashlight beam on your screen, percent of the game's (100 = unchanged): cone width, reach, brightness.
;flashlight_width=100
;flashlight_range=100
;flashlight_brightness=100

; Start in third person (/thirdperson).
;thirdperson=1
; Third-person toggle key (default N; off disables it).
;thirdperson_key=N

; Ask the bots to heal you (default H; off disables it). The host turns heal requests on (~ window, Cheats tab).
;bot_heal_key=H
; Host only: bots answer heal requests (default 0 = off) / a bot with a toolkit opens a pinged supply-stash door
; (default 1 = on). Both are in the ~ window's Cheats tab too.
;bot_heal_requests=1
;bot_toolkit_doors=0

; Host only, run rules (~ window, tab Run): burn cards are never used up in your game (nobody is charged).
;free_burncards=1

; Host only: who may join. Default: only your Steam friends. "anyone" also lets in people who aren't.
;allow_joins=friends
; Host only: always let these Steam IDs in (17-digit Steam IDs, comma-separated), friends or not.
;allow_steamids=7656119XXXXXXXXXX

; ADVANCED, most players never need this: host and join by IP address instead of through Steam.
; Needs port forwarding (UDP 7777), triggers the Windows Firewall prompt, and everyone in the game needs it.
; Then join=<host's IP> joins a host by address.
;host_ip=1

; Hide the ~ window's Updates tab (updates from the game, only when you click).
;updates=0

; Don't check GitHub for a newer b4bcoop at each game start (it installs one before the game opens).
;auto_update=0

; Turn the add-on shop off (its list is fetched from GitHub once per game start, in Fort Hope).
;shop=0

; Blocks all third-party network traffic (Epic/WB/Turtle Rock services) while you play. If something won't
; start or connect, try netguard=off and tell us.
;netguard=block
INI
# Private builds: B4B_INI_SET="auto_update=0;shop=0;launch=coop" turns those settings on in the shipped ini (the
# commented ";key=..." line becomes "key=value"; an unknown key is appended). Unset = the ini above, all commented.
if [[ -n ${B4B_INI_SET:-} ]]; then
  IFS=';' read -ra kvs <<< "$B4B_INI_SET"
  for kv in "${kvs[@]}"; do
    k=${kv%%=*}
    if grep -q "^;$k=" "$ini"; then sed -i "s|^;$k=.*|$kv|" "$ini"; else printf '\n%s\n' "$kv" >> "$ini"; fi
  done
fi
sed -i 's/$/\r/' "$ini"
cat > "$out/b4bcoop-README.txt" <<TXT
b4bcoop $version (protocol $protocol) - private Back 4 Blood co-op in offline mode, with your Steam friends.
No Warner Bros / Turtle Rock servers. Everyone who plays together needs the mod, and the same version.

This mod was made quickly for my group of friends, it is not something I intend to support with any degree of
dedication. Pull requests for features or bug fixes are welcome. If you find it useful and fun, consider starring
the project and sharing it with friends. Eventually running your own server might be the only way to play this game
cooperatively.

Two words used below: the HOST is the player whose game the others join (by default, whoever is in their own
Fort Hope); the ~ WINDOW is b4bcoop's own menu in the game: press the ~ key (left of 1).

Already have b4bcoop 0.7.0 or newer? You don't need to install again: update from inside the game (see UPDATE).

INSTALL (Windows, Linux and Steam Deck - the same steps)
1. Open the game folder: in Steam, right-click Back 4 Blood > Manage > Browse local files.
   It is the folder with Back4Blood.exe and a folder named Gobi in it. On Steam Deck, use Desktop Mode.
2. Extract EVERYTHING from this zip into that folder, and say yes if asked to merge. The zip's Gobi folder
   merges into the game's Gobi folder. No game file is replaced. You get:
     xinput1_3.dll                          next to Back4Blood.exe
     b4bcoop-README.txt, b4bcoop-COMMANDS.txt, b4bcoop-LICENSE.txt
     Gobi\Binaries\Win64\X3DAudio1_7.dll    the mod
     Gobi\Binaries\Win64\b4bcoop.ini        settings (optional)
3. That's all: no launch options, no scripts.

PLAY
1. Press Play in Steam. Steam itself must be online: friends connect through Steam.
2. b4bcoop asks how you want to play: pick "b4bcoop co-op" (picked for you after 15 seconds without input).
   "Online" = the official online game, see PLAY ONLINE. On Linux/Steam Deck the question appears from the second
   start on.
3. The game signs in Offline by itself and takes you to Fort Hope (no Sign in button, no Online/Offline question).
4. In Fort Hope you are hosting: your Steam friends can join you. Nothing to set up, no ports to open.
5. Start missions from the war table as usual. Everyone in your game follows you in.
INVITE A FRIEND: press ~, tab Session, and click Invite next to their name (or use Steam's own invite).
Only the host's Steam friends can join (allow_joins in OPTIONS changes that).
JOIN A FRIEND: in the Steam friends list, right-click your friend while they are in Back 4 Blood > Join Game,
or accept their invite, or press ~ > Session > Join next to their name. If your game isn't running, Steam starts
it and it joins them once it has signed in.
The ~ window has everything else: players (kick, ban, lock), joining and leaving, third-person camera, flashlight,
cheats, add-ons, settings and updates. The same things work as chat commands (/help in the game's chat).
The window, all chat commands and options, with examples: b4bcoop-COMMANDS.txt (next to this file).

PLAY ONLINE (the mod stays installed): press Play, pick "Online". b4bcoop moves its game file (X3DAudio1_7.dll) into
a b4bcoop-online folder next to Back4Blood.exe before Easy Anti-Cheat starts the game, so the online game runs
without any b4bcoop code; it moves back the next time you pick "b4bcoop co-op". Before each online start your
offline save is copied to PlayerProfileSettings-b4bcoop-before-online-<date>-<time>.sav next to it (5 kept).
"Remember my choice" skips the question from then on (launch= in b4bcoop.ini). To be asked again: hold Shift while
the game starts, or ~ window > Settings > Game start (in co-op), or launch=ask. Controller: A co-op, Y online,
X remember. A Steam Join Game always starts co-op. In co-op the game's own sign-in step is skipped (auto_signin=0
in b4bcoop.ini shows it; Online there still signs you in Offline).

OPTIONS (all optional): most are in the ~ window (saved for you). Or open Gobi\Binaries\Win64\b4bcoop.ini in a text
editor and remove the ';' in front of a line. Saved changes apply within a couple of seconds, also while you play
(host, join, host_ip: restart the game).
  host=0             don't host; your offline game stays private
  launch=coop        no question at the game start, always co-op (launch=online: always online; launch=ask: ask)
  teamsize=5         (host) 5 survivors instead of 4
  flashlight_key=L   the flashlight key; thirdperson_key=N the third-person camera; bot_heal_key=H ask the bots
                     to heal you (off turns a key off)
  thirdperson=1      start in third person
  allow_joins=anyone (host) also let in people who aren't your Steam friends;
                     or allow_steamids=<17-digit Steam ID> for one person
  host_ip=1          ADVANCED: host and join by IP address instead of through Steam. Needs port
                     forwarding (UDP 7777) and triggers the Windows Firewall prompt.
  More options and details: b4bcoop-COMMANDS.txt.

ADD-ONS (new outfits, weapon looks, textures): press ~, tab Add-ons. It lists the free add-ons from the add-on shop
and the ones you have: Add downloads one (most are ready at once), Wear / Take off an outfit (on your survivor at once,
kept for that survivor), Use / Stop a weapon look, Update, Remove. Or put an add-on's .pak file in a
b4bcoop-addons folder next to Back4Blood.exe and restart the game. Only you see your add-ons, and nobody learns which
ones you have; friends with the same add-on see your outfit, the others your normal survivor. By default hosts let in
players with any add-ons; a host can allow cosmetic ones only, or none (addons_policy=; your game checks yours before
joining). Details: b4bcoop-COMMANDS.txt, "Add-ons".

UPDATE: by itself: b4bcoop checks GitHub for an update at each start and installs it before the game opens (a
small window shows it; Skip update plays the version you have; auto_update=0 turns it off). Joining a friend through
Steam installs your friend's version. In game: press ~, tab Updates > Check for updates > Download and install on
next start, then restart the game. Every download must carry the b4bcoop release signature, and your b4bcoop.ini,
add-ons and bans stay as they are. "Go back to ..." in the same tab returns to the version you had. Or by hand:
extract the new zip over the old files. Details: b4bcoop-COMMANDS.txt, "Updates".

REMOVE: delete these files from the game folder.
1. Next to Back4Blood.exe: xinput1_3.dll, b4bcoop-README.txt, b4bcoop-COMMANDS.txt, b4bcoop-LICENSE.txt, and
   the b4bcoop-addons folder (only there if you installed add-ons), the b4bcoop-update folder (only there after an
   update), the b4bcoop-online folder (only there while b4bcoop is switched off for online play).
2. In Gobi\Binaries\Win64: X3DAudio1_7.dll, b4bcoop.ini, b4bcoop-outfits.txt (only there if you wore an add-on
   outfit), all b4bcoop-*.log files, and b4bcoop-bans.txt (only there if you banned someone).
3. Left over from older versions, if present, in Gobi\Binaries\Win64: dwmapi.dll, "Play B4B co-op.cmd",
   steam_appid.txt. On Linux also remove the launch option WINEDLLOVERRIDES="dwmapi=n,b" %command%
Steam's "Verify integrity of game files" does NOT remove these: they are extra files, not game files.
Your offline progress stays either way.

Making your own add-ons: see the modkit (b4bcoop-modkit-<version>.zip on the b4bcoop Releases page).

TROUBLESHOOTING
- "Everyone needs the same version": someone has another b4bcoop version. Press ~, tab Updates: it
  offers the host's version (or the latest). Or everyone downloads the latest release and extracts it again.
- Play online / with Easy Anti-Cheat without removing the mod: pick "Online" when the game starts (PLAY ONLINE).
  The launch option  -b4bcoop=off  does the same without asking.
- b4bcoop gone after playing online: pick "b4bcoop co-op" at the next start (hold Shift if Online is your remembered
  choice). By hand: move the Gobi folder from b4bcoop-online back into the game folder, or extract the zip again.
- The log: Gobi\Binaries\Win64\b4bcoop-<date>-<time>-<number>.log (the newest one; the last 20 are kept). Its first
  lines show the version. No new log after starting the game = the mod didn't load.
- Windows: this install layout hasn't been tested on a Windows PC yet (it has on Linux / Proton). If the
  mod doesn't load there, please open an issue on GitHub and attach Gobi\Binaries\Win64\b4bcoop-launcher.log
  if that file exists.
- Join Game missing or not working: both need the mod (same version), Steam online, and the host in
  Fort Hope or a mission. Fallback: the host looks up
  their 17-digit Steam ID (Steam > click your account name at the top right > Account details, or ~ > Session >
  Copy), and you type  /join steam:<that number>  in the game's chat while in your own Fort Hope.

SAFETY
Unofficial, not affiliated with Turtle Rock Studios or Warner Bros. Games. Offline mode only: it blocks the game's
online services while it runs, and is moved out of the game's folders before an online start (PLAY ONLINE). By
default nothing is opened to the network: players connect through Steam. b4bcoop itself only talks to Steam and
GitHub (the update check at each start, auto_update=0 turns it off; the add-on shop's list; updates). No warranty
(see b4bcoop-LICENSE.txt).
Source code and how to check this download: https://github.com/actuallydan/b4b-coop
TXT
sed -i 's/$/\r/' "$out/b4bcoop-README.txt"

# Reproducible zip: fixed timestamps and file order, no extra attributes.
find "$out" -type d -exec chmod 755 {} + ; find "$out" -type f -exec chmod 644 {} +
find "$out" -exec touch -d '2020-01-01 00:00:00 UTC' {} +
if command -v zip >/dev/null; then
  (cd "$out" && find . -mindepth 1 | sed 's|^\./||' | LC_ALL=C sort | TZ=UTC zip -qX -@ "$dist/$zip")
else   # Git Bash on Windows has no Info-ZIP: same entries, order and timestamps from Python (tools/zipdir.py)
  (cd "$out" && find . -mindepth 1 | sed 's|^\./||' | LC_ALL=C sort | python3 "$root/tools/zipdir.py" "$dist/$zip")
fi
(cd "$dist" && sha256sum "$zip" b4bcoop/xinput1_3.dll b4bcoop/Gobi/Binaries/Win64/X3DAudio1_7.dll > SHA256SUMS)
"$root/tools/sign-release.sh" manifest "$dist/$zip" "$version" "$protocol" > "$dist/b4bcoop-update.txt"
echo "$dist/$zip"
