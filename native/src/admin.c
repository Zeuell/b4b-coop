// Chat commands (chat.c queues them) and the host's admin actions, also available as agent commands (dev builds).
// docs/investigations/chat-commands.md.
//
// Roles: a command typed on this machine acts on this machine. Each chat command has a permission (cmds.h), checked
// here before its handler runs: CMD_ANYONE (every player: own view, own light, own session, lists), CMD_HOST (this
// machine must be the server: listen host, or standalone for settings like teamsize/bots; a client gets "host only"),
// CMD_CHEAT (host, and cheats on: cheats.c's verbs, which cheats_perm() reports), CMD_OWNSAVE (own save, cheats on in
// this session: the host with cheats on, or a client whose host said its cheats are on; cheats_own_save_ok()).
// A client's commands never reach the host (chat.c intercepts before anything is sent).
//
// Host-side enforcement (hook AGameModeBase::PreLogin override, the function that calls slotguard's ApproveLogin):
// the join policy (joinpolicy.c: by default Steam friends only) is checked first, then the joiner's b4bcoop protocol
// (version_refused), then addons_policy (addons_refused, addons_mp.c: the joiner checked itself), all before the game's own
// PreLogin runs; then a banned player id, or any new player while the
// session is locked, gets a login error before a PlayerController exists. Bans persist in b4bcoop-bans.txt next to the agent config (cmds_config_path()).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdarg.h>
#include "MinHook.h"
#include "ue.h"
#include "log.h"
#include "cmds.h"
#include "updcore.h"
#include "overlay.h"

#define ADDR_PRELOGIN  VA(0x1419FE9C0ull)  // void AGobiGameMode::PreLogin(this, const <parsed options>& Options (TArray,
                                           //   options_str), const FString& Address, const FUniqueNetIdRepl& UniqueId,
                                           //   FString& ErrorMessage); logs "PreLogin Options"
#define ADDR_RESIZE    VA(0x140BAF260ull)  // TArray<TCHAR>::ResizeForCopy(this, NewMax, PrevMax): GMalloc-backed storage
#define ADDR_FREE      VA(0x140C823B0ull)  // FMemory::Free
static const uint8_t SIG_PRELOGIN[] = {0x48,0x89,0x5c,0x24,0x08,0x48,0x89,0x6c,0x24,0x10,0x48,0x89,0x74,0x24,0x18,0x57,
                                       0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57,0x48,0x83,0xec,0x40,0x4c,0x8b,0xf9,0x4c,
                                       0x8b,0xea,0x48,0x8b,0x89,0xf8,0x02,0x00,0x00};
static const uint8_t SIG_RESIZE[] = {0x48,0x89,0x5c,0x24,0x10,0x48,0x89,0x74,0x24,0x18,0x57,0x48,0x83,0xec,0x20,0x48,
                                     0x63,0xda,0x48,0x8b,0xf9};
static const uint8_t SIG_FREE[] = {0x48,0x85,0xc9,0x74,0x49,0x53,0x48,0x83,0xec,0x20,0x48,0x8b,0xd9};

typedef void (*PreLoginFn)(UObject *gm, const TArray *opts, const FString *addr, const void *uid, FString *err);
typedef void (*ResizeFn)(FString *s, int32_t new_max, int32_t prev_max);
static PreLoginFn orig_prelogin;
static ResizeFn resize_fn;
static int alloc_ok;

static int locked;
static char lock_allow[16][80];   // player keys present when the session was locked (they may rejoin on map change)
static int n_lock_allow;
static int bots_mode = -1;         // -1 game default, 0 off, 1 on (host, from the next map)
static int n_refused;

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

// Replace a game-owned FString's contents (memory from the game's allocator: the engine frees it later).
static int fstring_assign_game(FString *s, const char *text) {
    if (!alloc_ok) return -1;
    int n = (int)strlen(text) + 1;
    if (s->data) ((void (*)(void *))ADDR_FREE)(s->data);
    s->data = NULL; s->num = n; s->max = 0;
    resize_fn(s, n, 0);
    if (!s->data) { s->num = 0; return -1; }
    for (int i = 0; i < n; i++) s->data[i] = (wchar_t)(unsigned char)text[i];
    return 0;
}

static int is_pc(UObject *o) {
    static UClass *c;
    if (!c) c = ue_find_class("PlayerController");
    return o && c && ue_is_a(o, c);
}

static UObject *game_state(void) { UObject *w = ue_world(); return w ? ue_get_ptr(w, "GameState") : NULL; }

static int is_client(void) {
    UObject *w = ue_world(), *nd = w ? ue_get_ptr(w, "NetDriver") : NULL;
    return nd && ue_get_ptr(nd, "ServerConnection");
}

static void ps_name(UObject *ps, char *buf, size_t n) {
    int32_t off = ps ? ue_prop_offset(ps, "PlayerNamePrivate") : -1;
    if (off >= 0) ascii_of((FString *)((char *)ps + off), buf, n); else snprintf(buf, n, "?");
}

// A bot's player state has no player name; its hero is GobiPlayerState.BotRowHandle. Row handles in this build carry
// an unreflected display name after the row name ({table, FName row, FString display}); row names are GUIDs.
static int bot_name(UObject *ps, char *buf, size_t n) {
    int32_t off = ps ? ue_prop_offset(ps, "BotRowHandle") : -1;
    UObject *table = off >= 0 ? *(UObject **)((char *)ps + off) : NULL;
    if (!table) return 0;
    ascii_of((FString *)((char *)ps + off + 0x10), buf, n);
    if (!buf[0]) snprintf(buf, n, "bot");
    return 1;
}

// An NPC ally (e.g. Emmett in Evansburgh) has a player state with no name, no bot hero row and a non-hero pawn: its
// pawn's class without the _BP_C suffix ("Emmett"). Was listed as "- [bot]" on the host and a blank line on clients.
static int npc_name(UObject *ps, char *buf, size_t n) {
    UObject *pawn = ps ? ue_get_ptr(ps, "PawnPrivate") : NULL;
    char c[128];
    if (!pawn || !ue_obj_name(U_CLASS(pawn), c, sizeof c) || !strncmp(c, "Hero", 4)) return 0;
    char *s = strstr(c, "_BP_C");
    if (!s) s = strstr(c, "_C");
    if (s) *s = 0;
    snprintf(buf, n, "%s", c);
    return 1;
}

// Steam id from a FUniqueNetIdRepl {vtable, TSharedPtr<FUniqueNetId> {object, refcount}, ReplicationBytes}: the
// Steam net id object holds the 64-bit id after its vtable. Accept it only if it looks like an individual Steam id.
static int uid_str(const void *repl, char *buf, size_t n) {
    buf[0] = 0;
    if (!repl) return 0;
    const uint8_t *obj = *(const uint8_t **)((const char *)repl + 8);
    if (!obj) return 0;
    for (int off = 8; off <= 0x18; off += 8) {
        uint64_t v = *(const uint64_t *)(obj + off);
        if ((v >> 32) == 0x01100001ull) { snprintf(buf, n, "steam:%llu", (unsigned long long)v); return 1; }
    }
    return 0;
}

static uint64_t uid_id64(const void *repl) {
    char b[40];
    return uid_str(repl, b, sizeof b) ? strtoull(b + 6, NULL, 10) : 0;
}

// Ban/lock key for a player: its Steam id, else its name.
static void ps_key(UObject *ps, char *buf, size_t n) {
    int32_t off = ps ? ue_prop_offset(ps, "UniqueId") : -1;
    if (off >= 0 && uid_str((char *)ps + off, buf, n)) return;
    char nm[64]; ps_name(ps, nm, sizeof nm);
    snprintf(buf, n, "name:%s", nm);
}

// Player list = GameState.PlayerArray (humans and bots), indices as shown by /players.
static int player_array(UObject ***arr) {
    UObject *gs = game_state();
    int32_t off = gs ? ue_prop_offset(gs, "PlayerArray") : -1;
    if (off < 0) return 0;
    TArray *pa = (TArray *)((char *)gs + off);
    *arr = (UObject **)pa->data;
    return pa->num;
}

static int ps_ping_ms(UObject *ps) {
    int32_t off = ps ? ue_prop_offset(ps, "Ping") : -1;
    return off >= 0 ? *((uint8_t *)ps + off) * 4 : -1;   // replicated as ms/4
}

// Find a player by PlayerArray index ("2" or "#2") or name (exact, case-insensitive; else a unique prefix).
static UObject *find_player(const char *arg, Out *o) {
    UObject **pa; int n = player_array(&pa);
    if (!arg || !*arg) { out_printf(o, "which player? (/players)\n"); return NULL; }
    const char *a = arg[0] == '#' ? arg + 1 : arg;
    char *end;
    long idx = strtol(a, &end, 10);
    if (*a && !*end) {
        if (idx >= 0 && idx < n) return pa[idx];
        out_printf(o, "no player #%ld\n", idx);
        return NULL;
    }
    UObject *hit = NULL; int prefix_hits = 0;
    char nm[64];
    for (int i = 0; i < n; i++) {
        ps_name(pa[i], nm, sizeof nm);
        if (!_stricmp(nm, arg)) return pa[i];
        if (!_strnicmp(nm, arg, strlen(arg))) { hit = pa[i]; prefix_hits++; }
    }
    if (prefix_hits == 1) return hit;
    out_printf(o, prefix_hits ? "'%s' matches %d players, use the number from /players\n" : "no player '%s'\n", arg, prefix_hits);
    return NULL;
}

void admin_ps_key(UObject *ps, char *buf, size_t n) { ps_key(ps, buf, n); }
void admin_ps_name(UObject *ps, char *buf, size_t n) { ps_name(ps, buf, n); if (!buf[0] || !strcmp(buf, "?")) bot_name(ps, buf, n); }

// ---- bans (b4bcoop-bans.txt: "<key> <name>" per line) ----
static const char *bans_path(void) {
    static char path[640];
    if (!path[0]) {
        const char *cfg = cmds_config_path();
        const char *s1 = strrchr(cfg, '\\'), *s2 = strrchr(cfg, '/'), *sep = s1 > s2 ? s1 : s2;
        int dir = sep ? (int)(sep - cfg + 1) : 0;
        snprintf(path, sizeof path, "%.*sb4bcoop-bans.txt", dir, cfg);
    }
    return path;
}

typedef struct { char key[80], name[64]; } Ban;
static Ban bans[64];
static int n_bans;

static void bans_load(void) {
    n_bans = 0;
    FILE *f = fopen(bans_path(), "r");
    if (!f) return;
    char line[200];
    while (n_bans < 64 && fgets(line, sizeof line, f)) {
        char *nl = strpbrk(line, "\r\n"); if (nl) *nl = 0;
        if (!line[0] || line[0] == '#') continue;
        char *sp = strchr(line, ' ');
        if (sp) *sp++ = 0;
        snprintf(bans[n_bans].key, sizeof bans[0].key, "%s", line);
        snprintf(bans[n_bans].name, sizeof bans[0].name, "%s", sp ? sp : "");
        n_bans++;
    }
    fclose(f);
}

static int bans_save(void) {
    FILE *f = fopen(bans_path(), "w");
    if (!f) { LOG("admin: cannot write %s", bans_path()); return -1; }
    fprintf(f, "# b4bcoop bans: <steam:id64 | name:PlayerName> <name>\n");
    for (int i = 0; i < n_bans; i++) fprintf(f, "%s %s\n", bans[i].key, bans[i].name);
    fclose(f);
    return 0;
}

static int is_banned(const char *key, const char *name) {
    bans_load();
    for (int i = 0; i < n_bans; i++) {
        if (key[0] && !strcmp(bans[i].key, key)) return 1;
        if (!strncmp(bans[i].key, "name:", 5) && name[0] && !_stricmp(bans[i].key + 5, name)) return 1;
    }
    return 0;
}

// ---- login gate ----
// The login URL options as "?key=value?key=value". B4B's PreLogin gets them parsed, not as one FString: a TArray of
// 32-byte {FString key, FString value} entries (the game's own "PreLogin Options:" log line joins them, 0x14414D310).
static void options_str(const TArray *opts, char *buf, size_t n) {
    size_t k = 0;
    buf[0] = 0;
    if (!opts || !opts->data || opts->num < 0 || opts->num > 64) return;
    for (int i = 0; i < opts->num && k + 3 < n; i++) {
        const FString *kv = (const FString *)((const char *)opts->data + i * 32);
        buf[k++] = '?';
        ascii_of(&kv[0], buf + k, n - k);
        k += strlen(buf + k);
        if (k + 2 >= n) break;
        buf[k++] = '=';
        ascii_of(&kv[1], buf + k, n - k);
        k += strlen(buf + k);
    }
}

static void opt_value(const char *opts, const char *key, char *buf, size_t n) {
    buf[0] = 0;
    size_t kl = strlen(key);
    for (const char *p = opts; (p = strchr(p, '?')); p++) {
        if (_strnicmp(p + 1, key, kl) || p[1 + kl] != '=') continue;
        const char *v = p + 2 + kl;
        size_t k = 0;
        while (v[k] && v[k] != '?' && k + 1 < n) { buf[k] = v[k]; k++; }
        buf[k] = 0;
        return;
    }
}

// Join policy, before the game's own PreLogin: over Steam P2P the SteamID comes from the authenticated P2P session
// (steamnet.c already refused anyone the policy rejects; this is the second check), over IP it is the login's claim.
// Returns 1 if the login was refused here.
static int policy_refused(const TArray *opts, const FString *addr, const void *uid, FString *err) {
    char o[1024], name[64], ip[64], why[96];
    ascii_of(addr, ip, sizeof ip);
    uint64_t claimed = uid_id64(uid), p2p = steamnet_peer_of_addr(ip), id = p2p ? p2p : claimed;
    if (joinpolicy_check(id, why, sizeof why)) {
        LOG("admin: join policy: steam:%llu%s allowed (%s)", (unsigned long long)id, p2p ? " (Steam P2P)" : "", why);
        return 0;
    }
    options_str(opts, o, sizeof o);
    opt_value(o, "Name", name, sizeof name);
    LOG("admin: login %s from %s (steam:%llu%s): refused by the join policy: %s", name, ip, (unsigned long long)id,
        p2p ? ", Steam P2P" : ", claimed", why);
    if (!err || fstring_assign_game(err, joinpolicy_error())) { LOG("admin: could not set the login error, allowing"); return 0; }
    n_refused++;
    joinpolicy_notify_refused(id, why);
    return 1;
}

// Version gate, right after the join policy: the joiner's b4bcoop protocol (URL option b4bcoop=<n>, b4bcoopver=<x.y.z>,
// added by cmds.c to every join) must equal ours (VERSION; bumped when host and clients must match). Missing = an
// older b4bcoop. Both sides get the same sentence: the joiner as its login error, the host as a chat line.
static void clean_token(char *s) {   // from the joiner's URL: keep it printable and short
    for (char *c = s; *c; c++) if (!isalnum((unsigned char)*c) && *c != '.' && *c != '-') *c = '?';
    if (strlen(s) > 24) s[24] = 0;
}
static int version_refused(const TArray *opts, const void *uid, FString *err) {
    char o[1024], proto[32], ver[40], want[16];
    options_str(opts, o, sizeof o);
#ifndef B4B_RELEASE
    log_redact_addons(o);   // an older b4bcoop's add-on summary is never logged (#35)
    LOG("admin: PreLogin options: %s", o);
#endif
    opt_value(o, "b4bcoop", proto, sizeof proto);
    snprintf(want, sizeof want, "%d", coop_protocol());
    if (!strcmp(proto, want)) return 0;
    char name[64], key[80], theirs[80], msg[240];
    opt_value(o, "b4bcoopver", ver, sizeof ver);
    opt_value(o, "Name", name, sizeof name);
    clean_token(proto); clean_token(ver);
    if (proto[0]) snprintf(theirs, sizeof theirs, "%s (protocol %s)", ver[0] ? ver : "?", proto);
    else snprintf(theirs, sizeof theirs, "an older b4bcoop");
    snprintf(msg, sizeof msg, "Host runs b4bcoop %s (protocol %d); you have %s. Everyone needs the same version.",
             coop_version(), coop_protocol(), theirs);
    LOG("admin: login %s: refused, other b4bcoop version: %s", name, msg);
    if (!err || fstring_assign_game(err, msg)) { LOG("admin: could not set the login error, allowing"); return 0; }
    n_refused++;
    static char told[16][80]; static int n_told;   // one host notice per player per session (old clients retry)
    if (!uid_str(uid, key, sizeof key)) snprintf(key, sizeof key, "name:%s", name);
    for (int i = 0; i < n_told; i++) if (!strcmp(told[i], key)) return 1;
    if (n_told < 16) snprintf(told[n_told++], sizeof told[0], "%s", key);
    int newer = ver[0] && upd_version_cmp(ver, coop_version()) > 0 && updater_hint()[0];   // they have a newer b4bcoop
    chat_local("%s could not join: they have %s%s; you have b4bcoop %s (protocol %d). Everyone needs the same version.%s",
               name[0] ? name : "A player", proto[0] ? "b4bcoop " : "", theirs, coop_version(), coop_protocol(),
               newer ? " Press ~, tab Updates, to check for a newer version." : "");
    return 1;
}

// Add-ons (#22, #35): the joiner says it checked its own add-ons against our addons_policy (?b4bcoopaddonsok=,
// cmds.c); an older b4bcoop sends its summary instead (?b4bcoopaddons=). addons_mp.c decides; nothing is kept.
static int addons_refused(const TArray *opts, const void *uid, FString *err) {
    char o[1600], claim[32], summary[512], name[64], key[80], msg[320];
    options_str(opts, o, sizeof o);
    opt_value(o, "b4bcoopaddonsok", claim, sizeof claim);
    opt_value(o, "b4bcoopaddons", summary, sizeof summary);
    opt_value(o, "Name", name, sizeof name);
    if (!uid_str(uid, key, sizeof key)) snprintf(key, sizeof key, "name:%s", name);
    if (!addons_login_check(claim, summary, name, key, msg, sizeof msg)) return 0;
    if (!err || fstring_assign_game(err, msg)) { LOG("admin: could not set the login error, allowing"); return 0; }
    n_refused++;
    return 1;
}

static void prelogin_detour(UObject *gm, const TArray *opts, const FString *addr, const void *uid, FString *err) {
    if (policy_refused(opts, addr, uid, err)) return;   // strangers never reach the game's login code
    if (version_refused(opts, uid, err)) return;        // another b4bcoop protocol: nothing would work right
    if (addons_refused(opts, uid, err)) return;         // add-ons the host's addons_policy doesn't allow
    orig_prelogin(gm, opts, addr, uid, err);
    if (err && err->num > 1) return;   // already refused (e.g. slotguard's "Server full.")
    char o[1024], name[64], key[80], ip[64];
    options_str(opts, o, sizeof o);
    ascii_of(addr, ip, sizeof ip);
    opt_value(o, "Name", name, sizeof name);
    if (!uid_str(uid, key, sizeof key)) snprintf(key, sizeof key, "name:%s", name);
    const char *why = NULL;
    if (is_banned(key, name)) why = "You are banned from this session.";
    else if (locked) {
        int ok = 0;
        for (int i = 0; i < n_lock_allow; i++) if (!strcmp(lock_allow[i], key)) ok = 1;
        if (!ok) why = "The host locked the session.";
    }
    LOG("admin: login %s from %s (%s): %s", name, ip, key, why ? why : "ok");
    if (!why || !err) return;
    if (fstring_assign_game(err, why)) { LOG("admin: could not set the login error, allowing"); return; }
    n_refused++;
}

// ---- actions ----
static void players(Out *o) {
    UObject **pa; int n = player_array(&pa);
    UObject *me = ue_local_pc(), *my_ps = me ? ue_get_ptr(me, "PlayerState") : NULL;
    int host = !is_client();
    if (!n) { out_printf(o, "no players (not in a game)\n"); return; }
    for (int i = 0; i < n; i++) {
        char nm[64], key[80];
        UObject *ps = pa[i], *owner = ue_get_ptr(ps, "Owner");
        ps_name(ps, nm, sizeof nm);
        char bn[64];
        // a client doesn't see other players' controllers: a nameless player state with a bot hero is a bot
        int npc = !nm[0] && !bot_name(ps, bn, sizeof bn) && npc_name(ps, nm, sizeof nm);
        int is_bot = !npc && (host ? !is_pc(owner) : (!nm[0] && bot_name(ps, bn, sizeof bn)));
        if (is_bot && !nm[0] && !bot_name(ps, nm, sizeof nm)) snprintf(nm, sizeof nm, "-");
        out_printf(o, "#%d %s%s%s", i, nm, ps == my_ps ? " (you)" : "", is_bot ? " [bot]" : npc ? " [npc]" : "");
        if (host && !is_bot && !npc && ps != my_ps) out_printf(o, " %dms", ps_ping_ms(ps));
        if (host && !is_bot && !npc) { ps_key(ps, key, sizeof key); out_printf(o, " %s", key); }
        out_printf(o, "\n");
    }
}

static void ping(Out *o) {
    if (!is_client()) { out_printf(o, "you are the host (no round trip)\n"); return; }
    UObject *pc = ue_local_pc(), *ps = pc ? ue_get_ptr(pc, "PlayerState") : NULL;
    int ms = ps_ping_ms(ps);
    if (ms < 0) out_printf(o, "ping unknown\n"); else out_printf(o, "ping to host: %d ms\n", ms);
}

// Send one chat line to one player's client (ClientTeamMessage, sender = our player state). On the host's own
// controller the RPC runs locally.
static int send_line_t(UObject *pc, const char *text, int kind) {   // kind: chat_notice_type
    static UFunction *f;
    UObject *me = ue_local_pc(), *my_ps = me ? ue_get_ptr(me, "PlayerState") : NULL;
    if (!f && pc) f = ue_find_function(U_CLASS(pc), "ClientTeamMessage");
    FField *ps = f ? ue_find_prop(f, "SenderPlayerState") : NULL, *s = f ? ue_find_prop(f, "S") : NULL;
    FField *t = f ? ue_find_prop(f, "Type") : NULL;
    if (!ps || !s || !t || !my_ps || UFN_PARMSSIZE(f) > 64) return -1;
    static wchar_t w[320];
    uint8_t p[64] = {0};
    *(UObject **)(p + FP_OFFSET(ps)) = my_ps;
    fstring_set((FString *)(p + FP_OFFSET(s)), text, w, 320);
    *(FName *)(p + FP_OFFSET(t)) = chat_notice_type(kind);
    ue_process_event(pc, f, p);
    return 0;
}
static int send_line(UObject *pc, const char *text) { return send_line_t(pc, text, 0); }
int admin_notice_to(UObject *pc, const char *text) { return send_line(pc, text); }   // models.c etc.
int admin_data_to(UObject *pc, const char *text) { return send_line_t(pc, text, 2); }   // weaponlooks.c
// A data line of any length, wide text as is (handoff.c: a campaign run's JSON in chunks). The RPC's parameters are
// serialized during ProcessEvent, so the buffer only has to live for the call.
int admin_data_to_w(UObject *pc, const wchar_t *text, int len) {
    static UFunction *f;
    UObject *me = ue_local_pc(), *my_ps = me ? ue_get_ptr(me, "PlayerState") : NULL;
    if (!f && pc) f = ue_find_function(U_CLASS(pc), "ClientTeamMessage");
    FField *ps = f ? ue_find_prop(f, "SenderPlayerState") : NULL, *s = f ? ue_find_prop(f, "S") : NULL;
    FField *t = f ? ue_find_prop(f, "Type") : NULL;
    if (!ps || !s || !t || !my_ps || UFN_PARMSSIZE(f) > 64 || len < 0) return -1;
    wchar_t *w = malloc((len + 1) * sizeof(wchar_t));
    if (!w) return -1;
    memcpy(w, text, len * sizeof(wchar_t));
    w[len] = 0;
    uint8_t p[64] = {0};
    *(UObject **)(p + FP_OFFSET(ps)) = my_ps;
    FString *fs = (FString *)(p + FP_OFFSET(s));
    fs->data = w; fs->num = len + 1; fs->max = len + 1;
    *(FName *)(p + FP_OFFSET(t)) = chat_notice_type(2);
    ue_process_event(pc, f, p);
    free(w);
    return 0;
}

// Shared with cheats.c: the /players list, player lookup, a display name (bots: their hero), and a notice every
// player sees (the /say path, prefixed "[b4bcoop] ").
int admin_player_array(UObject ***arr) { return player_array(arr); }
UObject *admin_find_player(const char *arg, Out *o) { return find_player(arg, o); }
void admin_display_name(UObject *ps, char *buf, size_t n) {
    ps_name(ps, buf, n);
    if (!buf[0] && !bot_name(ps, buf, n)) snprintf(buf, n, "?");
}
int admin_is_client(void) { return is_client(); }

void admin_notice(const char *fmt, ...) {
    char msg[260], line[300];
    va_list ap; va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    snprintf(line, sizeof line, "[b4bcoop] %s", msg);
    UObject **pa; int n = player_array(&pa), sent = 0;
    for (int i = 0; i < n; i++) {
        UObject *pc = ue_get_ptr(pa[i], "Owner");
        if (is_pc(pc) && !send_line(pc, line)) sent++;
    }
    LOG("admin: notice to %d player(s): %s", sent, msg);
    if (!sent) chat_local("%s", line);   // no player controller (loading): at least the host sees it
}

static void say(const char *msg, Out *o) {
    if (!msg || !*msg) { out_printf(o, "usage: /say <message>\n"); return; }
    char line[300];
    snprintf(line, sizeof line, "[host] %s", msg);
    UObject **pa; int n = player_array(&pa), sent = 0;
    for (int i = 0; i < n; i++) {
        UObject *pc = ue_get_ptr(pa[i], "Owner");
        if (is_pc(pc) && !send_line(pc, line)) sent++;
    }
    LOG("admin: say to %d player(s): %s", sent, msg);
    if (!sent) out_printf(o, "no one to tell\n");
}

// Kicks are delayed a little so the "you were kicked" line (reliable RPC) leaves before the connection closes.
typedef struct { UObject *pc; int32_t idx; float in; } Pending;
static Pending pending[8];

static int kick_ps(UObject *ps, const char *why, Out *o) {
    UObject *pc = ue_get_ptr(ps, "Owner");
    char nm[64]; ps_name(ps, nm, sizeof nm);
    if (!is_pc(pc)) { out_printf(o, "%s is a bot\n", nm); return -1; }
    if (pc == ue_local_pc()) { out_printf(o, "that's you\n"); return -1; }
    for (int i = 0; i < 8; i++) {
        if (pending[i].pc && ue_object_at(pending[i].idx) == pending[i].pc) continue;
        char line[160];
        snprintf(line, sizeof line, "You were %s by the host.", why);
        send_line_t(pc, line, 1);   // the client's agent disconnects on this (the close below may take a timeout to arrive)
        pending[i].pc = pc; pending[i].idx = U_INDEX(pc); pending[i].in = 0.5f;
        char key[80]; ps_key(ps, key, sizeof key);
        for (int k = 0; k < n_lock_allow; k++)
            if (!strcmp(lock_allow[k], key)) { lock_allow[k][0] = 0; }
        LOG("admin: %s %s (%s)", why, nm, key);
        return 0;
    }
    out_printf(o, "too many kicks at once\n");
    return -1;
}

void admin_tick(float dt) {
    for (int i = 0; i < 8; i++) {
        if (!pending[i].pc) continue;
        if (ue_object_at(pending[i].idx) != pending[i].pc) { pending[i].pc = NULL; continue; }
        if ((pending[i].in -= dt) > 0) continue;
        slotguard_kick(pending[i].pc);
        pending[i].pc = NULL;
    }
}

static int need_players(Out *o) {
    if (!ue_is_listen_server(ue_world())) { out_printf(o, "not hosting a session\n"); return 0; }
    return 1;
}

static void kick(const char *arg, Out *o) {
    if (!need_players(o)) return;
    UObject *ps = find_player(arg, o);
    char nm[64];
    if (ps && !kick_ps(ps, "kicked", o)) { ps_name(ps, nm, sizeof nm); out_printf(o, "kicked %s\n", nm); }
}

static void ban(const char *arg, Out *o) {
    if (!need_players(o)) return;
    UObject *ps = find_player(arg, o);
    if (!ps) return;
    char nm[64], key[80];
    ps_name(ps, nm, sizeof nm);
    ps_key(ps, key, sizeof key);
    UObject *pc = ue_get_ptr(ps, "Owner");
    if (!is_pc(pc) || pc == ue_local_pc()) { out_printf(o, "%s can't be banned\n", nm); return; }
    bans_load();
    if (!is_banned(key, nm) && n_bans < 64) {
        snprintf(bans[n_bans].key, sizeof bans[0].key, "%s", key);
        snprintf(bans[n_bans].name, sizeof bans[0].name, "%s", nm);
        n_bans++;
        if (bans_save()) { out_printf(o, "could not write %s\n", bans_path()); return; }
    }
    kick_ps(ps, "banned", o);
    out_printf(o, "banned %s (%s)\n", nm, key);
}

static void unban(const char *arg, Out *o) {
    bans_load();
    if (!arg || !*arg) { out_printf(o, "usage: /unban <name|steam:id|#n from /bans|all>\n"); return; }
    int removed = 0;
    int idx = (arg[0] == '#') ? atoi(arg + 1) : -1;
    for (int i = 0; i < n_bans; i++) {
        int hit = !strcmp(arg, "all") || i == idx || !strcmp(bans[i].key, arg) || !_stricmp(bans[i].name, arg);
        if (!hit) continue;
        out_printf(o, "unbanned %s (%s)\n", bans[i].name, bans[i].key);
        bans[i] = bans[--n_bans];
        i--; removed++;
        if (idx >= 0) break;
    }
    if (!removed) { out_printf(o, "no ban matches '%s' (/bans)\n", arg); return; }
    bans_save();
}

static void list_bans(Out *o) {
    bans_load();
    if (!n_bans) { out_printf(o, "no bans\n"); return; }
    for (int i = 0; i < n_bans; i++) out_printf(o, "#%d %s %s\n", i, bans[i].name, bans[i].key);
}

static void lock(int on, Out *o) {
    locked = on;
    n_lock_allow = 0;
    if (on) {   // everyone here now may come back (clients reconnect when the host changes map)
        UObject **pa; int n = player_array(&pa);
        UObject *me = ue_local_pc();
        for (int i = 0; i < n && n_lock_allow < 16; i++) {
            UObject *pc = ue_get_ptr(pa[i], "Owner");
            if (is_pc(pc) && pc != me) ps_key(pa[i], lock_allow[n_lock_allow++], sizeof lock_allow[0]);
        }
    }
    LOG("admin: session %s (%d player(s) may rejoin)", on ? "locked" : "unlocked", n_lock_allow);
    out_printf(o, on ? "session locked: no new players (current players can still rejoin)\n" : "session unlocked\n");
}

// bots on|off: whether empty hero slots get bots, from the next map. PlayerSlotManager.bSupportsBots is copied from
// the game mode's bSupportsBots when the slot manager is created, and TeamSupportsBots() (the bot fill check) reads
// it; we set both right before InitSlots (teamsize.c's hook calls admin_on_initslots).
void admin_on_initslots(UObject *psm) {
    if (bots_mode < 0) return;
    UObject *w = ue_world(), *gm = w ? ue_get_ptr(w, "AuthorityGameMode") : NULL;
    int32_t a = ue_prop_offset(psm, "bSupportsBots"), b = gm ? ue_prop_offset(gm, "bSupportsBots") : -1;
    if (a >= 0) *((uint8_t *)psm + a) = (uint8_t)bots_mode;
    if (b >= 0) *((uint8_t *)gm + b) = (uint8_t)bots_mode;
    LOG("admin: bSupportsBots=%d for this map", bots_mode);
}

static void bots(const char *arg, Out *o) {
    if (arg && !strcmp(arg, "on")) bots_mode = 1;
    else if (arg && !strcmp(arg, "off")) bots_mode = 0;
    else if (arg && !strcmp(arg, "default")) bots_mode = -1;
    else if (arg && *arg) { out_printf(o, "usage: /bots on|off|default\n"); return; }
    UObject *gs = game_state(), *psm = gs ? ue_get_ptr(gs, "PlayerSlotManager") : NULL;
    int32_t off = psm ? ue_prop_offset(psm, "bSupportsBots") : -1;
    out_printf(o, "bots: %s from the next map (this map: %s)\n", bots_mode < 0 ? "game default" : bots_mode ? "on" : "off",
               off < 0 ? "?" : *((uint8_t *)psm + off) ? "on" : "off");
}

// restart: fail the mission the way a team wipe does (MissionGameMode::OnMissionEnd(false)); the game then runs its
// own retry path. Only when the difficulty's mission-end behavior is a retry, never when it would end the run.
static void restart(Out *o) {
    UObject *w = ue_world(), *gm = w ? ue_get_ptr(w, "AuthorityGameMode") : NULL, *gs = game_state();
    UClass *mc = ue_find_class("MissionGameMode");
    if (!gm || !mc || !ue_is_a(gm, mc)) { out_printf(o, "not in a mission\n"); return; }
    UFunction *fb = gs ? ue_find_function(U_CLASS(gs), "GetMissionEndedBehavior") : NULL;
    FField *rv = fb ? ue_find_prop(fb, "ReturnValue") : NULL;
    if (!rv) { out_printf(o, "restart: GetMissionEndedBehavior not found\n"); return; }
    uint8_t pb[16] = {0};
    ue_process_event(gs, fb, pb);
    int beh = pb[FP_OFFSET(rv)];
    static const char *B[] = {"RestartMission", "PreviousCheckpoint", "GameOver"};
    LOG("admin: restart requested, mission-ended behavior %d", beh);
    if (beh != 0 && beh != 1) { out_printf(o, "restart refused: failing now would end the run (%s)\n", beh == 2 ? B[2] : "?"); return; }
    UFunction *f = ue_find_function(U_CLASS(gm), "OnMissionEnd");
    FField *pbs = f ? ue_find_prop(f, "bSuccess") : NULL, *pc = f ? ue_find_prop(f, "Context") : NULL;
    if (!pbs || !pc) { out_printf(o, "restart: OnMissionEnd not found\n"); return; }
    static wchar_t ctx[32];
    uint8_t p[64] = {0};
    fstring_set((FString *)(p + FP_OFFSET(pc)), "b4bcoop restart", ctx, 32);
    ue_process_event(gm, f, p);
    out_printf(o, "restarting: mission failed on purpose (%s)\n", B[beh]);
}

// ready [vote]: host readies every player so nobody has to click: the loadout screen's Ready
// (GobiPlayerState::ServerRequestPlayerReady(true)) or, with "vote", the post-round screen's
// (ServerSetReadyForPostRoundVote). Server RPCs called on the server run locally, so this works for remote players too.
void admin_ready(const char *rest, Out *o) {
    int vote = rest && !strncmp(rest, "vote", 4);
    const char *fname = vote ? "ServerSetReadyForPostRoundVote" : "ServerRequestPlayerReady";
    UObject *w = ue_world();
    UObject *gs = w ? ue_get_ptr(w, "GameState") : NULL;
    UClass *psc = ue_find_class("GobiPlayerState");
    int32_t off = gs ? ue_prop_offset(gs, "PlayerArray") : -1;
    if (!psc || off < 0) { out_printf(o, "no gamestate\n"); return; }
    TArray *pa = (TArray *)((char *)gs + off);
    int n = 0;
    for (int i = 0; i < pa->num; i++) {
        UObject *ps = ((UObject **)pa->data)[i];
        UFunction *f = ps && ue_is_a(ps, psc) ? ue_find_function(U_CLASS(ps), fname) : NULL;
        if (!f) continue;
        uint8_t p[16] = {0};
        FField *pr = vote ? NULL : ue_find_prop(f, "bReady");
        int32_t ob = pr ? FP_OFFSET(pr) : -1;
        if (ob >= 0) p[ob] = 1;
        ue_process_event(ps, f, p);
        n++;
    }
    LOG("admin: %s on %d player(s)", fname, n);
    out_printf(o, "%s: %d player(s)\n", fname, n);
}

// ---- dispatch ----
static const struct { const char *name; int perm; const char *usage; } CMDS[] = {
    {"help", CMD_ANYONE, "/help"}, {"join", CMD_ANYONE, "/join steam:<id64> (or <ip[:port]> with host_ip=1)"},
    {"host", CMD_ANYONE, "/host"}, {"leave", CMD_ANYONE, "/leave"}, {"players", CMD_ANYONE, "/players"},
    {"flashlight", CMD_ANYONE, "/flashlight [on|off|auto]"}, {"thirdperson", CMD_ANYONE, "/thirdperson [on|off|distance|side|height|fov|reset]"},
    {"ping", CMD_ANYONE, "/ping"},
    {"kick", CMD_HOST, "/kick <name|#>"}, {"ban", CMD_HOST, "/ban <name|#>"}, {"unban", CMD_HOST, "/unban <name|steam:id|#n|all>"},
    {"bans", CMD_HOST, "/bans"}, {"lock", CMD_HOST, "/lock"}, {"unlock", CMD_HOST, "/unlock"}, {"teamsize", CMD_HOST, "/teamsize N"},
    {"restart", CMD_HOST, "/restart"}, {"ready", CMD_HOST, "/ready [vote]"}, {"bots", CMD_HOST, "/bots on|off|default"},
    {"say", CMD_HOST, "/say <message>"}, {"handoff", CMD_ANYONE, "/handoff [<name|#>]"},   // host part checked in handoff.c
    {"rules", CMD_HOST, "/rules [free_burncards on|off]"},   // rules.c: the host's run rules (~ Run tab)
    {"model", CMD_ANYONE, "/model list|<name>|reset"}, {"models", CMD_ANYONE, "/models [on|off]"},   // host parts checked in models.c
    {"addons", CMD_ANYONE, "/addons [on|off|info <#>|policy]"}, {"addon", CMD_ANYONE, "/addons [on|off|info <#>|policy]"},
};
#define N_CMDS ((int)(sizeof CMDS / sizeof CMDS[0]))

static void help(Out *o) {
    out_printf(o, "b4bcoop %s (protocol %d)\n/join steam:<id64>  /host  /leave\n/players  /ping  /flashlight [on|off|auto]\n"
                  "/thirdperson  (your own camera)\n/model list|<name>|reset\n/addons [on|off|info <#>|players]\n",
               coop_version(), coop_protocol());
    if (is_client()) { out_printf(o, "(/kick /ban /lock ... are for the host)\n/cheats  (when the host has cheats on: your own save)\n"); return; }
    out_printf(o, "host: /kick /ban <name|#>  /unban  /bans\n/lock  /unlock  /teamsize N  /bots on|off\n"
                  "/restart  /ready [vote]  /say <msg>\n/handoff <name|#>  (they host; the run goes on)\n"
                  "/cheats on|off  (sandbox, /cheats help)\n"
                  "/model <player> <name>  /models on|off\n/addons policy any|cosmetic|none|match\n"
                  "//text sends a message starting with /\n");
}

// Host actions shared by chat and the agent CLI. Returns 1 if handled.
static int run_admin(const char *verb, char *rest, Out *o) {
    if (!strcmp(verb, "kick")) kick(rest, o);
    else if (!strcmp(verb, "ban")) ban(rest, o);
    else if (!strcmp(verb, "unban")) unban(rest, o);
    else if (!strcmp(verb, "bans")) list_bans(o);
    else if (!strcmp(verb, "lock")) lock(1, o);
    else if (!strcmp(verb, "unlock")) lock(0, o);
    else if (!strcmp(verb, "bots")) bots(rest, o);
    else if (!strcmp(verb, "say")) say(rest, o);
    else if (!strcmp(verb, "restart")) restart(o);
    else return 0;
    return 1;
}

// A chat command typed by the local player (text after '/').
void admin_slash(char *line, Out *o) {
    char *verb = strtok(line, " ");
    char *rest = strtok(NULL, "");
    while (rest && *rest == ' ') rest++;
    if (!verb) { help(o); return; }
    for (char *c = verb; *c; c++) *c = (char)tolower((unsigned char)*c);
    int i = 0;
    for (; i < N_CMDS && strcmp(CMDS[i].name, verb); i++) {}
    int perm = i < N_CMDS ? CMDS[i].perm : cheats_perm(verb);
    if (perm < 0) { out_printf(o, "unknown command /%s (/help)\n", verb); return; }
    const char *why = NULL;
    if (perm == CMD_OWNSAVE && !cheats_own_save_ok(&why)) { out_printf(o, "/%s: %s\n", verb, why); return; }
    if (perm != CMD_ANYONE && perm != CMD_OWNSAVE && is_client()) { out_printf(o, "/%s: host only (you are a client)\n", verb); return; }
    if (perm == CMD_CHEAT && !cheats_enabled()) { out_printf(o, "/%s: cheats are off (the host types /cheats on first)\n", verb); return; }
    if (i == N_CMDS) { cheats_slash(verb, rest, o); return; }   // cheats.c: /cheats, /god, /fly, ...
    LOG("admin: /%s %s", verb, rest ? rest : "");
    if (!strcmp(verb, "help")) help(o);
    else if (!strcmp(verb, "players")) players(o);
    else if (!strcmp(verb, "ping")) ping(o);
    else if (!strcmp(verb, "flashlight")) cmd_flashlight(rest && *rest ? rest : "toggle", o);
    else if (!strcmp(verb, "thirdperson")) cmd_thirdperson(rest, o);
    else if (!strcmp(verb, "model") || !strcmp(verb, "models")) models_slash(verb, rest, o);
    else if (!strcmp(verb, "addons") || !strcmp(verb, "addon")) addons_slash(verb, rest, o);
    else if (!strcmp(verb, "handoff")) handoff_slash(rest, o);
    else if (!strcmp(verb, "rules")) rules_slash(rest, o);
    else if (!strcmp(verb, "join")) {
        if (!rest || !*rest) { out_printf(o, "usage: %s\n", CMDS[i].usage); return; }
        out_printf(o, "joining %s ...\n", rest);
        coop_join(rest);
    } else if (!strcmp(verb, "host")) {
        UObject *w = ue_world();
        char pkg[256] = "";
        if (w) ue_world_package(w, pkg, sizeof pkg);
        if (is_client()) out_printf(o, "you are in someone's session: /leave first\n");
        else if (ue_is_listen_server(w)) out_printf(o, "already hosting (%d player(s) connected)\n", ue_num_clients(w));
        else if (!strstr(pkg, "FortHope")) out_printf(o, "go back to Fort Hope first\n");
        else { out_printf(o, "hosting: others can now /join you\n"); coop_host(); }
    } else if (!strcmp(verb, "leave")) {
        if (!is_client()) { out_printf(o, "you are not in someone else's session\n"); return; }
        out_printf(o, "leaving, back to your own Fort Hope\n");
        coop_leave();
    } else if (!strcmp(verb, "teamsize")) {
        if (!rest || atoi(rest) < 1) { out_printf(o, "usage: /teamsize N (1-8, from the next map)\n"); return; }
        teamsize_cmd("teamsize", rest, o);
    } else if (!strcmp(verb, "ready")) {
        if (!need_players(o)) return;
        admin_ready(rest, o);
    } else run_admin(verb, rest, o);
}

// ~ overlay tab (overlay.h): the /players list with per-player Kick/Ban, and every host command. Each control runs the
// chat command (ov_run -> admin_slash), so permissions and replies are the chat's.
static void players_panel(void) {
    static double t_bans;
    static int shown_bans;
    UObject **pa; int n = player_array(&pa);
    UObject *me = ue_local_pc(), *my_ps = me ? ue_get_ptr(me, "PlayerState") : NULL;
    int client = is_client();
    const char *why;
    int host_ok = ov_allowed(CMD_HOST, &why);
    if (!n) ov_text_dim("No players (not in a game).");
    else if (ov_table_begin("players", 4)) {
        static const char *H[] = {"#", "Player", "Ping / Steam ID", ""};
        ov_table_header(H, 4);
        for (int i = 0; i < n; i++) {
            char nm[64], key[80], bn[64];
            UObject *ps = pa[i], *owner = ue_get_ptr(ps, "Owner");
            ps_name(ps, nm, sizeof nm);
            int npc = !nm[0] && !bot_name(ps, bn, sizeof bn) && npc_name(ps, nm, sizeof nm);
            int is_bot = npc || (!client ? !is_pc(owner) : !nm[0]);   // a client: a nameless player state is a bot
            if (is_bot && !nm[0] && !bot_name(ps, nm, sizeof nm)) snprintf(nm, sizeof nm, "-");
            ov_push_id(i);
            ov_table_next(); ov_text("%d", i);
            ov_table_next(); ov_text("%s%s%s", nm, ps == my_ps ? " (you)" : "", npc ? " [npc]" : is_bot ? " [bot]" : "");
            ov_table_next();
            if (!client && !is_bot) {
                ps_key(ps, key, sizeof key);
                if (ps != my_ps) ov_text("%d ms  %s", ps_ping_ms(ps), key); else ov_text("%s", key);
            } else if (ps == my_ps && client) ov_text("%d ms to the host", ps_ping_ms(ps));
            ov_table_next();
            if (!is_bot && ps != my_ps) {
                ov_begin_disabled(!host_ok, why);
                char lab[32];
                snprintf(lab, sizeof lab, "Kick##%d", i);
                if (ov_button(lab)) ov_run("kick #%d", i);
                ov_same_line();
                snprintf(lab, sizeof lab, "Ban##%d", i);
                if (ov_button_confirm(lab, "Sure? Ban")) ov_run("ban #%d", i);
                ov_same_line();
                snprintf(lab, sizeof lab, "Make host##%d", i);
                if (ov_button_confirm(lab, "Sure? Make host")) ov_run("handoff #%d", i);
                ov_tooltip("They host from now on and everyone moves to them; in a mission the run continues from the start of this chapter.");
                ov_end_disabled();
            }
            ov_pop_id();
        }
        ov_table_end();
    }
    handoff_players_panel();
    if (ov_button("Ping")) ov_run("ping");
    ov_same_line();
    if (ov_button("List in the log (/players)")) ov_run("players");

    ov_heading("Host");
    ov_begin_perm(CMD_HOST);
    static char msg[200];
    ov_width(18);
    int send = ov_input_text("##say", msg, sizeof msg, "message to everyone (/say)");
    ov_same_line();
    if ((ov_button("Say") || send) && msg[0]) { ov_run("say %s", msg); msg[0] = 0; }
    int lk = locked;
    if (ov_checkbox("Locked: no new players##lock", &lk)) ov_run(lk ? "lock" : "unlock");
    ov_tooltip("Players who are in the session now can still come back (map changes).");
    ov_text("Bots in empty slots (from the next map):");
    if (ov_radio("game default##bots", bots_mode < 0)) ov_run("bots default");
    ov_same_line();
    if (ov_radio("on##bots", bots_mode == 1)) ov_run("bots on");
    ov_same_line();
    if (ov_radio("off##bots", bots_mode == 0)) ov_run("bots off");
    static int ts;
    if (!ts) ts = teamsize_get() ? teamsize_get() : 4;
    ov_width(8);
    ov_input_int("Team size##teamsize", &ts, 1, 8);
    ov_same_line();
    if (ov_button("Apply##teamsize")) ov_run("teamsize %d", ts);
    ov_tooltip("From the next map. 5+ players: see teamsize in the commands reference.");
    ov_same_line();
    ov_text_dim("now %d", teamsize_get() ? teamsize_get() : 4);
    if (ov_button("Ready everyone")) ov_run("ready");
    ov_same_line();
    if (ov_button("Ready post-round vote")) ov_run("ready vote");
    ov_same_line();
    if (ov_button_confirm("Restart mission", "Sure? Restart")) ov_run("restart");
    ov_tooltip("Fails the mission on purpose; the game retries it (refused when that would end the run).");
    ov_heading("Bans");
    if (ov_table_begin("bans", 3)) {
        if (GetTickCount64() / 1000.0 - t_bans > 1.0) { bans_load(); t_bans = GetTickCount64() / 1000.0; shown_bans = n_bans; }
        for (int i = 0; i < shown_bans && i < n_bans; i++) {
            char lab[32];
            ov_table_next(); ov_text("%s", bans[i].name[0] ? bans[i].name : "?");
            ov_table_next(); ov_text("%s", bans[i].key);
            ov_table_next();
            snprintf(lab, sizeof lab, "Unban##%d", i);
            if (ov_button(lab)) { ov_run("unban #%d", i); t_bans = 0; }
        }
        ov_table_end();
    }
    if (!n_bans) ov_text_dim("No bans.");
    else if (ov_button_confirm("Unban all", "Sure? Unban all")) { ov_run("unban all"); t_bans = 0; }
    ov_end_perm();
    (void)host_ok;
}

#ifndef B4B_RELEASE
// Agent CLI (dev builds): kick ban unban bans lock unlock bots say restart, plus `slash <text>` (run a chat command
// directly, without the chat box) and `who` (the /players listing).
int admin_cmd(const char *verb, char *rest, Out *o) {
    if (!strcmp(verb, "slash")) { if (rest) admin_slash(rest[0] == '/' ? rest + 1 : rest, o); return 1; }
    if (!strcmp(verb, "who")) { players(o); return 1; }
    if (!strcmp(verb, "admin")) {
        out_printf(o, "admin: locked=%d allow=%d bots=%d refused=%d bans=%d (%s) prelogin=%d alloc=%d\n", locked,
                   n_lock_allow, bots_mode, n_refused, (bans_load(), n_bans), bans_path(), orig_prelogin != NULL, alloc_ok);
        return 1;
    }
    return run_admin(verb, rest, o);
}
#endif  // !B4B_RELEASE

int admin_init(void) {
    alloc_ok = !memcmp((void *)ADDR_RESIZE, SIG_RESIZE, sizeof SIG_RESIZE) && !memcmp((void *)ADDR_FREE, SIG_FREE, sizeof SIG_FREE);
    if (alloc_ok) resize_fn = (ResizeFn)ADDR_RESIZE; else LOG("admin: FString allocator signature mismatch");
    if (memcmp((void *)ADDR_PRELOGIN, SIG_PRELOGIN, sizeof SIG_PRELOGIN)) LOG("admin: PreLogin signature mismatch");
    else if (MH_CreateHook((void *)ADDR_PRELOGIN, (void *)prelogin_detour, (void **)&orig_prelogin) != MH_OK ||
             MH_EnableHook((void *)ADDR_PRELOGIN) != MH_OK) { LOG("admin: PreLogin hook failed"); orig_prelogin = NULL; }
    bans_load();
    overlay_add_panel("Players", 20, players_panel);
    LOG("admin: login gate %s, %d ban(s) in %s", orig_prelogin ? "on" : "OFF", n_bans, bans_path());
    return 0;
}
