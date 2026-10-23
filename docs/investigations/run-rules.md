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

## 2. Baseline cards (`baseline_cards=`): see §2 once implemented.
