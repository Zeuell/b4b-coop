// intel: where a map's purchasable card stands end up (a host run rule; rules.c's Run tab, /rules intel, ini
// intel_cards=). docs/investigations/run-rules.md §4.
//
// The game's Loot Spawn System (LSS, server only, one pass at mission start: "LSS SpawnItems - Spawned=N") hands each
// level-placed loot spawner an item; the card items of /Game/Items/Tables/Cards_DT are actors: the free pickups
// (FreeCardDraw_BP, Card_DeckDraw_Pickup_BP: Items.Cards.RandomCard, the "intel files") and the card *shrines* a
// player buys a card from (CardShrine_Basic_BP, CardShrine_WildCard_BP, Shrine_ScavengerCard_BP, Shrine_UpgradeCard_BP,
// CardShrine_Legendary_BP: Items.Cards.Basic ...; Cost). They land wherever the roll put them: a side room, a
// corner of the map. The rule moves the shrines (never the free pickups) to one place:
//   intel_cards=saferoom   a row beside the start saferoom's vendor (SafeRoomVendor_BP, the one nearest the heroes)
//   intel_cards=treasure   (later) into the map's treasure / toolkit room
// How: the host's game thread looks for shrine actors of the current world twice a second once a mission runs; the
// ones not moved yet are teleported with the game's own K2_SetActorTransform (server authority; a replicated actor's
// move reaches the clients like any other). One log line per map, one chat notice.
// Dev: `intel` (state), `intel list` (every card actor + where), `intel move` (now), `intel layout <ahead> <side>
// <gap>` (units from the vendor; defaults 0, 180, 110).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "ue.h"
#include "log.h"
#include "cmds.h"

enum { INTEL_OFF, INTEL_SAFEROOM, INTEL_TREASURE };
static const char *const MODE_NAMES[] = {"off", "saferoom", "treasure"};
static int mode;                           // ini intel_cards=
static float lay_ahead = 140, lay_side = 110, lay_gap = 95;   // units from the vendor: in front, to its right, between stands (tuned live)
static UObject *done_world;                // the world the current pass belongs to
static double since;
static int n_moved_total, n_moved_map;
#define MAX_MOVED 64
static struct { UObject *o; int32_t idx; } moved[MAX_MOVED];
static int nmoved;

typedef struct { float q[4]; float t[3], pad0; float s[3], pad1; } Xform;   // FTransform (float, 0x30)

static int call_parms(UObject *o, const char *fn, void *p, size_t cap) {
    UFunction *f = o ? ue_find_function(U_CLASS(o), fn) : NULL;
    if (!f || UFN_PARMSSIZE(f) > cap) return -1;
    ue_process_event(o, f, p);
    return 0;
}
static int32_t poff(UObject *o, const char *fn, const char *prm) {
    UFunction *f = o ? ue_find_function(U_CLASS(o), fn) : NULL;
    FField *p = f ? ue_find_prop(f, prm) : NULL;
    return p ? FP_OFFSET(p) : -1;
}
static int get_xform(UObject *a, Xform *x) {
    uint8_t p[128] = {0};
    int32_t o = poff(a, "GetTransform", "ReturnValue");
    if (o < 0 || call_parms(a, "GetTransform", p, sizeof p)) return -1;
    memcpy(x, p + o, sizeof *x);
    return 0;
}
static int set_xform(UObject *a, const Xform *x) {
    static uint8_t p[1024];
    memset(p, 0, sizeof p);
    int32_t o = poff(a, "K2_SetActorTransform", "NewTransform"), t = poff(a, "K2_SetActorTransform", "bTeleport");
    if (o < 0) return -1;
    memcpy(p + o, x, sizeof *x);
    if (t >= 0) p[t] = 1;
    return call_parms(a, "K2_SetActorTransform", p, sizeof p);
}
// a quaternion's forward (X) and right (Y) axes
static void axes(const float q[4], float fwd[3], float right[3]) {
    float x = q[0], y = q[1], z = q[2], w = q[3];
    fwd[0] = 1 - 2 * (y * y + z * z); fwd[1] = 2 * (x * y + z * w);     fwd[2] = 2 * (x * z - y * w);
    right[0] = 2 * (x * y - z * w);   right[1] = 1 - 2 * (x * x + z * z); right[2] = 2 * (y * z + x * w);
}

#define in_world cheats_in_world   // the persistent level's actors and the streamed sublevels' (cheats.c)
static int is_shrine_class(UClass *c) {
    char n[96];
    ue_obj_name(c, n, sizeof n);
    // CardShrine_Basic is the free "intel" one (a random card, no price): left where the game puts it
    return strstr(n, "Shrine") && strstr(n, "_BP_C") && !strstr(n, "PVP") && !strstr(n, "Shrine_Basic");
}
static int is_card_pickup_class(UClass *c) {
    char n[96];
    ue_obj_name(c, n, sizeof n);
    return strstr(n, "_BP_C") && (strstr(n, "FreeCardDraw") || strstr(n, "Card_DeckDraw"));
}
static int already(UObject *o) { for (int i = 0; i < nmoved; i++) if (moved[i].o == o) return 1; return 0; }

// shrines of this world not moved yet (up to max), the vendor nearest to the local hero
static int collect(UObject *w, UObject **out, int max, UObject **vendor_out, int all) {
    static UClass *c_actor;
    if (!c_actor) c_actor = ue_find_class("Actor");
    UObject *pc = ue_local_pc(), *pawn = pc ? ue_get_ptr(pc, "Pawn") : NULL;
    Xform px; int have_p = pawn && !get_xform(pawn, &px);
    UObject *vendor = NULL; float best = 1e30f;
    int n = 0;
    char cn[96];
    for (int32_t i = 0, cnt = ue_num_objects(); i < cnt; i++) {
        UObject *o = ue_object_at(i);
        if (!o || (U_FLAGS(o) & 0x30) || !U_CLASS(o) || !ue_is_a(o, c_actor) || !ue_object_usable(o, i) || !in_world(o, w)) continue;
        UClass *c = U_CLASS(o);
        if (is_shrine_class(c)) { if ((all || !already(o)) && n < max) out[n++] = o; continue; }
        if (vendor_out && strstr(ue_obj_name(c, cn, sizeof cn), "SafeRoomVendor")) {
            Xform vx;
            if (get_xform(o, &vx)) continue;
            float d = have_p ? (vx.t[0] - px.t[0]) * (vx.t[0] - px.t[0]) + (vx.t[1] - px.t[1]) * (vx.t[1] - px.t[1]) : 0;
            if (!vendor || d < best) { vendor = o; best = d; }
        }
    }
    if (vendor_out) *vendor_out = vendor;
    return n;
}

// the map's treasure room: a RoomVolume actor named RoomVolume_Treasure*; its bounds give the centre and the floor
static int treasure_room(UObject *w, float center[3], float ext[3]) {
    static UClass *c_actor;
    if (!c_actor) c_actor = ue_find_class("Actor");
    char n[96];
    for (int32_t i = 0, cnt = ue_num_objects(); i < cnt; i++) {
        UObject *o = ue_object_at(i);
        if (!o || (U_FLAGS(o) & 0x30) || !U_CLASS(o) || !ue_is_a(o, c_actor) || !ue_object_usable(o, i) || !in_world(o, w)) continue;
        if (strncmp(ue_obj_name(o, n, sizeof n), "RoomVolume_Treasure", 19)) continue;
        uint8_t p[64] = {0};
        int32_t oo = poff(o, "GetActorBounds", "Origin"), oe = poff(o, "GetActorBounds", "BoxExtent"), oc = poff(o, "GetActorBounds", "bOnlyCollidingComponents");
        if (oo < 0 || oe < 0) return 0;
        if (oc >= 0) p[oc] = 0;
        if (call_parms(o, "GetActorBounds", p, sizeof p)) return 0;
        memcpy(center, p + oo, 12); memcpy(ext, p + oe, 12);
        return 1;
    }
    return 0;
}
static int move_treasure(UObject *w, Out *o) {
    UObject *shr[MAX_MOVED];
    int n = collect(w, shr, MAX_MOVED, NULL, 0);
    if (!n) return 0;
    float c[3], e[3];
    char b[96];
    if (!treasure_room(w, c, e)) { if (o) out_printf(o, "no treasure room volume in this map\n"); LOG("intel: %d shrine(s) but no RoomVolume_Treasure in this map", n); return 0; }
    // a row through the room's centre along its longer side, on the floor (the volume's bottom + a hand), standing up
    int along_x = e[0] >= e[1];
    int k = 0;
    for (int i = 0; i < n; i++) {
        Xform x;
        if (get_xform(shr[i], &x)) continue;
        float off = lay_gap * (float)((k + 1) / 2) * ((k & 1) ? -1.f : 1.f);
        Xform nx = x;
        nx.q[0] = nx.q[1] = nx.q[2] = 0; nx.q[3] = 1;
        nx.t[0] = c[0] + (along_x ? off : 0); nx.t[1] = c[1] + (along_x ? 0 : off); nx.t[2] = c[2] - e[2] + 10;
        if (set_xform(shr[i], &nx)) continue;
        if (nmoved < MAX_MOVED) { moved[nmoved].o = shr[i]; moved[nmoved].idx = U_INDEX(shr[i]); nmoved++; }
        LOG("intel: %s moved (%.0f %.0f %.0f) -> (%.0f %.0f %.0f) in the treasure room (centre %.0f %.0f %.0f, extent %.0f %.0f %.0f)",
            ue_obj_name(shr[i], b, sizeof b), x.t[0], x.t[1], x.t[2], nx.t[0], nx.t[1], nx.t[2], c[0], c[1], c[2], e[0], e[1], e[2]);
        if (o) out_printf(o, "moved %s\n", ue_obj_name(shr[i], b, sizeof b));
        k++;
    }
    n_moved_total += k; n_moved_map += k;
    return k;
}

static int move_saferoom(UObject *w, Out *o) {
    UObject *shr[MAX_MOVED], *vendor = NULL;
    int n = collect(w, shr, MAX_MOVED, &vendor, 0);
    if (!n) return 0;
    Xform vx;
    char b[96];
    if (!vendor || get_xform(vendor, &vx)) { if (o) out_printf(o, "no start saferoom vendor found\n"); LOG("intel: %d shrine(s) but no SafeRoomVendor in this map", n); return 0; }
    float fwd[3], right[3];
    axes(vx.q, fwd, right);
    int k = 0;
    for (int i = 0; i < n; i++) {
        Xform x;
        if (get_xform(shr[i], &x)) continue;
        float side = lay_side + lay_gap * (float)(k / 2);
        if (k & 1) side = -side;                       // alternately right and left of the vendor
        Xform nx = x;
        memcpy(nx.q, vx.q, sizeof nx.q);                // facing the way the vendor faces
        for (int a = 0; a < 3; a++) nx.t[a] = vx.t[a] + fwd[a] * lay_ahead + right[a] * side;
        if (set_xform(shr[i], &nx)) continue;
        if (nmoved < MAX_MOVED) { moved[nmoved].o = shr[i]; moved[nmoved].idx = U_INDEX(shr[i]); nmoved++; }
        LOG("intel: %s moved (%.0f %.0f %.0f) -> (%.0f %.0f %.0f) beside %s", ue_obj_name(shr[i], b, sizeof b), x.t[0], x.t[1], x.t[2],
            nx.t[0], nx.t[1], nx.t[2], ue_obj_name(vendor, b + 48, sizeof b - 48));
        if (o) out_printf(o, "moved %s\n", ue_obj_name(shr[i], b, sizeof b));
        k++;
    }
    n_moved_total += k; n_moved_map += k;
    return k;
}

static void reset_map(UObject *w) { done_world = w; nmoved = 0; n_moved_map = 0; }

void intel_tick(float dt) {
    if (mode == INTEL_OFF) return;
    UObject *w = ue_world();
    if (!w) return;
    if (w != done_world) reset_map(w);
    since += dt;
    if (since < 0.5) return;
    since = 0;
    if (!ue_is_listen_server(w) && admin_is_client()) return;   // a client's copy of the actors is not ours to move
    if (!cheats_in_mission()) return;
    int k = mode == INTEL_SAFEROOM ? move_saferoom(w, NULL) : move_treasure(w, NULL);
    if (k && n_moved_map == k) admin_notice(mode == INTEL_SAFEROOM ? "host rule: the purchasable card stands of this map are in the start saferoom, by the vendor"
                                                                    : "host rule: the purchasable card stands of this map are in the treasure room");
}

int intel_mode(void) { return mode; }
const char *intel_mode_name(void) { return MODE_NAMES[mode]; }
static int parse_mode(const char *v) {
    if (!v) return INTEL_OFF;
    for (int i = 0; i < 3; i++) if (!_stricmp(v, MODE_NAMES[i])) return i;
    if (!strcmp(v, "1") || !_stricmp(v, "on")) return INTEL_SAFEROOM;
    return INTEL_OFF;
}
static void ini_pair(const char *k, const char *v, void *ctx) {
    (void)ctx;
    if (!strcmp(k, "intel_cards")) mode = parse_mode(v);
    else if (!strcmp(k, "intel_layout")) sscanf(v, "%f %f %f", &lay_ahead, &lay_side, &lay_gap);
}
int intel_live(const char *key, const char *val) {
    if (!strcmp(key, "intel_cards")) { int was = mode; mode = parse_mode(val); if (was != mode) { LOG("intel: card stands %s", MODE_NAMES[mode]); done_world = NULL; } }
    else if (!strcmp(key, "intel_layout")) { if (val) sscanf(val, "%f %f %f", &lay_ahead, &lay_side, &lay_gap); }
    else return 0;
    return 1;
}
// chat: /rules intel off|saferoom|treasure
void intel_slash(const char *arg, Out *o) {
    if (!arg || !*arg) { out_printf(o, "card stands: %s (usage: /rules intel off|saferoom|treasure)\n", MODE_NAMES[mode]); return; }
    int m = parse_mode(arg);
    cmds_ini_apply("intel_cards", MODE_NAMES[m], 1);
    out_printf(o, "card stands: %s (saved in b4bcoop.ini; from the next mission start)\n", MODE_NAMES[m]);
    admin_notice(m == INTEL_SAFEROOM ? "host rule: purchasable card stands will be in the start saferoom" :
                 m == INTEL_TREASURE ? "host rule: purchasable card stands will be in the treasure room" : "host rule: card stands spawn where the game puts them again");
}

int intel_init(void) {
    cmds_ini_each(ini_pair, NULL);
    LOG("intel: card stands %s (intel_cards=), layout ahead %.0f side %.0f gap %.0f", MODE_NAMES[mode], lay_ahead, lay_side, lay_gap);
    return 0;
}

#ifndef B4B_RELEASE
int intel_cmd(const char *verb, char *rest, Out *o) {
    if (strcmp(verb, "intel")) return 0;
    UObject *w = ue_world();
    char *sub = rest ? strtok(rest, " ") : NULL;
    if (sub && !strcmp(sub, "move")) { if (w) { int k = mode == INTEL_TREASURE ? move_treasure(w, o) : move_saferoom(w, o); out_printf(o, "%d moved\n", k); } return 1; }
    if (sub && !strcmp(sub, "room")) { float c[3], e[3]; if (w && treasure_room(w, c, e)) out_printf(o, "treasure room centre (%.0f %.0f %.0f) extent (%.0f %.0f %.0f)\n", c[0], c[1], c[2], e[0], e[1], e[2]); else out_printf(o, "no treasure room\n"); return 1; }
    if (sub && !strcmp(sub, "layout")) {
        char *a = strtok(NULL, " "), *s = strtok(NULL, " "), *g = strtok(NULL, " ");
        if (a) lay_ahead = (float)atof(a); if (s) lay_side = (float)atof(s); if (g) lay_gap = (float)atof(g);
        out_printf(o, "layout ahead %.0f side %.0f gap %.0f (intel move re-places nothing already moved: use `intel reset` first)\n", lay_ahead, lay_side, lay_gap);
        return 1;
    }
    if (sub && !strcmp(sub, "reset")) { nmoved = 0; n_moved_map = 0; out_printf(o, "forgot the moved ones\n"); return 1; }
    if (sub && !strcmp(sub, "list")) {
        static UClass *c_actor; if (!c_actor) c_actor = ue_find_class("Actor");
        char cn[96], on[96];
        int n = 0;
        for (int32_t i = 0, cnt = w ? ue_num_objects() : 0; i < cnt; i++) {
            UObject *x = ue_object_at(i);
            if (!x || (U_FLAGS(x) & 0x30) || !U_CLASS(x) || !ue_is_a(x, c_actor) || !ue_object_usable(x, i) || !in_world(x, w)) continue;
            UClass *c = U_CLASS(x);
            int shrine = is_shrine_class(c), pickup = is_card_pickup_class(c), vendor = strstr(ue_obj_name(c, cn, sizeof cn), "SafeRoomVendor") != NULL;
            if (!shrine && !pickup && !vendor) continue;
            Xform t; int ok = !get_xform(x, &t);
            out_printf(o, "  %-8s %-40s %-32s at (%.0f %.0f %.0f)%s\n", shrine ? "shrine" : pickup ? "pickup" : "vendor", ue_obj_name(x, on, sizeof on), cn,
                       ok ? t.t[0] : 0, ok ? t.t[1] : 0, ok ? t.t[2] : 0, already(x) ? "  [moved]" : "");
            n++;
        }
        out_printf(o, "%d card actor(s)\n", n);
        return 1;
    }
    out_printf(o, "intel: %s, layout ahead %.0f side %.0f gap %.0f, moved %d this map, %d total; in mission %d, host %d\n", MODE_NAMES[mode],
               lay_ahead, lay_side, lay_gap, n_moved_map, n_moved_total, cheats_in_mission(), w ? ue_is_listen_server(w) : -1);
    return 1;
}
#endif
