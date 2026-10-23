# b4b-coop

Unofficial private co-op for Back 4 Blood (up to 4 players), no WB/Turtle Rock services. Approach: take the
game's **offline mode** (full local progression in `PlayerProfileSettings.json`) and turn the offline session
into a **listen server** others join — Seamless Co-op style. An injected agent DLL does the engine work.
Player defaults (0.3.0): every offline Fort Hope hosts, friends join through Steam "Join Game" over Steam P2P only,
the game's UDP socket is bound to 127.0.0.1 (nothing reachable from the network); `host_ip=1` is the advanced IP
opt-in. Player docs: README.md (top) and its copy in `launch/package.sh` (`b4bcoop-README.txt`); every chat command
and ini key: `docs/COMMANDS.md` (shipped as `b4bcoop-COMMANDS.txt`; update it with any player-facing change).

Detailed engine findings (addresses, obfuscated layouts, class names): `docs/NOTES.md`. Read it before touching
`native/` or `tools/`.

## Layout
- `native/` — agent DLL (C, zig cc + MinHook), built as a proxy of `X3DAudio1_7.dll` (loads from the game dir on Proton
  and Windows with no launch options) and of `dwmapi.dll` (legacy, dev only: `launch/install.sh --legacy`, not shipped). Proxies are generated into `native/proxy/` by
  `tools/gen-proxy.py`. `native/launcher/redirect.c` → `xinput1_3.dll` for the game root: the Steam launcher stub
  loads it (Windows by itself; Proton through the `AppDefaults\Back4Blood.exe` DllOverride online.c sets) and it asks
  **b4bcoop co-op / Online** (#47; task dialog, `launcher.rc` manifest; co-op after 15 s with no input; remembered as ini `launch=`; Shift or
  `-b4bcoop=ask` asks anyway; `+b4bcoop_join` = co-op): co-op starts the game instead of the EAC bootstrapper,
  Online moves the agent DLLs to `<game>\b4bcoop-online\` first and lets the stub start EAC unchanged; the next co-op
  pick moves them back. Co-op starts then **update b4bcoop before the game starts** (`auto_update()`, never on the
  Online path): GitHub `releases/latest` (or a Steam join's `ver:` = the host's version, `upd_auto_plan`), 2 s for
  the check, nothing shown when offline/no answer/nothing newer; a newer release in a task dialog with progress +
  Skip update, verified and installed with the tab's code (`updinst.c`), its own mapped DLL renamed into the backup;
  versions put back (`state.txt skip=`, `failed\`) not again by itself; result in the launcher log +
  `b4bcoop-update\auto.txt`. Ini `auto_update` (player builds default 1, dev builds 0). Dev: env
  `B4B_LAUNCHER_ANSWER=coop|online[+remember]|cancel|wait`, `B4B_LAUNCHER_UPDATE=off|on|cancel`, ini
  `update_api/update_repo/update_pubkey` (as the agent), `-b4bcoop_test_suspend` (EAC's launcher created suspended),
  `rundll32 xinput1_3.dll,b4bcoop_prompt_test`, `rundll32 xinput1_3.dll,b4bcoop_update_test <root>[|<options>]`
  (the update alone, no stub/game); `B4B_STUB=1 launch/run.sh` runs the stub. docs/investigations/launch.md,
  online-mode.md, updater.md §7.
  `native/build.sh` → `native/out/` (dev build); `native/build.sh --release` → `native/out/release/` (player build,
  defines `B4B_RELEASE`: no TCP command server, no dev/test commands, no `offline=1` automation; what
  `launch/package.sh` ships); both produce all three DLLs and generate `native/out/gen/b4bcoop_version.h` from
  `VERSION` (see "Versioning"). Dev-only code is under `#ifndef B4B_RELEASE` (whole
  `testing.c`, the command server, every CLI-only `*_cmd` handler). Player builds are verified from their log
  (`b4bcoop loaded (player build) as X3DAudio1_7.dll`), not with `tools/b4b.py`.
  - `ue.c/h` reflection layer for B4B's modified UE 4.25 (XOR'd GUObjectArray, shuffled FField, UObject +8).
  - `main.c` Tick hook; logs `b4bcoop <version> (protocol N)` at load; `-b4bcoop=off` on the command line = the launcher
    kept the agent out; finding itself loaded anyway it closes the game (online.c). Dev builds: game-thread command
    queue + TCP command server (127.0.0.1:47112, first free of +0..7; `B4B_COOP_PORT` pins it).
  - `online.c` (#47) the agent's side of the launch choice: on Wine sets the launcher's DllOverride (DllMain);
    `-b4bcoop=off/online` with the agent loaded closes the game before it runs; the game's sign-in popup answer
    Online becomes Offline (hook on SignInTask_OnlineOfflinePopup::OnPopupClosed 0x141B43CA0); `~` Settings "Game
    start" (ini `launch`). Dev `signin online` (refused unless that hook is on). docs/investigations/online-mode.md.
  - `travel.c` SetClientTravel hook: host's absolute travel → `servertravel ...?listen` (and no HostStartedSoloGame kick
    while hosting); client follow/rejoin; drops a failed join's `?closed` (no own-camp reload). join-timing.md.
  - `netguard.c` outbound-traffic guard from DllMain (DNS/WinHTTP/TCP allowlist, EOS network off; `netguard`
    command, `netguard=` ini keys); docs/investigations/outbound-traffic.md.
  - `uelog.c` captures UE_LOG into `Gobi/Binaries/Win64/b4bcoop-<yyyymmdd>-<hhmmss>-<winpid>.log` (last 20 kept: Wine reuses PIDs; tests `b4bcoop-<B4B_COOP_TAG>-<winpid>.log`).
  - `cards.c` host card-ownership override for remote players (interim).
  - `flashlight.c` manual flashlight toggle (`flashlight` command, `flashlight list`, ini hotkey); verified live.
    docs/investigations/flashlight.md.
  - `rewards.c` host forwards remote players' dropped rewards (SP, STP, unlocks, consumables) to their clients via the
    game's unused ClientExecute*Command RPCs. Details: `docs/investigations/client-rewards.md`.
  - `rewardguard.c` client: validates those RPCs before they reach the offline save (SP/STP bounds, burn-card -1 only
    for a card this client played this map, duffel-bag products only); rejects logged `rewardguard: REJECTED`. Dev:
    `rewardguard [products|difficulty|product <row>]`, host `rewardtest sp|stp|cons|unlock|slb ...`.
    client-rewards.md §8.
  - `burncards.c` host: remote players can play burn cards (quantity trusted), and each charge is keyed to the player
    who played it. Details: `docs/investigations/burn-cards.md`.
  - `signin.c` auto sign-in Offline (press Sign in; online.c answers the Online/Offline question before its popup
    exists, hook on the task's start 0x141B43960): on by default (every agent start is a co-op start, #47), ini
    `auto_signin=0` (dev alias `offline=0`) shows the game's own; a Steam join arms it then (presence.c).
  - `testing.c` (dev builds only) unattended testing: `signin`, `mission [raw] [map] [difficulty]`,
    `ready [vote]`, `endmission [1|0]`, `burncard list|status|charge|map|[row]`, `callp <Class> <Func> [args]`,
    `takeover <slot>` (finish a hot-join bot take-over), `tp volumes|<slot> <x y z>|<slot> volume <n>`, rewards Easy
    never gives: `stp <N>` (forces the skull-totem count for the next `endmission 1`), `items` / `giveitem <slot> <#>`
    (hand a pickup, e.g. a duffel bag, to a hero; `giveitem <slot> row <DataTable> <Row>` any item row, e.g. a weapon),
    `duffelreward <slot> <product guid> [delta]`, `face` (heroes with mesh, position, `(you)`; custom heads' face bones, `face comm` speaks, `face look`
    frames a face, `face walk` makes a bot run somewhere), `fnprobe <va>` (count
    a native function's calls/return values, for investigations).
  - `paks.c` (model mods #23) the engine's pak layer. Both builds: mounts our unsigned paks right after the retail
    ones, exempted by identity from the three signature paths (player builds without add-ons hook only Initialize,
    to remember the pak layer for the shop, or nothing with `shop=0`; the exemptions come with the first mount).
    `paks_mount_runtime` mounts later on the game thread (shop), `paks_file_exists` asks the pak layer.
    Dev only: `dumpassets <glob> [outdir]` extracts files as the engine reads them (IterateDirectory + OpenRead on
    FPakPlatformFile), `paks`, `mountpak <path> [order]`, ini `modpaks=<windows dir>` (raw paks, order 3000+). Pak
    writer: `modkit/b4bpak.py` (`tools/b4bpak.py` forwards). docs/investigations/model-mods-paks.md.
  - `addons.c` add-ons (#20): `<game>\b4bcoop-addons\*.pak` (ini `addons_dir=`, `addons=0`), read in DllMain:
    `addonlist.txt` on/off + load order (later wins, read order 1000+; edited by hand or `/addons on|off`, no order UI),
    embedded `b4bcoop-addoninfo.txt`, index SHA1 checked, conflicts (same file; "mixed" = one package from two add-ons)
    logged + one chat notice; chat `/addons [on|off|info <#>]` (off/order: next start; on: loaded at once when safe);
    rows of the `~` Add-ons tab (`addons_info`, `addons_look_buttons`: Wear / Take off = `models_wear` / Use / Stop;
    addons.md §8). Packer `modkit/addon.py pack|info|check`. `addons_mp.c` (#22, private
    #35): nothing about a player's add-ons is sent; the host announces `addons_policy` (presence ` addons:<policy>`,
    else a login refusal ending `[addons:<policy>]`), the joiner checks itself, declines locally or logs in with
    `?b4bcoopaddonsok=<policy>`; older clients' `?b4bcoopaddons=` summary still judged, never logged (`log_redact_addons`).
    Client-side only, no protocol bump. docs/investigations/addons.md. Loading without a restart (shop.md §1):
    `addons_add_runtime` (the shop's Add: appended to addonlist.txt) / `addons_load_now` (also `/addons on`, Turn on) /
    `addons_update_runtime` (Update, mounts `.shop\<id>.pak.new` above all add-ons): cosmetic only; as a client
    only within the host's announced policy (`addons_session_allows`); new paths always; replaced files only whole
    packages (.uasset/.uexp/.ubulk/.uptnl) that are not in memory (`paks_package_loaded`, GUObjectArray) while the
    engine's `IsAsyncLoading` (`paks_async_loading`) is idle (busy: the shop asks again for 15 s), never another add-on's
    files above it in the load order (owner table `own_*`); else next start. Then `models_outfits_refresh` (rows on the
    customization screen next frame) / `wlooks_refresh` (updated entries too).
  - `looks.c` the `~` **Add-ons** tab (2026-10-13; replaced the Looks & add-ons tab's Wear / Installed / Get more modes,
    which had replaced the Models, Add-ons and Browse tabs): one table, the installed add-ons (addons.c, hand-dropped
    paks too) and the shop's list (shop.c; same content = one row): thumbnail | name + size | installed state | Add,
    Update, Undo, Turn on, Wear / Use / Stop, Remove; installed first, sort name / time installed both ways, search,
    filter All / Installed / Available; no load order, no on/off, no details; Host rules (`models_host_panel`,
    `addons_mp_panel`) at the bottom; thumbnails 6 em, the large picture on hover (`shop_preview`), the table fills the
    tab (`ov_table_begin_scroll`) in a window of its own size (`overlay_panel_size`). Dev `overlay tab Models|Browse|Get more|Wear|Looks|Installed` = this tab
    (`overlay_add_alias`). docs/investigations/overlay.md "Add-ons tab".
  - `shop.c` the add-on shop (#36): its list is fetched by itself once per game start on the first arrival in Fort
    Hope (`auto_fetch`; host or client), Refresh at will (ini `shop=0`: no list, no downloads): `catalog.json` + `.sig`
    (ed25519, its own shop key, never the release key: `signkeys.h`; `upd_catalog_*` in updcore.c) from the public
    shop repo `actuallydan/back4blood-shop` (its workflow builds + signs the list; raw.githubusercontent.com, inside
    netguard's updater scope), thumbnails when the tab first shows the list (SHA-256 from the signed list, then
    stb_image in `imgdecode.c`, `ov_texture`), Add = pak streamed to `b4bcoop-addons\.shop\<id>.pak.part`, size + SHA-256
    checked, renamed to `<id>.pak`, `addons_add_runtime`; Remove (any add-on file)/Update via `.shop\pending.txt`,
    applied by `shop_early` (DllMain, from addons_scan); an Update is also loaded at once when nothing of the old
    version is in memory (`addons_update_runtime`). Rows for looks.c: `shop_row`. Nothing about installed add-ons is
    sent. Dev: ini `shop_catalog=`, `shop_pubkey=`; `shop status|list|fetch|add|remove|undo <id>`. Catalog tool
    `tools/shop-catalog.py build|sign|verify`. docs/investigations/shop.md.
  - `teamsize.c` opt-in 5+ player team (`teamsize=N` ini/command, raises `Config.TeamSize` before InitSlots; `slots`
    dumps the slot layout). docs/investigations/five-players.md.
  - `lineup.c` post-round/pre-round/character-select lineup with 5+ heroes (#8): spawns an extra mannequin when the
    hero team has more slots than the lineup level's 4 and places it in the back row; `lineup` dumps it.
    five-players.md §6. Also the mission-start player list (#28): adds PlayerLoadoutsEntry rows to its 4-row box
    before OnSlotsUpdated (five-players.md §7).
  - `hud.c` teammate lists with 5+ heroes, render transforms only (from the widgets' cached geometry, every frame;
    widgets found once a second; nothing runs with 4 heroes): the HUD's bottom-left list (`GameHUDScreen_WBP.
    PartyStatus`) stays one column, moved up and scaled (floor 0.6) to fit between the objectives' real bottom
    (`HUDMissions`) and your own panel; the character select's player list (`PlayerLoadouts`) puts rows 5-8 in a second
    column. Dev `hud [floor|margin|gap <v>]`, `hudtree <class|path suffix> [depth]` (any widget tree with pixel rects,
    slots, transforms), `hudxf`. five-players.md §8.
  - `dialogue.c` client: one cutscene radio line per cue (the host's replicated pick and the client's own level pick
    of `PlayClientCinematicVO` doubled lines/subtitles; the second within 4 s is dropped). Dev `subprobe` (testing.c)
    logs every HUD subtitle with its call stack. model-swap.md #37 "Doubled subtitle".
  - `fxsig.c` (#46) both roles: a particle component still in the significance manager but no longer active (a client
    gets the listen host's replicated `bIsActive=0` without the deactivation broadcast) is unregistered before
    `ResetParticles`; before that, GC freed it and the next significance ParallelFor crashed the client (Titan Tunnels
    finale). Dev `fxsig [fix|list|guard on|off|kill|gc]`. docs/investigations/particle-significance.md.
  - `supplylines.c` (#48) client: the game's supply-line manager (UCaravanManager, owner of the list the supply-line
    screen shows; replaces bought-out/expired lines, next static line, new dynamic ones) is turned on only by the
    "profile loaded" event, which a client's profile often fires before the manager exists; off for 3 s on a network
    client with the profile loaded → the agent calls the game's handler (0x141CB65F0) itself. Local, no protocol
    bump; env `B4BCOOP_NO_SUPPLYLINES=1` = off. Dev `supplylines [fix|off|update|auto on|off|buy <line#> [item#|all]|
    sp <N>]`. docs/investigations/supply-lines.md.
  - `bots.c` host: bots open pinged toolkit (supply-stash) doors. Retail bots answer only an actor ping of a door
    inside their ~12 m tether, and a ping blocked by a teammate becomes a location ping they ignore; the host turns a
    location ping aimed at a closed toolkit door into the door's ping and sends the nearest bot with the toolkit (40 m),
    lifting its tether checks for that door (hooks on ServerSpawnPing_Implementation 0x141E887C0, tether 0x1415E9AB0 /
    0x1415E96E0, ClearAllTargets 0x1415B5D50). Ini `bot_toolkit_doors=0`. Dev `botdoor ...`. docs/investigations/bot-pings.md.
    Heal requests (any machine: key `bot_heal_key`, default H; host decides, ini `bot_heal_requests=1`, off by default, `~` Cheats tab):
    the nearest bot with an Aid_Heal item usable on a teammate gets a job: its item-use cooldowns removed (120 s after
    any successful use), thresholds 1.0 + score +10 for the requester (hooks on the item-use score 0x1415D6030 / target
    eval 0x1415D67D0), saferoom rule lifted, tether lifted; next bot after 4 s untaken / 8 s + 0.5 s/m unhealed. A
    client's request = ServerSpawnCommWheelPing(GroupUp) with Scale3D (1,1,0.875) (hook 0x141E88840; older host shows
    "Group up"; no protocol bump). Dev `botheal ...`. bot-pings.md "Heal requests".
  - `handoff.c` host hand-off (protocol 3): the host sends each client a silent `handoff plan host|join ...` line every
    second (next host = the one named with `/handoff` / `~` Players **Make host**, else the first other player;
    `steam:<id64>`, or `127.0.0.1:<port+1>` for local copies; none with `host_ip=1`) and in a mission the campaign
    run record (JSON in chunks, or `handoff newrun` in chapter 1). Host quit (RequestExit hook) or a lost
    established connection → the next host hosts its own Fort Hope, waits for the others (60 s), writes the run into
    its profile (0x141BC1780) and resumes it (`Matchmaking::JoinRun` 0x141AE9240): chapter restarts, run kept.
    Ini `host_handoff=0` = no automatic take-over. Dev `handoff [runs|resume|go]`. docs/investigations/host-handoff.md.
  - `slotguard.c` host: a joiner with no free survivor slot gets "Server full." at login (bots' slots count as free),
    a slotless player is kicked instead of spawned (was a host crash, #7); `slotguard` command.
    docs/investigations/slot-guard.md.
  - `overlay.cpp` the `~` power-user window (#26): Dear ImGui over the game's D3D12 swap chain (frame built on the game
    thread, rendered from a snapshot on Present), game input and hotkeys blocked while open; ini `overlay=0`,
    `overlay_key`, `overlay_scale`. Panels: `overlay.h` (`overlay_add_panel` from a module's init + C `ov_*` widgets;
    actions `ov_run` = the chat path `admin_slash`, settings `ov_setting` = ini live handler + writer; `ov_begin_perm`
    greys out host/cheat controls). Tabs live next to their module (presence.c Session, admin.c Players,
    thirdperson.c Camera, flashlight.c Flashlight, cheats.c Cheats, looks.c Add-ons). Dev `overlay
    open|close|status|tab|press|set|locate|mouse|wheel|log`, `screenshot <path>` (presented frame as PNG,
    launch/shot.sh); e2e has a smoke check. docs/investigations/overlay.md (parity checklist). **It replaces the chat
    commands:** port every chat command into it; once it has them all, new features get overlay controls (+ ini keys)
    only, not new chat commands. Don't remove existing chat commands (keep `/say`).
  - `chat.c` in-game chat commands: hooks the local player's Say/SayTeam, `/cmd` is run locally and never sent;
    replies as local chat lines; host notices via ClientTeamMessage with our own type (a third type `b4bcoopdata`:
    silent data lines for the client's agent, `admin_data_to`). Test: `type <text>` (real key
    presses), `key <vk>` (one key press, `key 0x1B` = Esc), `click <x> <y>` (mouse click, e.g. post-round Continue), `chat status`, `popup [close]` (dev builds).
    `admin.c` the commands (`/help join host leave players ping kick ban lock bots restart say ready ...`, same verbs
    on the dev CLI) and the host's PreLogin gate (join policy first, then the b4bcoop protocol (`?b4bcoop=` login
    option; mismatch → "Host runs b4bcoop X (protocol N); you have Y (protocol M). Everyone needs the same version."
    on both sides), then bans in `b4bcoop-bans.txt`, lock). B4B's PreLogin gets the options as a parsed TArray of
    {FString key, value} (`options_str`), not one FString. Every chat command has a permission (`cmds.h`: `CMD_ANYONE`
    own game only / `CMD_HOST` / `CMD_CHEAT` host + cheats on / `CMD_OWNSAVE` own save, session cheats on; admin.c `CMDS`, cheats.c `VERBS` via `cheats_perm()`),
    checked once in `admin_slash` before any handler. docs/investigations/chat-commands.md.
  - `models.c` `/model` runtime model swaps (#19, `models` branch): another survivor's outfits/pieces through the
    game's own replicated `PlayerSlot.CurrentCustomizationSet` (`ServerSelectCustomizationSet`, no validation), NPC
    bodies as a made-up row `b4bcoop.npc.<name>` put on by every b4bcoop machine; host `/model <player>`,
    `/models off` (hook on the RPC implementation), campaign-run save keeps own looks; add-on outfits (addoninfo
    `outfit=`, `b4bmod survivor --as`) as made-up row `b4bcoop.outfit.<name>`, 3P + FP arms, others see the
    survivor (new-assets.md §8); made-up rows are applied wherever the game applies a set (hook on the per-slot apply
    0x141B75B90: heroes, cutscene stand-ins, lineup mannequins, #37). Customization screen (#33, new-assets.md §11):
    real rows `b4bcoop.outfit.<name>` inserted into every `<Hero>_Customization_DT` on this machine; the profile never
    stores them (equip hook keeps the previous look, choice in `b4bcoop-outfits.txt`, GetProfileCustomization hook
    answers with it); ini `outfits_screen=0`. The Add-ons tab's Wear makes the same choice without the screen
    (`models_wear`: `b4bcoop-outfits.txt` line + the game's re-init from the profile, at once, Fort Hope or mission,
    host or client; Take off = `models_wear(NULL)`; row state `models_wearing`). Dev `mdl ...` (`mdl screen`, `mdl cs
    open|list|equip|close`: the screen without walking there, `tick_screen` leaves its view; `mdl standins`). `weaponlooks.c`: add-on weapon looks (addoninfo `weapon=`,
    `b4bmod weapon --as`), made-up skin row `b4bcoop.weapon.<name>` in the weapon's replicated
    `ItemMeshManagementComponent.CustomizationRow`, meshes swapped by every machine with the add-on, others see the
    default weapon; a dropped weapon keeps its look on the floor (pickup's `3P_<Code>_SM`; the host pairs it by
    PreviousOwner, add-on or not, and names it to clients and late joiners in `wlook floor|gone|hello` data lines;
    clients guess by position until then), the next owner's own choice applies; world pickups and the dropped magazine
    stay retail; `~` Add-ons tab: Use / Stop on the add-on's row; dev `wlook dump|swap|select|drop|drophero|row|pickups|use|guess` (new-assets.md §9).
    docs/investigations/model-swap.md, player page docs/commands-models.md.
  - `rules.c` the host's run rules (`~` tab **Run**, order 55; chat `/rules`, CMD_HOST; ini keys, live): host-side
    only, no protocol bump, the players hear a chat notice. `free_burncards=1`: burncards.c's `FindPPCByHydraId`
    detour returns NULL for every `b4bcoop.burn.<n>` key (`rules_free_burncards`), so the game's saferoom charge is
    dropped for host and clients (`rules_note_free_charge` notices once per 10 s). `baseline_cards=<rows>` (+
    `baseline_cards_bots`): hook on `GCM::GrantLoadoutCardsForSlot` 0x141769690 (slot data via 0x14177a950, layout
    byte-checked): after the original, if the slot's `HasReceivedLoadoutCards` (+0x59) just turned on, every listed
    card not in its `ActiveHeroCards` is added with the game's `AddGameplayCardToSlotByRowHandle` (cheats.c helpers
    `cheats_card_find/add/each`, `cheats_slot_for_ps`); one notice per map. Run tab: the list (built once per GCM
    from `AllCards`) with search/Add/Remove. docs/investigations/run-rules.md.
  - `runui.c` the run rules on the game's create-run screen (host): a DEFAULT CARDS panel built from the game's widgets
    (TextButton_WBP_C rows via WidgetBlueprintLibrary::Create + AddChild, like lineup.c) into the live
    MatchmakingSettingsPanel_WBP's root canvas (right third); "+ Default Cards" opens the deck builder's
    CardPaginator_WBP_C over BackgroundBlur_WBP_C (SetEquippedCards then SetCards; GetVisibleCard(i) + IsHovered +
    the mouse button's edge = a click, no delegates; re-laid after each toggle, page kept) with a search box and Done.
    Every action is a chat command (`rules ...`). Card list: PlayerCards_MASTER_DT rows through
    `cheats_card_table_each` (deck-buildable + Hero*Ability), no card manager needed. Blueprint classes come from
    `ue_find_any_class` (ue_find_class is native-only) and are looked up when the panel appears (they load with the
    menus). Ini `runui=0`; dev `runui [rebuild|click <n>]`. run-rules.md §3.
  - `cheats.c` Cheats: opt-in, host-only sandbox through chat (`/cheats on|off`, then `/god /heal /revive /ammo /copper
    /card /fly /noclip /walk /tp /freecam /size /horde /director /spawn /killall /freeze /slomo /win /lose`); own-save
    cheats `/supply /skulls /burncards /unlockall` (#49, `CMD_OWNSAVE`): every player while the session has cheats on,
    the issuing machine's own profile only (ExecuteCommand on its local component, `rewards_execute_local`, nothing
    sent; save backup first); the host tells each client its state with a silent `cheats on|off` data line
    (`admin_data_to`: on toggle, 2 s/15 s after a player is first seen in a map), kept per server connection; an older
    host never says it, so its clients refuse (no protocol bump); joining someone turns one's own cheats off. The toggle carries between camp and missions (both ways) and the next chapter, every
    map change resets the effects (notice `cheats still on; effects reset`), off in the menus; every cheat touching
    others is a host notice; rewards, stats and achievements always flow (no cheated-map withholding since #30
    follow-up). Dev `cheat <cmd>`, `cheatprobe` (`state`, `killas <#n>`: kills credited to a player).
  - `poststats.c` every build logs each player's post-round values once per post-round screen (`poststats:` lines,
    host and clients). Dev: `poststats`, UI probes `uitext <path part>` (live TextBlock texts), `uihide <suffix> [vis]`,
    `callw <suffix> <Func>`, `funcs <Class>`, `objat <addr>`. docs/investigations/post-round-stats.md.
    Player reference: the Cheats section of `docs/COMMANDS.md`. Host-side only, no protocol bump.
  - `thirdperson.c` `/thirdperson` (CMD_ANYONE, host and clients, no cheats): the local hero's own over-the-shoulder
    camera (PlayerViewComponent +0x200 + UpdateView), first person while aiming, game's own 3P moments left alone; on
    until toggled off (all maps, sessions; not saved). Local only, no protocol bump. Ini `thirdperson=1` (start on),
    `thirdperson_key=` (toggle, default N); camera settings `/thirdperson distance|side|height|fov|reset` + ini
    `thirdperson_distance|side|height|fov` (default distance 180, right shoulder side 40: shots come from the eyes,
    so side/height offsets move hits off the crosshair by that much). Item pickups (weapons, ammo, card shrines) in 3P:
    the hero's ItemObserverComponent turns itself off in third person and looks from the FP camera; hooks on its
    refresh and on the observation view gather fix both in our 3P (#27). Free look (#31, ini `thirdperson_freelook`,
    `thirdperson_freelook_return`): standing still the mouse orbits the camera (UpdateRotation hook eats RotationInput,
    spring arm stops following), busy → swings back. Dev `thirdperson view|aim|arm|decals|watch|use|usables|lookat|
    itemfix|freelook|look|mouse|heroes|keys`, `fnprobe`. docs/investigations/third-person.md.
  - `updater.c` in-game updates (#34): `~` tab **Updates** (ini `updates=0` hides it; the launcher's switch
    **Update b4bcoop automatically when the game starts** = ini `auto_update`, its last result from
    `b4bcoop-update\auto.txt`). HTTP, check, download, staging/swap/undo, `state.txt` and revert live in
    `updinst.c` (Windows, no game code, no netguard), shared with the launcher. Check = GitHub
    `releases/latest` (or `tags/v<host's ver>` after a version refusal) + the release's signed manifest
    `b4bcoop-update.txt`; Download = zip + `.sig` in memory, size/SHA-256/ed25519 checked (`updcore.c`, portable:
    SHA-256, Monocypher, puff, zip, JSON; unit tests `native/test/run.sh`), then staged in `<game>\b4bcoop-update\` and
    swapped by renames (running files → `backup\<ver>\`; never ini/bans/add-ons/logs); new version runs next start.
    `updater_early` (DllMain) counts a fresh update's starts: 15 s of ticks = healthy, else the 3rd start reverts.
    WinHTTP loaded on first use; netguard lets GitHub's hosts through only inside `netguard_updater_scope`. Dev: ini
    `update_api=`, `update_repo=`, `update_pubkey=`, `update_never_healthy=1`; `update status|check [ver]|install|
    goback|fetch <url> [noscope]`. Live test `tools/update-test.py`. docs/investigations/updater.md.
  - `joinpolicy.c` host: who may join. Default only the host's Steam friends (`ISteamFriends::HasFriend`) and its own
    SteamID; ini `allow_joins=friends|anyone`, `allow_steamids=<id64>,...`; dev `allow_self=0`, `joinpolicy [check
    <id64>]`. Checked at the Steam P2P session request (steamnet.c, authenticated id) and in PreLogin (admin.c; IP
    joins, `host_ip=1` only: the login's claimed id). docs/investigations/steam-p2p.md "Join policy".
  - `presence.c` Steam "Join Game": while hosting, rich presence `connect=+b4bcoop_join steam:<id64> proto:<n>
    ver:<x.y.z> addons:<policy>` (+ `addr:<ip:port>` only with `host_ip=1`); join requests (callback 337) and the same
    string on the command line become a session join target (overrides host=/join=, auto sign-in Offline), or stop at
    once with the version message if `proto:` differs, or with the add-on message if our add-ons fail `addons:`. `presence [on|off]`, `steamjoin <string>` (simulate), `invite`, `friends`;
    ini `presence=0`, `presence_addr=` (host_ip=1 only). docs/investigations/steam-invites.md.
  - `steamnet.c` Steam P2P: UDP shim under the retail net driver (ws2_32 sendto/recvfrom ↔ ISteamNetworking P2P,
    Steam peers get fake 198.18.x.y addresses); `join steam:<id64>`, SteamID in `status`, `steamnet [on|off]`, ini
    `steam_p2p=0`. Its `bind` hook puts the game exe's wildcard UDP binds on 127.0.0.1 unless `host_ip=1` (not other
    modules': Windows steamclient64 binds in-process). Steam's own relay pings (~27 UDP sockets on 0.0.0.0 from
    steamclient.so in the game process; dev `steamnet ping 0` triggers them) are outside that promise: steam-p2p.md
    "Loopback binding". USteamNetDriver can't work here (no STEAM socket subsystem).
    docs/investigations/steam-p2p.md (incl. "Loopback binding", "Two-account result").
  - `cmds.c` dev commands: `status players host join leave exec find call peek poke`; config = `b4bcoop.ini` next to the DLL
    or `B4B_COOP_CONFIG=<windows path>` (`cmds_config_path()`; no ini = defaults; keys `host` (default 1, 0 when
    `join=` is set) `join host_ip steam_p2p allow_joins allow_steamids flashlight_*`, dev `offline allow_self
    b4bcoop_protocol_override`). `coop_join(target)` = join entry point (`steam:<id64>`; `ip[:port]` only 127.x unless
    `host_ip=1`, else refused with `coop_ip_join_off_msg()`; appends `?b4bcoop=<proto>?b4bcoopver=<ver>`; any thread);
    `join=` may list alternatives (`steam:<id>,1.2.3.4:7777`); `coop_host`, `coop_leave`; `coop_version/protocol`.
    Every join (Steam, `/join`, `~` window, `join=`) is one target in one loop (`auto_tick`): attempt once signed in
    and in our own camp, retry 3/8/15 s after a failure, a session target gives up after 3 min (#41,
    docs/investigations/join-timing.md; matrix `tools/jointest.py`).
- `launch/` — `install.sh [--release] [--legacy]` (build+copy DLLs, dev by default, `--legacy` = dev-only dwmapi.dll;
  rm before cp — never overwrite a mapped DLL in place), `package.sh` (player zip `dist/b4bcoop-<version>.zip`
  mirroring the game folder, its `b4bcoop-README.txt` (CRLF, mirrors README's player section: keep in sync) +
  `dist/SHA256SUMS`, reproducible), `run.sh` (Proton, no EAC; `B4B_PREFIX` = alternate compatdata; writes
  `steam_appid.txt`), `multi.sh`/`multi-stop.sh`/`instance.sh`/`shot.sh` (N local test instances, below), `gamelock.sh`,
  `lane.sh`/`lane-restore.sh`/`flatpak-steam.sh` (live-test lanes, below),
  `winpy.sh`, `probed.sh`, `uninstall.sh` (the README's Remove list).
- `tools/` — `b4b.py` agent CLI (`B4B_AGENT=n-1` = instance n), `appinfo.py` (Steam appinfo.vdf dump), `testprefix.py` (test prefixes), `e2e.py` / `charsuite.py` (regression suites), `jointest.py` (join timing matrix), `pe.py` static analysis, `memprobe.py` +
  `probed.py`/`probe.py` live memory (Windows Python inside the prefix), `sdkdump.py`, `winpoke.py`, `fetch-deps.sh`.
- `modkit/` — the mod maker's kit (#21), a separate deliverable (players never need it; nothing of it is in the player
  zip or the agent): `b4bmod.py` (one command: setup/status/config, find/extract (offline, `dotnet/pakx` = CUE4Parse
  from NuGet with its managed Oodle decoder), info/tree/export/texture/mi (`dotnet/b4bmod`, UAssetAPI), mesh
  info/export/import/edit (`skm.py`, `skmgltf.py`, `upkg.py`), survivor/weapon (`b4bmodel.py` + `blender/b4bfit.py`
  in headless Blender, static meshes `sm.py`: model → fitted, LODs, textures, then packed/installed), pack/install/
  check (`addon.py`)), `b4bmod.cmd`/`b4bmod.sh`, `README.md` (Windows first; how to get every third-party piece),
  `docs/` (author guides; models: `docs/meshes.md`), `package.sh` → `dist/modkit/b4bcoop-modkit-<version>.zip`
  (reproducible; refuses 64-hex strings). The pak AES key is built into b4bmod.py (public, same for every copy;
  `config aes_key`, `B4B_AES_KEY`, `--aes-key` override); pakx checks it against every pak index SHA1. No native
  Oodle, ever: CUE4Parse's OodleSharp only. Dev-only asset tools stay in `tools/modkit/` (`pakscan.py`,
  `customversions.py`, `assetcheck/`, `uassetrt/`, `testassets/`, `source_survivor.py` + `vpk.py`: a Source engine
  character from a local game install (e.g. the L4D2 survivors, #40, private only) -> glb via SourceIO in headless
  Blender, for `b4bmod survivor`). Data: `~/.local/share/b4b-coop/`
  (`%LOCALAPPDATA%\b4b-coop` on Windows).
- `sdk/` — local only (gitignored, kept out of the public repo): reflection dump of all `/Script` classes.
  Regenerate with `tools/sdkdump.py` (see docs/NOTES.md).

## Setup
`tools/fetch-deps.sh` (zig 0.15.2 sha256-checked, MinHook pinned to commit 8af6b4a, ImGui, Monocypher 4.0.2 and zlib
1.3.1's puff (updater), stb_image 2.30 (shop thumbnails) pinned by commit + sha256, Windows Python, .venv; `--build` = only what native/build.sh needs) →
`launch/install.sh`. CI: `.github/workflows/ci.yml` builds dev + player and runs `native/test/run.sh` (updater unit
tests) on every push/PR; `release.yml` builds the zip on a `v<version>` tag (must match `VERSION`) and publishes it
with `SHA256SUMS`, a build provenance attestation and the in-game updater's files: `b4bcoop-update.txt` (manifest)
and ed25519 `.sig`s of it, the zip and SHA256SUMS (`tools/sign-release.sh`, secret `B4B_RELEASE_SIGNING_KEY`, public
key `docs/release-signing.pub.pem` = `B4B_RELEASE_PUBKEY_BYTES` in `native/src/signkeys.h`; private copy
`~/.local/share/b4b-coop/keys/release-signing.key`, never commit; rotation/loss: updater.md §3). The add-on shop's catalog
has its own key: `docs/shop-signing.pub.pem` = `B4B_SHOP_PUBKEY_BYTES`, private `~/.local/share/b4b-coop/keys/shop-signing.key`
+ secret `SHOP_SIGNING_KEY` in actuallydan/back4blood-shop, never commit; rotation/loss: shop.md §2. No Steam launch options needed (the agent is `X3DAudio1_7.dll`; the dev-only legacy
`dwmapi.dll` needs `WINEDLLOVERRIDES="dwmapi=n,b" %command%`). Steam's only public launch entry runs the root `Back4Blood.exe` stub →
`start_protected_game.exe` (EAC) → `Gobi/Binaries/Win64/Back4Blood.exe Gobi -SaveToUserDir`. Game build pinned: Steam buildid 14216215;
the agent verifies byte signatures and refuses to hook on mismatch.

## Versioning
`VERSION` (repo root) holds both numbers; `native/build.sh` turns them into `native/out/gen/b4bcoop_version.h`
(`B4B_VERSION`, `B4B_PROTOCOL`), and `launch/package.sh` names the zip after the version.
- **version** (semver): bump for **every release**; tag the release `v<version>` (release.yml checks it).
- **protocol** (integer): bump whenever host and client behavior must match: new RPC use, changed reward/burn-card
  forwarding rules, anything a client must understand or a host must expect from the client. The host refuses a
  login with another protocol (admin.c), and a Steam Join Game to a host with another protocol stops before
  connecting (presence.c). A pure host-side or client-side fix needs no protocol bump.
- A version refusal points to the `~` Updates tab, which offers the host's version (updater.c).
- Test a mismatch: dev ini `b4bcoop_protocol_override=N` fakes another protocol (e.g.
  `B4B_INI_EXTRA2="b4bcoop_protocol_override=2" launch/multi.sh 2`).

## Commit messages
Subjects are public: `tools/relnotes.py` turns main's first-parent history (the `--no-ff` merges and direct commits)
into the GitHub release notes (release.yml). So:
- Subject = one plain-English line a player or contributor understands, imperative or descriptive, under ~100 chars,
  issues as `(#N)`. No agent/lane names, test-infra jargon, addresses (`0x14...`), file-name prefixes or "WIP"; that
  goes in the body. Merges get a descriptive subject (what the branch brings), never git's default "Merge branch 'x'".
- Category = a `Release-note:` trailer, in the last paragraph above `Co-Authored-By`/`Claude-Session` (which never
  reach the notes): `new`, `change`, `fix`, `docs` (player docs), `dev` (tests, tools, CLAUDE.md, investigations:
  grouped under "Developer"), `none` (left out). Bare value = the subject is the note; `Release-note: fix: <text>`
  = that text instead; repeat the trailer for several notes. Every player-facing change needs one. Put them on the
  merge (it wins), or on the branch commits (used when the merge has none). Release bumps ("Release X.Y.Z") are
  skipped. Older commits without trailers: the subject is used, the category guessed from the files touched.
- Preview: `tools/relnotes.py` (since the last `v*` tag), `tools/relnotes.py v0.5.0`, `tools/relnotes.py A..B`; hand-edit
  a published release with `gh release edit <tag> --notes-file` (keep the provenance text below the notes).
- CI (`ci.yml` job `commits`) runs `tools/relnotes.py --check <pushed range>`: fails on `fixup!`/`squash!`, WIP,
  default merge subjects and malformed `Release-note:` values; only warns on addresses, lane/agent jargon,
  file-name prefixes and long subjects.

## How to run N local instances (unattended)
`launch/multi.sh N` (N = 1-5; install the DLL first). Instance 1 hosts (by default: its ini has no `host=` line), the
others join `127.0.0.1:7787` (loopback: allowed without `host_ip`, all game sockets are on 127.0.0.1); no clicks:
the agent presses Sign in and answers the Online/Offline popup with Offline. Returns when the host sees N players
(3 instances ≈ 90 s). Then e.g. `B4B_AGENT=0 .venv/bin/python tools/b4b.py mission Normal` starts a new Evansburgh
campaign run on the host (same path as the war table); clients follow automatically. `launch/multi-stop.sh` kills
only test instances (SIGKILL by PID, matched on `B4B_PREFIX` in /proc/<pid>/environ).
- Instance n: prefix `~/.local/share/b4b-coop/prefixes/test<n>` (clone of the real one, ~600 MB; muted, 960x540,
  low settings, ui cvars in its Engine.ini), config `<prefix>/b4bcoop.ini`, agent port 47112+n-1 (`B4B_AGENT=n-1`),
  log `b4bcoop-test<n>-<winpid>.log`, window "B4B #n" (`launch/shot.sh n out.png`).
- Env: `B4B_GAME_PORT` (7787 — deliberately not 7777, so tests can't reach a real session), `B4B_STAGGER`,
  `B4B_TIMEOUT`, `B4B_FRESH=1` (re-clone), `B4B_BLANK="2 3"` (fresh offline profile for those instances),
  `B4B_INI_EXTRA="netguard=off;netguard_eos=0"` (extra `b4bcoop.ini` lines for every instance), `B4B_INI_EXTRA<n>`
  (instance n only), `B4B_PORT_BASE` (agent ports; 47120 when another game with the agent holds 47112).
- **Test prefixes are shared between sessions: change them (`testprefix.py`, `B4B_FRESH`, `B4B_BLANK`, editing their
  `b4bcoop.ini` or saves) only while holding `launch/gamelock.sh`.** `testprefix.py` refuses to touch a prefix a game
  process is running on (`B4B_PREFIX` in /proc/<pid>/environ) unless `--force`.
- Each prefix keeps a golden profile copy in `profile-golden/` (clone time; e2e.py restores it). `B4B_STEAM=flatpak`
  uses its own set, `profile-golden-flatpak/` (dreamsofants' saves, made in the game: a save only loads under the
  account that wrote it); docs/investigations/test-profiles.md "Flatpak mode".
- The real prefix and its SaveGames are never written. Profile truth is the AES `PlayerProfileSettings.sav`; the
  `.json` is an export the game overwrites, so editing it does nothing. All copies share one Steam account: same
  name, same `offline.<steamid64>` id on the host.
- Ending a mission unattended: `mission Easy`, wait for both heroes, `ready` (match → InProgress), `endmission 1`
  (success) or `endmission 0` (failure). The post-round screen times out after ~2 min and moves on to the next chapter.
- Profile saves are deferred (~30 s after `ApplyCommandToOfflineData`); wait before `multi-stop.sh` (SIGKILL) or diffing.
- `-Port=` on the command line sets the listen port (UE `FURL` default port); in use → it binds the next one.
- **Lanes**: two independent live-test setups so two agents can run instances at the same time. `B4B_LANE=2` (default
  1) is read by every launch script and `tools/{b4b,testprefix,e2e}.py` (table: `launch/lane.sh`, `tools/lane.py`).
  Lane 1 = everything above. Lane 2 = the Flatpak Steam copy of the game (same build;
  `~/.var/app/com.valvesoftware.Steam/.local/share/Steam/steamapps/common/Back 4 Blood`), lock
  `/tmp/b4b-game-lane2.lock`, prefixes `prefixes/lane2/test<n>`, game port 7887, agent ports 47140+, windows
  "B4B L2 #n", artifacts `/tmp/b4b-e2e-l2-<time>`. Which Steam client the instances talk to (`B4B_STEAM`, lane.sh):
  `native` (default; both lanes: Dan's account Hergmgurk through the native client) or `flatpak` (lane 2 only, and
  it picks lane 2): Proton runs inside the Flatpak Steam's own sandbox (account dreamsofants 76561198994546085).
  That Steam must be started with `launch/flatpak-steam.sh start` (own SysV IPC namespace + the lane-2 prefixes in
  view; `status` checks it); run.sh refuses otherwise, because Flathub's Steam shares the host's IPC namespace and a
  game could register with the native client. Games join the sandbox with `flatpak enter` (never Steam's
  LaunchAlongsideSteam D-Bus name: the native Steam owns the same name). docs/investigations/flatpak-steam.md.
  **While Dan plays on his native account, only `B4B_STEAM=flatpak` may run**: the file
  `~/.local/share/b4b-coop/native-steam-in-use` marks that, and run.sh refuses native launches while it exists.
  Each lane's `gamelock.sh`, `multi-stop.sh` (matches only its own prefix root) and instances are independent.
  Both folders hold one of Dan's player installs (lane 1: his own account's; lane 2: his second account's,
  dreamsofants). `gamelock.sh acquire` backs up the lane's player files (`launch/lane-restore.sh`: top-level files of
  the game root and Win64, and <game>/b4bcoop-addons/, into `~/.local/share/b4b-coop/lane<n>-player-backup/`; also
  install.sh/run.sh: lane 2 always, lane 1 only while its lock is held); `release` stops the lane's instances,
  restores them sha256-exact and removes what the dev install added (test logs move to a new
  `~/.local/share/b4b-coop/lane<n>-logs/<time>/`), so no reinstall of main's build is needed. Acquire waits while
  the player's own game runs in that folder. Never edit his `b4bcoop.ini`.
  Usage: `export B4B_LANE=2; launch/gamelock.sh acquire <me>; launch/install.sh; launch/multi.sh 2; ...; release`
  (Flatpak Steam: `launch/flatpak-steam.sh start` once, then `export B4B_STEAM=flatpak B4B_GPU=4090` instead of
  `B4B_LANE=2`).
- **GPU**: `B4B_GPU=4090` (any part of an NVIDIA GPU name) runs test instances on that GPU in a headless gamescope
  (`launch/instance.sh`: `--backend headless --prefer-vk-device`, `VKD3D/DXVK_FILTER_DEVICE_NAME`), so they leave the
  display GPU (5090) to Dan. A GPU with no display attached can't present to the desktop directly (swap chain
  `E_INVALIDARG`), hence gamescope. No windows: multi.sh skips window labels, e2e still passes (13/13, both lanes).
  `launch/shot.sh N out.png` (any mode) asks the agent for dev `screenshot <windows path>`: the frame as presented
  (HUD, menus and the ~ overlay included), copied from the back buffer in the overlay's D3D12 Present hook
  (overlay.cpp, dev builds only); fallback the engine `shot` (3D scene only: black on UI-only screens such as the
  pre-round cards, which made e2e's end screenshots empty), then the window grab without `B4B_GPU`. `B4B_SHOT=engine|window`
  forces one.
  **Default for agents while Dan uses the 5090: `export B4B_GPU=4090`** (a game in the Flatpak sandbox presents to the
  host's gamescope through the Flatpak extension `org.freedesktop.Platform.VulkanLayer.gamescope//25.08`).
- 5 players: `B4B_INI_EXTRA="teamsize=5" launch/multi.sh 5` (opt-in `teamsize` in `native/src/teamsize.c`). Verified: a
  full mission and 2 chapter transitions with 5 humans. Without it, a 5th joiner is refused with "Server full."
  (`slotguard.c`; before that it crashed the host). Results: `docs/investigations/five-players.md` §5,
  `docs/investigations/slot-guard.md`.

## Regression suite
**Before merging/releasing: run `tools/e2e.py --quick`** (install the DLL under test first; the suite takes
`launch/gamelock.sh` itself, or pass `--no-lock` if you already hold it). It launches `multi.sh 2` and checks, each
with a timeout and PASS/FAIL: join, mission follow, client flashlight replicated to the host, a host and a client burn
card charged once to their own profiles, chat `/players` typed on the client, ready + `endmission 1` with the client's
SP forwarded, a seamless chapter transition, both profiles diffed after the deferred save (client SP +forwarded
amount exactly, host only its own), the 0.3.0 defaults (host without `host=`, presence `steam:` + `proto:` and no
`addr:`, protocol in the login options), no public peer on any game socket and nothing bound off loopback (`ss`
sampler, every second; sockets made through Wine's ws2_32 only: Steam's native steamclient.so relay pings, ss lines
without `wineserver`, are listed apart and are no failure; steam-p2p.md "Loopback binding"; so is HTTPS to GitHub's
raw.githubusercontent.com addresses: the add-on shop's list, fetched once per game start in Fort Hope). The `~`
window smoke check also opens the Add-ons tab (the list fetched by itself, Refresh). `--full` adds a vanilla
`multi.sh 5` (5th refused "Server full.", host survives) and a `teamsize=5` round (5 follow, SP forwarded to all 4
clients). Summary table at the end, exit 1 on failure; logs, agent transcript, profile diffs, ss samples and
screenshots in `/tmp/b4b-e2e-<time>/` (`--out`). `B4B_LANE=2 tools/e2e.py --quick` runs it on lane 2 (above), in
parallel with a lane-1 run; lane 2's lock release restores the second account's player build.
Before each session it restores every used prefix's golden profile (`<prefix>/profile-golden/`, `tools/testprefix.py
N --golden|--restore`; logged `profile testN: ...`; `--keep-profiles` skips it), so a wiped or grown profile never
breaks the next run; the profile checks still diff that run's before/after. A client joining during its own sign-in
used to get its profile reset ("HydraPublicId mismatch"); joins now wait for the sign-in
(docs/investigations/test-profiles.md).
**Updater: `native/test/run.sh`** (unit, no game) and **`tools/update-test.py`** (live, one instance: fake signed
release on a local fake GitHub API, refused bad downloads, install → restart → new version, go back, automatic revert;
39 checks; `B4B_LANE=2 B4B_STEAM=flatpak B4B_GPU=4090 tools/update-test.py [--no-lock]`). `lane-restore.sh` also
covers `<game>/b4bcoop-update/`.
**Add-on shop: `tools/shop-test.py`** (live, one instance, then `multi.sh 3` and `multi.sh 2`: local catalog signed with
a throwaway key on 127.0.0.1, own `addons_dir=`; the list fetched by itself in Fort Hope (bad signature refused),
Refresh, damaged/truncated downloads refused, the Add-ons table (installed first, A-Z / Z-A, time installed both ways,
filter, search; row order through `overlay locate`), Add outfit in Fort Hope mounted at once and worn through the
row's Wear (at once, the window stays open, saved; Take off; also in a mission), weapon look in a mission (Use / Stop), a
replacement of a loaded asset after restart, Remove at the next start, an Update loaded at once / one in use at the
next start, Wear over a `/model` wish kept after a restart, host Wear swap between two outfits + Take off, Add + Wear
as a client (host with the add-on sees it, a client without sees the survivor), a host with `addons_policy=none` keeps the client's Add for the next start; ~75 checks, ~12 min;
`B4B_LANE=2 B4B_STEAM=flatpak B4B_GPU=4090 tools/shop-test.py [--no-lock]`; test paks in
`~/.local/share/b4b-coop/shop-test/src/`, never committed).
**Automatic update before the game starts: `tools/autoupdate-test.py`** (live, test instance 1 through the root stub,
never online; two test releases of this tree signed with a throwaway key on a fake API, ini keys in the instance's own
ini): the real `releases/latest` (same version: nothing shown) with the dev and the player launcher, no answer / no DNS /
a slow API (≤ 2 s, no window), newer release with and without the prompt (window, mapped launcher renamed into the
backup, new version runs + healthy, Updates tab), a slow download (gamescope frames every 0.5 s), refused downloads,
Skip during the download, the four Steam join plans, pending / skip= / failed\ / Go back / automatic revert never
reinstalled, `auto_update=0` + the tab's checkbox; window screenshots through the launcher's dev `B4B_LAUNCHER_SHOT`;
56 checks, ~12 min; `B4B_LANE=2 B4B_STEAM=flatpak B4B_GPU=4090 tools/autoupdate-test.py [--no-lock] [--only a,b]`.
**Launch choice co-op / Online (#47): `tools/online-test.py`** (live, test instance 1 on its own prefix and ini, never
online: EAC's launcher is only ever created suspended, then killed): the agent's Wine DllOverride for the launcher,
the sign-in popup's Online answered Offline (`auto_signin=0`), the stub's prompt (no input: co-op after the 15 s
countdown, no EAC, agent loaded, signed in Offline without the game ever logging "prompting online/offline"; Online + remember: agent
moved to `b4bcoop-online`, ini `launch=online`, nothing of ours mapped in EAC's launcher, no game process), remembered
choice, Steam join = co-op (agent moved back), `-b4bcoop=ask` + cancel, `-b4bcoop=off` with the agent loaded closed;
`B4B_LANE=2 B4B_STEAM=flatpak B4B_GPU=4090 tools/online-test.py [--no-lock]`.
**Model mods (`models` branch): `tools/charsuite.py`**, the character suite (mesh-mods.md §17): builds every test
character of a local manifest (models can't be committed: `~/.local/share/b4b-coop/characters/suite.json`, format +
CC0 example `tools/charsuite-example.json`) with `b4bmod survivor --as` one at a time in its own extract folder
(`--twice`: byte-identical paks), preview.py stills, then (lock `charsuite`, `--install`, add-ons into the lane's game
folder) `multi.sh 2` (`--vanilla`: + a 3rd with `addons=0`): per outfit host `model <name>`, "wears outfit" in both
logs, the mesh on the host's hero on both, screenshots with the host placed in front of each client (`face look`),
new mesh/material/cloth/add-on log errors; `--mission N` wears N again in Evansburgh. Table + `contact.png` in
`/tmp/b4b-charsuite[-l2]-<time>/`, exit 1 on failure. Full run: `B4B_LANE=2 B4B_GPU=4090 tools/charsuite.py --twice
--vanilla --install` (~35 min for 13 characters).

## Branches
- `main`: shippable. Releases are tagged from here.
- `models`: the model/content-mod work (epic #23), merged into `main` for v0.6.0 (2026-09-26); feature branches now
  branch from `main`. Never commit game assets, extracted files, paks, test models or their screenshots (copyright);
  extracted content and test models live under `~/.local/share/b4b-coop/`.

## Gotchas
- UE4SS does not work on this game (obfuscated engine) — don't go back to it.
- Wine reparents the game to systemd: `/proc/<pid>/mem` is unreadable (yama=1). Use the Windows-side tools.
- `pkill -f <pattern>` kills your own shell when the pattern appears in the command; kill by PID from `pgrep`.
- SIGTERM on a Wine game whose wineserver is gone leaves a zombie with ~200 threads parked in ntsync that still holds
  its UDP port; use SIGKILL. Such a zombie (state Z, threads in ntsync_schedule) goes away when one of its
  threads gets SIGKILL: `kill -9 <tid from /proc/<pid>/task>`. Proton resets `STEAM_COMPAT_DATA_PATH` inside the game (use `WINEPREFIX`/own vars).
- Shipping build writes no engine log; the agent enables the UE_LOG gate (0x1469BD96D) and captures it.
- Native Windows: the exe is ASLR'd (Wine keeps the preferred base), so all static addresses go through `VA()` in
  `ue.h`. System d3d/dxgi DLLs load our dwmapi too and import ordinal-only exports, so every proxy
  (`native/proxy/*.c/.def`) is generated by `tools/gen-proxy.py` to re-export every real export. Both agent names
  may be present: one agent per process (named mutex; a dwmapi.dll from the same dir wins). A plain Steam Play is
  meant to work via the root `xinput1_3.dll` (unverified on Windows); fallback: launch.md §3 B (launch option). Build on Windows: Windows zig in `vendor/zig`, Git Bash.

## Progress
Verified live (2026-09-23/24; details and evidence in `docs/investigations/*.md` and closed GitHub issues):
- Offline Fort Hope as listen server (PacketRelayNetDriver, UDP 7777, DTLS); auto-host (default) / auto-join;
  camp → mission via server-travel redirect; chapter → chapter via the game's own seamless travel; clients take over
  bots through the retail PlayerSlotManager path. Real two-machine session with a Windows client on its own account.
- Remote players keep their own deck (`cards.c` safety net), their rewards (`rewards.c`: supply points etc. forwarded
  to their own profile, #4) and can play burn cards, charged to their own profile (`burncards.c`, #6).
- No third-party traffic offline (`netguard.c`, #5): 0 public connections in a 16-min session on Proton.
- Manual flashlight toggle with replication and sticky mode (`flashlight.c`, #2).
- 5-player co-op, opt-in `teamsize=5` (`teamsize.c`, #1; post-round lineup shows all 5, `lineup.c`, #8); joins
  beyond the slot count are rejected with "Server full." instead of crashing the host (`slotguard.c`, #7).
- Unattended N-instance local testing (`launch/multi.sh`, `testing.c`, #3).

- Two real Steam accounts on one machine (2026-09-24): Steam P2P join, mission follow and per-player rewards verified
  (docs/investigations/steam-p2p.md, "Two-account result"); second account runs in Flatpak Steam.
- 0.3.0 defaults (2026-09-24, local copies on Proton): hosting with no `host=` line, presence `steam:` + `proto:` and
  no `addr:`, game UDP on 127.0.0.1 (host and client) with loopback joins working (`e2e.py --quick` 12/12),
  remote-IP joins refused with the message, protocol mismatch refused on both sides, `host_ip=1` back to 0.0.0.0 and
  IP joins.
- 2026-09-25 (local copies on Proton, two lanes): `/thirdperson` (#25: camera settings, N key, shoulder default, aim
  correction toward the crosshair verified client-only on host damage); the `~` overlay (#26, D3D12 + ImGui) with every
  chat command, ini save and key binding; live ini reload; joins wait for the offline sign-in (fixes a profile reset
  race with `join=`); e2e: golden test profiles, Steam's own relay sockets (steamclient.so, 0.0.0.0) reported apart
  from game sockets.
- Model mods (`models` branch, 2026-09-25, epic #23, local sessions on Proton): add-ons in player builds (#20),
  multiplayer rules `addons_policy` (#22), texture/material edits, SKM + static-mesh writing, a CC0 FBX survivor
  (3P + FP arms, alpha hair) and an AK replacing AR02 as add-ons; added outfits and weapon looks (`--as`, `/model`)
  that others without the add-on see as vanilla; Models and Add-ons tabs in the `~` window; the separate `modkit/`
  (b4bmod, own zip). Not run on real Windows (modkit .NET tools); no facial animation.

- In-game updater (#34, 2026-09-27, lane 2 / Flatpak Proton): HTTPS check + download through WinHTTP under Proton,
  netguard scope, signed manifest/zip, bad downloads refused, install on next start, go back, automatic revert
  (`tools/update-test.py` 39/39). Not yet on native Windows; first real signed release = 0.6.2.

- Add-on shop (#36, 2026-09-27, lane 2 / Flatpak Proton, dev build + local catalog): Browse tab, signed catalog,
  thumbnails, runtime mount of new-path cosmetic add-ons (outfit worn right after Add, weapon look added mid-mission),
  replacements/removals/updates at the next start (`tools/shop-test.py` 32/32). No real shop repo yet.

- One `~` tab, Looks & add-ons (2026-10-12, lane 2 / Flatpak Proton): Models, Add-ons and Browse merged (Wear /
  Installed / Get more, one search box, Host rules), host and client, 960x540 and 1080p, text size 1.25 and 0.75;
  `tools/shop-test.py` 58/58.
- Add-ons tab (2026-10-13, lane 2 / native Steam on Proton): one table instead of Wear / Installed / Get more (shop list
  fetched by itself in Fort Hope, installed first, sort / filter / search, Wear = the customization screen, Use / Stop,
  no load order; large thumbnails + hover picture, the table fills a tab-sized window), host and client, 960x540 and
  1080p, text size 1.25 and 0.75, the real shop list (127 add-ons: thumbnails without a hitch, Add + Wear + Remove);
  `tools/shop-test.py` 68/68.

Known issues / open:
- #8 (fixed): the 5th hero in the post-round lineup stands in the back row, dimmer and without a name plate.
- Not yet run on native Windows: netguard (WinHTTP path), rewards/burn cards/slot guard/5 players across machines,
  and the no-script launch (root `xinput1_3.dll` redirect + `X3DAudio1_7.dll`; test script in
  docs/investigations/launch.md §6). On Proton the `X3DAudio1_7.dll` agent is verified with a plain Steam launch.
- All local test copies share one Steam id; two accounts: native + Flatpak Steam on one machine (`tools/jointest2acct.py`).
- Steam Join Game/invites (`presence.c`), now the players' main join path: the real callback 337 between two accounts
  (rungame URL in the client's Steam, `tools/jointest2acct.py`) passed in every host state, incl. a retry after a
  failed handshake, a mid-mission rejoin, burn-card charge and rewards (join-timing.md "Two-account check", 2026-10-05);
  the Join Game click in Steam's own UI, also with the game closed (Steam starts it), works in Dan's real sessions
  (2026-10, weeks of play; #10 closed). Local copies on one account
  overwrite each other's rich presence.
- Steam's own relay sockets (in-process steamclient) bind 0.0.0.0. Fine: the promise is peer-to-peer play without
  official game servers (Steam is how friends connect), not an air gap; only the game's own UDP stays on loopback.
- A client that disconnects before the saferoom-exit charge and doesn't come back keeps its burn card (one that rejoins
  is charged: burn-cards.md §6). Skull totem points and duffel-bag
  rewards reach the client (verified, client-rewards.md §6b), but a remote player's duffel roll can't see what they
  own, so they may get a product they already have (a no-op).

Next:
1. Real multi-machine session on the new build (Windows client, launch.md §6): netguard, rewards, burn cards, 5
   players, and whether Windows shows any Firewall prompt.
