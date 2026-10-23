// Host: let remote players play burn cards, and charge each card to the profile of the player who played it.
// See docs/investigations/burn-cards.md.
//
// Retail flow: the client's start-saferoom card UI lists the burn cards in the client's OWN profile and calls
// GobiPlayerController::ServerPlayBurnCard(card row). On the host that ends in GCM::PlayBurnCard (0x141776EA0), which
//   1. requires PPC::GetConsumableQuantity(player's profile component, product) > 0. Offline that reads the LocalPlayer's
//      PlayerProfileSettings; a remote controller has none, so it is 0 and the play is silently dropped;
//   2. on success queues the card in the player's slot (+0x78) under the key slot+0x88 = PPC.HydraPublicId.
// On leaving the start saferoom, GCM's OnSafeRoomStateChanged charge (0x141768A30) groups the queued cards by that key,
// finds a profile component with FindPPCByHydraId (0x141BBEB10: first controller whose PlayerState.UserIds.HydraPublicId
// matches) and issues AdjustConsumableQuantity(-1). For a remote player rewards.c forwards that to the client.
// The key is unreliable here: the host's copy of a remote PPC's HydraPublicId is sometimes empty (charge dropped), and
// in same-account tests every player has the same id, so the charge merges and lands on whichever controller comes
// first (the host's profile).
//
// Fix, three narrow hooks:
//   - PlayBurnCard: remember which controller is playing; afterwards, if a card was queued, replace the slot's key with
//     a synthetic one ("b4bcoop.burn.<n>") bound to that controller.
//   - GetConsumableQuantity: during a remote player's PlayBurnCard, a 0 from the host's (missing) profile becomes 1.
//     Same trust model as cards.c: the client UI only offers cards its own profile holds.
//   - FindPPCByHydraId: resolve our synthetic keys to the bound controller's profile component; anything else native.
// The charge itself stays native (AdjustConsumableQuantity through ExecuteCommand), so a remote player's charge reaches
// their client exactly once via rewards.c, and nothing is deducted from the host's profile on their behalf.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "MinHook.h"
#include "ue.h"
#include "log.h"
#include "cmds.h"

// void GCM::PlayBurnCard(UGameplayCardManager*, AGobiPlayerState*, const FDataTableRowHandle* card)
#define ADDR_PLAYBURNCARD VA(0x141776EA0ull)
static const uint8_t SIG_PLAY[] = {0x48,0x89,0x7c,0x24,0x20,0x55,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57,0x48,0x8d,
                                   0x6c,0x24,0xc9,0x48,0x81,0xec,0xa0,0x00,0x00,0x00,0x33,0xff,0x4d,0x8b,0xe0,0x89};
// int32 UGobiPlayerProfileComponent::GetConsumableQuantity(const FDataTableRowHandle& product)  (acquired - spent, >= 0)
#define ADDR_GETQTY VA(0x141BC2A00ull)
static const uint8_t SIG_GETQTY[] = {0x48,0x89,0x5c,0x24,0x10,0x57,0x48,0x83,0xec,0x20,0x48,0x8b,0xfa,0x48,0x8b,0xd9,
                                     0x48,0x85,0xc9,0x74,0x42,0x48,0x8b,0x01,0xff,0x90,0x50,0x01,0x00,0x00,0x48,0x85};
// UGobiPlayerProfileComponent* FindPPCByHydraId(UObject* worldContext, const FString& id)
#define ADDR_FINDPPC VA(0x141BBEB10ull)
static const uint8_t SIG_FINDPPC[] = {0x48,0x89,0x5c,0x24,0x08,0x48,0x89,0x6c,0x24,0x10,0x48,0x89,0x74,0x24,0x18,0x57,
                                      0x41,0x56,0x41,0x57,0x48,0x83,0xec,0x20,0x4c,0x8b,0xf2,0x48,0x85,0xc9,0x0f,0x84};
// FString& FString::operator=(const FString&) — used so the game's allocator owns the key we write into the slot
#define ADDR_FSTRING_ASSIGN VA(0x140BAE580ull)
static const uint8_t SIG_ASSIGN[] = {0x40,0x53,0x48,0x83,0xec,0x20,0x48,0x8b,0xd9,0x48,0x3b,0xca,0x74,0x59};

// Layout assumptions, checked against the code that uses them:
static const struct { uint64_t va; uint8_t n; uint8_t b[16]; } LAYOUT[] = {
    // PlayBurnCard: slot = GCM+0x298 array, stride 0xD8 (mov rbx,[r13+298h]; movsxd rcx,[r13+2A0h]; imul rdx,rcx,0D8h)
    {0x141776F13, 14, {0x49,0x8b,0x9d,0x98,0x02,0x00,0x00,0x49,0x63,0x8d,0xa0,0x02,0x00,0x00}},
    {0x141776F21, 7,  {0x48,0x69,0xd1,0xd8,0x00,0x00,0x00}},
    // PlayBurnCard: queue for the charge: slot+0x78 array (Num at +0x80)
    {0x1417771DE, 7,  {0x4c,0x63,0xb3,0x80,0x00,0x00,0x00}},
    // PlayBurnCard: key: slot+0x88 = PPC.HydraPublicId (lea rdx,[r15+1A8h]; lea rcx,[rbx+88h])
    {0x14177722C, 14, {0x49,0x8d,0x97,0xa8,0x01,0x00,0x00,0x48,0x8d,0x8b,0x88,0x00,0x00,0x00}},
    // PlayBurnCard: the quantity check calls GetConsumableQuantity
    {0x141777090, 8,  {0x49,0x8b,0xcf,0xe8,0x68,0xb9,0x44,0x00}},
    // charge: key = slot+0x88 (r12 = slot+0x90; lea r9,[r12-8] below), lookup = FindPPCByHydraId
    {0x141768D98, 7,  {0x4c,0x8d,0xa1,0x90,0x00,0x00,0x00}},
    {0x141769202, 8,  {0x48,0x8b,0xd6,0xe8,0x06,0x59,0x45,0x00}},
};
#define SLOT_SIZE       0xD8
#define SLOT_QUEUE(s)   ((TArray *)((char *)(s) + 0x78))   // cards to charge on leaving the start saferoom (unreflected)
#define SLOT_KEY(s)     ((FString *)((char *)(s) + 0x88))  // hydra id the charge is looked up by (unreflected)
#define COMP_OWNER(c)   (*(UObject **)((char *)(c) + 0xD8)) // UActorComponent::OwnerPrivate

typedef void (*PlayFn)(UObject *gcm, UObject *ps, void *card);
typedef int32_t (*GetQtyFn)(UObject *ppc, void *product);
typedef UObject *(*FindPPCFn)(UObject *ctx, FString *id);
typedef FString *(*AssignFn)(FString *dst, const FString *src);
static PlayFn orig_play;
static GetQtyFn orig_getqty;
static FindPPCFn orig_findppc;

static UClass *cls_pc, *cls_lp;
static int32_t off_slots = -1;
static UObject *playing_remote, *playing_ps;   // remote PlayerController / its PlayerState inside PlayBurnCard
static int nlog;

// Synthetic charge keys: "b4bcoop.burn.<n>" -> the controller that played the card.
#define KEY_PREFIX L"b4bcoop.burn."
#define NKEYS 64
static struct { UObject *pc; int32_t idx; int remote; char who[80]; } keys[NKEYS];   // who: admin_ps_key of the player
static int next_key;

static UObject *human_pc(UObject *ps, int *remote) {
    if (!cls_pc) cls_pc = ue_find_class("PlayerController");
    if (!cls_lp) cls_lp = ue_find_class("LocalPlayer");
    UObject *pc = ps && cls_pc && cls_lp ? ue_get_ptr(ps, "Owner") : NULL;
    if (!pc || !ue_is_a(pc, cls_pc)) return NULL;
    UObject *player = ue_get_ptr(pc, "Player");
    if (!player) return NULL;
    *remote = !ue_is_a(player, cls_lp);   // UNetConnection => a remote client's controller
    return pc;
}

static const char *fstr(const FString *s, char *buf, size_t len) {
    size_t k = 0;
    for (int i = 0; s && s->data && i < s->num && s->data[i] && k + 1 < len; i++) buf[k++] = s->data[i] < 128 ? (char)s->data[i] : '?';
    buf[k] = 0;
    return buf;
}

static void play_detour(UObject *gcm, UObject *ps, void *card) {
    int remote = 0;
    UObject *pc = human_pc(ps, &remote);
    if (off_slots < 0 && gcm) off_slots = ue_prop_offset(gcm, "PlayerActiveGameplayCardDataArray");
    TArray *slots = off_slots >= 0 && gcm ? (TArray *)((char *)gcm + off_slots) : NULL;
    int n = slots ? slots->num : 0;
    int32_t before[16];
    if (n > 16) n = 16;
    for (int i = 0; i < n; i++) before[i] = SLOT_QUEUE((char *)slots->data + i * SLOT_SIZE)->num;

    playing_remote = pc && remote ? pc : NULL;
    playing_ps = ps;
    orig_play(gcm, ps, card);
    playing_remote = NULL;
    if (!pc || !slots) return;

    char row[128], id[128];
    ue_name(*(FName *)((char *)card + 8), row, sizeof row);
    for (int i = 0; i < n && i < slots->num; i++) {
        char *slot = (char *)slots->data + i * SLOT_SIZE;
        if (SLOT_QUEUE(slot)->num <= before[i]) continue;
        // queued: bind the slot's charge to this controller
        int k = next_key++ % NKEYS;
        keys[k].pc = pc; keys[k].idx = U_INDEX(pc); keys[k].remote = remote;
        admin_ps_key(ps, keys[k].who, sizeof keys[k].who);
        static wchar_t wkey[32];
        int len = _snwprintf(wkey, 31, KEY_PREFIX L"%d", k);
        wkey[31] = 0;
        FString src = {wkey, len + 1, len + 1};   // Num counts the terminator
        fstr(SLOT_KEY(slot), id, sizeof id);
        ((AssignFn)ADDR_FSTRING_ASSIGN)(SLOT_KEY(slot), &src);
        LOG("burncards: %s player played %s (slot %d); charge key '%s' -> b4bcoop.burn.%d",
            remote ? "remote" : "local", row, i, id, k);
        return;
    }
    if (remote && nlog++ < 20) LOG("burncards: remote player's %s was rejected by the host (already played / not playable)", row);
}

static int32_t getqty_detour(UObject *ppc, void *product) {
    int32_t q = orig_getqty(ppc, product);
    if (q <= 0 && playing_remote && ppc && (COMP_OWNER(ppc) == playing_remote || COMP_OWNER(ppc) == playing_ps)) {
        LOG("burncards: trusting remote player's quantity (host has no profile for them)");
        q = 1;
    }
    return q;
}

// A remote player who disconnected after playing a card and came back before the charge has a new controller: find
// it by the player key the card was played under (exactly one remote match; local copies in tests share one id).
static UObject *rejoined(const char *who) {
    UObject **pa; int n = admin_player_array(&pa), hits = 0;
    UObject *found = NULL;
    char key[80];
    for (int i = 0; who[0] && i < n; i++) {
        int remote = 0;
        UObject *pc = human_pc(pa[i], &remote);
        if (!pc || !remote) continue;
        admin_ps_key(pa[i], key, sizeof key);
        if (!strcmp(key, who)) { found = pc; hits++; }
    }
    return hits == 1 ? found : NULL;
}

static UObject *findppc_detour(UObject *ctx, FString *id) {
    size_t pl = wcslen(KEY_PREFIX);
    if (!id || !id->data || id->num < (int)pl + 1 || wcsncmp(id->data, KEY_PREFIX, pl)) return orig_findppc(ctx, id);
    int k = (int)wcstol(id->data + pl, NULL, 10);
    if (k < 0 || k >= NKEYS) return NULL;
    if (rules_free_burncards()) {   // host rule (rules.c): no profile to charge = the game drops the charge
        char who[16]; snprintf(who, sizeof who, "b4bcoop.burn.%d", k);
        rules_note_free_charge(who);
        return NULL;
    }
    UObject *pc = keys[k].pc;
    // a remote player's old controller can outlive its connection until the next GC: no connection = gone
    int live = pc && ue_object_at(keys[k].idx) == pc && (!keys[k].remote || ue_get_ptr(pc, "Player"));
    UObject *ppc = live ? ue_get_ptr(pc, "GobiPlayerProfileComponent") : NULL;
    const char *how = "";
    if (!ppc && keys[k].remote && (pc = rejoined(keys[k].who))) {
        ppc = ue_get_ptr(pc, "GobiPlayerProfileComponent");
        how = " (rejoined since the card was played)";
    }
    LOG("burncards: charging b4bcoop.burn.%d -> %s player's profile%s", k, keys[k].remote ? "remote" : "local",
        ppc ? how : ": controller gone, charge dropped");
    return ppc;
}

static int hook(uint64_t va, const uint8_t *sig, size_t n, void *detour, void **orig, const char *what) {
    if (memcmp((void *)va, sig, n)) { LOG("burncards: %s signature mismatch", what); return -1; }
    if (MH_CreateHook((void *)va, detour, orig) != MH_OK || MH_EnableHook((void *)va) != MH_OK) {
        LOG("burncards: %s hook failed", what);
        return -1;
    }
    return 0;
}

int burncards_init(void) {
    const char *e = getenv("B4BCOOP_NO_BURNCARDS");
    if (e && *e && *e != '0') { LOG("burncards: disabled by B4BCOOP_NO_BURNCARDS"); return 0; }
    for (size_t i = 0; i < sizeof LAYOUT / sizeof LAYOUT[0]; i++)
        if (memcmp((void *)VA(LAYOUT[i].va), LAYOUT[i].b, LAYOUT[i].n)) {
            LOG("burncards: layout check failed at %llx", (unsigned long long)LAYOUT[i].va);
            return -1;
        }
    if (memcmp((void *)ADDR_FSTRING_ASSIGN, SIG_ASSIGN, sizeof SIG_ASSIGN)) {
        LOG("burncards: FString assign signature mismatch");
        return -1;
    }
    // FindPPC first: a queued synthetic key must always resolve
    if (hook(ADDR_FINDPPC, SIG_FINDPPC, sizeof SIG_FINDPPC, (void *)findppc_detour, (void **)&orig_findppc, "FindPPCByHydraId") ||
        hook(ADDR_GETQTY, SIG_GETQTY, sizeof SIG_GETQTY, (void *)getqty_detour, (void **)&orig_getqty, "GetConsumableQuantity") ||
        hook(ADDR_PLAYBURNCARD, SIG_PLAY, sizeof SIG_PLAY, (void *)play_detour, (void **)&orig_play, "PlayBurnCard"))
        return -1;
    LOG("burncards: PlayBurnCard / GetConsumableQuantity / FindPPCByHydraId hooked");
    return 0;
}
