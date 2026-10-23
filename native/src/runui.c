// runui: the host's run rules on the game's own create-run screen (rules.c's baseline cards and free burn cards),
// built from the game's widgets so it looks like the rest of the screen. docs/investigations/run-rules.md §3.
//
// The war table's run setup is MatchmakingScreen_WBP -> ContentSwitcher -> MatchmakingSettingsPanel_WBP (the left
// column: DifficultySelect, ActSelect, Map; the right: SettingsInfo, 1302 x 719 at (1059, 470) of the 3840-wide
// design, nothing below it). Each time such a panel appears, a VerticalBox is added to its root CanvasPanel under
// SettingsInfo and filled with the game's own TextButton_WBP_C buttons (WidgetBlueprintLibrary::Create, like
// lineup.c's player-list rows), labelled through their SetText:
//   BASELINE CARDS                    a TextBlock, font copied from the "Difficulty" heading
//   [x  Weapon Scavenger]  ...        one button per baseline card: click removes it
//   [+  Default Cards]                opens the picker: a full-screen layer over the settings panel with the deck
//                                     builder's own card grid (CardPaginator_WBP_C: 5 x 2 cards a page, Prev/Next,
//                                     SetCards / SetEquippedCards / GetVisibleCard), a search box, [Done]; clicking a
//                                     card adds or removes it (the equipped mark shows the current defaults)
//   [Burn cards are free: on|off]     the other rule
// Clicks: no delegates are bound (nothing to bind a C function to); every frame the game thread asks each of our
// buttons IsHovered() and watches the left mouse button's down edge. The widgets belong to the screen (outer: its
// WidgetTree) and die with it; the panel is found again when the screen next opens. Every action goes through the
// chat command path (admin_slash "rules ...": the same permission checks and ini writes as the ~ window and /rules).
// Only the host sees it (a client's screen is the host's run anyway). Ini runui=0 turns it off.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include "ue.h"
#include "log.h"
#include "cmds.h"
#include "overlay.h"

#define PICK_ROWS 9
#define MAX_OURS 24

static int enabled = 1;
static UObject *panel, *box, *filter_box;     // the settings panel we built into; our VerticalBox; the picker's text box
static int32_t panel_idx;
static int picking, built_for;                // picker open; nbase/picking/free state the rows were built for
static char filter[64];
static struct { UObject *w; int kind, arg; } ours[MAX_OURS];   // our clickable widgets: kind 1 remove base[arg], 2 open picker, 3 add card[arg], 4 back, 5 toggle free
static int nours;
static int lmb_was;
static double since_check;
static UClass *c_text_button, *c_vbox, *c_hbox, *c_textblock, *c_editbox, *c_panelw;
static int32_t o_slots, o_content, o_wt, o_root;
static int n_panels_built, n_clicks, ready;
static UObject *modal, *modal_back, *paginator, *status_text, *modal_done;   // the picker (modal_open / modal_close): content, backdrop

// ---- reflection helpers (lineup.c's) ----
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
static FField *first_parm(UObject *o, const char *fn) {   // the function's first parameter (a BP SetText's "InText", whatever its name)
    UFunction *f = o ? ue_find_function(U_CLASS(o), fn) : NULL;
    return f ? US_CHILDPROPS(f) : NULL;
}
static UObject *create_widget(UObject *ctx, UClass *cls) {
    UClass *wbl = ue_find_class("WidgetBlueprintLibrary");
    UObject *cdo = wbl ? UC_CDO(wbl) : NULL;
    uint8_t p[128] = {0};
    int32_t ow = poff(cdo, "Create", "WorldContextObject"), ot = poff(cdo, "Create", "WidgetType"),
            op = poff(cdo, "Create", "OwningPlayer"), orv = poff(cdo, "Create", "ReturnValue");
    if (!cdo || !cls || ow < 0 || ot < 0 || op < 0 || orv < 0) return NULL;
    *(UObject **)(p + ow) = ctx;
    *(UClass **)(p + ot) = cls;
    *(UObject **)(p + op) = ue_local_pc();
    if (call_parms(cdo, "Create", p, sizeof p)) return NULL;
    return *(UObject **)(p + orv);
}
// FText from a C string, through KismetTextLibrary (the game's allocator owns it; the parms are never destructed)
static int make_text(const char *s, uint8_t *text18) {
    UClass *k = ue_find_class("KismetTextLibrary");
    UObject *cdo = k ? UC_CDO(k) : NULL;
    uint8_t p[96] = {0};
    int32_t is = poff(cdo, "Conv_StringToText", "inString"), orv = poff(cdo, "Conv_StringToText", "ReturnValue");
    if (is < 0) is = poff(cdo, "Conv_StringToText", "InString");
    if (!cdo || is < 0 || orv < 0) return 0;
    static wchar_t w[128];
    int n = 0;
    for (; s[n] && n < 127; n++) w[n] = (unsigned char)s[n];
    w[n] = 0;
    FString fs = {w, n + 1, n + 1};
    memcpy(p + is, &fs, sizeof fs);
    if (call_parms(cdo, "Conv_StringToText", p, sizeof p)) return 0;
    memcpy(text18, p + orv, 0x18);
    return 1;
}
static int set_text(UObject *w, const char *s) {   // UTextBlock / UEditableTextBox / TextButton_WBP_C: SetText(FText)
    FField *f = first_parm(w, "SetText");
    uint8_t p[64] = {0};
    if (!f || FP_ELSIZE(f) != 0x18 || !make_text(s, p + FP_OFFSET(f))) return 0;
    return !call_parms(w, "SetText", p, sizeof p);
}
static void text_of(UObject *w, char *out, size_t n) {   // UEditableTextBox::GetText -> ASCII
    out[0] = 0;
    uint8_t p[64] = {0};
    int32_t orv = poff(w, "GetText", "ReturnValue");
    if (orv < 0 || call_parms(w, "GetText", p, sizeof p)) return;
    UClass *k = ue_find_class("KismetTextLibrary");
    UObject *cdo = k ? UC_CDO(k) : NULL;
    uint8_t q[96] = {0};
    int32_t it = poff(cdo, "Conv_TextToString", "InText"), rv = poff(cdo, "Conv_TextToString", "ReturnValue");
    if (!cdo || it < 0 || rv < 0) return;
    memcpy(q + it, p + orv, 0x18);
    if (call_parms(cdo, "Conv_TextToString", q, sizeof q)) return;
    FString *fs = (FString *)(q + rv);
    size_t m = 0;
    for (int i = 0; fs->data && i < fs->num && fs->data[i] && m + 1 < n; i++) out[m++] = fs->data[i] < 128 ? (char)fs->data[i] : '?';
    out[m] = 0;
}
static int hovered(UObject *w) {
    uint8_t p[16] = {0};
    int32_t orv = poff(w, "IsHovered", "ReturnValue");
    if (orv < 0 || call_parms(w, "IsHovered", p, sizeof p)) return 0;
    return p[orv] != 0;
}
// a child of class `cls` anywhere under a widget (into nested UserWidgets too)
static UObject *find_class_in(UObject *w, const char *cls, int depth) {
    char b[64];
    if (!w || depth > 10) return NULL;
    if (!strcmp(ue_obj_name(U_CLASS(w), b, sizeof b), cls)) return w;
    UObject *wt = ue_find_prop((UStruct *)U_CLASS(w), "WidgetTree") ? *(UObject **)((char *)w + o_wt) : NULL;
    if (wt) { UObject *r = find_class_in(*(UObject **)((char *)wt + o_root), cls, depth + 1); if (r) return r; }
    if (!ue_is_a(w, c_panelw)) return NULL;
    TArray *s = (TArray *)((char *)w + o_slots);
    for (int i = 0; s->data && i < s->num; i++) {
        UObject *sl = ((UObject **)s->data)[i], *c = sl ? *(UObject **)((char *)sl + o_content) : NULL;
        UObject *r = find_class_in(c, cls, depth + 1);
        if (r) return r;
    }
    return NULL;
}
// a property's value through a setter on another widget (FSlateFontInfo, FSlateColor: copy-assigned by the setter)
static void copy_via(UObject *from, const char *prop, UObject *to, const char *setter) {
    FField *f = from ? ue_find_prop((UStruct *)U_CLASS(from), prop) : NULL;
    FField *a = first_parm(to, setter);
    if (!f || !a || FP_ELSIZE(f) != FP_ELSIZE(a) || FP_ELSIZE(f) > 160) return;
    uint8_t p[192] = {0};
    memcpy(p + FP_OFFSET(a), (char *)from + FP_OFFSET(f), FP_ELSIZE(f));
    call_parms(to, setter, p, sizeof p);
}
static UObject *add_to(UObject *parent, UObject *child) {   // PanelWidget::AddChild -> the slot
    uint8_t p[32] = {0};
    int32_t pc = poff(parent, "AddChild", "Content"), pr = poff(parent, "AddChild", "ReturnValue");
    if (!child || pc < 0 || pr < 0) return NULL;
    *(UObject **)(p + pc) = child;
    call_parms(parent, "AddChild", p, sizeof p);
    return *(UObject **)(p + pr);
}
static void slot_pad(UObject *slot, float l, float t, float r, float b) {   // VerticalBoxSlot::SetPadding(FMargin)
    FField *a = first_parm(slot, "SetPadding");
    if (!a || FP_ELSIZE(a) != 16) return;
    uint8_t p[32] = {0}; float m[4] = {l, t, r, b};
    memcpy(p + FP_OFFSET(a), m, 16);
    call_parms(slot, "SetPadding", p, sizeof p);
}
static UObject *new_widget(UObject *outer, UClass *cls) { return cheats_new_object(cls, outer); }

// ---- the card list for the picker: PlayerCards_MASTER_DT rows (the card manager only exists in a mission) ----
#define MAX_CARDS 512
static struct { char row[64], shown[64]; UObject *table; FName name; } cards[MAX_CARDS];
static int ncards;
static int card_cmp(const void *a, const void *b) { return _stricmp(((const char *)a) + 64, ((const char *)b) + 64); }
// the cards a host may pick: the deck builder's (AllowedForDeckbuilding) and the hero passives (Hero*Ability rows:
// "Prophet" Dan, ...), none hidden or PvE-disabled; test/debug rows stay out
static void card_cb(const CardRowInfo *c, void *ctx) {
    (void)ctx;
    if (ncards >= MAX_CARDS || c->hidden == 1 || c->pve_disabled == 1) return;
    int hero = !strncmp(c->row, "Hero", 4) && strstr(c->row, "Ability");
    if (c->deckbuilding != 1 && !hero) return;
    snprintf(cards[ncards].row, 64, "%s", c->row);
    snprintf(cards[ncards].shown, 64, "%s", c->shown[0] ? c->shown : c->row);
    cards[ncards].table = c->table; cards[ncards].name = c->name;
    ncards++;
}
static void cards_load(void) {
    if (ncards) return;
    cheats_card_table_each("PlayerCards_MASTER_DT", card_cb, NULL);
    qsort(cards, ncards, sizeof cards[0], card_cmp);
    LOG("runui: %d cards from PlayerCards_MASTER_DT", ncards);
}
static const char *shown_of(const char *row) {
    for (int i = 0; i < ncards; i++) if (!_stricmp(cards[i].row, row)) return cards[i].shown;
    return row;
}
static int has_ci(const char *hay, const char *needle) {
    size_t n = strlen(needle);
    for (; *hay; hay++) if (!_strnicmp(hay, needle, n)) return 1;
    return 0;
}


// ---- the picker: the deck builder's card grid over the settings panel ----
// A row handle array for the paginator's SetCards / SetEquippedCards: FDataTableRowHandle {UDataTable*, FName, FString}
// (0x20) in a buffer from the game's allocator (GMalloc->Realloc; the function copies the array, the buffer is reused)
#define ADDR_GMALLOC VA(0x1469E59F0ull)
#define HANDLE_SZ 0x20
static void *handles_buf;
static int handles_ok(void) {
    if (handles_buf) return 1;
    void *gm = *(void **)ADDR_GMALLOC;
    if (!gm) return 0;
    typedef void *(*ReallocFn)(void *, void *, size_t, uint32_t);
    handles_buf = ((ReallocFn)(*(void ***)gm)[4])(gm, NULL, (size_t)MAX_CARDS * HANDLE_SZ, 8);
    return handles_buf != NULL;
}
static int array_call(UObject *w, const char *fn, int (*want)(int i)) {   // fn(TArray<FDataTableRowHandle>) with the cards want() picks
    UFunction *f = w ? ue_find_function(U_CLASS(w), fn) : NULL;
    FField *a = f ? US_CHILDPROPS(f) : NULL;
    FField *inner = a ? *(FField **)((char *)a + 0x78) : NULL;   // ArrayProperty's element property (NOTES.md: subtype at +0x78)
    if (!a || FP_ELSIZE(a) != 16 || !inner || FP_ELSIZE(inner) != HANDLE_SZ || !handles_ok()) {
        static int said; if (!said++) LOG("runui: %s: not a TArray<FDataTableRowHandle> parameter (%p, inner %d)", fn, (void *)a, inner ? FP_ELSIZE(inner) : -1);
        return 0;
    }
    int n = 0;
    for (int i = 0; i < ncards && n < MAX_CARDS; i++) {
        if (!want(i)) continue;
        char *h = (char *)handles_buf + (size_t)n * HANDLE_SZ;
        memset(h, 0, HANDLE_SZ);
        *(UObject **)h = cards[i].table;
        *(FName *)(h + 8) = cards[i].name;
        n++;
    }
    uint8_t p[64] = {0};
    TArray arr = {handles_buf, n, n};
    memcpy(p + FP_OFFSET(a), &arr, sizeof arr);
    call_parms(w, fn, p, sizeof p);
    return n;
}
static int want_filtered(int i) { return !filter[0] || has_ci(cards[i].shown, filter) || has_ci(cards[i].row, filter); }
static int want_default(int i) { return rules_baseline_has(cards[i].row); }

static void modal_close(void) {
    uint8_t p[16] = {0};
    if (modal) call_parms(modal, "RemoveFromParent", p, sizeof p);
    if (modal_back) call_parms(modal_back, "RemoveFromParent", p, sizeof p);
    modal = modal_back = paginator = status_text = filter_box = modal_done = NULL;
}
static void modal_status(void) {
    char rows[16][64], line[600] = "Default cards: ";
    int n = rules_baseline_rows(rows, 16);
    for (int i = 0; i < n; i++) { size_t l = strlen(line); snprintf(line + l, sizeof line - l, "%s%s", i ? ", " : "", shown_of(rows[i])); }
    if (!n) strcat(line, "none. Click a card to make it one; click it again to take it off.");
    if (status_text) set_text(status_text, line);
    if (paginator) array_call(paginator, "SetEquippedCards", want_default);
}
static void modal_open(void) {
    if (modal) return;
    cards_load();
    UClass *c_pag = ue_find_any_class("CardPaginator_WBP_C"), *c_image = ue_find_class("Image");
    if (!c_pag) { LOG("runui: CardPaginator_WBP_C is not loaded (open the deck editor once): no picker"); picking = 0; return; }
    UObject *wt = *(UObject **)((char *)panel + o_wt), *root = wt ? *(UObject **)((char *)wt + o_root) : NULL;
    if (!root || !c_image) { picking = 0; return; }
    // two full-screen canvas children on top of everything: the backdrop (the deck editor's own BackgroundBlur_WBP_C,
    // blur + darkening; else a tinted Image) and the content box; the backdrop also takes the clicks meant for what is
    // under it
    UClass *c_blur = ue_find_any_class("BackgroundBlur_WBP_C");
    UObject *back = c_blur ? create_widget(panel, c_blur) : NULL;
    if (!back) back = new_widget(panel, c_image);
    UObject *parts[2] = { back, new_widget(panel, c_vbox) };
    for (int k = 0; k < 2; k++) {
        if (!parts[k]) { picking = 0; return; }
        uint8_t q[32] = {0};
        int32_t pc = poff(root, "AddChildToCanvas", "Content"), pr = poff(root, "AddChildToCanvas", "ReturnValue");
        *(UObject **)(q + pc) = parts[k];
        call_parms(root, "AddChildToCanvas", q, sizeof q);
        UObject *slot = *(UObject **)(q + pr);
        if (!slot) { picking = 0; return; }
        FField *a = first_parm(slot, "SetAnchors");
        if (a && FP_ELSIZE(a) == 16) { uint8_t pp[32] = {0}; float an[4] = {0, 0, 1, 1}; memcpy(pp + FP_OFFSET(a), an, 16); call_parms(slot, "SetAnchors", pp, sizeof pp); }
        a = first_parm(slot, "SetOffsets");
        if (a && FP_ELSIZE(a) == 16) { uint8_t pp[32] = {0}; float m[4] = {k ? 160.f : 0, k ? 120.f : 0, k ? 160.f : 0, k ? 60.f : 0}; memcpy(pp + FP_OFFSET(a), m, 16); call_parms(slot, "SetOffsets", pp, sizeof pp); }
        uint8_t z[16] = {0}; int32_t zo = poff(slot, "SetZOrder", "InZOrder");
        if (zo >= 0) { *(int32_t *)(z + zo) = 100 + k; call_parms(slot, "SetZOrder", z, sizeof z); }
    }
    modal_back = parts[0]; modal = parts[1];
    if (!c_blur || U_CLASS(modal_back) == c_image) {
        FField *a = first_parm(modal_back, "SetColorAndOpacity");
        char pn[64];
        LOG("runui: backdrop is a tinted Image (no BackgroundBlur_WBP_C): SetColorAndOpacity param %s size %d at %d",
            a ? ue_name(FF_NAME(a), pn, sizeof pn) : "-", a ? FP_ELSIZE(a) : -1, a ? FP_OFFSET(a) : -1);
        if (a && FP_ELSIZE(a) == 16) { uint8_t pp[32] = {0}; float col[4] = {0.02f, 0.02f, 0.03f, 0.94f}; memcpy(pp + FP_OFFSET(a), col, 16); call_parms(modal_back, "SetColorAndOpacity", pp, sizeof pp); }
    }
    UObject *vbox = modal;
    // content: heading row (heading, search, Done), the status line, the grid
    UObject *top = new_widget(panel, c_hbox);
    UObject *tsl = add_to(vbox, top);
    if (tsl) slot_pad(tsl, 0, 0, 0, 16);
    UObject *head = new_widget(panel, c_textblock);
    if (head) {
        set_text(head, "DEFAULT CARDS");
        UObject *src = find_class_in(panel, "TextBlock", 0);
        if (src) { copy_via(src, "Font", head, "SetFont"); copy_via(src, "ColorAndOpacity", head, "SetColorAndOpacity"); }
        UObject *hs = add_to(top, head);
        if (hs) slot_pad(hs, 0, 20, 60, 0);
    }
    filter_box = new_widget(panel, c_editbox);
    if (filter_box) {
        FField *h = first_parm(filter_box, "SetHintText");
        uint8_t p[64] = {0};
        if (h && make_text("search cards", p + FP_OFFSET(h))) call_parms(filter_box, "SetHintText", p, sizeof p);
        filter[0] = 0;
        UObject *fs = add_to(top, filter_box);
        if (fs) { slot_pad(fs, 0, 16, 60, 0); FField *sz = first_parm(fs, "SetSize"); if (sz && FP_ELSIZE(sz) == 8) { uint8_t pp[32] = {0}; float v[2] = {1.f, 1}; memcpy(pp + FP_OFFSET(sz), v, 8); call_parms(fs, "SetSize", pp, sizeof pp); } }
    }
    UObject *done = create_widget(panel, c_text_button);
    if (done) {
        set_text(done, "Done");
        add_to(top, done);
        modal_done = done;
        if (nours < MAX_OURS) { ours[nours].w = done; ours[nours].kind = 4; ours[nours].arg = 0; nours++; }
    }
    status_text = new_widget(panel, c_textblock);
    if (status_text) { UObject *ss = add_to(vbox, status_text); if (ss) slot_pad(ss, 0, 0, 0, 16); }
    paginator = create_widget(panel, c_pag);
    if (paginator) {
        UObject *ps = add_to(vbox, paginator);
        if (ps) { FField *sz = first_parm(ps, "SetSize"); if (sz && FP_ELSIZE(sz) == 8) { uint8_t pp[32] = {0}; float v[2] = {1.f, 1}; memcpy(pp + FP_OFFSET(sz), v, 8); call_parms(ps, "SetSize", pp, sizeof pp); } }
        array_call(paginator, "SetEquippedCards", want_default);
        int n = array_call(paginator, "SetCards", want_filtered);
        LOG("runui: picker open: %d card(s) in the deck builder's grid", n);
    } else LOG("runui: could not create CardPaginator_WBP_C");
    modal_status();
}
// the equipped mark and the count on a card only follow a new layout: cards set again, the page kept
static void modal_regrid(void) {
    if (!paginator) return;
    uint8_t p[32] = {0};
    int32_t orv = poff(paginator, "GetCurrentPage", "ReturnValue");
    int page = orv >= 0 && !call_parms(paginator, "GetCurrentPage", p, sizeof p) ? *(int32_t *)(p + orv) : 0;
    array_call(paginator, "SetEquippedCards", want_default);
    array_call(paginator, "SetCards", want_filtered);
    for (int i = 0; i < page && i < 64; i++) { uint8_t q[16] = {0}; call_parms(paginator, "NextPage", q, sizeof q); }
    modal_status();
}
// the grid's visible cards: the hovered one under a click toggles its row
static int modal_click(void) {
    if (!paginator) return 0;
    uint8_t p[32] = {0};
    int32_t orv = poff(paginator, "GetNumVisibleCards", "ReturnValue");
    if (orv < 0 || call_parms(paginator, "GetNumVisibleCards", p, sizeof p)) return 0;
    int n = *(int32_t *)(p + orv);
    int32_t ii = poff(paginator, "GetVisibleCard", "Index"), ir = poff(paginator, "GetVisibleCard", "ReturnValue");
    if (ii < 0) ii = poff(paginator, "GetVisibleCard", "InIndex");
    if (ii < 0) { FField *f0 = first_parm(paginator, "GetVisibleCard"); if (f0) ii = FP_OFFSET(f0); }
    if (ii < 0 || ir < 0) { static int said; if (!said++) LOG("runui: GetVisibleCard parameters not found"); return 0; }
    for (int i = 0; i < n && i < 32; i++) {
        uint8_t q[32] = {0};
        *(int32_t *)(q + ii) = i;
        if (call_parms(paginator, "GetVisibleCard", q, sizeof q)) return 0;
        UObject *card = *(UObject **)(q + ir);
        if (!card || !hovered(card)) continue;
        uint8_t r[64] = {0};
        int32_t hr = poff(card, "GetCardRowHandle", "ReturnValue");
        if (hr < 0 || call_parms(card, "GetCardRowHandle", r, sizeof r)) return 0;
        char row[64];
        ue_name(*(FName *)(r + hr + 8), row, sizeof row);
        n_clicks++;
        ov_run("rules baseline %s %s", rules_baseline_has(row) ? "remove" : "add", row);
        modal_regrid();
        return 1;
    }
    return 0;
}

// ---- building ----
static UObject *button(UObject *parent, const char *label, int kind, int arg) {
    UObject *b = create_widget(panel, c_text_button);
    if (!b) return NULL;
    set_text(b, label);
    UObject *sl = add_to(parent, b);
    if (sl) slot_pad(sl, 0, 8, 0, 0);
    if (nours < MAX_OURS) { ours[nours].w = b; ours[nours].kind = kind; ours[nours].arg = arg; nours++; }
    return b;
}
static void clear_box(void) {
    uint8_t p[16] = {0};
    if (box) call_parms(box, "ClearChildren", p, sizeof p);
    nours = 0;
    if (modal && modal_done) { ours[nours].w = modal_done; ours[nours].kind = 4; ours[nours].arg = 0; nours++; }   // the picker's Done stays clickable
}
static int state_key(void) { return ((picking ? 0 : rules_baseline_count()) << 8) | (picking << 1) | (rules_free_burncards() ? 1 : 0); }
static void build(void) {
    clear_box();
    char label[128];
    // heading, in the game's heading font
    UObject *head = new_widget(panel, c_textblock);
    if (head) {
        set_text(head, picking ? "DEFAULT CARDS: PICK ONE" : "DEFAULT CARDS (B4BCOOP)");
        UObject *src = find_class_in(panel, "TextBlock", 0);   // DifficultySelect's "DIFFICULTY" heading: the first TextBlock in the panel
        if (src) { copy_via(src, "Font", head, "SetFont"); copy_via(src, "ColorAndOpacity", head, "SetColorAndOpacity"); }
        UObject *sl = add_to(box, head);
        if (sl) slot_pad(sl, 0, 0, 0, 4);
    }
    if (!picking) {
        char rows[16][64]; int n = rules_baseline_rows(rows, 16);
        for (int i = 0; i < n; i++) { snprintf(label, sizeof label, "x   %s", shown_of(rows[i])); button(box, label, 1, i); }
        if (!n) { UObject *t = new_widget(panel, c_textblock); if (t) { set_text(t, "None yet: everyone plays with their own deck only."); add_to(box, t); } }
        button(box, "+   Default Cards", 2, 0);
        snprintf(label, sizeof label, "Burn cards are free:  %s", rules_free_burncards() ? "ON" : "OFF");
        button(box, label, 5, 0);
    } else modal_open();
    built_for = state_key();
}

static int attach(UObject *p) {   // our box into the panel's root canvas, under SettingsInfo
    UObject *wt = *(UObject **)((char *)p + o_wt), *root = wt ? *(UObject **)((char *)wt + o_root) : NULL;
    if (!root || !ue_find_function(U_CLASS(root), "AddChildToCanvas")) { LOG("runui: the settings panel has no root canvas"); return 0; }
    box = new_widget(p, c_vbox);
    if (!box) { LOG("runui: can't create a VerticalBox"); return 0; }
    uint8_t q[32] = {0};
    int32_t pc = poff(root, "AddChildToCanvas", "Content"), pr = poff(root, "AddChildToCanvas", "ReturnValue");
    if (pc < 0 || pr < 0) return 0;
    *(UObject **)(q + pc) = box;
    call_parms(root, "AddChildToCanvas", q, sizeof q);
    UObject *slot = *(UObject **)(q + pr);
    if (!slot) { LOG("runui: AddChildToCanvas failed"); box = NULL; return 0; }
    // like HorizontalBox_28: anchored to the top centre; the left column and SettingsInfo use x 155..2361 of 3840, the
    // right third (x 2440 on, from y 470, the difficulty text runs lower than SettingsInfo's box) is free
    FField *a = first_parm(slot, "SetAnchors");
    if (a && FP_ELSIZE(a) == 16) { uint8_t pp[32] = {0}; float an[4] = {0.5f, 0, 0.5f, 0}; memcpy(pp + FP_OFFSET(a), an, 16); call_parms(slot, "SetAnchors", pp, sizeof pp); }
    a = first_parm(slot, "SetAlignment");
    if (a && FP_ELSIZE(a) == 8) { uint8_t pp[32] = {0}; float al[2] = {0, 0}; memcpy(pp + FP_OFFSET(a), al, 8); call_parms(slot, "SetAlignment", pp, sizeof pp); }
    a = first_parm(slot, "SetPosition");
    if (a && FP_ELSIZE(a) == 8) { uint8_t pp[32] = {0}; float ps[2] = {2440 - 1920, 470}; memcpy(pp + FP_OFFSET(a), ps, 8); call_parms(slot, "SetPosition", pp, sizeof pp); }
    a = first_parm(slot, "SetSize");
    if (a && FP_ELSIZE(a) == 8) { uint8_t pp[32] = {0}; float sz[2] = {1240, 1370}; memcpy(pp + FP_OFFSET(a), sz, 8); call_parms(slot, "SetSize", pp, sizeof pp); }
    uint8_t z[16] = {0}; int32_t zo = poff(slot, "SetZOrder", "InZOrder");
    if (zo >= 0) { *(int32_t *)(z + zo) = 50; call_parms(slot, "SetZOrder", z, sizeof z); }
    return 1;
}

// the settings panel on screen: the class's live instance under the transient game instance, not the Blueprint's
// template widgets (WidgetTree / WidgetArchetype in the /Game/UI/Screens package), which ue_find_first_of returns first
static UObject *live_panel(void) {
    UClass *c = ue_find_any_class("MatchmakingSettingsPanel_WBP_C");   // a Blueprint class: not in ue_find_class; never cached (GC)
    char path[512];
    for (int32_t i = 0, n = c ? ue_num_objects() : 0; i < n; i++) {
        UObject *o = ue_object_at(i);
        if (!o || (U_FLAGS(o) & 0x30) || !ue_is_a(o, c) || !ue_object_usable(o, i)) continue;
        if (!strncmp(ue_full_path(o, path, sizeof path), "/Engine/Transient.", 18)) return o;
    }
    return NULL;
}

static int resolve(void);
static void forget(void) { panel = NULL; box = NULL; filter_box = NULL; nours = 0; picking = 0; c_text_button = NULL; modal = modal_back = paginator = status_text = modal_done = NULL; }

static void act(int kind, int arg) {
    char rows[16][64];
    n_clicks++;
    switch (kind) {
    case 1: if (rules_baseline_rows(rows, 16) > arg) ov_run("rules baseline remove %s", rows[arg]); break;
    case 2: picking = 1; filter[0] = 0; break;
    case 3: if (arg < ncards) ov_run("rules baseline add %s", cards[arg].row); break;
    case 4: picking = 0; break;
    case 5: ov_run("rules free_burncards %s", rules_free_burncards() ? "off" : "on"); break;
    }
    built_for = -1;   // rebuild next tick (the rule took effect through the chat path)
    if (modal && picking) modal_status();
}

void runui_tick(float dt) {
    if (!enabled) return;
    // the panel: looked for twice a second; gone (screen closed, GC) = forget everything
    if (panel && !ue_object_usable(panel, panel_idx)) forget();
    since_check += dt;
    if (!panel && since_check >= 0.5) {
        since_check = 0;
        if (!resolve()) return;
        UObject *p = live_panel();
        if (!p || admin_is_client()) return;
        if (!c_text_button) c_text_button = ue_find_any_class("TextButton_WBP_C");
        if (!c_text_button) { static int said; if (!said++) LOG("runui: TextButton_WBP_C not loaded yet"); return; }
        panel = p; panel_idx = U_INDEX(p); picking = 0;
        if (!attach(p)) { forget(); return; }
        build();
        n_panels_built++;
        LOG("runui: run rules panel built on the create-run screen (%d button(s))", nours);
    }
    if (!panel || !box) return;
    // the picker's search box: a change re-fills the grid
    if (filter_box && paginator) {
        char now[64]; text_of(filter_box, now, sizeof now);
        if (strcmp(now, filter)) { snprintf(filter, sizeof filter, "%s", now); array_call(paginator, "SetEquippedCards", want_default); array_call(paginator, "SetCards", want_filtered); modal_status(); }
    }
    if (built_for != state_key()) {
        if (!picking && modal) modal_close();
        build();
    }
    // clicks: left button down edge over one of our buttons (the game's UI has the input, we only look)
    int lmb = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    int edge = lmb && !lmb_was;
    lmb_was = lmb;
    if (!edge || overlay_is_open() || !cmds_game_focused()) return;
    if (modal && modal_click()) return;
    for (int i = 0; i < nours; i++)
        if (ours[i].w && hovered(ours[i].w)) { act(ours[i].kind, ours[i].arg); return; }
}

static void ini_pair(const char *k, const char *v, void *ctx) { (void)ctx; if (!strcmp(k, "runui")) enabled = atoi(v) != 0; }
int runui_live(const char *key, const char *val) {
    if (strcmp(key, "runui")) return 0;
    enabled = !val || atoi(val) != 0;
    if (!enabled) forget();
    return 1;
}

// UMG reflection, looked up when first needed (as hud.c does: at init some of it is not there yet)
static int resolve(void) {
    if (ready) return 1;
    c_vbox = ue_find_class("VerticalBox"); c_hbox = ue_find_class("HorizontalBox");
    c_textblock = ue_find_class("TextBlock"); c_editbox = ue_find_class("EditableTextBox");
    c_panelw = ue_find_class("PanelWidget");
    UClass *uw = ue_find_class("UserWidget"), *wt = ue_find_class("WidgetTree"), *ps = ue_find_class("PanelSlot");
    FField *a = c_panelw ? ue_find_prop(c_panelw, "Slots") : NULL, *b = ps ? ue_find_prop(ps, "Content") : NULL,
           *c = uw ? ue_find_prop(uw, "WidgetTree") : NULL, *d = wt ? ue_find_prop(wt, "RootWidget") : NULL;
    if (!c_vbox || !c_textblock || !c_editbox || !c_panelw || !a || !b || !c || !d) {
        static int said;
        if (!said++) LOG("runui: UMG reflection lookup failed (VerticalBox %p TextBlock %p EditableTextBox %p PanelWidget %p Slots %p Content %p WidgetTree %p RootWidget %p)",
                         (void *)c_vbox, (void *)c_textblock, (void *)c_editbox, (void *)c_panelw, (void *)a, (void *)b, (void *)c, (void *)d);
        return 0;
    }
    o_slots = FP_OFFSET(a); o_content = FP_OFFSET(b); o_wt = FP_OFFSET(c); o_root = FP_OFFSET(d);
    ready = 1;
    LOG("runui: UMG reflection ready");
    return 1;
}

int runui_init(void) {
    cmds_ini_each(ini_pair, NULL);
    LOG("runui: run rules on the create-run screen %s", enabled ? "on" : "off (runui=0)");
    return 0;
}

#ifndef B4B_RELEASE
int runui_cmd(const char *verb, char *rest, Out *o) {
    if (strcmp(verb, "runui")) return 0;
    if (rest && !strcmp(rest, "rebuild")) { built_for = -1; out_printf(o, "rebuilding\n"); return 1; }
    if (rest && !strncmp(rest, "click ", 6)) { int i = atoi(rest + 6); if (i >= 0 && i < nours) { act(ours[i].kind, ours[i].arg); out_printf(o, "clicked %d\n", i); } return 1; }
    char b[128];
    out_printf(o, "runui: %s, panel %s, box %p, %d button(s), picking %d modal %p paginator %p filter '%s', %d card(s), built %d, clicks %d\n",
               enabled ? "on" : "off", panel ? ue_obj_name(panel, b, sizeof b) : "-", (void *)box, nours, picking, (void *)modal, (void *)paginator, filter, ncards, n_panels_built, n_clicks);
    for (int i = 0; i < nours; i++) out_printf(o, "  %d: kind %d arg %d %s hovered=%d\n", i, ours[i].kind, ours[i].arg, ue_obj_name(ours[i].w, b, sizeof b), hovered(ours[i].w));
    return 1;
}
#endif
