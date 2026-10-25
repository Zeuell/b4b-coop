// Opt-in, host-only cheats / sandbox (#14). Player reference: docs/commands-cheats.md.
//
// Rules:
//  - Every cheat is a chat command run by admin.c's dispatcher (`/god`, `/fly`, ...). Nothing works until the host types
//    `/cheats on`; `/cheats off`, or the next map change that lands in the menus turns them off. Fort Hope, missions
//    and the next chapter keep them on (both ways), but every map change resets their effects (god, fly,
//    noclip, ammo, speed, freeze, size, free camera; the map's own spawns and director changes go with it).
//  - Host only: the machine must be the server (listen host or standalone). A client's `/cheat ...` never leaves its
//    machine (chat.c) and gets "host only". admin.c enforces it from VERBS' permissions (CMD_HOST / CMD_CHEAT).
//  - Transparent: enabling/disabling, and every cheat that touches another player or the whole session, is announced to
//    every player with admin.c's host notice (ClientTeamMessage, our own type; the same path as /say).
//  - No cheat writes another player's save: cheats act on the live session, or on the issuing machine's OWN offline
//    profile (`/supply`, `/skulls`, `/burncards`, `/unlockall`: ExecuteCommand on its local profile component only,
//    never forwarded, nothing sent). Rewards, stats and achievements of a map flow to everyone as usual, cheats or not
//    (Dan's call: simpler, and a stray cheat can't ruin someone's run; through v0.6.0 a map with cheats on sent the
//    other players none, #30).
//  - Own-save cheats (#49) are for every player while the session has cheats on (CMD_OWNSAVE): the host with its
//    cheats on, or a client whose host said so. The host tells each remote player its state with a silent data line
//    (admin_data_to, "cheats on|off": on every toggle and 2 s / 15 s after it first sees that player in a map); the
//    client keeps it for that server connection only. A host on an older b4bcoop never says it, so its clients refuse:
//    no protocol bump (older clients ignore the data line; nothing else of host or client behavior changes).
//    A player in their own Fort Hope is the host: their own /cheats on. Joining someone turns one's own cheats off.
//
// How (the shipping build compiled out Gobi's own cheat execs: GobiPlayerController.God/Heal/GiveUnlock/... are empty,
// APlayerController::AddCheats is `ret` so no controller ever gets a CheatManager, and ACharacter::ClientCheatFly/
// Ghost/Walk_Implementation are empty):
//  - god: HealthComponent.PushDamageDisabled + PushDeathDisabled (counters the game itself uses), popped exactly once.
//  - heal / revive: HealthComponent.Heal, LifeStateComponent.Revive; ammo: ClipAmmoComponent.bInfiniteReserveAmmo on
//    every weapon; copper: InventoryComponent.AdjustCurrency; card: GameplayCardManager.AddGameplayCardToSlotByRowHandle.
//  - fly/noclip/walk: CharacterMovement.SetMovementMode + AActor.SetActorEnableCollision on the server.
//  - size/freecam/slomo/freeze: the engine's own UCheatManager (ChangeSize, ToggleDebugCamera, Slomo, PlayersOnly are
//    intact), constructed for the host's PlayerController with StaticConstructObject_Internal and stored in
//    PlayerController.CheatManager.
//  - world: GameDirector.TriggerHordeOnDirector / ForcePacingPhaseOnDirector / GobiSpawnAIFromClass (statics),
//    LifeStateComponent.Kill on ridden, MissionGameMode.OnMissionEnd (win/lose, as dev `endmission`).
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include "ue.h"
#include "log.h"
#include "cmds.h"
#include "overlay.h"

typedef FName *(*FNameCtorFn)(FName *self, const wchar_t *name, int find_type);
#define ADDR_FNAME_CTOR VA(0x1424BC8E0ull)
// UObject* StaticConstructObject_Internal(UClass*, UObject* Outer, FName, EObjectFlags, EInternalObjectFlags,
//   UObject* Template, bool bCopyTransientsFromClassDefaults, FObjectInstancingGraph*, bool bAssumeTemplateIsArchetype)
#define ADDR_SCO VA(0x1426E4840ull)
static const uint8_t SIG_SCO[] = {0x4c,0x89,0x44,0x24,0x18,0x53,0x55,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,
                                  0x57,0x48,0x81,0xec,0xa8,0x01,0x00,0x00};
typedef UObject *(*ScoFn)(UClass *, UObject *, FName, uint32_t, uint32_t, UObject *, uint8_t, void *, uint8_t);

// FDataTableRowHandle in this build: {UDataTable*, FName RowName, FString (unreflected display name)} = 0x20
typedef struct { UObject *table; FName row; FString display; } RowHandle;
#define DT_ROWMAP(t) ((TArray *)((char *)(t) + 0x38))   // UDataTable::RowMap, TMap<FName, uint8*>

// ---- small helpers ----
static void ascii_of(const FString *s, char *buf, size_t n) {
    size_t k = 0;
    for (int i = 0; s && s->data && i < s->num && s->data[i] && k + 1 < n; i++)
        buf[k++] = s->data[i] < 128 ? (char)s->data[i] : '?';
    buf[k] = 0;
}

static void fstring_set(FString *s, const char *utf8, wchar_t *storage, int cap) {
    int n = 0;
    for (; utf8[n] && n < cap - 1; n++) storage[n] = (wchar_t)(unsigned char)utf8[n];
    storage[n] = 0;
    s->data = storage; s->num = n + 1; s->max = cap;
}

static int alive(UObject *o, int32_t idx) { return o && idx >= 0 && ue_object_at(idx) == o; }
#define LIVE_FLAGS 0x30   // RF_ClassDefaultObject | RF_ArchetypeObject

// One reflected call: parameters by name into a zeroed block, then ProcessEvent.
typedef struct { UObject *obj; UFunction *fn; uint8_t p[1024]; } Call;
static int call_prep(Call *c, UObject *obj, const char *fname) {
    c->obj = obj;
    c->fn = obj && U_CLASS(obj) ? ue_find_function(U_CLASS(obj), fname) : NULL;
    if (!c->fn || UFN_PARMSSIZE(c->fn) > sizeof c->p) { if (obj) LOG("cheats: no usable %s", fname); return 0; }
    memset(c->p, 0, UFN_PARMSSIZE(c->fn));
    return 1;
}
static void *carg(Call *c, const char *name) { FField *f = ue_find_prop(c->fn, name); return f ? c->p + FP_OFFSET(f) : NULL; }
static void call_go(Call *c) { ue_process_event(c->obj, c->fn, c->p); }
#define SET(c, name, type, v) do { void *_a = carg(c, name); if (_a) *(type *)_a = (v); } while (0)

// no-argument call returning an object / float / bool
static UObject *call_obj(UObject *o, const char *fn) {
    Call c; if (!call_prep(&c, o, fn)) return NULL;
    call_go(&c);
    void *r = carg(&c, "ReturnValue");
    return r ? *(UObject **)r : NULL;
}
static float call_float(UObject *o, const char *fn) {
    Call c; if (!call_prep(&c, o, fn)) return 0;
    call_go(&c);
    void *r = carg(&c, "ReturnValue");
    return r ? *(float *)r : 0;
}
static int call_bool(UObject *o, const char *fn) {
    Call c; if (!call_prep(&c, o, fn)) return -1;
    call_go(&c);
    uint8_t *r = carg(&c, "ReturnValue");
    return r ? *r != 0 : -1;
}
static int call_void(UObject *o, const char *fn) { Call c; if (!call_prep(&c, o, fn)) return 0; call_go(&c); return 1; }

// native (/Script) classes never unload: look each one up once (ue_find_class scans every object)
static UClass *cls(const char *name) {
    static struct { const char *name; UClass *c; } cache[48];
    for (int i = 0; i < 48 && cache[i].name; i++) if (!strcmp(cache[i].name, name)) return cache[i].c;
    UClass *c = ue_find_class(name);
    for (int i = 0; i < 48 && c; i++) if (!cache[i].name) { cache[i].name = name; cache[i].c = c; break; }
    return c;
}
static UObject *class_cdo(const char *name) { UClass *c = cls(name); return c ? UC_CDO(c) : NULL; }

// ---- world ----
static UObject *game_state(void) { UObject *w = ue_world(); return w ? ue_get_ptr(w, "GameState") : NULL; }
static UObject *game_mode(void) { UObject *w = ue_world(); return w ? ue_get_ptr(w, "AuthorityGameMode") : NULL; }
static int in_mission(void) {
    UObject *gm = game_mode();
    UClass *mc = cls("MissionGameMode");
    return gm && mc && ue_is_a(gm, mc);
}
// o belongs to world w: its level (persistent or a streamed sublevel) is owned by w
static int in_world(UObject *o, UObject *w) {
    UClass *lc = cls("Level");
    for (UObject *p = U_OUTER(o); p; p = U_OUTER(p)) {
        if (p == w) return 1;
        if (lc && ue_is_a(p, lc)) return ue_get_ptr(p, "OwningWorld") == w;
    }
    return 0;
}
// next live object of class c in the current world (not a CDO / archetype)
static UObject *next_live(UClass *c, UObject *after) {
    UObject *w = ue_world();
    for (int32_t i = after ? U_INDEX(after) + 1 : 0, n = ue_num_objects(); c && w && i < n; i++) {
        UObject *o = ue_object_at(i);
        if (o && !(U_FLAGS(o) & LIVE_FLAGS) && ue_is_a(o, c) && in_world(o, w)) return o;
    }
    return NULL;
}

// ---- players ----
static int is_a(UObject *o, const char *name);
static UObject *freecam_pc;
static int32_t freecam_pci;
// the host's own controller (while the free camera is on, the local player belongs to the debug camera controller)
static UObject *host_pc(void) {
    UObject *pc = ue_local_pc();
    if (pc && is_a(pc, "DebugCameraController") && alive(freecam_pc, freecam_pci)) return freecam_pc;
    return pc;
}
static UObject *host_ps(void) { UObject *pc = host_pc(); return pc ? ue_get_ptr(pc, "PlayerState") : NULL; }
static int is_a(UObject *o, const char *name) { UClass *c = o ? cls(name) : NULL; return c && ue_is_a(o, c); }
static UObject *ps_controller(UObject *ps) { return ps ? ue_get_ptr(ps, "Owner") : NULL; }
static UObject *ps_pc(UObject *ps) { UObject *c = ps_controller(ps); return is_a(c, "PlayerController") ? c : NULL; }
static UObject *ps_pawn(UObject *ps) {
    UObject *c = ps_controller(ps), *p = c ? ue_get_ptr(c, "Pawn") : NULL;
    if (!p && ps) p = ue_get_ptr(ps, "PawnPrivate");
    return is_a(p, "HeroCharacter") ? p : NULL;
}
static UObject *health_of(UObject *pawn) { return pawn ? call_obj(pawn, "GetHealthComponent") : NULL; }
static UObject *life_of(UObject *pawn) { return pawn ? call_obj(pawn, "GetLifeStateComponent") : NULL; }
static UObject *inventory_of(UObject *pawn) { return pawn ? call_obj(pawn, "GetInventoryComponent") : NULL; }

static int actor_loc(UObject *a, float v[3]) {
    Call c;
    if (!a || !call_prep(&c, a, "K2_GetActorLocation")) return -1;
    call_go(&c);
    float *r = carg(&c, "ReturnValue");
    if (!r) return -1;
    memcpy(v, r, 12);
    return 0;
}
static int actor_fwd(UObject *a, float v[3]) {
    Call c;
    if (!a || !call_prep(&c, a, "GetActorForwardVector")) return -1;
    call_go(&c);
    float *r = carg(&c, "ReturnValue");
    if (!r) return -1;
    memcpy(v, r, 12);
    return 0;
}

// A player's name for replies/notices; an NPC ally in the player list (e.g. Emmett) by its hero class
static void who_name(UObject *ps, char *buf, size_t n) {
    admin_display_name(ps, buf, n);
    UObject *pawn = strcmp(buf, "?") ? NULL : ps_pawn(ps);
    if (!pawn) return;
    char c[128];
    ue_obj_name(U_CLASS(pawn), c, sizeof c);
    char *e = strstr(c, "_BP_C");
    if (e) *e = 0;
    snprintf(buf, n, "%s", c);
}

// Who a command is for: "" / "me" = the host's own hero, "all" = every hero (humans and bots), else a player from
// /players (name or #n).
typedef struct { UObject *ps[16]; int n, all, others; char label[64]; } Targets;
static int targets(const char *spec, Targets *t, Out *o) {
    memset(t, 0, sizeof *t);
    UObject *me = host_ps();
    if (!spec || !*spec || !_stricmp(spec, "me")) {
        if (!ps_pawn(me)) { out_printf(o, "you have no hero right now\n"); return 0; }
        t->ps[t->n++] = me;
        snprintf(t->label, sizeof t->label, "the host");
        return 1;
    }
    if (!_stricmp(spec, "all")) {
        UObject **pa; int n = admin_player_array(&pa);
        for (int i = 0; i < n && t->n < 16; i++) {
            if (!ps_pawn(pa[i])) continue;
            t->ps[t->n++] = pa[i];
            if (pa[i] != me) t->others = 1;
        }
        t->all = 1;
        snprintf(t->label, sizeof t->label, "everyone");
        if (!t->n) { out_printf(o, "no heroes right now\n"); return 0; }
        return 1;
    }
    UObject *ps = admin_find_player(spec, o);
    if (!ps) return 0;
    if (!ps_pawn(ps)) { char nm[64]; who_name(ps, nm, sizeof nm); out_printf(o, "%s has no hero right now\n", nm); return 0; }
    t->ps[t->n++] = ps;
    t->others = ps != me;
    if (ps == me) snprintf(t->label, sizeof t->label, "the host");
    else who_name(ps, t->label, sizeof t->label);
    return 1;
}

// ---- state ----
static int on;
static UObject *cur_world;
static char pending_notice[200];
static float pending_in;
static int clients_seen;   // remote players in the last mission: the camp notice waits for them to be back
static int ammo_inf, frozen, freecam;
static float slomo = 1.f;
typedef struct { UObject *ps; int32_t psi; UObject *hc; int32_t hci; } God;
static God gods[16];
static int n_gods;
typedef struct { UObject *c; int32_t i; } Ref;
static Ref ammo_set[256];   // weapons we made infinite (restored by /ammo off, /cheats off)
static int n_ammo;

static void notice(const char *fmt, ...) {
    char msg[240];
    va_list ap; va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    admin_notice("%s", msg);
}

// ---- god ----
static void hc_push(UObject *hc, int push) {
    call_void(hc, push ? "PushDamageDisabled" : "PopDamageDisabled");
    call_void(hc, push ? "PushDeathDisabled" : "PopDeathDisabled");
}
static God *god_find(UObject *ps) {
    for (int i = 0; i < n_gods; i++) if (gods[i].ps == ps && alive(ps, gods[i].psi)) return &gods[i];
    return NULL;
}
// keep the counters on the hero this player controls now (a new hero after a rescue, a bot taken over, ...)
static void god_sync(God *g) {
    UObject *hc = health_of(ps_pawn(g->ps));
    if (hc == g->hc && alive(g->hc, g->hci)) return;
    if (alive(g->hc, g->hci)) hc_push(g->hc, 0);
    g->hc = hc; g->hci = hc ? U_INDEX(hc) : -1;
    if (hc) { hc_push(hc, 1); LOG("cheats: god on %p", (void *)hc); }
}
static void god_set(UObject *ps, int en) {
    God *g = god_find(ps);
    if (en && !g && n_gods < 16) {
        g = &gods[n_gods++];
        memset(g, 0, sizeof *g);
        g->ps = ps; g->psi = U_INDEX(ps); g->hci = -1;
        god_sync(g);
    } else if (!en && g) {
        if (alive(g->hc, g->hci)) hc_push(g->hc, 0);
        *g = gods[--n_gods];
    }
}
static void god_all_off(void) {
    for (int i = 0; i < n_gods; i++) if (alive(gods[i].hc, gods[i].hci)) hc_push(gods[i].hc, 0);
    n_gods = 0;
}

static void cmd_god(char *rest, Out *o) {
    char *who = rest ? strtok(rest, " ") : NULL, *mode = who ? strtok(NULL, " ") : NULL;
    if (who && (!_stricmp(who, "on") || !_stricmp(who, "off"))) { mode = who; who = NULL; }
    if (mode && _stricmp(mode, "on") && _stricmp(mode, "off")) { out_printf(o, "usage: /god [player|all] [on|off]\n"); return; }
    Targets t;
    if (!targets(who, &t, o)) return;
    int en = mode ? !_stricmp(mode, "on") : (t.all ? 1 : !god_find(t.ps[0]));
    for (int i = 0; i < t.n; i++) god_set(t.ps[i], en);
    out_printf(o, "god mode %s for %s\n", en ? "on" : "off", t.label);
    if (t.others) notice(en ? "host gave god mode to %s" : "host took god mode from %s", t.label);
}

// ---- heal / revive ----
// Heal the trauma first (InPermanentHealth: the lost part of the max health bar, capped by the game), then fill the
// health up to the restored max.
static int heal_pawn(UObject *pawn) {
    UObject *hc = health_of(pawn);
    Call c;
    if (!hc || !call_prep(&c, hc, "Heal")) return 0;
    SET(&c, "InPermanentHealth", float, 100000.f);
    SET(&c, "SourceActor", UObject *, pawn);
    SET(&c, "SourcePawn", UObject *, pawn);
    call_go(&c);
    float mx = call_float(hc, "GetCurrentMaxHealth"), now = call_float(hc, "GetHealth");
    if (mx > now && call_prep(&c, hc, "Heal")) {
        SET(&c, "Health", float, mx - now);
        SET(&c, "SourceActor", UObject *, pawn);
        SET(&c, "SourcePawn", UObject *, pawn);
        call_go(&c);
    }
    return 1;
}

static void cmd_heal(char *rest, Out *o) {
    Targets t;
    if (!targets(rest, &t, o)) return;
    int n = 0, down = 0;
    for (int i = 0; i < t.n; i++) {
        UObject *pawn = ps_pawn(t.ps[i]), *ls = life_of(pawn);
        if (ls && call_bool(ls, "IsAlive") != 1) { down++; continue; }
        n += heal_pawn(pawn);
    }
    out_printf(o, "healed %d hero(es)%s\n", n, down ? " (downed/dead ones need /revive)" : "");
    if (t.others && n) notice("host healed %s", t.label);
}

static int revive_pawn(UObject *pawn) {
    UObject *ls = life_of(pawn), *hc = health_of(pawn);
    if (!ls || call_bool(ls, "IsIncapped") != 1) return 0;
    Call c;
    if (!call_prep(&c, ls, "Revive")) return 0;
    uint8_t *info = carg(&c, "ReviveInfo");   // ReviveInfo {NewHealth, NewPermanentHealth, IncapStrikesToReturn, tag, loc}
    if (!info) return 0;
    float mx = hc ? call_float(hc, "GetCurrentMaxHealth") : 100.f;
    *(float *)info = mx > 0 ? mx * 0.5f : 50.f;        // NewHealth
    *(float *)(info + 4) = mx > 0 ? mx : 100.f;          // NewPermanentHealth: keep the max health it had (0 = the minimum)
    actor_loc(pawn, (float *)(info + 0x14));
    SET(&c, "Reviver", UObject *, pawn);
    call_go(&c);
    return 1;
}

static void cmd_revive(char *rest, Out *o) {
    Targets t;
    if (!targets(rest && *rest ? rest : "all", &t, o)) return;
    int n = 0, dead = 0;
    for (int i = 0; i < t.n; i++) {
        UObject *pawn = ps_pawn(t.ps[i]), *ls = life_of(pawn);
        if (revive_pawn(pawn)) n++;
        else if (ls && call_bool(ls, "IsDead") == 1) dead++;
    }
    out_printf(o, "revived %d downed hero(es)%s\n", n, dead ? "; dead heroes come back through the rescue closets as usual" : "");
    if (n && (t.others || t.all)) notice("host revived %s", t.label);
}

// ---- ammo ----
static void ammo_sync(void) {
    UClass *c = cls("ClipAmmoComponent");
    int32_t off = -1;
    for (UObject *a = next_live(c, NULL); a; a = next_live(c, a)) {
        if (off < 0) off = ue_prop_offset(a, "bInfiniteReserveAmmo");
        if (off < 0) return;
        uint8_t *f = (uint8_t *)a + off;
        if (*f || n_ammo >= 256) continue;   // already infinite (by design or by us), or no room to remember it
        *f = 1;
        ammo_set[n_ammo].c = a; ammo_set[n_ammo].i = U_INDEX(a); n_ammo++;
    }
}
static void ammo_restore(void) {
    UClass *c = cls("ClipAmmoComponent");
    int32_t off = -1;
    for (int i = 0; i < n_ammo; i++) {
        if (!alive(ammo_set[i].c, ammo_set[i].i)) continue;
        if (off < 0) off = ue_prop_offset(ammo_set[i].c, "bInfiniteReserveAmmo");
        if (off >= 0) *((uint8_t *)ammo_set[i].c + off) = 0;
    }
    n_ammo = 0;
    (void)c;
}
static void cmd_ammo(char *rest, Out *o) {
    if (!rest || (_stricmp(rest, "infinite") && _stricmp(rest, "on") && _stricmp(rest, "off"))) {
        out_printf(o, "usage: /ammo infinite|off (infinite reserve ammo for every hero; now %s)\n", ammo_inf ? "infinite" : "off");
        return;
    }
    int en = _stricmp(rest, "off") != 0;
    if (en == ammo_inf) { out_printf(o, "ammo is already %s\n", en ? "infinite" : "normal"); return; }
    ammo_inf = en;
    if (en) ammo_sync(); else ammo_restore();
    out_printf(o, "infinite reserve ammo %s\n", en ? "on" : "off");
    notice(en ? "host turned on infinite reserve ammo for everyone" : "host turned off infinite ammo");
}

// ---- copper ----
static void cmd_copper(char *rest, Out *o) {
    char *amt = rest ? strtok(rest, " ") : NULL, *who = amt ? strtok(NULL, " ") : NULL, *end = NULL;
    long v = amt ? strtol(amt, &end, 10) : 0;
    if (!amt || *end || !v || v > 100000 || v < -100000) { out_printf(o, "usage: /copper <+N|-N> [player|all] (default: all)\n"); return; }
    Targets t;
    if (!targets(who ? who : "all", &t, o)) return;
    static wchar_t cause[32];
    int n = 0;
    for (int i = 0; i < t.n; i++) {
        Call c;
        if (!call_prep(&c, inventory_of(ps_pawn(t.ps[i])), "AdjustCurrency")) continue;
        SET(&c, "CurrencyDelta", int32_t, (int32_t)v);
        void *cs = carg(&c, "Cause");
        if (cs) fstring_set(cs, "b4bcoop", cause, 32);
        call_go(&c);
        n++;
    }
    out_printf(o, "copper %+ld for %d hero(es)\n", v, n);
    if (n && (t.others || t.all)) {
        if (v > 0) notice("host gave %ld copper to %s", v, t.label);
        else notice("host took %ld copper from %s", -v, t.label);
    }
}

// ---- cards ----
static UObject *card_manager(void) { UObject *gs = game_state(); return gs ? ue_get_ptr(gs, "GameplayCardManager") : NULL; }
static void norm(const char *s, char *out, size_t n) {   // lowercase letters/digits only
    size_t k = 0;
    for (; *s && k + 1 < n; s++) if (isalnum((unsigned char)*s)) out[k++] = (char)tolower((unsigned char)*s);
    out[k] = 0;
}
#define ADDR_FMEMORY_FREE VA(0x140C823B0ull)
static const uint8_t SIG_FREE[] = {0x48,0x85,0xc9,0x74,0x49,0x53,0x48,0x83,0xec,0x20,0x48,0x8b,0xd9};
// UDataTable RowMap: TMap sparse array of {FName, uint8* row, hash} (0x18), allocation bits TBitArray at +0x10 (inline
// dwords) or its heap pointer at +0x20, NumBits at +0x28 (as rewardguard.c's sparse_at)
static uint8_t *dt_row(UObject *table, FName row) {
    TArray *m = table ? DT_ROWMAP(table) : NULL;
    if (!m || !m->data) return NULL;
    const uint32_t *bits = *(uint32_t *const *)((char *)m + 0x20);
    if (!bits) bits = (const uint32_t *)((char *)m + 0x10);
    int nbits = *(int32_t *)((char *)m + 0x28);
    for (int i = 0; i < m->num && i < nbits; i++) {
        if (!((bits[i >> 5] >> (i & 31)) & 1)) continue;   // free slot
        char *e = (char *)m->data + (size_t)i * 0x18;
        if (((FName *)e)->idx == row.idx && ((FName *)e)->num == row.num) return *(uint8_t **)(e + 8);
    }
    return NULL;
}
// a card's name as the game shows it: GameplayCardRow.Name (FText at +0x10) through KismetTextLibrary.Conv_TextToString
static void card_display(const RowHandle *h, char *buf, size_t n) {
    buf[0] = 0;
    uint8_t *row = dt_row(h->table, h->row);
    Call c;
    if (!row || !call_prep(&c, class_cdo("KismetTextLibrary"), "Conv_TextToString")) return;
    void *in = carg(&c, "InText");
    FString *out = carg(&c, "ReturnValue");
    if (!in || !out) return;
    memcpy(in, row + 0x10, 0x18);   // borrowed: the parms block is never destructed
    call_go(&c);
    ascii_of(out, buf, n);
    if (out->data && !memcmp((void *)ADDR_FMEMORY_FREE, SIG_FREE, sizeof SIG_FREE)) ((void (*)(void *))ADDR_FMEMORY_FREE)(out->data);
}

// AllCards (every gameplay card handle) -> the handle(s) whose row name or shown name matches
static int card_match(UObject *gcm, const char *want, RowHandle **hit, Out *list, int max_list) {
    int32_t off = ue_prop_offset(gcm, "AllCards");
    if (off < 0) return -1;
    TArray *all = (TArray *)((char *)gcm + off);
    char w[96], rn[128], dn[128], a[128], b[128];
    norm(want ? want : "", w, sizeof w);
    int exact = 0, sub = 0, shown = 0;
    RowHandle *first_sub = NULL;
    for (int i = 0; i < all->num; i++) {
        RowHandle *h = (RowHandle *)((char *)all->data + (size_t)i * sizeof(RowHandle));
        ue_name(h->row, rn, sizeof rn);
        card_display(h, dn, sizeof dn);
        norm(rn, a, sizeof a); norm(dn, b, sizeof b);
        if (w[0] && (!strcmp(a, w) || !strcmp(b, w))) { *hit = h; exact = 1; break; }
        if (!w[0] || strstr(a, w) || strstr(b, w)) {
            if (!first_sub) first_sub = h;
            sub++;
            if (list && shown++ < max_list) out_printf(list, "  %s  (%s)\n", dn[0] ? dn : "?", rn);
        }
    }
    if (exact) return 1;
    if (sub == 1) { *hit = first_sub; return 1; }
    return sub ? -sub : 0;
}

static UObject *slot_of(UObject *pawn) {
    UObject *gs = game_state(), *psm = gs ? ue_get_ptr(gs, "PlayerSlotManager") : NULL;
    int32_t off = psm ? ue_prop_offset(psm, "TeamSlots") : -1;
    TArray *teams = off >= 0 ? (TArray *)((char *)psm + off) : NULL;
    for (int t = 0; teams && t < teams->num; t++) {
        TArray *slots = (TArray *)((uint8_t *)teams->data + (size_t)t * 0x18 + 8);   // {team, TArray<PlayerSlot*>}
        for (int i = 0; i < slots->num; i++) {
            UObject *s = ((UObject **)slots->data)[i];
            if (s && ue_get_ptr(s, "AssignedPawn") == pawn) return s;
        }
    }
    return NULL;
}

// ---- shared with rules.c / intel.c ----
int cheats_in_mission(void) { return in_mission(); }
int cheats_in_world(UObject *o, UObject *w) { return o && w && in_world(o, w); }
// ---- shared with rules.c (baseline cards): the card list, a card by name, a card into a slot ----
UObject *cheats_card_manager(void) { return card_manager(); }
UObject *cheats_slot_for_ps(UObject *ps) {
    UObject *s = ps && ue_prop_offset(ps, "OwnedPlayerSlot") >= 0 ? ue_get_ptr(ps, "OwnedPlayerSlot") : NULL;
    return s ? s : slot_of(ps_pawn(ps));
}
// the PlayerSlot with that slot index and team (PlayerSlot.SlotIdx int32 + Team byte; the card data's
// FPlayerSlotIndex {SlotId, Team} names the same pair), e.g. a bot's at the mission start, before its pawn and
// state are linked up
UObject *cheats_slot_by_index(int32_t id, uint8_t team) {
    UObject *gs = game_state(), *psm = gs ? ue_get_ptr(gs, "PlayerSlotManager") : NULL;
    int32_t off = psm ? ue_prop_offset(psm, "TeamSlots") : -1;
    TArray *teams = off >= 0 ? (TArray *)((char *)psm + off) : NULL;
    for (int t = 0; teams && t < teams->num; t++) {
        TArray *slots = (TArray *)((uint8_t *)teams->data + (size_t)t * 0x18 + 8);
        for (int i = 0; i < slots->num; i++) {
            UObject *s = ((UObject **)slots->data)[i];
            int32_t so = s ? ue_prop_offset(s, "SlotIdx") : -1, to = s ? ue_prop_offset(s, "Team") : -1;
            if (so < 0 || to < 0) continue;
            if (*(const int32_t *)((const char *)s + so) == id && *((const uint8_t *)s + to) == team) return s;
        }
    }
    // once: what the slots look like, for the day the layout differs (ue_prop_offset -1 = no such property)
    static int dumped;
    if (!dumped++) {
        char cn[96], pn[96];
        LOG("cheats: slot %d/%d not found; PlayerSlotManager %p, %d team(s)", id, team, (void *)psm, teams ? teams->num : -1);
        for (int t = 0; teams && t < teams->num; t++) {
            TArray *slots = (TArray *)((uint8_t *)teams->data + (size_t)t * 0x18 + 8);
            LOG("cheats:   team entry %d: %d slot(s), first 8 bytes %016llx", t, slots->num, (unsigned long long)*(uint64_t *)((uint8_t *)teams->data + (size_t)t * 0x18));
            for (int i = 0; i < slots->num && i < 8; i++) {
                UObject *s = ((UObject **)slots->data)[i];
                if (!s) { LOG("cheats:     slot[%d] null", i); continue; }
                int32_t so = ue_prop_offset(s, "SlotIdx"), to = ue_prop_offset(s, "Team");
                LOG("cheats:     slot[%d] %s SlotIdx@%d = %d, Team@%d = %d", i, ue_obj_name(s, cn, sizeof cn), so,
                    so >= 0 ? *(int32_t *)((char *)s + so) : -1, to, to >= 0 ? *((uint8_t *)s + to) : 255);
                if (i == 0) for (UStruct *st = U_CLASS(s); st; st = US_SUPER(st)) {
                    char line[600] = ""; int n = 0;
                    for (FField *f = US_CHILDPROPS(st); f && n < 40; f = FF_NEXT(f), n++) { size_t l = strlen(line); snprintf(line + l, sizeof line - l, " %s@%d", ue_name(FF_NAME(f), pn, sizeof pn), FP_OFFSET(f)); }
                    if (line[0]) LOG("cheats:     %s props:%s", ue_obj_name(st, cn, sizeof cn), line);
                }
            }
        }
    }
    return NULL;
}
int cheats_card_each(UObject *gcm, void (*fn)(const CardRef *c, const char *row, const char *shown, void *ctx), void *ctx) {
    int32_t off = gcm ? ue_prop_offset(gcm, "AllCards") : -1;
    if (off < 0) return -1;
    TArray *all = (TArray *)((char *)gcm + off);
    char rn[128], dn[128];
    for (int i = 0; i < all->num; i++) {
        RowHandle *h = (RowHandle *)((char *)all->data + (size_t)i * sizeof(RowHandle));
        CardRef c = {h->table, h->row};
        ue_name(h->row, rn, sizeof rn);
        card_display(h, dn, sizeof dn);
        fn(&c, rn, dn, ctx);
    }
    return all->num;
}
int cheats_card_find(UObject *gcm, const char *name, CardRef *out) {
    RowHandle *h = NULL;
    if (!gcm || card_match(gcm, name, &h, NULL, 0) != 1 || !h) return 0;
    out->table = h->table; out->row = h->row;
    return 1;
}
UObject *cheats_new_object(UClass *c, UObject *outer) {
    if (!c || !outer || memcmp((void *)ADDR_SCO, SIG_SCO, sizeof SIG_SCO)) return NULL;
    FName none = {0, 0};
    return ((ScoFn)ADDR_SCO)(c, outer, none, 0, 0, NULL, 0, NULL, 0);
}
// every row of a card table found by its object name, with the shown name (GameplayCardRow.Name through
// Conv_TextToString, as card_display); the card manager is not needed (Fort Hope has none)
int cheats_card_table_each(const char *table, void (*fn)(const CardRowInfo *c, void *ctx), void *ctx) {
    char nm[160];
    UObject *t = NULL;
    for (int32_t i = 0, n = ue_num_objects(); i < n && !t; i++) {
        UObject *x = ue_object_at(i);
        if (x && U_CLASS(x) && !(U_FLAGS(x) & 0x30) && !strcmp(ue_obj_name(x, nm, sizeof nm), table)) t = x;
    }
    TArray *m = t ? DT_ROWMAP(t) : NULL;
    if (!m || !m->data) return -1;
    // field offsets from the row struct (GameplayCardRow): a UScriptStruct, so ue_find_prop walks it like a class
    UStruct *rs = ue_get_ptr(t, "RowStruct");
    FField *fcat = rs ? ue_find_prop(rs, "Category") : NULL, *fhid = rs ? ue_find_prop(rs, "Hidden") : NULL,
           *fdb = rs ? ue_find_prop(rs, "AllowedForDeckbuilding") : NULL, *fpve = rs ? ue_find_prop(rs, "DisabledForPvE") : NULL;
    const uint32_t *bits = *(uint32_t *const *)((char *)m + 0x20);
    if (!bits) bits = (const uint32_t *)((char *)m + 0x10);
    int nbits = *(int32_t *)((char *)m + 0x28), count = 0;
    char rn[128], dn[128];
    for (int i = 0; i < m->num && i < nbits; i++) {
        if (!((bits[i >> 5] >> (i & 31)) & 1)) continue;
        char *e = (char *)m->data + (size_t)i * 0x18;
        uint8_t *row = *(uint8_t **)(e + 8);
        RowHandle h = {t, *(FName *)e, {0}};
        ue_name(h.row, rn, sizeof rn);
        card_display(&h, dn, sizeof dn);
        CardRowInfo c = {rn, dn, fcat && row ? row[FP_OFFSET(fcat)] : -1, fhid && row ? row[FP_OFFSET(fhid)] != 0 : -1,
                         fdb && row ? row[FP_OFFSET(fdb)] != 0 : -1, fpve && row ? row[FP_OFFSET(fpve)] != 0 : -1, t, h.row};
        fn(&c, ctx);
        count++;
    }
    return count;
}
int cheats_card_add(UObject *gcm, UObject *slot, const CardRef *card) {
    Call c;
    if (!gcm || !slot || !call_prep(&c, gcm, "AddGameplayCardToSlotByRowHandle")) return 0;
    SET(&c, "PlayerSlot", UObject *, slot);
    RowHandle *ph = carg(&c, "GameplayCardRowHandle");
    if (!ph) return 0;
    ph->table = card->table; ph->row = card->row;   // display string left empty (not needed for the lookup)
    call_go(&c);
    return 1;
}

static void cmd_card(char *rest, Out *o) {
    UObject *gcm = card_manager();
    if (!gcm || !in_mission()) { out_printf(o, "cards: only during a mission\n"); return; }
    char *a = rest ? strtok(rest, " ") : NULL;
    if (!a) { out_printf(o, "usage: /card <card name> [player]  |  /card list [filter]\n"); return; }
    RowHandle *h = NULL;
    if (!_stricmp(a, "list")) {
        char *f = strtok(NULL, "");
        size_t before = o->len;
        int r = card_match(gcm, f, &h, o, 40);
        if (r == 1 && h && o->len == before) { char dn[128], rn[128]; card_display(h, dn, sizeof dn); out_printf(o, "  %s  (%s)\n", dn, ue_name(h->row, rn, sizeof rn)); }
        else if (r < -40) out_printf(o, "... %d matches, narrow it down: /card list <filter>\n", -r);
        else if (!r) out_printf(o, "no card matches '%s'\n", f ? f : "");
        return;
    }
    // the card name may have spaces; a trailing word naming a player (or "all"/"me") is the target
    char name[128], *who = NULL, *more = strtok(NULL, "");
    snprintf(name, sizeof name, "%s%s%s", a, more ? " " : "", more ? more : "");
    char *sp = strrchr(name, ' ');
    if (sp) {
        UObject **pa; int n = admin_player_array(&pa), is_player = !_stricmp(sp + 1, "all") || !_stricmp(sp + 1, "me") || sp[1] == '#';
        char nm[64];
        for (int i = 0; i < n && !is_player; i++) { admin_display_name(pa[i], nm, sizeof nm); if (!_stricmp(nm, sp + 1)) is_player = 1; }
        if (is_player) { *sp = 0; who = sp + 1; }
    }
    int r = card_match(gcm, name, &h, NULL, 0);
    if (r != 1) {
        if (!r) out_printf(o, "no card matches '%s' (/card list [filter])\n", name);
        else { out_printf(o, "'%s' matches %d cards:\n", name, -r); card_match(gcm, name, &h, o, 10); }
        return;
    }
    Targets t;
    if (!targets(who, &t, o)) return;
    char dn[128], rn[128];
    card_display(h, dn, sizeof dn);
    ue_name(h->row, rn, sizeof rn);
    int n = 0;
    for (int i = 0; i < t.n; i++) {
        UObject *slot = slot_of(ps_pawn(t.ps[i]));
        Call c;
        if (!slot || !call_prep(&c, gcm, "AddGameplayCardToSlotByRowHandle")) continue;
        SET(&c, "PlayerSlot", UObject *, slot);
        RowHandle *ph = carg(&c, "GameplayCardRowHandle");
        if (!ph) continue;
        ph->table = h->table; ph->row = h->row;   // display string left empty (not needed for the lookup)
        LOG("cheats: card %s (%s) to slot %p", rn, dn, (void *)slot);
        call_go(&c);
        n++;
    }
    out_printf(o, "card %s added for %d hero(es)\n", dn[0] ? dn : rn, n);
    if (n && (t.others || t.all)) notice("host gave the card %s to %s", dn[0] ? dn : rn, t.label);
}

// ---- engine CheatManager ----
static UObject *cheat_manager(UObject *pc) {
    int32_t off = pc ? ue_prop_offset(pc, "CheatManager") : -1;
    if (off < 0) return NULL;
    UObject **slot = (UObject **)((char *)pc + off);
    if (*slot) return *slot;
    if (memcmp((void *)ADDR_SCO, SIG_SCO, sizeof SIG_SCO)) { LOG("cheats: StaticConstructObject signature mismatch"); return NULL; }
    UClass *cc = ue_get_ptr(pc, "CheatClass");
    UClass *base = cls("CheatManager");
    if (!cc || !base || !UC_CDO(cc) || !ue_is_a(UC_CDO(cc), base)) cc = base;
    if (!cc) return NULL;
    FName none = {0, 0};
    UObject *cm = ((ScoFn)ADDR_SCO)(cc, pc, none, 0, 0, NULL, 0, NULL, 0);
    if (cm) *slot = cm;   // referenced by the controller's UPROPERTY: lives as long as the controller
    char a[128], b[128];
    LOG("cheats: created %s for %s", cm ? ue_obj_name(cm, a, sizeof a) : "(null)", ue_obj_name(pc, b, sizeof b));
    return cm;
}
static int cm_call(UObject *pc, const char *fn, const float *arg) {
    UObject *cm = cheat_manager(pc);
    Call c;
    if (!cm || !call_prep(&c, cm, fn)) return 0;
    if (arg) { FField *p = US_CHILDPROPS(c.fn); if (p) *(float *)(c.p + FP_OFFSET(p)) = *arg; }
    call_go(&c);
    return 1;
}

// fly / noclip / walk [player|all], humans only. The engine's CheatManager Fly/Ghost/Walk end in ACharacter::ClientCheat*
// _Implementation, which the shipping build left empty, so we do what they would: movement mode Flying (or Falling,
// which lands into Walking) on the server's CharacterMovement, and actor collision off/on for noclip. The owning client
// follows the server's movement mode through the normal movement corrections.
static int set_move(UObject *pawn, int mode) {   // 0 walk, 1 fly, 2 noclip
    UObject *cmc = pawn ? ue_get_ptr(pawn, "CharacterMovement") : NULL;
    Call c;
    if (!cmc || !call_prep(&c, pawn, "SetActorEnableCollision")) return 0;
    SET(&c, "bNewActorEnableCollision", uint8_t, mode != 2);
    call_go(&c);
    if (!call_prep(&c, cmc, "SetMovementMode")) return 0;
    SET(&c, "NewMovementMode", uint8_t, mode ? 5 /*MOVE_Flying*/ : 3 /*MOVE_Falling*/);
    call_go(&c);
    return 1;
}

static void cmd_move(const char *verb, char *rest, Out *o) {
    int mode = !strcmp(verb, "fly") ? 1 : !strcmp(verb, "noclip") ? 2 : 0;
    Targets t;
    if (!targets(rest, &t, o)) return;
    if (mode == 2 && t.others) {   // collision is not replicated: their own game would still stop them at walls
        out_printf(o, "/noclip: only for your own hero (another player's game still collides); /fly works for them\n");
        return;
    }
    int n = 0;
    for (int i = 0; i < t.n; i++) if (ps_pc(t.ps[i]) && set_move(ps_pawn(t.ps[i]), mode)) n++;
    if (!n) { out_printf(o, "/%s: no player to apply it to (bots can't)\n", verb); return; }
    out_printf(o, "%s: %s%s\n", verb, t.label, mode ? " (/walk to land)" : "");
    if (t.others) notice(!mode ? "host put %s back on their feet (walk)" : "host gave %s %s", t.label, mode == 1 ? "fly mode" : "noclip");
}

// size <x>: the host's own hero only. CheatManager.ChangeSize scales the capsule and the mesh on the server; neither is
// replicated, so on another player's hero their own client would fight it (and nobody else would see it).
static float size_now = 1.f;
static void cmd_size(char *rest, Out *o) {
    char *end = NULL;
    float f = rest ? strtof(rest, &end) : 0;
    if (!rest || *end || f < 0.25f || f > 4.f) { out_printf(o, "usage: /size <0.25-4> (your own hero; 1 = normal)\n"); return; }
    if (!ps_pawn(host_ps()) || !cm_call(host_pc(), "ChangeSize", &f)) { out_printf(o, "/size: you have no hero right now\n"); return; }
    size_now = f;
    out_printf(o, "your size: %.2f (only you see it)\n", f);
}

// The engine's debug camera: CheatManager.ToggleDebugCamera on the host's controller spawns a DebugCameraController and
// hands it the local player; the same call on the debug controller's own CheatManager gives the player back.
static void cmd_freecam(Out *o) {
    UObject *lp = ue_local_pc();
    if (is_a(lp, "DebugCameraController")) {
        if (!cm_call(lp, "ToggleDebugCamera", NULL)) { out_printf(o, "/freecam: can't leave the free camera\n"); return; }
        freecam = 0;
        out_printf(o, "free camera off\n");
        return;
    }
    if (!is_a(lp, "GobiPlayerControllerBase")) { out_printf(o, "no player controller\n"); return; }
    if (!cm_call(lp, "ToggleDebugCamera", NULL)) { out_printf(o, "/freecam: not available\n"); return; }
    freecam = is_a(ue_local_pc(), "DebugCameraController");
    freecam_pc = lp; freecam_pci = U_INDEX(lp);
    out_printf(o, freecam ? "free camera on: only your view; press F8 to come back (chat doesn't work in it)\n"
                          : "/freecam: the camera didn't switch\n");
}

static void cmd_slomo(char *rest, Out *o) {
    char *end = NULL;
    float f = rest ? strtof(rest, &end) : 0;
    if (!rest || *end || f < 0.1f || f > 5.f) { out_printf(o, "usage: /slomo <0.1-5> (1 = normal; now %.2f)\n", slomo); return; }
    if (!cm_call(host_pc(), "Slomo", &f)) { out_printf(o, "/slomo: not available\n"); return; }
    slomo = f;
    out_printf(o, "game speed %.2f\n", f);
    notice("host set the game speed to %.2fx", f);
}

static void cmd_freeze(Out *o) {
    if (!cm_call(host_pc(), "PlayersOnly", NULL)) { out_printf(o, "/freeze: not available\n"); return; }
    frozen = !frozen;
    out_printf(o, frozen ? "AI frozen (/freeze again to resume)\n" : "AI resumed\n");
    notice(frozen ? "host froze the ridden and bots (/freeze)" : "host unfroze the world");
}

// ---- teleport ----
// a saferoom of this map: named Saferoom_BP_End / _Start; otherwise the end one = the one farthest from the team
static UObject *saferoom(int want_end, float at[3]) {
    UClass *c = cls("SafeRoom");
    float team[3] = {0}, v[3];
    int nt = 0;
    UObject **pa; int n = admin_player_array(&pa);
    for (int i = 0; i < n; i++) if (!actor_loc(ps_pawn(pa[i]), v)) { team[0] += v[0]; team[1] += v[1]; team[2] += v[2]; nt++; }
    if (nt) { team[0] /= nt; team[1] /= nt; team[2] /= nt; }
    UObject *best = NULL;
    float bd = want_end ? -1.f : 1e30f;
    char nm[128];
    for (UObject *s = next_live(c, NULL); s; s = next_live(c, s)) {   // the map's own naming first: Saferoom_BP_End/_Start
        ue_obj_name(s, nm, sizeof nm);
        if (strstr(nm, want_end ? "_End" : "_Start") && !actor_loc(s, at)) return s;
    }
    for (UObject *s = next_live(c, NULL); s; s = next_live(c, s)) {
        UObject *vol = ue_get_ptr(s, "DefaultSaferoomVolume");
        if (actor_loc(s, v)) continue;
        if (vol) { Call k; if (call_prep(&k, vol, "K2_GetComponentLocation")) { call_go(&k); float *r = carg(&k, "ReturnValue"); if (r) memcpy(v, r, 12); } }
        float d = (v[0] - team[0]) * (v[0] - team[0]) + (v[1] - team[1]) * (v[1] - team[1]) + (v[2] - team[2]) * (v[2] - team[2]);
        if (want_end ? d > bd : d < bd) { bd = d; best = s; memcpy(at, v, 12); }
    }
    return best;
}

// K2_TeleportTo (it moves the pawn out of walls) at a spot around `to`: the n-th hero sent gets its own spot, and
// beside=1 skips the exact spot (a player stands there)
static int teleport(UObject *pawn, const float to[3], int first) {
    static const float off[][2] = {{0, 0}, {120, 0}, {-120, 0}, {0, 120}, {0, -120}, {120, 120}, {-120, -120}, {120, -120}, {-120, 120}};
    for (int j = 0; j < 9; j++) {
        int k = (first + j) % 9;
        Call c;
        if (!call_prep(&c, pawn, "K2_TeleportTo")) return 0;
        float *d = carg(&c, "DestLocation");
        if (!d) return 0;
        d[0] = to[0] + off[k][0]; d[1] = to[1] + off[k][1]; d[2] = to[2];
        void *rot = carg(&c, "DestRotation");
        Call r;
        if (rot && call_prep(&r, pawn, "K2_GetActorRotation")) { call_go(&r); void *rv = carg(&r, "ReturnValue"); if (rv) memcpy(rot, rv, 12); }
        call_go(&c);
        uint8_t *ok = carg(&c, "ReturnValue");
        if (ok && *ok) return 1;
    }
    return 0;
}

static void cmd_tp(char *rest, Out *o) {
    char *a = rest ? strtok(rest, " ") : NULL, *b = a ? strtok(NULL, " ") : NULL;
    if (!a) { out_printf(o, "usage: /tp <player|saferoom|start>  |  /tp <player|all> <player|me|saferoom|start>\n"); return; }
    const char *who = b ? a : "me", *dest = b ? b : a;
    float to[3];
    char dlabel[64];
    int beside = 0;
    if (!_stricmp(dest, "saferoom") || !_stricmp(dest, "start")) {
        int end = !_stricmp(dest, "saferoom");
        if (!saferoom(end, to)) { out_printf(o, "no saferoom in this map\n"); return; }
        to[2] += 20.f;
        snprintf(dlabel, sizeof dlabel, end ? "the saferoom" : "the start saferoom");
    } else {
        Targets d;
        if (!targets(dest, &d, o) || d.all) { if (d.all) out_printf(o, "can't teleport to everyone\n"); return; }
        if (actor_loc(ps_pawn(d.ps[0]), to)) { out_printf(o, "no position for %s\n", dest); return; }
        snprintf(dlabel, sizeof dlabel, "%s", d.label);
        beside = 1;
    }
    Targets t;
    if (!targets(who, &t, o)) return;
    int n = 0;
    for (int i = 0; i < t.n; i++) {
        UObject *pawn = ps_pawn(t.ps[i]);
        float me[3];
        if (!actor_loc(pawn, me) && fabsf(me[0] - to[0]) < 1 && fabsf(me[1] - to[1]) < 1) continue;   // the destination itself
        n += teleport(pawn, to, beside + n);
    }
    out_printf(o, "teleported %d hero(es) to %s\n", n, dlabel);
    if (n && t.others) notice("host teleported %s to %s", t.label, dlabel);
}

// ---- world / director ----
static void cmd_horde(Out *o) {
    Call c;
    UObject *cdo = class_cdo("GameDirector");
    if (!in_mission() || !call_prep(&c, cdo, "TriggerHordeOnDirector")) { out_printf(o, "/horde: only during a mission\n"); return; }
    static wchar_t why[32];
    SET(&c, "WorldContextObject", UObject *, ue_world());
    void *r = carg(&c, "TriggerReason");
    if (r) fstring_set(r, "b4bcoop", why, 32);
    SET(&c, "bHordeAudioCue", uint8_t, 1);
    call_go(&c);
    out_printf(o, "horde triggered\n");
    notice("host triggered a horde");
}

static void cmd_director(char *rest, Out *o) {
    static const struct { const char *name; uint8_t v; const char *label; } P[] = {
        {"calm", 1, "calm"}, {"build", 2, "build-up"}, {"violent", 2, "build-up"}, {"peak", 4, "peak"},
        {"fade", 8, "peak fade"}, {"recover", 16, "recover"},
    };
    int i = 0;
    for (; rest && i < (int)(sizeof P / sizeof P[0]) && _stricmp(rest, P[i].name); i++) {}
    if (!rest || i == (int)(sizeof P / sizeof P[0])) { out_printf(o, "usage: /director calm|build|peak|fade|recover\n"); return; }
    Call c;
    if (!in_mission() || !call_prep(&c, class_cdo("GameDirector"), "ForcePacingPhaseOnDirector")) { out_printf(o, "/director: only during a mission\n"); return; }
    static wchar_t why[32];
    SET(&c, "WorldContextObject", UObject *, ue_world());
    SET(&c, "Phase", uint8_t, P[i].v);
    void *r = carg(&c, "Reason");
    if (r) fstring_set(r, "b4bcoop", why, 32);
    call_go(&c);
    Call g;   // what the director says now
    int now = -1;
    if (call_prep(&g, class_cdo("GameDirector"), "GetDirectorPacingPhase")) {
        SET(&g, "WorldContextObject", UObject *, ue_world());
        call_go(&g);
        uint8_t *rv = carg(&g, "ReturnValue");
        if (rv) now = *rv;
    }
    out_printf(o, "director pacing: %s (phase now %d)\n", P[i].label, now);
    notice("host forced the director's pacing to %s", P[i].label);
}

// Ridden: friendly name -> the Blueprint class (loaded on demand).
static const struct { const char *name, *path; } RIDDEN[] = {
#include "cheats_ridden.h"
};
#define N_RIDDEN ((int)(sizeof RIDDEN / sizeof RIDDEN[0]))

static UClass *load_class(const char *path) {
    // already loaded?
    const char *dot = strrchr(path, '.');
    UClass *c = dot ? ue_find_class(dot + 1) : NULL;
    if (c) return c;
    Call k;   // KismetSystemLibrary::LoadClassAsset_Blocking(TSoftClassPtr): {FWeakObjectPtr, int32 tag, FSoftObjectPath {FName, FString}}
    if (!call_prep(&k, class_cdo("KismetSystemLibrary"), "LoadClassAsset_Blocking")) return NULL;
    uint8_t *soft = carg(&k, "AssetClass");
    if (!soft) return NULL;
    *(int32_t *)soft = -1;
    static wchar_t w[256];
    int n = 0;
    for (; path[n] && n < 255; n++) w[n] = (wchar_t)path[n];
    w[n] = 0;
    ((FNameCtorFn)ADDR_FNAME_CTOR)((FName *)(soft + 0x10), w, 1);
    call_go(&k);
    UObject **r = carg(&k, "ReturnValue");
    LOG("cheats: loaded %s -> %p", path, r ? (void *)*r : NULL);
    return r ? *r : NULL;
}

static void cmd_spawn(char *rest, Out *o) {
    char *a = rest ? strtok(rest, " ") : NULL, *b = a ? strtok(NULL, " ") : NULL;
    int i = 0;
    for (; a && i < N_RIDDEN && _stricmp(a, RIDDEN[i].name); i++) {}
    int cnt = b ? atoi(b) : 1;
    if (!a || i == N_RIDDEN || cnt < 1 || cnt > 10) {
        out_printf(o, "usage: /spawn <type> [1-10]; types:");
        for (int k = 0; k < N_RIDDEN; k++) if (k == 0 || strcmp(RIDDEN[k].name, RIDDEN[k - 1].name)) out_printf(o, " %s", RIDDEN[k].name);
        out_printf(o, "\n");
        return;
    }
    if (!in_mission()) { out_printf(o, "/spawn: only during a mission\n"); return; }
    UObject *pawn = ps_pawn(host_ps());
    float at[3], fwd[3];
    if (actor_loc(pawn, at) || actor_fwd(pawn, fwd)) { out_printf(o, "you have no hero right now\n"); return; }
    UClass *cls = NULL;
    for (int k = i; k < N_RIDDEN && !_stricmp(RIDDEN[k].name, a) && !cls; k++) cls = load_class(RIDDEN[k].path);
    if (!cls) { out_printf(o, "/spawn: couldn't load the %s class\n", a); return; }
    int n = 0;
    for (int k = 0; k < cnt; k++) {
        Call c;
        if (!call_prep(&c, class_cdo("GameDirector"), "GobiSpawnAIFromClass")) break;
        SET(&c, "WorldContextObject", UObject *, ue_world());
        SET(&c, "PawnClass", UObject *, cls);
        float *l = carg(&c, "Location");
        float d = 500.f + 120.f * k, side = (k % 2 ? 1 : -1) * 80.f * ((k + 1) / 2);
        if (l) { l[0] = at[0] + fwd[0] * d - fwd[1] * side; l[1] = at[1] + fwd[1] * d + fwd[0] * side; l[2] = at[2] + 50.f; }
        float *r = carg(&c, "Rotation");
        if (r) r[1] = atan2f(-fwd[1], -fwd[0]) * 57.29578f;   // yaw facing the host
        SET(&c, "bNoCollisionFail", uint8_t, 1);
        call_go(&c);
        UObject **rv = carg(&c, "ReturnValue");
        if (rv && *rv) n++;
    }
    out_printf(o, "spawned %d/%d %s\n", n, cnt, a);
    if (n) notice("host spawned %d %s", n, a);
}

// Every living ridden: GobiCharacters that aren't heroes (players, bots, and NPC allies like Emmett are
// HeroCharacters) and aren't controlled by a player. Common ridden and the mutations (Common_/Hag_/Snitcher_/
// Brute_AICharacterBP) are plain GobiCharacters; the specials are ZombieCharacters.
static void cmd_killall(Out *o) {
    UClass *gc = cls("GobiCharacter"), *hc = cls("HeroCharacter"), *pcc = cls("PlayerController");
    if (!gc) { out_printf(o, "/killall: no ridden here\n"); return; }
    int n = 0;
    for (UObject *z = next_live(gc, NULL); z; z = next_live(gc, z)) {
        if (hc && ue_is_a(z, hc)) continue;   // never heroes (players, bots, NPC allies)
        UObject *ctrl = ue_get_ptr(z, "Controller");
        if (ctrl && pcc && ue_is_a(ctrl, pcc)) continue;   // never a player-controlled character
        UObject *ls = life_of(z);
        if (!ls || call_bool(ls, "IsAlive") != 1) continue;
        if (call_void(ls, "Kill")) n++;
    }
    out_printf(o, "killed %d ridden\n", n);
    if (n) notice("host killed all ridden (%d)", n);
}

static void cmd_endmission(int ok, Out *o) {
    UObject *gm = game_mode();
    Call c;
    if (!in_mission() || !call_prep(&c, gm, "OnMissionEnd")) { out_printf(o, "/%s: only during a mission\n", ok ? "win" : "lose"); return; }
    static wchar_t ctx[32];
    SET(&c, "bSuccess", uint8_t, (uint8_t)ok);
    void *cx = carg(&c, "Context");
    if (cx) fstring_set(cx, "b4bcoop cheat", ctx, 32);
    notice(ok ? "host ended the mission: success" : "host ended the mission: failure");
    call_go(&c);
    out_printf(o, "mission ended (%s)\n", ok ? "success" : "failure");
}

// ---- this machine's own progression (host or client: always the local player's own save) ----
static UObject *own_profile(Out *o) {
    UObject *pc = host_pc(), *player = pc ? ue_get_ptr(pc, "Player") : NULL;
    if (!player || !is_a(player, "LocalPlayer")) { out_printf(o, "no local profile\n"); return NULL; }
    // Signed in with the profile loaded = in Fort Hope or a mission. On the title/legal screens a command is accepted
    // but goes nowhere (seen: /supply before sign-in changed nothing), so refuse instead of claiming it worked. A client
    // is in someone's session, so it signed in before joining (its map is the host's).
    char pkg[256] = "";
    if (ue_world()) ue_world_package(ue_world(), pkg, sizeof pkg);
    if (!admin_is_client() && !strstr(pkg, "FortHope") && !in_mission()) {
        out_printf(o, "sign in and go to Fort Hope first (your save isn't loaded yet)\n");
        return NULL;
    }
    UObject *ppc = ue_get_ptr(pc, "GobiPlayerProfileComponent");
    if (!ppc) out_printf(o, "no profile component (sign in first)\n");
    return ppc;
}

// UGobiPlayerProfileComponent::ExecuteCommand(const FPlayerProfileCommand&, bool bPersist) on a local controller applies
// the command to that player's offline data; rewards.c owns the hook and runs it for us (rewards_execute_local: local
// components only, never forwarded). The client RPC handlers can't be used: they return early on the server. Commands
// are built with their own C++ vtables (from the RPC thunks' default construction), checked through GetType first.
#define VT_ADJUST_SP   VA(0x1454E8880ull)   // FAdjustSupplyPointsCommand, GetType() == 5
#define VT_UNLOCK      VA(0x1454E88F8ull)   // FUnlockProductCommand, GetType() == 6
#define VT_ADJUST_STP  VA(0x1454E8920ull)   // FAdjustSkullTotemPointsCommand, GetType() == 20 (exec 0x142131220)
#define VT_ADJUST_CONS VA(0x1454E8BA0ull)   // FAdjustConsumableQuantityCommand, GetType() == 19 (exec 0x1421310E0)
static int command_ok(uintptr_t vt, uint8_t type) {
    const uint8_t *get_type = *(const uint8_t *const *)(vt + 8);   // slot 1: mov al, <type>; ret
    return get_type && get_type[0] == 0xb0 && get_type[1] == type && get_type[2] == 0xc3;
}

// Before the first /supply, /skulls, /burncards or /unlockall write of this game session: copy PlayerProfileSettings.sav (the save itself)
// and .json (the game's export) to PlayerProfileSettings-b4bcoop-backup-<yyyymmdd-hhmmss>.sav/.json next to them.
// The save folder: %LOCALAPPDATA%\Back4Blood\Steam\Saved\SaveGames (the game runs with -SaveToUserDir), else the
// game's own Gobi\Saved\SaveGames. 0 = backed up (now or earlier this session; *where = the backup), 1 = nothing to
// back up: the save folder is there (the game's other saves, PlayerSettingsGame.sav, are in it) but the profile has
// never been saved (a new offline profile; *where = the folder), -1 = save folder not found or the copy failed: no
// write then (the save could be somewhere we don't look).
static int save_dir_ok(const wchar_t *dir, const wchar_t *file) {
    wchar_t f[MAX_PATH];
    _snwprintf(f, MAX_PATH, L"%ls\\%ls", dir, file);
    return GetFileAttributesW(f) != INVALID_FILE_ATTRIBUTES;
}
static int profile_backup(char *where, size_t n) {
    static char done[MAX_PATH * 3];
    if (done[0]) { snprintf(where, n, "%s", done); return 0; }
    wchar_t dirs[2][MAX_PATH], path[MAX_PATH], exe[MAX_PATH], *dir = NULL;
    int nd = 0;
    DWORD k = GetEnvironmentVariableW(L"LOCALAPPDATA", dirs[0], MAX_PATH);
    if (k > 0 && k < MAX_PATH - 64) { wcscat(dirs[0], L"\\Back4Blood\\Steam\\Saved\\SaveGames"); nd = 1; }
    if (GetModuleFileNameW(NULL, exe, MAX_PATH)) {   // ...\Gobi\Binaries\Win64\Back4Blood.exe
        for (int up = 0; up < 3; up++) { wchar_t *s = wcsrchr(exe, L'\\'); if (s) *s = 0; }
        _snwprintf(dirs[nd++], MAX_PATH, L"%ls\\Saved\\SaveGames", exe);
    }
    for (int i = 0; i < nd && !dir; i++) if (save_dir_ok(dirs[i], L"PlayerProfileSettings.sav")) dir = dirs[i];
    if (!dir) {
        for (int i = 0; i < nd; i++)
            if (save_dir_ok(dirs[i], L"PlayerSettingsGame.sav")) {
                WideCharToMultiByte(CP_UTF8, 0, dirs[i], -1, where, (int)n, NULL, NULL);
                LOG("cheats: backup: no profile save yet in %s (new profile), nothing to back up", where);
                return 1;
            }
        LOG("cheats: backup: no save folder found");
        return -1;
    }
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t stamp[32];
    _snwprintf(stamp, 32, L"%04d%02d%02d-%02d%02d%02d", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    static const wchar_t *const EXT[] = {L"sav", L"json"};
    for (int i = 0; i < 2; i++) {
        wchar_t src[MAX_PATH];
        _snwprintf(src, MAX_PATH, L"%ls\\PlayerProfileSettings.%ls", dir, EXT[i]);
        _snwprintf(path, MAX_PATH, L"%ls\\PlayerProfileSettings-b4bcoop-backup-%ls.%ls", dir, stamp, EXT[i]);
        if (GetFileAttributesW(src) == INVALID_FILE_ATTRIBUTES && i == 1) continue;   // the .json export is optional
        if (!CopyFileW(src, path, TRUE)) { LOG("cheats: backup: copy %ls failed (%lu)", src, GetLastError()); return -1; }
    }
    _snwprintf(path, MAX_PATH, L"%ls\\PlayerProfileSettings-b4bcoop-backup-%ls.sav", dir, stamp);
    WideCharToMultiByte(CP_UTF8, 0, path, -1, done, sizeof done, NULL, NULL);
    LOG("cheats: profile backed up: %s (+ .json)", done);
    snprintf(where, n, "%s", done);
    return 0;
}

static void cmd_supply(char *rest, Out *o) {
    char *end = NULL;
    long v = rest ? strtol(rest, &end, 10) : 0;
    if (!rest || *end || v < 1 || v > 100000) { out_printf(o, "usage: /supply <+N> (1-100000; your own supply points)\n"); return; }
    UObject *ppc = own_profile(o);
    if (!ppc) return;
    if (!command_ok(VT_ADJUST_SP, 5)) { out_printf(o, "/supply: not available in this game build\n"); return; }
    char bak[MAX_PATH * 3];
    int b = profile_backup(bak, sizeof bak);
    if (b < 0) { out_printf(o, "/supply: couldn't back up your save first, so nothing was changed\n"); return; }
    struct { uintptr_t vt; int32_t delta, pad; } cmd = {VT_ADJUST_SP, (int32_t)v, 0};   // FAdjustSupplyPointsCommand
    LOG("cheats: own profile (%s): AdjustSupplyPoints %+ld", admin_is_client() ? "client" : "host", v);
    if (rewards_execute_local(ppc, &cmd)) { out_printf(o, "/supply: not available\n"); return; }
    out_printf(o, "+%ld supply points in YOUR save. This is permanent (saved in a few seconds).\n", v);
    if (b == 0) out_printf(o, "Backup of your save from before: %s (and .json)\n", bak);
    else out_printf(o, "(new profile: no earlier save to back up)\n");
}

// the profile component's own reading (GetSupplyPoints / GetSkullTotemPoints), -1 if unavailable
static int profile_int(UObject *ppc, const char *fn) {
    Call c;
    if (!call_prep(&c, ppc, fn)) return -1;
    call_go(&c);
    int32_t *r = carg(&c, "ReturnValue");
    return r ? *r : -1;
}

static void cmd_skulls(char *rest, Out *o) {
    char *end = NULL;
    long v = rest ? strtol(rest, &end, 10) : 0;
    if (!rest || *end || v < 1 || v > 100000) { out_printf(o, "usage: /skulls <+N> (1-100000; your own skull totem points)\n"); return; }
    UObject *ppc = own_profile(o);
    if (!ppc) return;
    if (!command_ok(VT_ADJUST_STP, 20)) { out_printf(o, "/skulls: not available in this game build\n"); return; }
    char bak[MAX_PATH * 3];
    int b = profile_backup(bak, sizeof bak);
    if (b < 0) { out_printf(o, "/skulls: couldn't back up your save first, so nothing was changed\n"); return; }
    struct { uintptr_t vt; int32_t delta, pad; } cmd = {VT_ADJUST_STP, (int32_t)v, 0};   // FAdjustSkullTotemPointsCommand
    int before = profile_int(ppc, "GetSkullTotemPoints");
    LOG("cheats: own profile (%s): AdjustSkullTotemPoints %+ld (had %d)", admin_is_client() ? "client" : "host", v, before);
    if (rewards_execute_local(ppc, &cmd)) { out_printf(o, "/skulls: not available\n"); return; }
    int now = profile_int(ppc, "GetSkullTotemPoints");
    LOG("cheats: own profile: skull totem points now %d", now);
    out_printf(o, "+%ld skull totem points in YOUR save", v);
    if (now >= 0) out_printf(o, " (now %d)", now);
    out_printf(o, ". This is permanent (saved in a few seconds).\n");
    if (b == 0) out_printf(o, "Backup of your save from before: %s (and .json)\n", bak);
    else out_printf(o, "(new profile: no earlier save to back up)\n");
}

// ---- burn cards in your own save ----
// A burn card in the offline save is a consumable product of the products table (ProductRow.ProductType Consumable);
// having N of it = PlayerProfileData.Consumables[product] acquired - spent. The start saferoom's burn card screen
// (GameplayCardManager::GetPlayerBurnCards) offers every one with a quantity above 0, and playing it charges 1 as usual.
// ProductRow.Unlock is the card's row (GameplayCardRow: the name the game shows). DLC products are left out (as
// /unlockall does).
#define DT_ROWSTRUCT(t) (*(UObject **)((char *)(t) + 0x30))   // UDataTable::RowStruct
#define PRODUCT_CONSUMABLE 1                                   // EProductType::Consumable (ProductRow +0x91)
typedef struct { FName row; char name[80], prod[64], card[64]; } Burn;
#define MAX_BURNS 256
static Burn burns[MAX_BURNS];
static const char *burn_names[MAX_BURNS];
static int n_burns;
static UObject *burns_table;
static UObject *products_table(UObject *ppc) {
    UObject *t = ppc ? ue_get_ptr(ppc, "ProductsTable") : NULL;
    if (!t) { UObject *m = ue_find_first_of("GobiPlayerProfileManager"); t = m ? ue_get_ptr(m, "ProductsTable") : NULL; }
    return t;
}
static int burn_cmp(const void *a, const void *b) { return _stricmp(((const Burn *)a)->name, ((const Burn *)b)->name); }
static int burns_load(UObject *ppc) {
    UObject *t = products_table(ppc);
    char b[128];
    if (!t) return 0;
    if (t == burns_table && n_burns) return n_burns;
    n_burns = 0;
    burns_table = t;
    int dlc = 0;
    UObject *rs = DT_ROWSTRUCT(t);
    if (!rs || strcmp(ue_obj_name(rs, b, sizeof b), "ProductRow")) { LOG("cheats: products table: unexpected row struct"); return 0; }
    TArray *m = DT_ROWMAP(t);
    const uint32_t *bits = *(uint32_t *const *)((char *)m + 0x20);
    if (!bits) bits = (const uint32_t *)((char *)m + 0x10);
    int nbits = *(int32_t *)((char *)m + 0x28);
    for (int i = 0; m->data && i < m->num && i < nbits && n_burns < MAX_BURNS; i++) {
        if (!((bits[i >> 5] >> (i & 31)) & 1)) continue;
        char *e = (char *)m->data + (size_t)i * 0x18;
        uint8_t *row = *(uint8_t **)(e + 8);
        if (!row || row[0x91] != PRODUCT_CONSUMABLE) continue;
        if (row[0xa9] != 0) { if (dlc++ < 24) LOG("cheats: burn card product %s left out (DLC %d)", ue_name(*(FName *)e, b, sizeof b), row[0xa9]); continue; }
        Burn *x = &burns[n_burns];
        memset(x, 0, sizeof *x);
        x->row = *(FName *)e;
        ue_name(x->row, x->prod, sizeof x->prod);
        RowHandle *card = (RowHandle *)(row + 8);   // ProductRow.Unlock
        UObject *crs = card->table ? DT_ROWSTRUCT(card->table) : NULL;
        if (crs && !strcmp(ue_obj_name(crs, b, sizeof b), "GameplayCardRow")) {
            ue_name(card->row, x->card, sizeof x->card);
            card_display(card, x->name, sizeof x->name);
        }
        if (!x->name[0]) snprintf(x->name, sizeof x->name, "%s", x->card[0] ? x->card : x->prod);
        n_burns++;
    }
    qsort(burns, n_burns, sizeof burns[0], burn_cmp);
    for (int i = 0; i < n_burns; i++) burn_names[i] = burns[i].name;
    LOG("cheats: %d burn card products (%d DLC ones left out)", n_burns, dlc);
    return n_burns;
}
// by shown name, card row or product row: exact (letters/digits, any case), else the one substring match.
// 1 = *hit, 0 = none, -n = n matches (listed, up to max_list)
static int burn_match(const char *want, Burn **hit, Out *list, int max_list) {
    char w[96], a[96], b[96], c[96];
    norm(want ? want : "", w, sizeof w);
    int sub = 0, shown = 0;
    Burn *first = NULL;
    for (int i = 0; i < n_burns; i++) {
        Burn *x = &burns[i];
        norm(x->name, a, sizeof a); norm(x->card, b, sizeof b); norm(x->prod, c, sizeof c);
        if (w[0] && (!strcmp(a, w) || (b[0] && !strcmp(b, w)) || !strcmp(c, w))) { *hit = x; return 1; }
        if (!w[0] || strstr(a, w) || (b[0] && strstr(b, w))) {
            if (!first) first = x;
            sub++;
            if (list && shown++ < max_list) out_printf(list, "  %s  (%s)\n", x->name, x->card[0] ? x->card : x->prod);
        }
    }
    if (sub == 1) { *hit = first; return 1; }
    return -sub;
}

static void cmd_burncards(char *rest, Out *o) {
    if (!rest) { out_printf(o, "usage: /burncards <card name> [count]  |  /burncards list [filter]\n"); return; }
    UObject *ppc = own_profile(o);
    if (!ppc) return;
    if (!burns_load(ppc)) { out_printf(o, "/burncards: the burn card list isn't loaded here\n"); return; }
    Burn *h = NULL;
    if (!_strnicmp(rest, "list", 4) && (!rest[4] || rest[4] == ' ')) {
        char *f = rest[4] ? rest + 5 : NULL;
        int r = burn_match(f, &h, o, 60);
        if (r == 1 && h && f) out_printf(o, "  %s  (%s)\n", h->name, h->card[0] ? h->card : h->prod);
        else if (r < -60) out_printf(o, "... %d burn cards, narrow it down: /burncards list <filter>\n", -r);
        else if (!r) out_printf(o, "no burn card matches '%s'\n", f ? f : "");
        return;
    }
    // the card name may have spaces; a trailing number is the count
    char name[128];
    long n = 1;
    snprintf(name, sizeof name, "%s", rest);
    char *sp = strrchr(name, ' '), *end = NULL;
    if (sp) {
        long v = strtol(sp + 1, &end, 10);
        if (end != sp + 1 && !*end) { n = v; *sp = 0; }
    }
    if (n < 1 || n > 99) { out_printf(o, "/burncards: count 1-99\n"); return; }
    int r = burn_match(name, &h, NULL, 0);
    if (r != 1) {
        if (!r) out_printf(o, "no burn card matches '%s' (/burncards list [filter])\n", name);
        else { out_printf(o, "'%s' matches %d burn cards:\n", name, -r); burn_match(name, &h, o, 10); }
        return;
    }
    if (!command_ok(VT_ADJUST_CONS, 19)) { out_printf(o, "/burncards: not available in this game build\n"); return; }
    char bak[MAX_PATH * 3];
    int b = profile_backup(bak, sizeof bak);
    if (b < 0) { out_printf(o, "/burncards: couldn't back up your save first, so nothing was changed\n"); return; }
    struct { uintptr_t vt; RowHandle product; int32_t delta, pad; FString reason; } cmd;   // FAdjustConsumableQuantityCommand (0x40)
    memset(&cmd, 0, sizeof cmd);
    cmd.vt = VT_ADJUST_CONS;
    cmd.product.table = burns_table;
    cmd.product.row = h->row;
    static wchar_t disp[64];   // handle display name: the card row (the save itself takes the products table's name)
    if (h->card[0]) fstring_set(&cmd.product.display, h->card, disp, 64);
    cmd.delta = (int32_t)n;
    LOG("cheats: own profile (%s): AdjustConsumableQuantity %s (%s, card %s) %+ld", admin_is_client() ? "client" : "host",
        h->prod, h->name, h->card, n);
    if (rewards_execute_local(ppc, &cmd)) { out_printf(o, "/burncards: not available\n"); return; }
    out_printf(o, "+%ld %s burn card%s in YOUR save. This is permanent (saved in a few seconds); play %s in a "
                  "mission's start saferoom.\n", n, h->name, n == 1 ? "" : "s", n == 1 ? "it" : "them");
    if (b == 0) out_printf(o, "Backup of your save from before: %s (and .json)\n", bak);
    else out_printf(o, "(new profile: no earlier save to back up)\n");
}

// Every product in the supply lines (StaticCaravans: tutorial line + every chain), unlockable (not a consumable like a
// burn card), not DLC, that this profile hasn't unlocked yet.
static void cmd_unlockall(char *rest, Out *o) {
    UObject *ppc = own_profile(o);
    if (!ppc) return;
    UClass *scc = cls("StaticCaravans");
    UObject *sc = NULL;
    for (int32_t i = 0, n = ue_num_objects(); scc && i < n && !sc; i++) {
        UObject *x = ue_object_at(i);
        if (x && !(U_FLAGS(x) & 0x10) && ue_is_a(x, scc)) sc = x;
    }
    if (!sc) { out_printf(o, "/unlockall: use it in Fort Hope (the supply lines are not loaded here)\n"); return; }
    int32_t ot = ue_prop_offset(sc, "TutorialCaravan"), oc = ue_prop_offset(sc, "CaravanChains");
    if (ot < 0 || oc < 0) return;
    RowHandle *list[1024];
    int n = 0;
    TArray *items = (TArray *)((char *)sc + ot + 0x20);   // StaticCaravan {Merchant handle, TArray<handle> Items}
    for (int k = 0; k < items->num && n < 1024; k++) list[n++] = (RowHandle *)items->data + k;
    TArray *chains = (TArray *)((char *)sc + oc);
    for (int ci = 0; ci < chains->num; ci++) {
        TArray *cars = (TArray *)((char *)chains->data + ci * 0x10);
        for (int k = 0; k < cars->num; k++) {
            TArray *it = (TArray *)((char *)cars->data + k * 0x30 + 0x20);
            for (int j = 0; j < it->num && n < 1024; j++) list[n++] = (RowHandle *)it->data + j;
        }
    }
    int dry = rest && !_stricmp(rest, "check");
    if (!dry && !command_ok(VT_UNLOCK, 6)) { out_printf(o, "/unlockall: not available in this game build\n"); return; }
    char bak[MAX_PATH * 3] = "";
    int b = dry ? 1 : profile_backup(bak, sizeof bak);
    if (b < 0) { out_printf(o, "/unlockall: couldn't back up your save first, so nothing was changed\n"); return; }
    int done = 0, owned = 0, skipped = 0;
    UObject *pc = host_pc();
    for (int k = 0; k < n; k++) {
        RowHandle *h = list[k];
        uint8_t *row = dt_row(h->table, h->row);   // ProductRow
        if (!row || row[0x91] != 0 /*Unlockable*/ || row[0xa9] != 0 /*no DLC*/) { skipped++; continue; }
        Call q;   // CaravanManager::IsCaravanItemCompletedForPlayer(PC, CaravanItem{handle, Purchased})
        if (call_prep(&q, class_cdo("CaravanManager"), "IsCaravanItemCompletedForPlayer")) {
            SET(&q, "PlayerController", UObject *, pc);
            RowHandle *ci = carg(&q, "CaravanItem");
            if (ci) { ci->table = h->table; ci->row = h->row; }
            call_go(&q);
            uint8_t *rv = carg(&q, "ReturnValue");
            if (rv && *rv) { owned++; continue; }
        }
        if (dry) { done++; continue; }
        struct { uintptr_t vt; RowHandle product; FString reason; } cmd;   // FUnlockProductCommand (0x38)
        memset(&cmd, 0, sizeof cmd);
        cmd.vt = VT_UNLOCK;
        cmd.product.table = h->table; cmd.product.row = h->row;
        if (rewards_execute_local(ppc, &cmd)) { out_printf(o, "/unlockall: not available\n"); break; }
        done++;
    }
    LOG("cheats: own profile (%s): unlockall %d new, %d already owned, %d skipped (consumable/DLC) of %d", admin_is_client() ? "client" : "host", done, owned, skipped, n);
    if (dry) { out_printf(o, "/unlockall would unlock %d item(s) (%d already yours, %d consumables/DLC skipped)\n", done, owned, skipped); return; }
    out_printf(o, "unlocked %d supply-line item(s) in YOUR save (%d were already yours; burn cards and DLC items skipped). "
                  "This is permanent.\n", done, owned);
    if (b == 0) out_printf(o, "Backup of your save from before: %s (and .json)\n", bak);
    else out_printf(o, "(new profile: no earlier save to back up)\n");
}

// ---- on/off ----
static int effects_active(void) {
    return n_gods || ammo_inf || frozen || freecam || slomo != 1.f || size_now != 1.f;
}
// world_alive = 0: a map change. What lived in the old world (its heroes, weapons, world settings, the host's old
// controller) goes away with it; only our bookkeeping is reset.
static void all_effects_off(int world_alive) {
    if (world_alive) {
        god_all_off();
        if (ammo_inf) ammo_restore();
        if (frozen) cm_call(host_pc(), "PlayersOnly", NULL);
        if (slomo != 1.f) { float one = 1.f; cm_call(host_pc(), "Slomo", &one); }
        if (size_now != 1.f && ps_pawn(host_ps())) { float one = 1.f; cm_call(host_pc(), "ChangeSize", &one); }
        if (is_a(ue_local_pc(), "DebugCameraController")) cm_call(ue_local_pc(), "ToggleDebugCamera", NULL);
    }
    n_gods = 0; n_ammo = 0;
    ammo_inf = frozen = freecam = 0;
    slomo = size_now = 1.f;
}

// ---- the session's cheat state for own-save cheats (CMD_OWNSAVE, #49) ----
// Client: what the host's data lines said ("cheats on|off"), for the server connection they came on (-1 = nothing
// from this host yet: an older b4bcoop, or not told yet).
static int host_on = -1;
static UObject *host_conn;
static int32_t host_conn_i = -1;
static UObject *server_conn(void) {
    UObject *w = ue_world(), *nd = w ? ue_get_ptr(w, "NetDriver") : NULL;
    return nd ? ue_get_ptr(nd, "ServerConnection") : NULL;
}
static int host_state(void) {
    UObject *c = server_conn();
    return c && c == host_conn && alive(c, host_conn_i) ? host_on : -1;
}
void cheats_host_data(const char *text) {
    int v = !strcmp(text, "cheats on") ? 1 : !strcmp(text, "cheats off") ? 0 : -1;
    UObject *c = server_conn();
    if (v < 0 || !c) return;
    if (v != host_state()) LOG("cheats: the host's cheats are %s (own-save cheats %s here)", v ? "on" : "off", v ? "allowed" : "off");
    host_on = v; host_conn = c; host_conn_i = U_INDEX(c);
}
int cheats_own_save_ok(const char **why) {
    static const char *w;
    if (!why) why = &w;
    *why = NULL;
    if (admin_is_client()) {
        int s = host_state();
        if (s == 1) return 1;
        *why = s == 0 ? "the host has cheats off (when the host turns them on, you can change your own save)"
                      : "the host hasn't turned cheats on (a host on an older b4bcoop can't allow this)";
        return 0;
    }
    if (on) return 1;
    *why = "cheats are off: type /cheats on first (in someone else's game: when the host turns them on)";
    return 0;
}

// Host: tell every remote player the state (data line, not shown): on every change, and 2 s / 15 s after a player is
// first seen in this map (a late joiner, every player after a map change).
#define MAX_PEERS 16
static struct { UObject *pc; int32_t i; float age; int told, sends; } peers[MAX_PEERS];
static void peers_tick(float dt) {
    UObject *w = ue_world(), *me = host_pc();
    if (!w || !ue_is_listen_server(w)) return;
    for (int k = 0; k < MAX_PEERS; k++) if (peers[k].pc && !alive(peers[k].pc, peers[k].i)) memset(&peers[k], 0, sizeof peers[k]);
    UObject **pa;
    int n = admin_player_array(&pa);
    for (int j = 0; j < n; j++) {
        UObject *pc = ps_pc(pa[j]), *player = pc ? ue_get_ptr(pc, "Player") : NULL;
        if (!pc || pc == me || !is_a(player, "NetConnection")) continue;
        int k = 0;
        for (; k < MAX_PEERS && peers[k].pc != pc; k++) {}
        if (k == MAX_PEERS) {
            for (k = 0; k < MAX_PEERS && peers[k].pc; k++) {}
            if (k == MAX_PEERS) continue;
            peers[k].pc = pc; peers[k].i = U_INDEX(pc); peers[k].age = 0; peers[k].told = -1; peers[k].sends = 0;
        }
        peers[k].age += dt;
        int due = peers[k].told != on ? peers[k].age >= 2.f : (peers[k].sends == 1 && peers[k].age >= 15.f);
        if (!due) continue;
        if (admin_data_to(pc, on ? "cheats on" : "cheats off")) continue;
        if (peers[k].told != on) {
            char nm[64];
            who_name(pa[j], nm, sizeof nm);
            LOG("cheats: told %s: cheats %s", nm, on ? "on" : "off");
        }
        peers[k].told = on;
        peers[k].sends++;
    }
}

static void set_on(int en, Out *o) {
    if (admin_is_client()) {
        int s = host_state();
        out_printf(o, "/cheats on|off: host only (you are a client). The host's cheats are %s%s.\n",
                   s == 1 ? "on" : s == 0 ? "off" : "unknown", s == 1 ? ": /supply /skulls /burncards /unlockall change YOUR save" : "");
        return;
    }
    if (en == on) { out_printf(o, "cheats are already %s\n", on ? "on" : "off"); return; }
    if (!en) all_effects_off(1);
    on = en;
    LOG("cheats: %s by the host", en ? "ENABLED" : "disabled");
    out_printf(o, en ? "cheats on. /cheats help lists them. They stay on into the mission; every map change resets "
                       "their effects.\n"
                     : "cheats off\n");
    notice(en ? "host enabled cheats" : "host disabled cheats");
}

// Chat verbs and who may run them (admin.c checks the permission before cheats_slash runs): /cheats lists them for
// everyone (on/off: the host's, set_on), the session cheats need the host with cheats on, the own-save ones a session
// with cheats on (CMD_OWNSAVE: the host, or a client whose host has them on).
static const struct { const char *name; int perm; } VERBS[] = {{"cheats", CMD_ANYONE}, {"god", CMD_CHEAT},
    {"heal", CMD_CHEAT}, {"revive", CMD_CHEAT}, {"ammo", CMD_CHEAT}, {"copper", CMD_CHEAT}, {"card", CMD_CHEAT},
    {"fly", CMD_CHEAT}, {"noclip", CMD_CHEAT}, {"walk", CMD_CHEAT}, {"tp", CMD_CHEAT}, {"freecam", CMD_CHEAT},
    {"size", CMD_CHEAT}, {"horde", CMD_CHEAT}, {"director", CMD_CHEAT}, {"spawn", CMD_CHEAT}, {"killall", CMD_CHEAT},
    {"freeze", CMD_CHEAT}, {"slomo", CMD_CHEAT}, {"win", CMD_CHEAT}, {"lose", CMD_CHEAT}, {"unlockall", CMD_OWNSAVE},
    {"supply", CMD_OWNSAVE}, {"skulls", CMD_OWNSAVE}, {"burncards", CMD_OWNSAVE}};

int cheats_perm(const char *verb) {
    for (int i = 0; i < (int)(sizeof VERBS / sizeof VERBS[0]); i++) if (!strcmp(VERBS[i].name, verb)) return VERBS[i].perm;
    return -1;
}
int cheats_enabled(void) { return on; }

static void help(Out *o) {
    static const char own[] = "YOUR save only, permanent (everyone, while cheats are on): /supply +N  /skulls +N\n"
                              "/burncards <card> [count]  /burncards list [filter]  /unlockall\n";
    if (admin_is_client()) {
        int s = host_state();
        out_printf(o, "%sthe host's cheats: %s\n", own, s == 1 ? "ON" : s == 0 ? "off" : "unknown (off or an older b4bcoop)");
        return;
    }
    out_printf(o, "cheats (host only, after /cheats on; [p] = player name, #n, me or all):\n"
                  "/god [p] [on|off]  /heal [p]  /revive [p]  /ammo infinite|off  /copper +N [p]  /card <name>|list [p]\n"
                  "/fly [p]  /noclip  /walk [p]  /tp [p] <p|saferoom|start>  /freecam  /size <x>\n"
                  "/horde  /director calm|build|peak  /spawn <type> [n]  /killall  /freeze  /slomo <x>  /win  /lose\n"
                  "%s"
                  "state: cheats %s", own, on ? "ON" : "off");
    if (on) out_printf(o, ", god %d, ammo %s, speed %.2f%s%s", n_gods, ammo_inf ? "infinite" : "normal", slomo, frozen ? ", frozen" : "", freecam ? ", freecam" : "");
    out_printf(o, "\n");
}

// a verb from VERBS whose permission admin.c already checked
void cheats_slash(const char *verb, char *rest, Out *o) {
    while (rest && *rest == ' ') rest++;
    if (rest && !*rest) rest = NULL;
    if (rest) { char *e = rest + strlen(rest); while (e > rest && e[-1] == ' ') *--e = 0; }
    LOG("cheats: /%s %s", verb, rest ? rest : "");
    if (!strcmp(verb, "cheats")) {
        if (!rest || !_stricmp(rest, "help") || !_stricmp(rest, "status")) help(o);
        else if (!_stricmp(rest, "on")) set_on(1, o);
        else if (!_stricmp(rest, "off")) set_on(0, o);
        else out_printf(o, "usage: /cheats on|off|help\n");
        return;
    }
    if (!ue_world() || !game_state() || !host_pc()) { out_printf(o, "/%s: not now (loading)\n", verb); return; }
    if (!strcmp(verb, "god")) cmd_god(rest, o);
    else if (!strcmp(verb, "heal")) cmd_heal(rest, o);
    else if (!strcmp(verb, "revive")) cmd_revive(rest, o);
    else if (!strcmp(verb, "ammo")) cmd_ammo(rest, o);
    else if (!strcmp(verb, "copper")) cmd_copper(rest, o);
    else if (!strcmp(verb, "card")) cmd_card(rest, o);
    else if (!strcmp(verb, "fly") || !strcmp(verb, "noclip") || !strcmp(verb, "walk")) cmd_move(verb, rest, o);
    else if (!strcmp(verb, "tp")) cmd_tp(rest, o);
    else if (!strcmp(verb, "freecam")) cmd_freecam(o);
    else if (!strcmp(verb, "size")) cmd_size(rest, o);
    else if (!strcmp(verb, "horde")) cmd_horde(o);
    else if (!strcmp(verb, "director")) cmd_director(rest, o);
    else if (!strcmp(verb, "spawn")) cmd_spawn(rest, o);
    else if (!strcmp(verb, "killall")) cmd_killall(o);
    else if (!strcmp(verb, "freeze")) cmd_freeze(o);
    else if (!strcmp(verb, "slomo")) cmd_slomo(rest, o);
    else if (!strcmp(verb, "win") || !strcmp(verb, "lose")) cmd_endmission(!strcmp(verb, "win"), o);
    else if (!strcmp(verb, "supply")) cmd_supply(rest, o);
    else if (!strcmp(verb, "skulls")) cmd_skulls(rest, o);
    else if (!strcmp(verb, "burncards")) cmd_burncards(rest, o);
    else if (!strcmp(verb, "unlockall")) cmd_unlockall(rest, o);
}

// Map change: every effect ends with the map (their actors are gone; a good way to clear the sandbox), the toggle stays.
// Whether cheats stay on is decided once the new map has its game mode (a map change passes through worlds without
// one): Fort Hope and missions (both ways, the next chapter, a restart) keep them on, with a notice that the effects
// were reset; the menus (no mission, not Fort Hope) turn them off.
static int world_check, world_had_effects;
static void on_world_change(void) {
    world_had_effects = effects_active();
    all_effects_off(0);
    if (on) world_check = 1;
}

#ifndef B4B_RELEASE
// dev: `cheatprobe press <vk|lmb|rmb> <seconds>` holds a key or mouse button on this game's window (window messages;
// the game ignores posted mouse buttons in play; `cheatprobe input ...` sends them through SendInput instead, which
// works for fire/ADS when the window is raised, e.g. wmctrl -a)
static int press_what, press_si;   // vk, or -1 lmb, -2 rmb; press_si: SendInput (this prefix's own wineserver queue)
static float press_left;
static void press_input(int what, int down) {
    if (press_si) {   // "hardware" input: goes through the same path as real devices (the game ignores posted mouse buttons)
        INPUT in = {0};
        if (what < 0) {
            in.type = INPUT_MOUSE;
            in.mi.dwFlags = what == -1 ? (down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP) : (down ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP);
        } else {
            in.type = INPUT_KEYBOARD;
            in.ki.wVk = (WORD)what;
            in.ki.wScan = (WORD)MapVirtualKeyW(what, 0);
            in.ki.dwFlags = down ? 0 : KEYEVENTF_KEYUP;
        }
        SendInput(1, &in, sizeof in);
        return;
    }
    HWND w = NULL;
    while ((w = FindWindowExW(NULL, w, L"UnrealWindow", NULL))) {
        DWORD pid = 0;
        GetWindowThreadProcessId(w, &pid);
        if (pid == GetCurrentProcessId() && IsWindowVisible(w)) break;
    }
    if (!w) return;
    RECT r; GetClientRect(w, &r);
    LPARAM at = MAKELPARAM((r.right - r.left) / 2, (r.bottom - r.top) / 2);
    if (what == -1) PostMessageW(w, down ? WM_LBUTTONDOWN : WM_LBUTTONUP, down ? MK_LBUTTON : 0, at);
    else if (what == -2) PostMessageW(w, down ? WM_RBUTTONDOWN : WM_RBUTTONUP, down ? MK_RBUTTON : 0, at);
    else {
        UINT sc = MapVirtualKeyW(what, 0);
        PostMessageW(w, down ? WM_KEYDOWN : WM_KEYUP, what, 1 | (sc << 16) | (down ? 0 : 0xC0000000u));
    }
}
#endif

static int own_window_focused(void) {
    DWORD pid = 0;
    HWND w = GetForegroundWindow();
    if (w) GetWindowThreadProcessId(w, &pid);
    return w && pid == GetCurrentProcessId();
}

void cheats_tick(float dt) {
    UObject *w = ue_world();
    if (w != cur_world) {
        cur_world = w;
        on_world_change();
        memset(peers, 0, sizeof peers);   // every player is told again in the new map
    }
    if (on && w && admin_is_client()) {   // joined someone else's game: our own cheats end (theirs count now)
        all_effects_off(0);
        on = 0;
        world_check = 0;
        pending_notice[0] = 0;
        LOG("cheats: off (joined someone else's session)");
    }
    peers_tick(dt);
    if (on && !world_check && w) { int n = ue_num_clients(w); if (n > clients_seen) clients_seen = n; }
    if (world_check && on && w && game_mode()) {
        world_check = 0;
        char pkg[256] = "";
        ue_world_package(w, pkg, sizeof pkg);
        if (!in_mission() && !strstr(pkg, "FortHope")) {
            on = 0;
            LOG("cheats: off (map change to the menus)");
            snprintf(pending_notice, sizeof pending_notice, "cheats turned off");
            pending_in = 60.f;
        } else {   // Fort Hope <-> mission, next chapter, restart
            LOG("cheats: still on in the new map; effects reset%s", world_had_effects ? " (some were active)" : "");
            snprintf(pending_notice, sizeof pending_notice, "cheats still on; effects reset");
            pending_in = 60.f;
        }
    }
    // after a map change the clients reconnect and load a little later: tell everyone once every player has a hero
    // again (their chat box exists by then), 5 s after that, or after a minute at most
    if (pending_notice[0] && game_state() && host_pc()) {
        pending_in -= dt;
        static float grace = -1;
        if (grace < 0) {
            UObject **pa; int n = admin_player_array(&pa), ready = 0;
            for (int i = 0; i < n; i++) if (ps_pc(pa[i]) && ps_pawn(pa[i])) ready++;
            if (ready > clients_seen) grace = 5.f;
        } else grace -= dt;
        if (pending_in <= 0 || (grace != -1 && grace <= 0)) {
            notice("%s", pending_notice);
            pending_notice[0] = 0;
            clients_seen = 0;
            grace = -1;
        }
    }
    // F8 leaves the free camera (the debug camera's own controller doesn't take chat): the debug controller's own input
    // (PlayerController.WasInputKeyJustPressed), or the key itself while our window has the focus
    if (freecam) {
        UObject *dcc = ue_local_pc();
        int f8 = (GetAsyncKeyState(VK_F8) & 1) && own_window_focused();
        Call k;
        if (!f8 && is_a(dcc, "DebugCameraController") && call_prep(&k, dcc, "WasInputKeyJustPressed")) {
            static FName f8name;
            if (!f8name.idx) ((FNameCtorFn)ADDR_FNAME_CTOR)(&f8name, L"F8", 1);
            FName *key = carg(&k, "Key");   // FKey {FName KeyName, TSharedPtr<FKeyDetails>}: looked up by name
            if (key) { *key = f8name; call_go(&k); uint8_t *r = carg(&k, "ReturnValue"); f8 = r && *r; }
        }
        if (f8) { static Out tmp; out_reset(&tmp); cmd_freecam(&tmp); LOG("cheats: F8: %s", tmp.buf); }
    }
#ifndef B4B_RELEASE
    if (press_left > 0 && (press_left -= dt) <= 0) press_input(press_what, 0);
#endif
    static float acc;
    if (!on || (acc += dt) < 0.5f) return;
    acc = 0;
    for (int i = 0; i < n_gods; i++) {
        if (!alive(gods[i].ps, gods[i].psi)) { gods[i] = gods[--n_gods]; i--; continue; }
        god_sync(&gods[i]);
    }
    if (ammo_inf) ammo_sync();
}

// ---- ~ overlay tab "Cheats" (overlay.h): every cheat verb as a control, run through admin_slash (ov_run), so the
// permissions (host; cheats on), the host notices and the rewards rule are the chat command's ----
static char tgt_items[18][72];
static const char *tgt_ptrs[18];
static char tgt_spec[18][8];
static int n_tgt;
static void targets_refresh(int with_all) {   // "me", ["all"], "#n Name" for every player with a hero
    n_tgt = 0;
    snprintf(tgt_items[n_tgt], sizeof tgt_items[0], "me (host)"); snprintf(tgt_spec[n_tgt++], 8, "me");
    if (with_all) { snprintf(tgt_items[n_tgt], sizeof tgt_items[0], "everyone"); snprintf(tgt_spec[n_tgt++], 8, "all"); }
    UObject **pa; int n = admin_player_array(&pa);
    UObject *me = host_ps();
    for (int i = 0; i < n && n_tgt < 18; i++) {
        if (pa[i] == me || !ps_pawn(pa[i])) continue;
        char nm[64];
        who_name(pa[i], nm, sizeof nm);
        snprintf(tgt_items[n_tgt], sizeof tgt_items[0], "#%d %s", i, nm);
        snprintf(tgt_spec[n_tgt++], 8, "#%d", i);
    }
    for (int i = 0; i < n_tgt; i++) tgt_ptrs[i] = tgt_items[i];
}

// "Your own save" (#49): every player while the session has cheats on (the host's, for a client); a client sees it
// first (everything else on this tab is the host's)
static void own_save_panel(void) {
    static int supply = 1000, skulls = 100, burn_pick, burn_n = 1;
    ov_heading("Your own save (permanent)");
    ov_begin_perm(CMD_OWNSAVE);
    ov_width(8);
    ov_input_int("##supply", &supply, 1, 100000);
    ov_same_line();
    if (ov_button_confirm("Add supply points", "Sure? Permanent")) ov_run("supply %d", supply);
    ov_width(8);
    ov_input_int("##skulls", &skulls, 1, 100000);
    ov_same_line();
    if (ov_button_confirm("Add skull totem points", "Sure? Permanent")) ov_run("skulls %d", skulls);
    UObject *pc = host_pc(), *ppc = pc ? ue_get_ptr(pc, "GobiPlayerProfileComponent") : NULL;
    if (ppc && burns_load(ppc)) {
        ov_width(14);
        ov_combo("##burncard", &burn_pick, burn_names, n_burns);
        ov_same_line();
        ov_width(5);
        ov_input_int("##burnn", &burn_n, 1, 99);
        ov_same_line();
        Burn *bc = &burns[burn_pick >= 0 && burn_pick < n_burns ? burn_pick : 0];
        if (ov_button_confirm("Add burn cards", "Sure? Permanent"))
            ov_run("burncards %s %d", bc->card[0] ? bc->card : bc->prod, burn_n);
    } else ov_text_dim("Burn cards: sign in and go to Fort Hope first.");
    if (ov_button_confirm("Unlock all", "Sure? Permanent")) ov_run("unlockall");
    ov_tooltip("Every supply-line item (Fort Hope only; burn cards and DLC items are not included).");
    ov_text_dim("Only your own save (a backup is made first). Never sent to the host or the other players. In someone "
                "else's game this works while the host has cheats on.");
    ov_end_perm();
}
static void cheats_panel(void) {
    static int tgt, tp_dest, spawn_type, spawn_n = 1, copper = 500;
    static float size = 1.f, speed = 1.f;
    static char card[80];
    ov_begin_perm(CMD_HOST);
    int en = admin_is_client() ? host_state() == 1 : on;   // a client sees the host's state
    if (ov_checkbox("Cheats on##cheats", &en)) ov_run(en ? "cheats on" : "cheats off");
    ov_tooltip("Host only. Everyone is told. Stays on between Fort Hope and missions, but every map change "
               "resets the effects (god, fly, ammo, speed, ...). Stays on in Fort Hope too; off in the menus.");
    int tk = bots_toolkit_doors();
    ov_begin_disabled(tk < 0, "Not available with this game version.");
    if (ov_checkbox("Bots open pinged toolkit doors##bottoolkit", &tk)) ov_setting("bot_toolkit_doors", tk ? "1" : "0", 1);
    ov_end_disabled();
    ov_tooltip("Host only, works without cheats. Ping a supply-stash door and the nearest bot carrying a toolkit "
               "(up to 40 m away) goes and opens it. Off = the game's own behaviour (a bot only reacts when it is close).");
    int hr = bots_heal_requests();
    ov_begin_disabled(hr < 0, "Not available with this game version.");
    if (ov_checkbox("Bots heal you on request##botheal", &hr)) ov_setting("bot_heal_requests", hr ? "1" : "0", 1);
    ov_end_disabled();
    ov_tooltip("Host only, works without cheats. A player presses the heal key (Settings, Keys; default H) and the "
               "nearest bot carrying a healing item comes and heals them, at any missing health. Off = the game's own "
               "behaviour (bots heal a teammate below about 75% health, 65% in a fight, never in a saferoom).");
    ov_end_perm();
    if (admin_is_client()) own_save_panel();
    if (on) ov_text_dim("god on %d hero(es), ammo %s, speed %.2fx%s%s", n_gods, ammo_inf ? "infinite" : "normal", slomo,
                        frozen ? ", AI frozen" : "", freecam ? ", free camera" : "");
    ov_begin_perm(CMD_CHEAT);
    targets_refresh(1);
    ov_width(12);
    ov_combo("Player##target", &tgt, tgt_ptrs, n_tgt);
    const char *who = tgt_spec[tgt < n_tgt ? tgt : 0];
    ov_same_line();
    ov_text_dim("for the buttons marked *");

    ov_heading("Survival");
    if (ov_button("God on *")) ov_run("god %s on", who);
    ov_same_line();
    if (ov_button("God off *")) ov_run("god %s off", who);
    ov_same_line();
    if (ov_button("Heal *")) ov_run("heal %s", who);
    ov_same_line();
    if (ov_button("Revive *")) ov_run("revive %s", who);
    int inf = ammo_inf;
    if (ov_checkbox("Infinite reserve ammo (everyone)##ammo", &inf)) ov_run(inf ? "ammo infinite" : "ammo off");
    ov_width(7);
    ov_input_int("##copper", &copper, -100000, 100000);
    ov_same_line();
    if (ov_button("Give copper *")) ov_run("copper %+d %s", copper ? copper : 1, who);
    ov_width(12);
    int give = ov_input_text("##card", card, sizeof card, "card name");
    ov_same_line();
    if ((ov_button("Give card *") || give) && card[0]) ov_run("card %s %s", card, who);
    ov_same_line();
    if (ov_button("List cards")) ov_run("card list %s", card);

    ov_heading("Movement and camera");
    if (ov_button("Fly *")) ov_run("fly %s", who);
    ov_same_line();
    ov_begin_disabled(strcmp(who, "me") != 0, "Noclip is for your own hero only (another player's game still collides).");
    if (ov_button("Noclip")) ov_run("noclip");
    ov_end_disabled();
    ov_same_line();
    if (ov_button("Walk *")) ov_run("walk %s", who);
    ov_same_line();
    if (ov_button("Free camera")) ov_run("freecam");
    ov_tooltip("Only your view; F8 comes back.");
    static const char *base_dest[] = {"the end saferoom", "the start saferoom"};
    const char *dest_items[20]; char dest_spec[20][8]; int nd = 0;
    for (int i = 0; i < 2; i++) { dest_items[nd] = base_dest[i]; snprintf(dest_spec[nd++], 8, "%s", i ? "start" : "saferoom"); }
    for (int i = 0; i < n_tgt && nd < 20; i++) {
        if (!strcmp(tgt_spec[i], "all")) continue;
        dest_items[nd] = tgt_items[i]; snprintf(dest_spec[nd++], 8, "%s", tgt_spec[i]);
    }
    ov_width(12);
    ov_combo("##tpdest", &tp_dest, dest_items, nd);
    ov_same_line();
    if (ov_button("Teleport * there")) ov_run("tp %s %s", who, dest_spec[tp_dest < nd ? tp_dest : 0]);
    ov_width(12);
    ov_slider("##size", &size, 0.25f, 4.f, "size %.2f");
    if (ov_edit_done()) ov_run("size %.2f", size);
    ov_tooltip("Your own hero only (1 = normal); applied when you let go.");

    ov_heading("World and director");
    if (ov_button("Horde")) ov_run("horde");
    ov_same_line();
    if (ov_button("Kill all ridden")) ov_run("killall");
    ov_same_line();
    if (ov_button(frozen ? "Unfreeze AI" : "Freeze AI")) ov_run("freeze");
    static const char *phases[] = {"calm", "build", "peak", "fade", "recover"};
    ov_text("Director:");
    for (int i = 0; i < 5; i++) { ov_same_line(); if (ov_button(phases[i])) ov_run("director %s", phases[i]); }
    const char *types[N_RIDDEN]; int nt = 0;
    for (int k = 0; k < N_RIDDEN; k++) if (k == 0 || strcmp(RIDDEN[k].name, RIDDEN[k - 1].name)) types[nt++] = RIDDEN[k].name;
    ov_width(9);
    ov_combo("##spawntype", &spawn_type, types, nt);
    ov_same_line();
    ov_width(7);
    ov_slider_int("##spawnn", &spawn_n, 1, 10);
    ov_same_line();
    if (ov_button("Spawn")) ov_run("spawn %s %d", types[spawn_type < nt ? spawn_type : 0], spawn_n);
    ov_width(12);
    ov_slider("##speed", &speed, 0.1f, 5.f, "game speed %.2fx");
    if (ov_edit_done()) ov_run("slomo %.2f", speed);
    ov_same_line();
    if (ov_button("Normal speed")) { speed = 1.f; ov_run("slomo 1"); }
    if (ov_button_confirm("Win the mission", "Sure? Win")) ov_run("win");
    ov_same_line();
    if (ov_button_confirm("Lose the mission", "Sure? Lose")) ov_run("lose");

    ov_end_perm();

    if (!admin_is_client()) own_save_panel();
}

int cheats_init(void) {
    overlay_add_panel("Cheats", 50, cheats_panel);
    return 0;
}

#ifndef B4B_RELEASE
// Dev CLI: `cheat <command line>` = the host typing /<command line> (e.g. `cheat god all`; admin.c's dispatcher, so
// the same permission checks); `cheatprobe ...` finds
// class paths, card names and saferooms for testing.
int cheats_cmd(const char *verb, char *rest, Out *o) {
    if (!strcmp(verb, "cheat")) {
        if (!rest || !*rest) { help(o); return 1; }
        if (rest[0] == '/') rest++;
        char verb1[32];
        size_t k = strcspn(rest, " ");
        snprintf(verb1, sizeof verb1, "%.*s", (int)k, rest);
        if (cheats_perm(verb1) < 0) { out_printf(o, "not a cheat command: %s\n", verb1); return 1; }
        admin_slash(rest, o);
        return 1;
    }
    if (strcmp(verb, "cheatprobe")) return 0;
    char *what = rest ? strtok(rest, " ") : NULL, *arg = what ? strtok(NULL, " ") : NULL;
    char a[256], b[256], a2[256];
    if (what && !strcmp(what, "classes") && arg) {   // loaded subclasses of <arg> with their paths
        UClass *base = ue_find_class(arg), *cc = ue_find_class("Class");
        int n = 0;
        for (int32_t i = 0, m = ue_num_objects(); base && i < m; i++) {
            UObject *x = ue_object_at(i);
            if (!x || !U_CLASS(x) || !ue_is_a(x, cc)) continue;
            for (UStruct *s = x; s; s = US_SUPER(s)) if (s == base) { out_printf(o, "%s\n", ue_full_path(x, a, sizeof a)); n++; break; }
        }
        out_printf(o, "%d class(es)\n", n);
    } else if (what && !strcmp(what, "live") && arg) {   // live instances of a class in this world
        UClass *c = ue_find_class(arg);
        int n = 0;
        for (UObject *x = next_live(c, NULL); x && n < 60; x = next_live(c, x), n++) {
            float v[3] = {0};
            actor_loc(x, v);
            out_printf(o, "%p %s (%s) at %.0f %.0f %.0f\n", (void *)x, ue_obj_name(x, a, sizeof a), ue_obj_name(U_CLASS(x), b, sizeof b), v[0], v[1], v[2]);
        }
    } else if (what && !strcmp(what, "names") && arg) {   // every FName containing <arg> (soft paths of unloaded assets too)
        const uint8_t *pool = (const uint8_t *)ADDR_NAMEPOOL;
        uint32_t cur_block = *(const uint32_t *)(pool + 0), cursor = *(const uint32_t *)(pool + 4);   // this build: +0 block, +4 cursor
        char **blocks = (char **)(pool + 0x10);
        int hits = 0;
        for (uint32_t bi = 0; bi <= cur_block && hits < 200; bi++) {
            const uint8_t *blk = (const uint8_t *)blocks[bi];
            uint32_t end = bi == cur_block ? cursor : 0x20000;
            for (uint32_t off = 0; blk && off + 2 <= end && hits < 200;) {
                uint16_t hdr = *(const uint16_t *)(blk + off);
                int wide = hdr & 1, len = hdr >> 6;
                if (!len) break;
                if (!wide && len < (int)sizeof a) {
                    memcpy(a, blk + off + 2, len); a[len] = 0;
                    if (strstr(a, arg)) { out_printf(o, "%s\n", a); hits++; }
                }
                off += (2 + len * (wide ? 2 : 1) + 1) & ~1u;
            }
        }
        out_printf(o, "%d name(s)\n", hits);
    } else if (what && !strcmp(what, "load") && arg) {   // try loading a class by path
        UClass *c = load_class(arg);
        out_printf(o, "%s -> %p\n", arg, (void *)c);
    } else if (what && !strcmp(what, "hp") && arg) {   // hp <#n> [value]: HealthComponent.SetHealth (0 = a lethal hit)
        UObject *ps = admin_find_player(arg, o), *ls = life_of(ps_pawn(ps)), *hc = health_of(ps_pawn(ps));
        Call c;
        if (!ls || !hc || !call_prep(&c, hc, "SetHealth")) { out_printf(o, "no hero\n"); return 1; }
        char *hv = strtok(NULL, " ");
        SET(&c, "Health", float, hv ? (float)atof(hv) : 0.f);
        call_go(&c);
        out_printf(o, "hp: health %.0f alive %d incapped %d dead %d\n", call_float(hc, "GetHealth"), call_bool(ls, "IsAlive"),
                   call_bool(ls, "IsIncapped"), call_bool(ls, "IsDead"));
    } else if (what && !strcmp(what, "key") && arg) {   // key <vk>: post a key press to this game's window (dev)
        HWND w = NULL;
        while ((w = FindWindowExW(NULL, w, L"UnrealWindow", NULL))) {
            DWORD pid = 0;
            GetWindowThreadProcessId(w, &pid);
            if (pid == GetCurrentProcessId() && IsWindowVisible(w)) break;
        }
        int vk = (int)strtol(arg, NULL, 0);
        UINT sc = MapVirtualKeyW(vk, 0);
        if (w) { PostMessageW(w, WM_KEYDOWN, vk, 1 | (sc << 16)); PostMessageW(w, WM_KEYUP, vk, 1 | (sc << 16) | 0xC0000000u); }
        out_printf(o, "key 0x%02x -> window %p\n", vk, (void *)w);
    } else if (what && !strcmp(what, "damage") && arg) {   // damage <#n> [amount] [damage type class path]
        char *amt = strtok(NULL, " "), *dt_path = strtok(NULL, " ");
        UObject *ps = admin_find_player(arg, o), *pawn = ps_pawn(ps), *hc = health_of(pawn);
        Call c;
        if (!hc || !call_prep(&c, hc, "Damage")) { out_printf(o, "no hero/health\n"); return 1; }
        SET(&c, "InDamage", float, amt ? (float)atof(amt) : 30.f);
        SET(&c, "DamageTypeClass", UObject *, dt_path ? load_class(dt_path) : cls("GobiDamageType"));
        SET(&c, "Instigator", UObject *, host_pc());
        SET(&c, "SourcePawn", UObject *, ps_pawn(host_ps()));
        SET(&c, "SourceActor", UObject *, ps_pawn(host_ps()));
        float before = call_float(hc, "GetHealth");
        call_go(&c);
        out_printf(o, "damage: health %.0f -> %.0f (damage disabled %d)\n", before, call_float(hc, "GetHealth"),
                   call_bool(hc, "IsDamageDisabled"));
    } else if (what && !strcmp(what, "killas") && arg) {   // killas <#n> [count]: lethal damage to ridden, credited
        // to player #n (Instigator = its controller, SourcePawn = its hero), so the game's own kill/damage stat
        // trackers count it for that player (post-round stats tests)
        char *cnt_s = strtok(NULL, " ");
        int want = cnt_s ? atoi(cnt_s) : 5, n = 0;
        UObject *ps = admin_find_player(arg, o), *ctrl = ps_controller(ps), *pawn = ps_pawn(ps);
        UClass *gc = cls("GobiCharacter"), *hcc = cls("HeroCharacter"), *pcc = cls("PlayerController");
        if (!ctrl || !pawn || !gc) { out_printf(o, "killas: no controller/hero\n"); return 1; }
        for (UObject *z = next_live(gc, NULL); z && n < want; z = next_live(gc, z)) {
            if (hcc && ue_is_a(z, hcc)) continue;
            UObject *zc = ue_get_ptr(z, "Controller");
            if (zc && pcc && ue_is_a(zc, pcc)) continue;
            UObject *ls = life_of(z), *hc = health_of(z);
            Call c;
            if (!ls || !hc || call_bool(ls, "IsAlive") != 1 || !call_prep(&c, hc, "Damage")) continue;
            SET(&c, "InDamage", float, 100000.f);
            SET(&c, "DamageTypeClass", UObject *, cls("GobiDamageType"));
            SET(&c, "Instigator", UObject *, ctrl);
            SET(&c, "SourcePawn", UObject *, pawn);
            SET(&c, "SourceActor", UObject *, pawn);
            call_go(&c);
            if (call_bool(ls, "IsAlive") != 1) n++;
        }
        out_printf(o, "killas: %d ridden killed as %s\n", n, arg);
    } else if (what && !strcmp(what, "ammo")) {   // the host hero's weapons: clip / reserve / infinite flag
        UClass *c = cls("ClipAmmoComponent");
        UObject *me = ps_pawn(host_ps());
        for (UObject *a = next_live(c, NULL); a; a = next_live(c, a)) {
            UObject *owner = *(UObject **)((char *)a + 0xD8);   // UActorComponent::OwnerPrivate (unreflected)
            UObject *oo = owner ? ue_get_ptr(owner, "Owner") : NULL;
            if (me && owner != me && oo != me) continue;
            Call k; int clip = -1, res = -1;
            if (call_prep(&k, a, "GetCurrentClipAmount")) { call_go(&k); int32_t *r = carg(&k, "ReturnValue"); if (r) clip = *r; }
            if (call_prep(&k, a, "GetCurrentReserveAmount")) { call_go(&k); int32_t *r = carg(&k, "ReturnValue"); if (r) res = *r; }
            int32_t f = ue_prop_offset(a, "bInfiniteReserveAmmo");
            out_printf(o, "%s of %s: clip %d reserve %d infinite %d\n", ue_obj_name(a, b, sizeof b), owner ? ue_obj_name(owner, a2, sizeof a2) : "-",
                       clip, res, f >= 0 ? *((uint8_t *)a + f) : -1);
        }
    } else if (what && (!strcmp(what, "press") || !strcmp(what, "input")) && arg) {   // press|input <vk|lmb|rmb> [seconds]:
        char *secs = strtok(NULL, " ");                                                // hold it (released by the tick);
        if (press_left > 0) press_input(press_what, 0);                                // input = SendInput
        press_si = !strcmp(what, "input");
        press_what = !strcmp(arg, "lmb") ? -1 : !strcmp(arg, "rmb") ? -2 : (int)strtol(arg, NULL, 0);
        press_left = secs ? (float)atof(secs) : 0.2f;
        press_input(press_what, 1);
        out_printf(o, "holding %s for %.1f s\n", arg, press_left);
    } else if (what && !strcmp(what, "bind")) {   // bind [<Action> <Key>]: InputSettings action mappings (list / add one)
        UObject *is = class_cdo("InputSettings");
        char *key = strtok(NULL, " ");
        if (!is) { out_printf(o, "no InputSettings\n"); return 1; }
        int axis = arg && !strcmp(arg, "axis");   // bind axis <Axis> <Key>: an axis mapping, scale 1
        if (axis) { arg = key; key = strtok(NULL, " "); }
        if (arg && key) {
            Call c;
            if (!call_prep(&c, is, axis ? "AddAxisMapping" : "AddActionMapping")) { out_printf(o, "no Add*Mapping\n"); return 1; }
            uint8_t *m = carg(&c, "KeyMapping");
            wchar_t wa[64], wk[64];
            mbstowcs(wa, arg, 63); wa[63] = 0; mbstowcs(wk, key, 63); wk[63] = 0;
            ((FNameCtorFn)ADDR_FNAME_CTOR)((FName *)m, wa, 1);
            ((FNameCtorFn)ADDR_FNAME_CTOR)((FName *)(m + 0x10), wk, 1);
            if (axis) *(float *)(m + 8) = 1.f;   // FInputAxisKeyMapping {FName AxisName, float Scale, FKey Key}
            SET(&c, "bForceRebuildKeymaps", uint8_t, 1);
            call_go(&c);
            out_printf(o, "added %s -> %s\n", arg, key);
        }
        TArray *am = (TArray *)((char *)is + 0x88);
        for (int j = 0; j < am->num; j++) {
            uint8_t *e = (uint8_t *)am->data + j * 0x28;
            out_printf(o, "%s=%s ", ue_name(*(FName *)e, a, sizeof a), ue_name(*(FName *)(e + 0x10), b, sizeof b));
        }
        out_printf(o, "\n%d action mapping(s)\n", am->num);
    } else if (what && !strcmp(what, "cm")) {   // the host PC's CheatManager
        UObject *pc = host_pc();
        out_printf(o, "pc %p cheatmanager %p class %p\n", (void *)pc, (void *)(pc ? ue_get_ptr(pc, "CheatManager") : NULL),
                   (void *)(pc ? ue_get_ptr(pc, "CheatClass") : NULL));
    } else if (what && !strcmp(what, "state")) {
        out_printf(o, "on=%d gods=%d ammo=%d(%d) frozen=%d slomo=%.2f freecam=%d\n", on, n_gods,
                   ammo_inf, n_ammo, frozen, slomo, freecam);
        for (int i = 0; i < n_gods; i++) {
            char nm[64]; who_name(gods[i].ps, nm, sizeof nm);
            UObject *hc = gods[i].hc;
            out_printf(o, "  god %s hc=%p alive=%d damage_disabled=%d\n", nm, (void *)hc, alive(hc, gods[i].hci),
                       alive(hc, gods[i].hci) ? call_bool(hc, "IsDamageDisabled") : -1);
        }
        UObject **pa; int n = admin_player_array(&pa);
        for (int i = 0; i < n; i++) {
            UObject *pawn = ps_pawn(pa[i]), *hc = health_of(pawn), *inv = inventory_of(pawn);
            char nm[64]; who_name(pa[i], nm, sizeof nm);
            float v[3] = {0};
            actor_loc(pawn, v);
            Call c; int cur = -1;
            if (inv && call_prep(&c, inv, "GetCurrency")) { call_go(&c); int32_t *r = carg(&c, "ReturnValue"); if (r) cur = *r; }
            UObject *cmc = pawn ? ue_get_ptr(pawn, "CharacterMovement") : NULL, *ls = life_of(pawn);
            int32_t mm = cmc ? ue_prop_offset(cmc, "MovementMode") : -1, col = pawn ? ue_prop_offset(pawn, "bActorEnableCollision") : -1;
            out_printf(o, "  #%d %s pawn=%p health=%.0f/%.0f copper=%d alive=%d incap=%d move=%d collision=%d at %.0f %.0f %.0f\n", i, nm,
                       (void *)pawn, hc ? call_float(hc, "GetHealth") : -1, hc ? call_float(hc, "GetCurrentMaxHealth") : -1, cur,
                       ls ? call_bool(ls, "IsAlive") : -1, ls ? call_bool(ls, "IsIncapped") : -1, mm >= 0 ? *((uint8_t *)cmc + mm) : -1,
                       col >= 0 ? *((uint8_t *)pawn + col) : -1, v[0], v[1], v[2]);
        }
    } else if (what && (!strcmp(what, "spawnactor") || !strcmp(what, "emitter")) && arg) {
        // spawnactor <Blueprint class path> [dist] [dz]: spawn an actor (e.g. a weapon's world pickup,
        //   /Game/Items/Weapons/Assault/AR02/AR02_1_Pickup_BP.AR02_1_Pickup_BP_C) in front of the host's hero;
        // emitter <particle system path> [dist] [dz]: play a particle system there (e.g. a dropped magazine,
        //   /Game/VFX/Systems/EmptyMags/VFX_EmptyMag_AR02_3P_P.VFX_EmptyMag_AR02_3P_P). Model-mod checks (dev).
        char *ds = strtok(NULL, " "), *zs = ds ? strtok(NULL, " ") : NULL;
        float d = ds ? (float)atof(ds) : 150.f, dz = zs ? (float)atof(zs) : 0.f, at[3], fwd[3];
        UObject *pawn = ps_pawn(host_ps()), *gs = class_cdo("GameplayStatics");
        if (actor_loc(pawn, at) || actor_fwd(pawn, fwd) || !gs) { out_printf(o, "no hero\n"); return 1; }
        float loc[3] = {at[0] + fwd[0] * d, at[1] + fwd[1] * d, at[2] + dz};
        Call c;
        if (!strcmp(what, "spawnactor")) {
            UClass *cl = load_class(arg);
            float xf[12] = {0, 0, 0, 1, loc[0], loc[1], loc[2], 0, 1, 1, 1, 0};   // FTransform: quat, translation, scale
            if (!cl || !call_prep(&c, gs, "BeginDeferredActorSpawnFromClass")) { out_printf(o, "can't load %s\n", arg); return 1; }
            SET(&c, "WorldContextObject", UObject *, ue_world());
            SET(&c, "ActorClass", UObject *, (UObject *)cl);
            void *t = carg(&c, "SpawnTransform");
            if (t) memcpy(t, xf, sizeof xf);
            SET(&c, "CollisionHandlingOverride", uint8_t, 1);   // AlwaysSpawn
            call_go(&c);
            UObject **r = carg(&c, "ReturnValue"), *a = r ? *r : NULL;
            if (!a || !call_prep(&c, gs, "FinishSpawningActor")) { out_printf(o, "spawn failed\n"); return 1; }
            SET(&c, "Actor", UObject *, a);
            t = carg(&c, "SpawnTransform");
            if (t) memcpy(t, xf, sizeof xf);
            call_go(&c);
            LOG("cheatprobe: spawned %s at %.0f %.0f %.0f -> %p", arg, loc[0], loc[1], loc[2], (void *)a);
            out_printf(o, "spawned %p at %.0f %.0f %.0f\n", (void *)a, loc[0], loc[1], loc[2]);
        } else {
            // KismetSystemLibrary::LoadAsset_Blocking(TSoftObjectPtr): {FWeakObjectPtr, int32 tag, FSoftObjectPath}
            UObject *ps = NULL;
            if (call_prep(&c, class_cdo("KismetSystemLibrary"), "LoadAsset_Blocking")) {
                uint8_t *soft = carg(&c, "Asset");
                static wchar_t w[256];
                int n = 0;
                for (; arg[n] && n < 255; n++) w[n] = (wchar_t)arg[n];
                w[n] = 0;
                if (soft) {
                    *(int32_t *)soft = -1;
                    ((FNameCtorFn)ADDR_FNAME_CTOR)((FName *)(soft + 0x10), w, 1);
                    call_go(&c);
                    UObject **r = carg(&c, "ReturnValue");
                    ps = r ? *r : NULL;
                }
            }
            if (!ps || !call_prep(&c, gs, "SpawnEmitterAtLocation")) { out_printf(o, "can't load %s\n", arg); return 1; }
            SET(&c, "WorldContextObject", UObject *, ue_world());
            SET(&c, "EmitterTemplate", UObject *, ps);
            float *l = carg(&c, "Location"), *s = carg(&c, "Scale");
            if (l) memcpy(l, loc, sizeof loc);
            if (s) s[0] = s[1] = s[2] = 1.f;
            SET(&c, "bAutoDestroy", uint8_t, 1);
            call_go(&c);
            UObject **r = carg(&c, "ReturnValue");
            LOG("cheatprobe: emitter %s at %.0f %.0f %.0f -> %p", arg, loc[0], loc[1], loc[2], r ? (void *)*r : NULL);
            out_printf(o, "emitter %p at %.0f %.0f %.0f\n", r ? (void *)*r : NULL, loc[0], loc[1], loc[2]);
        }
    } else out_printf(o, "usage: cheatprobe classes <Base> | live <Class> | cm | state | killas <#n> [count] | spawnactor <class> | emitter <ps>\n");
    return 1;
}
#endif
