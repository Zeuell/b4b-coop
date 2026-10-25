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
static int move_basic;                     // ini intel_basic=1: the free CardShrine_Basic goes along
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
// a point onto the walkable navmesh near it (NavigationSystemV1::K2_ProjectPointToNavigation): a spot inside a crate
// pile, a wall or above a drop has no navmesh within the extent and is refused; the projected point sits on the floor
static int nav_project(UObject *w, const float in[3], float out[3], float ext_xy, float ext_z) {
    UClass *k = ue_find_class("NavigationSystemV1");
    UObject *cdo = k ? UC_CDO(k) : NULL;
    uint8_t p[128] = {0};
    int32_t owc = poff(cdo, "K2_ProjectPointToNavigation", "WorldContextObject"), opt = poff(cdo, "K2_ProjectPointToNavigation", "Point"),
            opl = poff(cdo, "K2_ProjectPointToNavigation", "ProjectedLocation"), oqe = poff(cdo, "K2_ProjectPointToNavigation", "QueryExtent"),
            orv = poff(cdo, "K2_ProjectPointToNavigation", "ReturnValue");
    if (!cdo || owc < 0 || opt < 0 || opl < 0 || orv < 0) { static int said; if (!said++) LOG("intel: no K2_ProjectPointToNavigation"); return -1; }
    *(UObject **)(p + owc) = w;
    memcpy(p + opt, in, 12);
    if (oqe >= 0) { float e[3] = {ext_xy, ext_xy, ext_z}; memcpy(p + oqe, e, 12); }
    if (call_parms(cdo, "K2_ProjectPointToNavigation", p, sizeof p)) return -1;
    if (!p[orv]) return 0;
    memcpy(out, p + opl, 12);
    return 1;
}
// a candidate spot is good when the navmesh has a point within `slack` of it (horizontally) and nothing of ours is
// closer than `apart`; the result is the navmesh point (on the floor)
static int spot_ok(UObject *w, const float cand[3], float out[3], float slack, const float (*used)[3], int nused, float apart) {
    float pr[3];
    int r = nav_project(w, cand, pr, slack + 40, 160);
    if (r < 0) { memcpy(out, cand, 12); return 1; }   // no navmesh API: take it as is
    if (!r) return 0;
    float dx = pr[0] - cand[0], dy = pr[1] - cand[1];
    if (dx * dx + dy * dy > slack * slack) return 0;
    for (int i = 0; i < nused; i++) {
        float ux = pr[0] - used[i][0], uy = pr[1] - used[i][1];
        if (ux * ux + uy * uy < apart * apart) return 0;
    }
    memcpy(out, pr, 12);
    return 1;
}
static int room_enabled(UObject *room) {   // RoomVolume::IsRoomEnabled (the LSS trims the inactive treasure rooms)
    uint8_t p[16] = {0};
    int32_t orv = poff(room, "IsRoomEnabled", "ReturnValue");
    if (orv < 0 || call_parms(room, "IsRoomEnabled", p, sizeof p)) return -1;
    return p[orv] != 0;
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
    // CardShrine_Basic is the free "intel" one (a random card, no price): left where the game puts it unless asked
    return strstr(n, "Shrine") && strstr(n, "_BP_C") && !strstr(n, "PVP") && (move_basic || !strstr(n, "Shrine_Basic"));
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
        if (room_enabled(o) == 0) continue;   // the trimmed twin
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
    // spots on the room's floor (navmesh), nearest the centre first, lay_gap apart, inside the volume
    float used[MAX_MOVED][3]; int nused = 0;
    int k = 0;
    float floor_z = c[2] - e[2] + 40;
    for (int i = 0; i < n; i++) {
        Xform x;
        if (get_xform(shr[i], &x)) continue;
        float best[3]; int found = 0; float bestd = 1e30f;
        for (float gx = -e[0] + 60; gx <= e[0] - 60; gx += 60)
            for (float gy = -e[1] + 60; gy <= e[1] - 60; gy += 60) {
                float cand[3] = {c[0] + gx, c[1] + gy, floor_z}, out[3];
                float d = gx * gx + gy * gy;
                if (d >= bestd || !spot_ok(w, cand, out, 30, used, nused, lay_gap)) continue;
                memcpy(best, out, 12); bestd = d; found = 1;
            }
        if (!found) { LOG("intel: no free floor spot in the treasure room for stand %d", k); break; }
        Xform nx = x;
        nx.q[0] = nx.q[1] = nx.q[2] = 0; nx.q[3] = 1;
        memcpy(nx.t, best, 12);
        memcpy(used[nused++], best, 12);
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
    float used[MAX_MOVED][3]; int nused = 0;
    int k = 0;
    for (int i = 0; i < n; i++) {
        Xform x;
        if (get_xform(shr[i], &x)) continue;
        float side = lay_side + lay_gap * (float)(k / 2);
        if (k & 1) side = -side;                       // alternately right and left of the vendor
        Xform nx = x;
        memcpy(nx.q, vx.q, sizeof nx.q);                // facing the way the vendor faces
        // the spot, else the same further out in front (steps of 60), else further to the side
        int found = 0;
        for (int t = 0; t < 8 && !found; t++) {
            float ahead = lay_ahead + 60.f * (float)(t % 4), sd = side + (t >= 4 ? (side < 0 ? -lay_gap : lay_gap) : 0);
            float cand[3], out[3];
            for (int a = 0; a < 3; a++) cand[a] = vx.t[a] + fwd[a] * ahead + right[a] * sd;
            if (spot_ok(w, cand, out, 35, used, nused, lay_gap * 0.8f)) { memcpy(nx.t, out, 12); found = 1; }
        }
        if (!found) { LOG("intel: no free floor spot by the vendor for stand %d", k); break; }
        memcpy(used[nused++], nx.t, 12);
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
    else if (!strcmp(k, "intel_basic")) move_basic = atoi(v) != 0;
    else if (!strcmp(k, "intel_layout")) sscanf(v, "%f %f %f", &lay_ahead, &lay_side, &lay_gap);
}
int intel_live(const char *key, const char *val) {
    if (!strcmp(key, "intel_cards")) { int was = mode; mode = parse_mode(val); if (was != mode) { LOG("intel: card stands %s", MODE_NAMES[mode]); done_world = NULL; } }
    else if (!strcmp(key, "intel_layout")) { if (val) sscanf(val, "%f %f %f", &lay_ahead, &lay_side, &lay_gap); }
    else if (!strcmp(key, "intel_basic")) move_basic = val && atoi(val) != 0;
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
    if (sub && !strcmp(sub, "rooms")) {
        static UClass *c_actor; if (!c_actor) c_actor = ue_find_class("Actor");
        char on[96];
        for (int32_t i = 0, cnt = w ? ue_num_objects() : 0; i < cnt; i++) {
            UObject *x = ue_object_at(i);
            if (!x || (U_FLAGS(x) & 0x30) || !U_CLASS(x) || !ue_is_a(x, c_actor) || !ue_object_usable(x, i) || !in_world(x, w)) continue;
            if (strncmp(ue_obj_name(x, on, sizeof on), "RoomVolume", 10)) continue;
            uint8_t p[64] = {0}; float c[3] = {0}, e[3] = {0};
            int32_t oo = poff(x, "GetActorBounds", "Origin"), oe = poff(x, "GetActorBounds", "BoxExtent");
            if (oo >= 0 && oe >= 0 && !call_parms(x, "GetActorBounds", p, sizeof p)) { memcpy(c, p + oo, 12); memcpy(e, p + oe, 12); }
            out_printf(o, "  %-28s enabled %d  centre (%.0f %.0f %.0f) extent (%.0f %.0f %.0f)\n", on, room_enabled(x), c[0], c[1], c[2], e[0], e[1], e[2]);
        }
        return 1;
    }
    if (sub && !strcmp(sub, "nav")) {   // nav <x> <y> <z>: what the navmesh makes of a point
        float in[3] = {0}, out[3];
        char *a = strtok(NULL, " "), *b2 = strtok(NULL, " "), *c2 = strtok(NULL, " ");
        if (a && b2 && c2) { in[0] = (float)atof(a); in[1] = (float)atof(b2); in[2] = (float)atof(c2); }
        int r = w ? nav_project(w, in, out, 80, 160) : -1;
        out_printf(o, "project (%.0f %.0f %.0f): %s (%.0f %.0f %.0f)\n", in[0], in[1], in[2], r > 0 ? "on navmesh" : r == 0 ? "no navmesh near" : "no API", out[0], out[1], out[2]);
        return 1;
    }
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
