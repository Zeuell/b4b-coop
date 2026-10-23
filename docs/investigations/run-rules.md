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

### Open questions (live test)
- Does the game grant the deck again on every chapter (then the baseline is re-added per chapter: fine, idempotent),
  or does the run snapshot carry the active cards over (then once)? Either way the cards persist.
- The character-select / loadout screens list the slot's active cards: the baseline should show up there after the
  grant, with the card's own icon and text (nothing new is drawn).
- Team-once cards (`MaxTeamCount`) as a baseline: the game's add may refuse the second copy; the log shows it.

### Verification (planned)
Host sets `Scavenger_Weapons,Talent_ShareTheWealth,Talent_DownInFront` (Run tab or `/rules baseline add ...`), starts
a mission with one client: host log `GrantLoadoutCardsForSlot` for each slot followed by `rules: baseline cards for
<name>: 3 added: ...` (bots included), the cards in every survivor's card list, one chat notice, `DrawCount 0` on the
client as before (the deck grant itself is unchanged). Chapter 2: cards still active. A client hot-joining mid-mission
gets them with its deck.
