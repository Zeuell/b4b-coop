// Post-round stats diagnostics (#30, docs/investigations/post-round-stats.md). Every build logs each player's
// post-round values once per post-round screen ("poststats: ..." lines), so a session's logs (host and clients) show
// what each machine had. Dev builds: `poststats` (same dump, any time) and UI probes for unattended tests
// (`uitext`, `uihide`, `callw`, `funcs`, `objat`). #30 itself was cheats.c withholding stats from a map with cheats
// on; cheats no longer withhold anything.
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "ue.h"
#include "cmds.h"
#include "log.h"

// GobiPlayerState: PostRoundStatValues (TArray<int32>, one per PostRoundStatConfigs entry), PostRoundLineupStats
// (double[11], EPostRoundLineupStat), ScoreboardStats (double[2]); all Replicated, written on the server in
// GobiPlayerState::Tick (0x141BCE320 -> 0x141BCE650) from PlayerStatsComponent (PS+0x768). Engine PlayerState's
// bool byte (bIsABot .. bFromPreviousLevel) at the bIsABot offset.
static int32_t off_values = -1, off_lineup = -1, off_score = -1, off_configs = -1, off_stats = -1, off_bits = -1;

static int offsets(UObject *ps) {
    if (off_values < 0) {
        off_values = ue_prop_offset(ps, "PostRoundStatValues");
        off_lineup = ue_prop_offset(ps, "PostRoundLineupStats");
        off_score = ue_prop_offset(ps, "ScoreboardStats");
        off_configs = ue_prop_offset(ps, "PostRoundStatConfigs");
        off_stats = ue_prop_offset(ps, "PlayerStatsComponent");
        off_bits = ue_prop_offset(ps, "bIsABot");
    }
    return off_values >= 0 && off_lineup >= 0 && off_score >= 0;
}

// PlayerStatsComponent stat banks (EPlayerProfileStat, 0x58 bytes each, value first): +0x158 this map (reconciled
// into the profile and reset on leaving the map), +0x1078 since the component was created (the post-round values),
// +0x1F98 not yet sent to the owning client (ClientApplyStatDeltas). IncrementStatValue 0x1422B6890.
#define BANK_MAP  0x158
#define BANK_ALL  0x1078
#define STAT_KILLS 0   // EPlayerProfileStat::RiddenKilled

typedef void (*Emit)(void *ctx, const char *line);

static void dump(Emit emit, void *ctx) {
    UObject **pa; int n = admin_player_array(&pa);
    char line[512];
    if (!n) { emit(ctx, "no players"); return; }
    if (!offsets(pa[0])) { emit(ctx, "properties not found"); return; }
    for (int i = 0; i < n; i++) {
        UObject *ps = pa[i];
        char nm[64];
        admin_display_name(ps, nm, sizeof nm);
        TArray *v = (TArray *)((char *)ps + off_values);
        double *l = (double *)((char *)ps + off_lineup), *s = (double *)((char *)ps + off_score);
        UObject *sc = off_stats >= 0 ? *(UObject **)((char *)ps + off_stats) : NULL;
        int k = snprintf(line, sizeof line, "#%d %s values[%d]:", i, nm, v->num);
        for (int j = 0; j < v->num && j < 16 && k < (int)sizeof line - 16; j++)
            k += snprintf(line + k, sizeof line - k, " %d", ((int32_t *)v->data)[j]);
        k += snprintf(line + k, sizeof line - k, " lineup:");
        for (int j = 0; j < 11 && k < (int)sizeof line - 16; j++) k += snprintf(line + k, sizeof line - k, " %.0f", l[j]);
        k += snprintf(line + k, sizeof line - k, " scoreboard: %.0f %.0f bits=%02x", s[0], s[1],
                      off_bits >= 0 ? *((uint8_t *)ps + off_bits) : 0xff);
        if (sc) snprintf(line + k, sizeof line - k, " stats: kills map %d all %d",
                         *(int32_t *)((char *)sc + BANK_MAP + STAT_KILLS * 0x58),
                         *(int32_t *)((char *)sc + BANK_ALL + STAT_KILLS * 0x58));
        else snprintf(line + k, sizeof line - k, " stats: none");
        emit(ctx, line);
    }
}

static void emit_log(void *ctx, const char *line) { (void)ctx; LOG("poststats: %s", line); }

// Once per post-round screen (GameState.MatchState WaitingPostMatch), 5 s in: the replicated values have arrived.
static int logged;
static float in_post;
void poststats_tick(float dt) {
    static float acc;
    if ((acc += dt) < 0.5f) return;
    float step = acc; acc = 0;
    UObject *w = ue_world(), *gs = w ? ue_get_ptr(w, "GameState") : NULL;
    int32_t off = gs ? ue_prop_offset(gs, "MatchState") : -1;
    char ms[64] = "";
    if (off >= 0) ue_name(*(FName *)((char *)gs + off), ms, sizeof ms);
    if (strcmp(ms, "WaitingPostMatch")) { in_post = 0; logged = 0; return; }
    if (logged || (in_post += step) < 5.f) return;
    logged = 1;
    LOG("poststats: post-round values (%s)", admin_is_client() ? "client" : "host");
    dump(emit_log, NULL);
}

#ifndef B4B_RELEASE
static void emit_out(void *ctx, const char *line) { out_printf((Out *)ctx, "%s\n", line); }

// dev: `uitext <path substring> [max]` prints the Text of every live TextBlock / RichTextBlock whose full path
// contains the substring (what a panel shows, without a screenshot)
static void uitext(const char *needle, int max, Out *o) {
    UClass *tb = ue_find_class("TextBlock"), *rtb = ue_find_class("RichTextBlock"), *ktl = ue_find_class("KismetTextLibrary");
    UFunction *conv = ktl ? ue_find_function(ktl, "Conv_TextToString") : NULL;
    if (!conv || !UC_CDO(ktl)) { out_printf(o, "no Conv_TextToString\n"); return; }
    FField *pin = ue_find_prop(conv, "InText"), *pret = ue_find_prop(conv, "ReturnValue");
    static char path[1024];
    int hits = 0;
    for (int i = 0, n = ue_num_objects(); i < n && hits < max; i++) {
        UObject *x = ue_object_at(i);
        if (!x || !U_CLASS(x) || (U_FLAGS(x) & 0x30)) continue;
        if (!((tb && ue_is_a(x, tb)) || (rtb && ue_is_a(x, rtb)))) continue;
        ue_full_path(x, path, sizeof path);
        if (!strstr(path, needle)) continue;
        int32_t off = ue_prop_offset(x, "Text");
        if (off < 0) continue;
        uint8_t parms[128] = {0};
        memcpy(parms + FP_OFFSET(pin), (char *)x + off, 0x18);
        ue_process_event(UC_CDO(ktl), conv, parms);
        FString *s = (FString *)(parms + FP_OFFSET(pret));
        char txt[256] = "";
        for (int k = 0; s->data && k < s->num && k < 255 && s->data[k]; k++) txt[k] = s->data[k] < 128 ? (char)s->data[k] : '?';
        const char *tail = strstr(path, needle) + strlen(needle);   // the path below the match, WidgetTree_0 dropped
        static char shortp[512];
        int k2 = 0;
        for (const char *p = tail; *p && k2 < 500; ) {
            if (!strncmp(p, "WidgetTree_0.", 13)) { p += 13; continue; }
            shortp[k2++] = *p++;
        }
        shortp[k2] = 0;
        tail = shortp;
        int32_t voff = ue_prop_offset(x, "Visibility");
        out_printf(o, "%s [vis %d] \"%s\"\n", tail, voff >= 0 ? *((uint8_t *)x + voff) : -1, txt);
        hits++;
    }
    out_printf(o, "%d text(s)\n", hits);
}

int poststats_cmd(const char *verb, char *rest, Out *o) {
    if (!strcmp(verb, "uitext") && rest) {
        char *needle = strtok(rest, " "), *m = strtok(NULL, " ");
        uitext(needle, m ? atoi(m) : 60, o);
        return 1;
    }
    if (!strcmp(verb, "objat") && rest) {   // objat <hex address>: class and full path of a live object
        UObject *x = (UObject *)(uintptr_t)strtoull(rest, NULL, 16);
        char p[1024], c[128];
        int ok = 0;
        for (int i = 0, n = ue_num_objects(); i < n && !ok; i++) ok = ue_object_at(i) == x;
        if (!ok) { out_printf(o, "not a live object\n"); return 1; }
        out_printf(o, "%s %s\n", ue_obj_name(U_CLASS(x), c, sizeof c), ue_full_path(x, p, sizeof p));
        return 1;
    }
    if (!strcmp(verb, "uihide") && rest) {   // uihide <path suffix> [visibility]: set live widgets whose path ends so
        UClass *wc = ue_find_class("Widget");  // Collapsed (1, default) or e.g. Visible (0): screenshots of what a
        char *sfx = strtok(rest, " "), *vs = strtok(NULL, " ");   // modal hides (the post-round summary behind popups)
        if (!sfx) return 1;
        rest = sfx;
        size_t L = strlen(rest);
        char p[1024];
        int n = 0;
        for (int i = 0, m = ue_num_objects(); i < m; i++) {
            UObject *x = ue_object_at(i);
            if (!x || !U_CLASS(x) || (U_FLAGS(x) & 0x30) || !wc || !ue_is_a(x, wc)) continue;
            ue_full_path(x, p, sizeof p);
            size_t pl = strlen(p);
            if (pl < L || strcmp(p + pl - L, rest) || !strstr(p, "Transient")) continue;
            UFunction *f = ue_find_function(U_CLASS(x), "SetVisibility");
            uint8_t v[16] = {(uint8_t)(vs ? atoi(vs) : 1)};   // ESlateVisibility
            if (f) { ue_process_event(x, f, v); n++; out_printf(o, "visibility %d: %s\n", v[0], p); }
        }
        out_printf(o, "%d widget(s)\n", n);
        return 1;
    }
    if (!strcmp(verb, "callw") && rest) {   // callw <path suffix> <Func>: call a parameterless function on live widgets
        char *sfx = strtok(rest, " "), *fn = strtok(NULL, " "), p[1024];
        if (!sfx || !fn) return 1;
        size_t L = strlen(sfx);
        for (int i = 0, m = ue_num_objects(); i < m; i++) {
            UObject *x = ue_object_at(i);
            if (!x || !U_CLASS(x) || (U_FLAGS(x) & 0x30)) continue;
            ue_full_path(x, p, sizeof p);
            size_t pl = strlen(p);
            if (pl < L || strcmp(p + pl - L, sfx) || !strstr(p, "Transient")) continue;
            UFunction *f = ue_find_function(U_CLASS(x), fn);
            if (!f) { out_printf(o, "no %s on %s\n", fn, p); continue; }
            static uint8_t parms[1024];
            memset(parms, 0, sizeof parms);
            ue_process_event(x, f, parms);
            out_printf(o, "called %s on %s\n", fn, p);
        }
        return 1;
    }
    if (!strcmp(verb, "funcs") && rest) {   // funcs <Class>: the UFunctions a class declares (incl. Blueprint classes)
        UClass *c = ue_find_class(rest);
        char nm[128];
        int n = 0;
        for (int i = 0, m = ue_num_objects(); i < m && !c; i++) {   // Blueprint classes: the class of a live instance
            UObject *x = ue_object_at(i);
            if (x && U_CLASS(x) && !strcmp(ue_obj_name(U_CLASS(x), nm, sizeof nm), rest)) c = U_CLASS(x);
        }
        if (!c) { out_printf(o, "no class %s\n", rest); return 1; }
        for (UObject *f = US_CHILDREN(c); f; f = UF_NEXT(f)) { out_printf(o, "%s\n", ue_obj_name(f, nm, sizeof nm)); n++; }
        out_printf(o, "%d function(s)\n", n);
        return 1;
    }
    if (!strcmp(verb, "props") && rest) {   // props <object name> [all]: an object's properties with offsets and plain values
        char nm[128], tn[64];
        char *name = strtok(rest, " "), *all = strtok(NULL, " ");
        UObject *x = NULL;
        for (int i = 0, m = ue_num_objects(); i < m && !x; i++) {
            UObject *y = ue_object_at(i);
            if (y && U_CLASS(y) && !(U_FLAGS(y) & 0x30) && !strcmp(ue_obj_name(y, nm, sizeof nm), name)) x = y;
        }
        if (!x) { out_printf(o, "no object %s\n", name); return 1; }
        out_printf(o, "%s (%s)\n", name, ue_obj_name(U_CLASS(x), nm, sizeof nm));
        for (UStruct *st = U_CLASS(x); st; st = US_SUPER(st)) {
            if (!all && !strcmp(ue_obj_name(st, nm, sizeof nm), "Actor")) break;   // the engine's own stop here unless `all`
            out_printf(o, " [%s]\n", ue_obj_name(st, nm, sizeof nm));
            for (FField *f = US_CHILDPROPS(st); f; f = FF_NEXT(f)) {
                const char *t = ue_name(*(FName *)FF_CLASS(f), tn, sizeof tn);
                const char *v = (const char *)x + FP_OFFSET(f);
                char val[160] = "";
                if (!strcmp(t, "BoolProperty")) snprintf(val, sizeof val, "= 0x%02x", (unsigned)*(const uint8_t *)v);
                else if (!strcmp(t, "IntProperty")) snprintf(val, sizeof val, "= %d", *(const int32_t *)v);
                else if (!strcmp(t, "FloatProperty")) snprintf(val, sizeof val, "= %g", *(const float *)v);
                else if (!strcmp(t, "ByteProperty") || !strcmp(t, "EnumProperty")) snprintf(val, sizeof val, "= %u", (unsigned)*(const uint8_t *)v);
                else if (!strcmp(t, "NameProperty")) { char b[96]; snprintf(val, sizeof val, "= %s", ue_name(*(const FName *)v, b, sizeof b)); }
                else if (!strcmp(t, "ObjectProperty") || !strcmp(t, "ClassProperty")) { UObject *r = *(UObject *const *)v; char b[96]; snprintf(val, sizeof val, "= %s", r ? ue_obj_name(r, b, sizeof b) : "null"); }
                else if (!strcmp(t, "ArrayProperty")) snprintf(val, sizeof val, "num %d", ((const TArray *)v)->num);
                out_printf(o, "  %-36s %-16s @%-4d %s\n", ue_name(FF_NAME(f), nm, sizeof nm), t, FP_OFFSET(f), val);
            }
        }
        return 1;
    }
    if (strcmp(verb, "poststats")) return 0;
    dump(emit_out, o);
    return 1;
}
#endif
