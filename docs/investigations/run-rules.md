# Run rules: the host decides how the run plays (`rules.c`, `~` Run tab)

Build 14216215. Private sessions want house rules that the retail game has no setting for. The listen host runs the
card and reward code for every player, so a rule applied by the host's agent holds for the whole lobby without any
client-side code: host-side hooks only, no protocol bump, a chat notice tells the players.

Module `native/src/rules.c`: ini keys (read at init, live-reloaded through `rules_live`), the `~` window tab **Run**
(order 55, between Cheats and Add-ons; `ov_begin_perm(CMD_HOST)` greys it out on a client), chat `/rules` (CMD_HOST
in admin.c's table). Every control runs the chat command (`ov_run("rules ...")`), which writes the ini key through
`cmds_ini_apply(..., save=1)`, so the three paths (tab, chat, editing `b4bcoop.ini`) end in the same place.

## 1. Free burn cards (`free_burncards=1`)

### Retail
A burn card played in the start saferoom (`ServerPlayBurnCard` -> `GCM::PlayBurnCard`) is queued in the player's
slot (`slot+0x78`, key `slot+0x88`); leaving the saferoom (`GCM::OnSafeRoomStateChanged`, charge at 0x141768A30)
groups the queue by key, looks the key up with `FindPPCByHydraId` (0x141BBEB10) and issues
`AdjustConsumableQuantity(-1)` on the profile component it returns. **No component = no charge** (the game's own
path for a player who left). burn-cards.md §2.

### With b4bcoop
`burncards.c` rewrites every human player's key to `b4bcoop.burn.<n>` and its `FindPPCByHydraId` detour resolves
those keys to the controller that played the card (remote players' charges are then forwarded by `rewards.c`).

### The rule
The detour asks `rules_free_burncards()` first: when on, it logs `rules: free burn cards: b4bcoop.burn.<n> not
charged`, tells the players once per 10 s (`admin_notice`: "burn cards are free in this session (host rule) ...")
and returns NULL. The game drops the charge exactly as for a departed player. Host and remote players alike, since
both go through the synthetic keys. Nothing is sent to a client, so a client's `rewardguard` sees no `-1` and nothing
changes in anyone's save.

What it does not do: a card you own 0 of still can't be played (the client's card screen lists its own profile's
cards with quantity > 0: `GetPlayerBurnCards`, client-local). Making those playable is a client-side change
(backlog).

### Verification (planned; see the session notes outside the repo)
Two players, chapter 1: both play a burn card, `/rules free_burncards on` before leaving the saferoom, walk out.
Expect on the host: `rules: free burn cards: b4bcoop.burn.0 not charged` (and `.1`), no `charging b4bcoop.burn`,
no `rewards: forwarding AdjustConsumableQuantity`; on the client no `[CLIENT RPC] adjusting consumable`. Both
profiles' `consumables.*.spent` unchanged after the deferred save (~30 s). With the rule off, +1 each (burn-cards.md
§5 diff recipe). The chat shows the notice once.

## 2. Baseline cards (`baseline_cards=<row>,<row>,...`, `baseline_cards_bots=0|1`)

The host names cards every survivor plays with, on top of their own 15-card deck (a "house loadout": Weapon
Scavenger, Share the Wealth, Down in Front, ...). Not a deck-size change: a run's active cards already grow beyond 15
with the cards found and bought on the way, and that is the mechanism used.

### Retail
`UGameplayCardManager::GrantLoadoutCardsForSlot(GCM*, AGobiPlayerState*)` @ `0x141769690` (card-draft.md) runs on
the server when a slot gets its deck: the mission start for every hero and a hot-joiner's bot take-over
(`ApplyLoadoutForCharacterTransfer`). Prologue: `40 55 41 54 41 56 41 57 48 8d 6c 24 c1 48 81 ec f8 00 00 00 45 33
c0 4c 8b f2 4c 8b f9 e8 ...`. It asks `0x14177a950(gcm, ps, bCreate=0)` for the state's slot data
(`PlayerActiveGameplayCardData`: the `GCM+0x298` array burncards.c also walks, stride 0xD8; found by
`ps+0x2a0`/`+0x2a4` = slot index/team) and returns at once when there is none, when `[slot+0x59]`
(`HasReceivedLoadoutCards`) is already set, or when `[GCM+0x3a0]->+0x120 != 3` (the match state). Then each deck card
that passes `OwnsCard` is added to the slot's `ActiveHeroCards` (`+0x10`, `FActiveGameplayCard` 0x28 each, the row's
FName at +8) and `+0x59` is set. Callers: `0x1417688ea`, `0x141769520`, `0x14177b180`.

### The rule
Hook on `GrantLoadoutCardsForSlot` (rules.c; prologue + the four layout bytes checked, else the rule is off and the
Run tab says so). The detour reads the slot data's `+0x59` before and after the original: only when it **turned on
in this call** (the deck was just granted; re-entries and non-mission calls change nothing) it resolves each
`baseline_cards` row through the GCM's `AllCards` (`cheats_card_find`, exact row or shown name), skips the ones
already in `ActiveHeroCards` (a card in the player's own deck is granted once), and adds the rest with the game's
own `AddGameplayCardToSlotByRowHandle` on the state's `UPlayerSlot` (`OwnedPlayerSlot`, else the pawn's slot:
`cheats_slot_for_ps`) — the `/card` cheat's path, no ownership check, so a joiner needs to own nothing. Bots too
unless `baseline_cards_bots=0`. Log `rules: baseline cards for <name>: N added: ...`; one `admin_notice` per map.

Why not add them in the `OwnsCard`/deck path: the deck is the player's own choice and lives in their profile; the
baseline is the host's and must not touch anyone's save. Active cards are run state (replicated to clients, saved in
the campaign run's snapshot like any found card: `CanBeSavedInSnapshot`), which is exactly the scope wanted.

### Live results (Windows, 2026-10-23, solo host + 3 bots, Evansburgh Hard)
`baseline_cards=Scavenger_Weapons,Talent_ShareTheWealth,Talent_DownInFront` in the ini. Three runs:
1. Host slot: `##> GrantLoadoutCardsForSlot` / `granting 15 cards` / `/GrantLoadoutCardsForSlot`, then
   `rules: baseline cards for ? (slot 0): 2 added: Scavenger_Weapons, Talent_DownInFront (1 already had, 0 unknown)`
   (Share the Wealth was in the player's deck). The game logged its own `AddGameplayCardToSlotByRowHandle for player
   ... Row: Scavenger_Weapons`, `LogAnalyticsCardSystemComponent: Received New Gameplay Card: Scavenger_Weapons`,
   the audio bank load, and the loot system `LSS Mod - Scavenger_Weapons_Mod_C ... Items.Weapons.Primaries (2.00)`:
   the card is in effect. One chat notice. The bots' three grants (`GrantAllLoadoutCards`, 15-30 s later) logged `no
   player slot`: a bot's state has no `OwnedPlayerSlot` and no pawn yet.
2. Slot lookup by the card data's index through `UPlayerSlot.SlotIndex`: no such property (the one-time dump in
   `cheats_slot_by_index` listed `PlayerSlot_BP_C`: `SlotIdx@672 ... Team@698 ...`).
3. With `SlotIdx` + `Team`: slot 0 `2 added`, slots 1-3 `3 added` each. The Tab **Active Cards** screen shows Down
   in Front ("+10 Health"), Weapon Scavenger ("More weapons spawn") and Share the Wealth with "Played by teammates:
   3, Played: 1": the bots' copies count as team cards like any other.
The player name is `?` in the log at grant time (names aren't set yet); the slot number identifies the survivor.

### Open
- Chapter 2 and later: re-granted per chapter (idempotent) or carried by the run snapshot; either way the cards stay.
- A client hot-joining mid-mission: the take-over's grant runs the same code (not yet seen live).
- Team-once cards (`MaxTeamCount`) as a baseline: the game's add may refuse the second copy; the log shows it.

## 3. The create-run screen (`runui.c`)

Players look for run options on the war table's run setup, not in a mod window. The screen is
`MatchmakingScreen_WBP_C` -> `ContentSwitcher` -> `MatchmakingSettingsPanel_WBP_C` (`notes/matchmaking-*-tree.txt`
outside the repo hold the dumps): left column `DifficultySelect` / `ActSelect` / `MapButton` (792 wide at x 155 of the
3840 design), `SettingsInfo` to its right (x 1059..2361; its text runs below the 719 px box), the right third empty.

### Building from the game's widgets
- Rows: `TextButton_WBP_C` (the screen's own button: `SetText`, hover visuals in its Blueprint) created with
  `WidgetBlueprintLibrary::Create` (lineup.c's recipe) into a `VerticalBox` added to the panel's root `CanvasPanel`
  (`AddChildToCanvas`, anchors (0.5, 0), position (520, 470), size 1240 x 1370, Z 50). Heading: a `TextBlock` with the
  font and colour of the panel's first `TextBlock` (the "Difficulty" heading) through `SetFont` /
  `SetColorAndOpacity` (the property's bytes copied into the setter's parms: the setter copy-assigns).
- Clicks: nothing in C can be bound to a Blueprint delegate, so every frame the game thread asks each of our widgets
  `IsHovered()` and watches the left button's down edge (`GetAsyncKeyState`, only with the game focused and the `~`
  window closed). The hovered one gets the click. The game's own UI keeps the input: we only look.
- Lifetime: the widgets' outer is the panel; they die with the screen. The panel is found again (every 0.5 s) when
  the screen opens: the class's **live** instance (`/Engine/Transient.` path), not the Blueprint's template widgets
  in `/Game/UI/Screens/...` which `ue_find_first_of` returns first. Blueprint classes (`*_WBP_C`) are not `Class`
  objects: `ue_find_any_class` (new in ue.c) finds them, and only once the menus loaded them.
- Picker: the deck builder's `CardPaginator_WBP_C` (5 x 2 `AnotherGameplayCard_WBP_C` a page, its own Prev/Next):
  `SetEquippedCards(TArray<FDataTableRowHandle>)` then `SetCards(...)` (the marks follow a new layout), handles built
  in a GMalloc buffer; `GetNumVisibleCards` / `GetVisibleCard(i)` -> `IsHovered` -> `GetCardRowHandle` for the click,
  then `SetEquippedCards` + `SetCards` again and `NextPage` x the page it was on. Backdrop: the deck editor's
  `BackgroundBlur_WBP_C`. Search: an `EditableTextBox`, `GetText` -> `Conv_TextToString` each frame. The card list:
  `PlayerCards_MASTER_DT`'s RowMap through `cheats_card_table_each` with the `RowStruct`'s `Hidden`,
  `AllowedForDeckbuilding`, `DisabledForPvE` fields: deck-buildable cards and the `Hero*Ability` passives (203).
  Both Blueprint classes exist only after the deck editor was opened once in the session.
- Every action is the chat command (`rules baseline add|remove <row>`, `rules free_burncards on|off`): one
  permission check, one ini writer, one notice, for the tab, the chat and this screen.

### Live (2026-10-23)
Panel built (5 buttons) on the first try once the two lookups above were fixed; clicks register (a card removed per
click), the picker shows 203 cards in the game's own card art, a click toggles a card and the mark follows at once.
Found on the way: `ue_find_class` misses Blueprint classes (silent NULLs), the archetype widgets shadow the live
screen, UMG reflection resolves late (as hud.c knew), the dev `peek` command crashes the game on a bad address
(never run it with a guessed argument).

## 4. Purchasable card stands in one place (`intel.c`, `intel_cards=off|saferoom|treasure`)

The game's Loot Spawn System (LSS, server, one pass per map: `LSS Init Rooms` -> `Selected - <item tag> ... <spawner>`
-> `SpawnItems - Spawned=N`, string xrefs 0x14190e043 / 0x141913d32 / 0x141910f9d) hands every level-placed loot
spawner an item. The card items (`/Game/Items/Tables/Cards_DT`, a GuidDataTable the modkit's `dt` can't read yet;
its import list names the classes) are actors: the free pickups `FreeCardDraw_BP`, `Card_DeckDraw_Pickup_BP`
(`Items.Cards.RandomCard`, the "intel files", `IntelFiles_Count_1` spawners) and `CardShrine_Basic_BP`
(`Items.Cards.Basic`, a free random card), and the stands a player buys a card from: `CardShrine_WildCard_BP`,
`Shrine_ScavengerCard_BP`, `Shrine_UpgradeCard_BP`, `CardShrine_Legendary_BP` (`Cost`). One shrine spawner sits in the
map's treasure room (`LootSpawner_Cards_BP3_2` under `RoomVolume_Treasure1` in Evansburgh A); the others anywhere.

### The rule
Host, no hook: from the tick, in a mission, twice a second, every shrine actor of the current world (class name
`*Shrine*_BP_C`, not `PVP`, not `Shrine_Basic`; `cheats_in_world` covers the streamed `_Loot` sublevel) that was not
moved yet is teleported with the game's `K2_SetActorTransform` (bTeleport):
- `saferoom`: a row 1.4 m in front of the start saferoom's vendor (`SafeRoomVendor_BP_C` nearest to the host's hero),
  1.1 m apart alternating right/left, facing as the vendor does (the vendor's quaternion -> forward/right axes).
  Defaults tuned live (`intel layout 140 110 95`): the first try (0, 180, 110) put the row through the crates
  behind the vendor.
- `treasure`: a row on the floor through the centre of `RoomVolume_Treasure*` along its longer side (`GetActorBounds`
  origin/extent; floor = origin.z - extent.z + 10). A map without such a volume keeps the game's placement.
One log line per stand, one chat notice per map. The LSS spawns before the heroes can move, so the stands are in
place before anyone sees them; a client gets the replicated move like any other.

### Live (2026-10-25, Evansburgh A "Resurgence", host + 3 bots)
`intel list`: 1 vendor, 14 `LootSpawner_Holdout_Crate_Card_DeckDraw` pickups, 4 shrines (3 WildCard, 1 Basic).
`saferoom`: all four moved beside the vendor (log `intel: CardShrine_WildCard_BP_C_... moved (29235 1085 960) ->
(28028 -470 160) beside SafeRoomVendor_BP_2`); Zeuell saw them in a line level with the vendor, one inside the crate
pile; re-placed live 1.4 m in front: all reachable and buyable ("HOLD TO BUY 500"). The Basic stand is the free one
(his read: "1 random intel card (generic)"), so the class is excluded from then on.

### Placement rules (second pass, same day)
- Every spot goes through `NavigationSystemV1::K2_ProjectPointToNavigation` (extent slack+40 / 160): no walkable
  point within the slack (35 by the vendor, 30 in the room) = refused; the projected point is the floor. The
  saferoom row then tries the same spot 60/120/180 further out, then one gap further to the side. The room takes
  the free navmesh spots nearest its centre on a 60 grid, `lay_gap` apart.
- `RoomVolume` reflects only `Type` (UInt32 @728), `Group` (@736), `AssociatedActors`; `IsRoomEnabled` is not a
  UFunction and the LSS's room status (`ERoomVolumeStatus`) is not reflected (`LootSpawnSubsystem` has only the
  tables/mods arrays). Two treasure volumes: the one with the most `ItemPickup` actors (the class every spawned
  pickup and shrine derives from: `CardShrine_WildCard_BP_C` -> `CardShrine_VendorBase_BP_C` -> `ItemPickupBase_BP_C`
  -> `ItemPickup_BP_C` -> `ItemPickup`) inside its bounds is the one the loot pass filled.
- Live, Evansburgh A: `treasure` put 3-4 Wild Card stands on the floor around the room centre (24699 2168, extent
  408 x 237 x 212), 120 apart; the room's own card spawner's stand among them. Dev `props <object>` (poststats.c)
  lists any object's reflected properties with values.
