// rules: the host's run rules for a private session, the `~` window's Run tab and b4bcoop.ini keys. Host-side only:
// the listen host runs the game's card and reward code for every player, so a rule set here applies to the whole
// lobby, and nothing is sent to the clients (no protocol bump). Clients see the tab greyed out with the reason.
//   free_burncards=1   burn cards are never charged: the charge the game issues on leaving the start saferoom is
//                      dropped for every player, host and clients alike (burncards.c asks rules_free_burncards()
//                      when the game looks up the profile to charge). Playing still needs owning at least one of the
//                      card (the client's own UI lists what its profile holds); it just isn't used up.
// Chat: /rules (state), /rules free_burncards on|off (CMD_HOST), the same as the tab's controls.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include "ue.h"
#include "log.h"
#include "cmds.h"
#include "overlay.h"

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

static void ini_pair(const char *k, const char *v, void *ctx) {
    (void)ctx;
    if (!strcmp(k, "free_burncards")) free_burncards = atoi(v) != 0;
}

int rules_live(const char *key, const char *val) {
    if (!strcmp(key, "free_burncards")) {
        int was = free_burncards;
        free_burncards = val && atoi(val) != 0;
        if (was != free_burncards) LOG("rules: free burn cards %s", free_burncards ? "on" : "off");
        return 1;
    }
    return 0;
}

// chat /rules [free_burncards on|off]  (CMD_HOST, checked by admin_slash)
void rules_slash(char *rest, Out *o) {
    char *k = rest ? strtok(rest, " ") : NULL, *v = k ? strtok(NULL, " ") : NULL;
    if (k && !strcmp(k, "free_burncards") && v) {
        int on = !strcmp(v, "on") || !strcmp(v, "1");
        if (!on && strcmp(v, "off") && strcmp(v, "0")) { out_printf(o, "usage: /rules free_burncards on|off\n"); return; }
        cmds_ini_apply("free_burncards", on ? "1" : "0", 1);
        if (!on) free_noticed_at = 0;
        out_printf(o, "free burn cards %s (saved in b4bcoop.ini)\n", on ? "on" : "off");
        admin_notice(on ? "host rule: burn cards are free this session (never used up)" : "host rule: burn cards are charged again");
        return;
    }
    if (k) { out_printf(o, "usage: /rules [free_burncards on|off]\n"); return; }
    out_printf(o, "run rules (host): free burn cards %s\n", free_burncards ? "on" : "off");
}

static void panel(void) {
    ov_begin_perm(CMD_HOST);
    ov_heading("Burn cards");
    int fb = free_burncards;
    if (ov_checkbox("Burn cards are free##freeburn", &fb)) ov_run("rules free_burncards %s", fb ? "on" : "off");
    ov_tooltip("Host only. Burn cards played in the start saferoom are never used up: the charge the game makes when "
               "the team leaves the saferoom is dropped, for you and for everyone in your game. Players still need "
               "to own at least one of a card to play it. Saved in b4bcoop.ini (free_burncards).");
    ov_text_dim("Rules apply to everyone in your game; nothing is sent to the others' saves. A joined player sees "
                "the host's rules in the chat notices.");
    ov_end_perm();
}

int rules_init(void) {
    cmds_ini_each(ini_pair, NULL);
    overlay_add_panel("Run", 55, panel);
    LOG("rules: free burn cards %s", free_burncards ? "on (free_burncards=1)" : "off");
    return 0;
}
