// rules: the host's run rules for a private session, the `~` window's Run tab and b4bcoop.ini keys. Host-side only:
// the listen host runs the game's card and reward code for every player, so a rule set here applies to the whole
// lobby, and nothing is sent to the clients (no protocol bump). Clients see the tab greyed out with the reason.
// docs/investigations/run-rules.md.
//   free_burncards=1   burn cards are never charged: the charge the game issues on leaving the start saferoom is
//                      dropped for every player, host and clients alike (burncards.c asks rules_free_burncards()
//                      when the game looks up the profile to charge). Playing still needs owning at least one of the
//                      card (the client's own UI lists what its profile holds); it just isn't used up.
//   baseline_cards=<row>,<row>,...   cards every survivor gets on top of their own deck: when the game grants a
//                      slot its deck (GCM::GrantLoadoutCardsForSlot, the 15 deck cards; hot-joiners and bots too),
//                      each listed card that the slot does not have yet is added with the game's own
//                      AddGameplayCardToSlotByRowHandle (no ownership check: a joiner needs to own nothing).
//                      Rows of PlayerCards_MASTER_DT, e.g. Scavenger_Weapons,Talent_ShareTheWealth,Talent_DownInFront.
//   baseline_cards_bots=0            leave the bots out (default 1: the whole team).
// Chat: /rules (state), /rules free_burncards on|off, /rules baseline add|remove <card>|clear|list (CMD_HOST), the
// same as the tab's controls.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include "MinHook.h"
#include "ue.h"
#include "log.h"
#include "cmds.h"
#include "overlay.h"

// ---- free burn cards ----
static int free_burncards;          // ini free_burncards=1
static ULONGLONG free_noticed_at;   // last "burn cards are free" notice to the players (one per batch of charges)

int rules_free_burncards(void) { return free_burncards; }

// burncards.c: a charge was dropped because of the rule (host, game thread); the players hear it once per batch
void rules_note_free_charge(const char *row) {
    ULONGLONG now = GetTickCount64();
    LOG("rules: free burn cards: %s not charged", row);
    if (now - free_noticed_at < 10000) return;
    free_noticed_at = now;
    admin_notice("burn cards are free in this session (host rule): nobody is charged for the cards played");
}

// ---- baseline cards ----
// void UGameplayCardManager::GrantLoadoutCardsForSlot(UGameplayCardManager*, AGobiPlayerState*): the slot data of the
// state (0x14177a950(gcm, ps, 0): GCM+0x298 array, stride 0xD8) must not have HasReceivedLoadoutCards (+0x59) yet;
// then every deck card passes OwnsCard (cards.c) and lands in the slot's ActiveHeroCards (+0x10, 0x28 each, row FName
// at +8), the leftovers become bonus draws; +0x59 is set. docs/investigations/card-draft.md.
#define ADDR_GRANT VA(0x141769690ull)
static const uint8_t SIG_GRANT[] = {0x40,0x55,0x41,0x54,0x41,0x56,0x41,0x57,0x48,0x8d,0x6c,0x24,0xc1,0x48,0x81,0xec,
                                    0xf8,0x00,0x00,0x00,0x45,0x33,0xc0,0x4c,0x8b,0xf2,0x4c,0x8b,0xf9,0xe8};
// PlayerActiveGameplayCardData* GCM::SlotDataFor(UGameplayCardManager*, AGobiPlayerState*, bool bCreate)
#define ADDR_SLOTDATA VA(0x14177a950ull)
static const uint8_t SIG_SLOTDATA[] = {0x48,0x89,0x5c,0x24,0x20,0x55,0x57,0x41,0x55,0x41,0x56,0x41,0x57,0x48,0x8d,0x6c,
                                       0x24,0x90,0x48,0x81,0xec,0x70,0x01,0x00};
static const struct { uint64_t va; uint8_t n; uint8_t b[8]; } LAYOUT[] = {
    {0x1417696ad, 5, {0xe8,0x9e,0x12,0x01,0x00}},          // Grant calls SlotDataFor (0x14177a950)
    {0x1417696f6, 4, {0x80,0x78,0x59,0x00}},               // cmp byte [slot+0x59], 0: HasReceivedLoadoutCards
    {0x14177a978, 7, {0x4c,0x8d,0xb1,0x98,0x02,0x00,0x00}},// SlotDataFor: lea r14, [gcm+0x298]
    {0x14177a986, 7, {0x48,0x69,0xfb,0xd8,0x00,0x00,0x00}},// SlotDataFor: imul rdi, rbx, 0xD8 (stride)
};
#define SLOT_ID(s)       (*(int32_t *)(s))                   // FPlayerSlotIndex SlotId (+0) and Team (+4): SlotDataFor's keys
#define SLOT_TEAM(s)     (*((uint8_t *)(s) + 4))
#define SLOT_ACTIVE(s)   ((TArray *)((char *)(s) + 0x10))   // ActiveHeroCards: FActiveGameplayCard, 0x28 each
#define SLOT_GRANTED(s)  (*((uint8_t *)(s) + 0x59))         // HasReceivedLoadoutCards
#define ACTIVE_ROW(a, i) (*(FName *)((char *)(a)->data + (size_t)(i) * 0x28 + 8))

typedef void (*GrantFn)(UObject *gcm, UObject *ps);
typedef void *(*SlotDataFn)(UObject *gcm, UObject *ps, uint8_t create);
static GrantFn orig_grant;
static int grant_hooked;

#define MAX_BASE 16
static char base_rows[MAX_BASE][64];   // ini baseline_cards= (row names, as written)
static int nbase;
static int base_bots = 1;              // ini baseline_cards_bots
static UObject *noticed_world;         // the players were told this map
static int base_added_total;

static void base_parse(const char *v) {
    nbase = 0;
    char buf[1024];
    snprintf(buf, sizeof buf, "%s", v ? v : "");
    for (char *t = strtok(buf, ", \t;"); t && nbase < MAX_BASE; t = strtok(NULL, ", \t;")) snprintf(base_rows[nbase++], sizeof base_rows[0], "%s", t);
}
static void base_join(char *out, size_t n) {
    out[0] = 0;
    for (int i = 0; i < nbase; i++) { size_t l = strlen(out); snprintf(out + l, n - l, "%s%s", i ? "," : "", base_rows[i]); }
}
static int base_index(const char *row) {
    for (int i = 0; i < nbase; i++) if (!_stricmp(base_rows[i], row)) return i;
    return -1;
}

int rules_baseline_count(void) { return nbase; }
int rules_baseline_rows(char (*out)[64], int max) {
    int n = nbase < max ? nbase : max;
    for (int i = 0; i < n; i++) snprintf(out[i], 64, "%s", base_rows[i]);
    return n;
}
int rules_baseline_has(const char *row) { return base_index(row) >= 0; }

static int slot_has(void *slot, FName row) {
    TArray *a = SLOT_ACTIVE(slot);
    for (int i = 0; a->data && i < a->num; i++) { FName r = ACTIVE_ROW(a, i); if (r.idx == row.idx && r.num == row.num) return 1; }
    return 0;
}
static int ps_is_bot(UObject *ps) {
    static UClass *cls_bot;
    if (!cls_bot) cls_bot = ue_find_class("BotController");
    UObject *owner = ps ? ue_get_ptr(ps, "Owner") : NULL;
    return owner && cls_bot && ue_is_a(owner, cls_bot);
}

static void grant_detour(UObject *gcm, UObject *ps) {
    void *before = gcm && ps ? ((SlotDataFn)ADDR_SLOTDATA)(gcm, ps, 0) : NULL;
    int had = before ? SLOT_GRANTED(before) : 1;
    orig_grant(gcm, ps);
    if (!nbase || had) return;                    // nothing to add, or the deck was granted earlier (a re-entry)
    void *slot = ((SlotDataFn)ADDR_SLOTDATA)(gcm, ps, 0);
    if (!slot || !SLOT_GRANTED(slot)) return;    // the game granted nothing (not in a mission, no slot)
    if (!base_bots && ps_is_bot(ps)) return;
    // the state's UPlayerSlot: a bot's state has no OwnedPlayerSlot and no pawn yet at the mission start's
    // GrantAllLoadoutCards, so the slot data's own index finds it in the PlayerSlotManager
    UObject *pslot = cheats_slot_by_index(SLOT_ID(slot), SLOT_TEAM(slot));
    if (!pslot) pslot = cheats_slot_for_ps(ps);
    char who[64], list[512] = "";
    admin_ps_name(ps, who, sizeof who);
    if (!pslot) { LOG("rules: baseline cards: no player slot %d/%d for %s", SLOT_ID(slot), SLOT_TEAM(slot), who); return; }
    int added = 0, missing = 0;
    for (int i = 0; i < nbase; i++) {
        CardRef c;
        if (!cheats_card_find(gcm, base_rows[i], &c)) { missing++; LOG("rules: baseline card '%s': no such card", base_rows[i]); continue; }
        if (slot_has(slot, c.row)) continue;      // already in their deck (or added before)
        if (cheats_card_add(gcm, pslot, &c)) {
            added++;
            size_t l = strlen(list); snprintf(list + l, sizeof list - l, "%s%s", l ? ", " : "", base_rows[i]);
        }
    }
    base_added_total += added;
    LOG("rules: baseline cards for %s (slot %d): %d added%s%s (%d already had, %d unknown)", who, SLOT_ID(slot), added,
        added ? ": " : "", list, nbase - added - missing, missing);
    UObject *w = ue_world();
    if (added && w && noticed_world != w) {
        noticed_world = w;
        char all[512]; base_join(all, sizeof all);
        admin_notice("host rule: baseline cards for every survivor this run: %s", all);
    }
}

// ---- ini ----
static void ini_pair(const char *k, const char *v, void *ctx) {
    (void)ctx;
    if (!strcmp(k, "free_burncards")) free_burncards = atoi(v) != 0;
    else if (!strcmp(k, "baseline_cards")) base_parse(v);
    else if (!strcmp(k, "baseline_cards_bots")) base_bots = atoi(v) != 0;
}

int rules_live(const char *key, const char *val) {
    if (!strcmp(key, "free_burncards")) {
        int was = free_burncards;
        free_burncards = val && atoi(val) != 0;
        if (was != free_burncards) LOG("rules: free burn cards %s", free_burncards ? "on" : "off");
    } else if (!strcmp(key, "baseline_cards")) {
        base_parse(val);
        char all[512]; base_join(all, sizeof all);
        LOG("rules: baseline cards: %s", nbase ? all : "none");
    } else if (!strcmp(key, "baseline_cards_bots")) base_bots = !val || atoi(val) != 0;
    else return 0;
    return 1;
}

static void base_save(void) {
    char all[512]; base_join(all, sizeof all);
    cmds_ini_apply("baseline_cards", nbase ? all : NULL, 1);
}

// ---- chat /rules ----
void rules_slash(char *rest, Out *o) {
    char *k = rest ? strtok(rest, " ") : NULL, *v = k ? strtok(NULL, "") : NULL;
    if (k && !strcmp(k, "free_burncards") && v) {
        int on = !strcmp(v, "on") || !strcmp(v, "1");
        if (!on && strcmp(v, "off") && strcmp(v, "0")) { out_printf(o, "usage: /rules free_burncards on|off\n"); return; }
        cmds_ini_apply("free_burncards", on ? "1" : "0", 1);
        if (!on) free_noticed_at = 0;
        out_printf(o, "free burn cards %s (saved in b4bcoop.ini)\n", on ? "on" : "off");
        admin_notice(on ? "host rule: burn cards are free this session (never used up)" : "host rule: burn cards are charged again");
        return;
    }
    if (k && !strcmp(k, "intel")) { intel_slash(v, o); return; }
    if (k && !strcmp(k, "baseline")) {
        char *sub = v ? strtok(v, " ") : NULL, *arg = sub ? strtok(NULL, "") : NULL;
        if (sub && !strcmp(sub, "clear")) { nbase = 0; base_save(); out_printf(o, "baseline cards: none\n"); return; }
        if (sub && !strcmp(sub, "bots") && arg) { int on = !strcmp(arg, "on") || !strcmp(arg, "1"); cmds_ini_apply("baseline_cards_bots", on ? "1" : "0", 1); out_printf(o, "baseline cards for bots %s\n", on ? "on" : "off"); return; }
        if (sub && (!strcmp(sub, "add") || !strcmp(sub, "remove")) && arg) {
            char row[64];
            snprintf(row, sizeof row, "%s", arg);
            UObject *gcm = cheats_card_manager();
            CardRef c;
            if (gcm && cheats_card_find(gcm, arg, &c)) ue_name(c.row, row, sizeof row);   // a shown name -> its row
            else if (!strcmp(sub, "add") && gcm) { out_printf(o, "no card matches '%s' (/rules baseline list [filter])\n", arg); return; }
            int at = base_index(row);
            if (!strcmp(sub, "add")) {
                if (at >= 0) { out_printf(o, "%s is a baseline card already\n", row); return; }
                if (nbase >= MAX_BASE) { out_printf(o, "at most %d baseline cards\n", MAX_BASE); return; }
                snprintf(base_rows[nbase++], sizeof base_rows[0], "%s", row);
            } else {
                if (at < 0) { out_printf(o, "%s is not a baseline card\n", row); return; }
                for (int i = at; i + 1 < nbase; i++) strcpy(base_rows[i], base_rows[i + 1]);
                nbase--;
            }
            base_save();
            char all[512]; base_join(all, sizeof all);
            out_printf(o, "baseline cards: %s (from the next mission start / join)\n", nbase ? all : "none");
            return;
        }
        if (!sub || !strcmp(sub, "list")) {
            char all[512]; base_join(all, sizeof all);
            out_printf(o, "baseline cards: %s%s\n", nbase ? all : "none", base_bots ? "" : " (bots left out)");
            return;
        }
        out_printf(o, "usage: /rules baseline add|remove <card> | clear | list | bots on|off\n");
        return;
    }
    if (k) { out_printf(o, "usage: /rules [free_burncards on|off | baseline add|remove <card>|clear|list|bots on|off | intel off|saferoom]\n"); return; }
    char all[512]; base_join(all, sizeof all);
    out_printf(o, "run rules (host): free burn cards %s; baseline cards: %s%s; card stands: %s\n", free_burncards ? "on" : "off",
               nbase ? all : "none", base_bots || !nbase ? "" : " (bots left out)", intel_mode_name());
}

// ---- the ~ window's Run tab ----
// The card list (row + shown name) comes from the GameplayCardManager's AllCards; shown names need a reflected call per
// card, so it is built once per card manager, not every frame.
#define MAX_CARDS 512
static struct { char row[64], shown[64]; } cards[MAX_CARDS];
static int ncards;
static UObject *cards_gcm;
static void card_cb(const CardRef *c, const char *row, const char *shown, void *ctx) {
    (void)c; (void)ctx;
    if (ncards >= MAX_CARDS) return;
    snprintf(cards[ncards].row, sizeof cards[0].row, "%s", row);
    snprintf(cards[ncards].shown, sizeof cards[0].shown, "%s", shown[0] ? shown : row);
    ncards++;
}
static int card_cmp(const void *a, const void *b) { return _stricmp(((const char *)a) + 64, ((const char *)b) + 64); }
static void cards_refresh(void) {
    UObject *gcm = cheats_card_manager();
    if (!gcm || gcm == cards_gcm) return;
    ncards = 0;
    if (cheats_card_each(gcm, card_cb, NULL) < 0) return;
    qsort(cards, ncards, sizeof cards[0], card_cmp);
    cards_gcm = gcm;
}
static int has_ci(const char *hay, const char *needle) {   // case-insensitive substring
    size_t n = strlen(needle);
    for (; *hay; hay++) if (!_strnicmp(hay, needle, n)) return 1;
    return 0;
}
static const char *shown_of(const char *row) {
    for (int i = 0; i < ncards; i++) if (!_stricmp(cards[i].row, row)) return cards[i].shown;
    return row;
}
static void panel(void) {
    static char filter[64];
    ov_begin_perm(CMD_HOST);
    ov_heading("Burn cards");
    int fb = free_burncards;
    if (ov_checkbox("Burn cards are free##freeburn", &fb)) ov_run("rules free_burncards %s", fb ? "on" : "off");
    ov_tooltip("Host only. Burn cards played in the start saferoom are never used up: the charge the game makes when "
               "the team leaves the saferoom is dropped, for you and for everyone in your game. Players still need "
               "to own at least one of a card to play it. Saved in b4bcoop.ini (free_burncards).");

    ov_heading("Purchasable card stands");
    ov_text_dim("Where the card stands you buy cards from (not the free card pickups) are placed when a mission starts.");
    if (ov_radio("Where the game puts them##intel0", intel_mode() == 0)) ov_run("rules intel off");
    ov_same_line();
    if (ov_radio("In the start saferoom, by the vendor##intel1", intel_mode() == 1)) ov_run("rules intel saferoom");
    ov_same_line();
    if (ov_radio("In the treasure room##intel2", intel_mode() == 2)) ov_run("rules intel treasure");
    ov_tooltip("Host only. Right after the game places a map's loot, every purchasable card stand is moved: in a row in front "
               "of the start saferoom's vendor, or onto the floor of the map's treasure room (a map without one keeps "
               "the game's placement). The free card pickups stay where they are. Saved in b4bcoop.ini (intel_cards).");

    ov_heading("Baseline cards");
    ov_text_dim("Cards every survivor gets on top of their own deck, when a mission starts or they join one. "
                "Everyone is told. Saved in b4bcoop.ini (baseline_cards).");
    cards_refresh();
    if (!nbase) ov_text_dim_line("None.");
    for (int i = 0; i < nbase; i++) {
        ov_push_id(i);
        if (ov_button("Remove")) ov_run("rules baseline remove %s", base_rows[i]);
        ov_same_line();
        ov_text("%s", shown_of(base_rows[i]));
        ov_same_line();
        ov_text_dim_line("(%s)", base_rows[i]);
        ov_pop_id();
    }
    int bots = base_bots;
    if (ov_checkbox("Bots get them too##basebots", &bots)) ov_run("rules baseline bots %s", bots ? "on" : "off");
    if (!grant_hooked) ov_text_warn("Not available with this game version (the deck grant could not be hooked).");
    if (ncards) {
        ov_width(14);
        ov_input_text("Search##basefilter", filter, sizeof filter, "card name");
        int shown = 0;
        for (int i = 0; i < ncards && shown < 12; i++) {
            if (filter[0] && !has_ci(cards[i].shown, filter) && !has_ci(cards[i].row, filter)) continue;
            if (base_index(cards[i].row) >= 0) continue;
            ov_push_id(1000 + i);
            if (ov_button("Add")) ov_run("rules baseline add %s", cards[i].row);
            ov_same_line();
            ov_text("%s", cards[i].shown);
            ov_same_line();
            ov_text_dim_line("(%s)", cards[i].row);
            ov_pop_id();
            shown++;
        }
        if (!shown) ov_text_dim_line(filter[0] ? "No card matches." : "Every card is a baseline card.");
        else ov_text_dim_line("Type to narrow the list (first 12 shown).");
    } else ov_text_dim("The card list appears once a mission has loaded the cards (or type /rules baseline add <row> in the chat).");
    ov_text_dim("Rules apply to everyone in your game; nothing is sent to the others' saves. A joined player sees "
                "the host's rules in the chat notices.");
    ov_end_perm();
}

int rules_init(void) {
    cmds_ini_each(ini_pair, NULL);
    overlay_add_panel("Run", 55, panel);
    int ok = 1;
    for (size_t i = 0; i < sizeof LAYOUT / sizeof LAYOUT[0]; i++)
        if (memcmp((void *)VA(LAYOUT[i].va), LAYOUT[i].b, LAYOUT[i].n)) { LOG("rules: layout check failed at %llx", (unsigned long long)LAYOUT[i].va); ok = 0; }
    if (ok && (memcmp((void *)ADDR_GRANT, SIG_GRANT, sizeof SIG_GRANT) || memcmp((void *)ADDR_SLOTDATA, SIG_SLOTDATA, sizeof SIG_SLOTDATA))) {
        LOG("rules: GrantLoadoutCardsForSlot signature mismatch, baseline cards off"); ok = 0;
    }
    if (ok && (MH_CreateHook((void *)ADDR_GRANT, (void *)grant_detour, (void **)&orig_grant) != MH_OK ||
               MH_EnableHook((void *)ADDR_GRANT) != MH_OK)) { LOG("rules: GrantLoadoutCardsForSlot hook failed"); ok = 0; }
    grant_hooked = ok;
    char all[512]; base_join(all, sizeof all);
    LOG("rules: free burn cards %s; baseline cards %s%s%s", free_burncards ? "on (free_burncards=1)" : "off",
        nbase ? all : "none", base_bots ? "" : " (bots left out)", grant_hooked ? " (deck grant hooked)" : "");
    return ok ? 0 : -1;
}
