#pragma once
#include <stddef.h>
#include "ue.h"

typedef struct { char buf[256 * 1024]; size_t len; } Out;
void out_reset(Out *o);
void out_printf(Out *o, const char *fmt, ...);

// Dev builds only (not B4B_RELEASE): cmds_run and every *_cmd CLI handler below that is not also a chat command.
void cmds_init(void);
void cmds_run(char *line, Out *out);   // game thread (dev: TCP command server in main.c)
void cmds_tick(float dt);              // game thread, every engine tick

void game_exec(const char *cmd);        // game thread: run a console command in the current world
int travel_init(void);
int uelog_init(void);
void travel_set_host(const char *addr);
void travel_tick(float dt);
void travel_on_handshake_failed(void);
int travel_following(void);            // client: in travel.c's rejoin window after following a server travel
void travel_end_follow(const char *why);  // client: the host refused the follow with a reason: no more rejoins
void cmds_join_failed(const char *why); // uelog.c: our pending join failed (retry soon, from our camp)
void cmds_join_answered(void);         // uelog.c: our pending join's DTLS handshake completed
int cmds_join_take_closed(void);       // travel.c: 1 = drop this ?closed travel (failed join, stay in our camp)
int cards_init(void);
int rewards_init(void);
int burncards_init(void);
int cmds_auto_host(void);
const char *cmds_config_path(void);    // b4bcoop.ini, or B4B_COOP_CONFIG
int cmds_parse_key(const char *v);     // ini hotkey value -> VK code (0 = no key)
// b4bcoop.ini: every active name=value pair in file order (-1: no file); the writer (update/insert/comment out one
// key, keeps comments and line endings; val NULL = comment it out; 0 = ok); the live reload poll (game thread).
int cmds_ini_each(void (*fn)(const char *key, const char *val, void *ctx), void *ctx);
int cmds_ini_set(const char *key, const char *val);
void cmds_ini_poll(float dt);
// A setting from the overlay: the module's live handler below (as if b4bcoop.ini changed), and with save the writer
// (only this key; val NULL = the default, the line is commented out). 0 = ok.
int cmds_ini_apply(const char *key, const char *val, int save);
const char *cmds_ini_value(const char *key);   // the key's value in b4bcoop.ini as last read or written, NULL = not set
// Live reload handlers (cmds_ini_poll): a key that changed while the game runs, val NULL = removed (back to the
// default). 1 = the key is theirs and was applied.
int thirdperson_live(const char *key, const char *val);
int flashlight_live(const char *key, const char *val);
int joinpolicy_live(const char *key, const char *val);
int presence_live(const char *key, const char *val);
int teamsize_live(const char *key, const char *val);
int overlay_live(const char *key, const char *val);
int addons_live(const char *key, const char *val);
// online.c (#47): the launch choice co-op / online (launcher: native/launcher/redirect.c). online_early from DllMain
// (1 = the game is being closed: -b4bcoop=off/online but we were loaded), online_init from the init thread, ini
// launch=ask|coop|online, its section in the ~ window's Settings tab.
int online_early(void *module);
int online_init(void);
int online_live(const char *key, const char *val);
void online_settings_panel(void);
// overlay.cpp: the `~` power-user window (#26; Dear ImGui over the game's D3D12 swap chain). Panels: overlay.h
int overlay_init(void);
void overlay_tick(float dt);
int overlay_is_open(void);
int overlay_cmd(const char *verb, char *rest, Out *o);   // dev builds: `overlay open|close|status|tab|press|set|log`
void overlay_note(const char *text);                     // a line in the window's log (any thread)
int flashlight_hotkey(void);                             // flashlight_key (VK, 0 = none)
int thirdperson_hotkey(void);                            // thirdperson_key
int cmds_game_focused(void);           // the game window is in front (hotkeys)
int cmds_hotkey_down(int vk);          // game thread: the key is held, through the game's input (not while typing in chat,
                                       // not while the overlay is open)
int flashlight_init(void);
void flashlight_tick(float dt);
void cmd_flashlight(const char *arg, Out *o);
// thirdperson.c: /thirdperson, the local player's own over-the-shoulder camera (#25; everyone, no cheats)
int thirdperson_init(void);                              // ini thirdperson=1 (start on), thirdperson_key=N
void thirdperson_tick(float dt);
void cmd_thirdperson(const char *arg, Out *o);          // on|off|status, NULL = toggle
int thirdperson_cmd(const char *verb, char *rest, Out *o);   // dev builds: `thirdperson [on|off|view [1|2|3]]`
int testing_cmd(const char *verb, char *rest, Out *o); // testing.c (dev builds): test commands; 1 if handled
int teamsize_init(void);
int teamsize_cmd(const char *verb, char *rest, Out *o);  // game thread; 1 if handled (also chat /teamsize)
int teamsize_get(void);                                  // teamsize= (0 = the game's)
int dialogue_init(void);                                 // dialogue.c: one cutscene VO line per cue on clients
int bots_init(void);                                     // bots.c: pinged toolkit doors opened by bots (host)
int bots_live(const char *key, const char *val);          // bot_toolkit_doors=0|1
int bots_toolkit_doors(void);                            // the setting; -1 = hooks off (signature mismatch)
int bots_cmd(const char *verb, char *rest, Out *o);      // dev builds: `botdoor [doors|ping|target|tp]`
void bots_tick(float dt);                                 // bot_heal_key (any machine), heal request jobs (host)
int bots_heal_requests(void);                            // bot_heal_requests; -1 = hooks off
int bots_heal_hotkey(void);                              // bot_heal_key (VK, 0 = none)
int bots_heal_cmd(const char *verb, char *rest, Out *o); // dev builds: `botheal [hurt|ask|keys|watch|fix]`
int fxsig_init(void);                                    // fxsig.c: inactive particles leave the significance manager (#46)
int fxsig_cmd(const char *verb, char *rest, Out *o);     // dev builds: `fxsig [fix | guard on|off]`
int supplylines_init(void);                              // supplylines.c: a client's supply lines refresh (#48)
void supplylines_tick(float dt);
int supplylines_cmd(const char *verb, char *rest, Out *o); // dev builds: `supplylines [fix|off|update|auto|buy|sp]`
int lineup_init(void);                                   // lineup.c: 5th+ hero in the character lineups (#8)
int lineup_cmd(const char *verb, char *rest, Out *o);    // dev builds: `lineup [off dx dy | fov deg | apply]`
int hud_init(void);                                      // hud.c: teammate list with 5+ heroes
int hud_cmd(const char *verb, char *rest, Out *o);       // dev builds: `hudtree`, `hudxf`, `hud`
void hud_tick(float dt);
void teamsize_tick(float dt);
int slotguard_init(void);
int slotguard_cmd(const char *verb, char *rest, Out *o);  // game thread; 1 if handled
void slotguard_tick(float dt);
void cmds_auto_join_backoff(double seconds);  // client: the host rejected us as full
// Join / host entry points (chat /join, Steam invites, CLI join). Any thread: off the game thread the request is
// queued for the next tick. target: "steam:<steamid64>" (Steam P2P, steamnet.c) or "ip[:port]" (default 7777; only
// 127.0.0.1 unless host_ip=1).
void coop_join(const char *target);
void coop_host(void);                  // host the current offline camp (UDP + Steam P2P)
void coop_leave(void);                 // client: disconnect, back to own camp, no auto-rejoin
// Version (VERSION -> b4bcoop_version.h) and protocol: host and clients must run the same protocol (admin.c login
// gate, presence.c connect string). Dev builds: b4bcoop_protocol_override=N fakes another protocol.
const char *coop_version(void);
int coop_protocol(void);
void coop_version_mismatch(char *buf, size_t n, const char *host_ver, const char *host_proto);  // joiner's message
// host_ip=1 (advanced): IP hosting/joining. Default 0: game UDP bound to 127.0.0.1, IP joins only to this machine.
int coop_host_ip(void);
int coop_is_loopback(const char *hostport);   // 127.x.y.z / localhost
const char *coop_ip_join_off_msg(void);       // why an IP join was refused
int slotguard_kick(UObject *pc);       // host: close a remote player's connection
int chat_init(void);
void chat_tick(float dt);
int chat_cmd(const char *verb, char *rest, Out *o);
void chat_local(const char *fmt, ...);  // local-only chat line(s)
#define CHAT_NOTICE_TYPE L"b4bcoop"      // ClientTeamMessage Type of host notices (shown by chat.c on the receiver)
#define CHAT_KICK_TYPE L"b4bcoopkick"    // ... a notice after which the receiving client leaves (kick/ban)
#define CHAT_DATA_TYPE L"b4bcoopdata"    // ... not shown: data for the client's agent (e.g. weapon looks on the floor);
                                         //     agents without a handler and clients without b4bcoop ignore it
FName chat_notice_type(int kind);        // 0 notice, 1 kick, 2 data
void chat_local_later(const char *text); // show after the next map load (e.g. why a join was refused)
void chat_local_soon(const char *text);  // show once no game popup covers the screen (no map change needed)
void chat_refusal_popup(const char *msg); // put a refused join's reason on the game's popup about it
void chat_on_join_failed(const char *error);  // uelog.c: PendingConnectionFailure on this client
void cmds_auto_join_stop(void);        // client: no more ini auto-join attempts this session
const char *cmds_last_join_target(void); // the target of the last join attempt, as given ("steam:<id64>", "ip[:port]")
void cmds_join_retry(const char *target, double delay);  // join that target again once back in our camp
int admin_init(void);
void admin_tick(float dt);
int admin_cmd(const char *verb, char *rest, Out *o);
void admin_slash(char *line, Out *o);  // a chat command typed by the local player (without the '/')
void admin_on_initslots(UObject *psm); // teamsize.c: right before APlayerSlotManager::InitSlots
void admin_ready(const char *rest, Out *o); // host: ready every player (chat /ready, dev `ready [vote]`)
int admin_notice_to(UObject *pc, const char *text);  // host: one chat notice line to that player's client (0 = sent)
int admin_data_to(UObject *pc, const char *text);    // host: one data line (CHAT_DATA_TYPE, not shown) to that client
void admin_ps_key(UObject *ps, char *buf, size_t n);   // steam:<id64> or name:<name>
void admin_ps_name(UObject *ps, char *buf, size_t n);  // player name, a bot's hero name
void cmds_set_session_join(const char *targets); // Steam join target(s), comma-separated; overrides host=/join=
const char *cmds_session_join(void);
void cmds_join_now(void);                      // attempt the session target now (leaves the current session)
// signin.c: auto sign-in Offline (Steam join; dev builds also offline=1)
void signin_arm(void);                         // arm it for a Steam join (next 10 minutes)
int signin_pending(void);                      // auto sign-in armed and not finished
int signin_on_title(void);                     // sign-in screen up (not signed in yet)
int signin_step(Out *o);                       // one step (dev `signin` command); 1 if it acted
int signin_step_as(Out *o, const wchar_t *answer);   // same, answering the popup with answer (dev `signin online`)
int online_hook_active(void);                  // online.c: the sign-in popup guard is hooked (Online -> Offline)
void signin_tick(float dt);
void presence_init(void);                      // presence.c: Steam rich presence, Join Game, invites
void presence_tick(float dt);
int presence_cmd(const char *verb, char *rest, Out *o);  // game thread; 1 if handled
int presence_has_friend(uint64_t id);          // 1 Steam friend, 0 not, -1 Steam not bound (any thread)
void presence_note_host_addons(const char *target); // joiner: a steam: host's add-on policy from its rich presence
const char *presence_persona(uint64_t id);     // persona name from the friends cache, "?" if unknown

// steamnet.c: Steam P2P transport (docs/investigations/steam-p2p.md)
#include <stdint.h>
uint64_t steamnet_local_id(void);                          // 0 if Steam is not available
const char *steamnet_last_error(void);                     // why Steam P2P is unavailable (last check)
int steamnet_p2p_on(void);                                 // Steam P2P usable (enabled, Steam up, hooks in)
// "steam:<id64>" -> url "<fake ip>:7777" (1); "host[:port]" -> "host:port" (0); -1 malformed; -2 no Steam P2P here
int steamnet_resolve_target(const char *target, char *url, size_t n);
void steamnet_init(void);                                  // init_thread: ws2_32 hooks, Steam callbacks
void presence_add_callback(void *cb, int id);              // presence.c: register a Steam callback (steamnet.c)
void steamnet_on_log(const char *cat, const char *msg);    // uelog.c: every engine log line
void steamnet_status(Out *o);
int steamnet_cmd(const char *verb, char *rest, Out *o);    // `steamnet [on|off]`; 1 if handled
void steamnet_config(const char *key, const char *value);  // b4bcoop.ini keys steam_p2p=, transport=
uint64_t steamnet_peer_of_addr(const char *addr);          // host: SteamID behind a fake P2P address, 0 if none
void steamnet_tick(float dt);                              // game thread

// joinpolicy.c: who may join (host). Default Steam friends + own account; ini allow_joins=, allow_steamids=
int joinpolicy_config(const char *key, const char *value);  // 1 if the key was ours
void joinpolicy_init(void);
int joinpolicy_check(uint64_t steamid, char *why, size_t n); // 1 allowed; why = deciding rule / refusal reason
const char *joinpolicy_error(void);                          // login error text for a refused joiner
void joinpolicy_notify_refused(uint64_t steamid, const char *why);  // any thread: host chat notice (next tick)
void joinpolicy_tick(float dt);
int joinpolicy_cmd(const char *verb, char *rest, Out *o);   // dev builds
void joinpolicy_get(int *anyone, char *ids, size_t n);       // overlay: allow_joins=anyone, allow_steamids as "id,id"

// rewardguard.c: client-side validation of host-sent profile commands (ClientExecute*Command RPCs)
int rewardguard_init(void);
void rewardguard_tick(float dt);
int rewardguard_cmd(const char *verb, char *rest, Out *o);  // dev builds: `rewardguard`, host `rewardtest ...`

// paks.c: engine pak layer (docs/investigations/model-mods-paks.md). Both builds: mounts the add-ons (addons.c).
void paks_early_init(void);                                // DllMain: hook FPakPlatformFile::Initialize (if needed)
int paks_mount_unsigned(const wchar_t *path, uint32_t order); // addons_mount only: mount one of our unsigned paks
int paks_cmd(const char *verb, char *rest, Out *o);        // dev builds: `paks`, `mountpak`, `dumpassets`; 1 if handled
int paks_runtime_ready(void);                              // the pak layer is known (runtime mounts possible)
int paks_file_exists(const char *key);                     // "gobi/content/..." exists in the engine's file system
int paks_mount_runtime(const wchar_t *path, uint32_t order, char *err, size_t en);  // game thread: mount now
int paks_async_loading(void);                              // engine IsAsyncLoading(): 1 busy, 0 idle, -1 unknown
int paks_package_loaded(const char *const *names, int n);  // index of the first package ("/game/x/y") in memory, -1 none
// addons.c: L4D-style add-ons, <game>\b4bcoop-addons\*.pak + addonlist.txt (docs/investigations/addons.md)
#define MAX_ADDONS 512
int addons_scan(void);                        // DllMain: config, folder, load order, conflicts; number to mount
void addons_unavailable(const char *why);     // paks.c: hooks unavailable, nothing gets mounted
void addons_mount(void);                      // FPakPlatformFile::Initialize hook, right after the retail paks
void addons_init(void);                       // init_thread: queue the player notice (conflicts, bad add-ons)
void addons_slash(const char *verb, char *rest, Out *o);  // chat /addons (any thread that runs chat commands)
typedef struct { const char *title, *file, *hash; int gameplay; unsigned kinds; const char *reason; } AddonRef;
int addons_active(AddonRef *out, int max);    // add-ons mounted in this game, load order
// Added outfits (addoninfo outfit= lines) of the mounted add-ons: object paths "/Game/X/Y.Y", meshfp "" = none
typedef struct { const char *name, *hero, *mesh3p, *meshfp, *title, *addon; } AddonOutfit;
int addons_outfits(AddonOutfit *out, int max);
// Added weapon looks (addoninfo weapon= lines): code = weapon code (AR02); mesh object paths, "" = none
typedef struct { const char *name, *code, *fp, *sm3p, *skm3p, *title, *addon; } AddonWeapon;
int addons_weapons(AddonWeapon *out, int max);
const char *addons_title_of(const char *id8);  // title of an installed add-on (on or off) with that short id, or NULL
// runtime (the add-on shop, shop.c; game thread)
int addons_runtime_wanted(void);              // add-ons on and shop not off: paks.c keeps the pak layer reachable
int addons_enabled(void);
const wchar_t *addons_dir_w(void);            // the add-ons folder (may not exist yet)
const char *addons_dir8(void);
typedef struct { int on, on_at_start, mounted, valid, gameplay; char hash[41], state[40]; } AddonState;
int addons_state(const char *file, AddonState *st);   // 1 = present in the folder (st filled)
int addons_set_on(const char *file, int on);          // addonlist.txt on/off (next start); 1 = saved
// game thread, add-ons loaded while the game runs (~ window: the Add-ons tab): 2 loaded now, 1 at the next start (msg:
// why), 3 the game's loader is busy (ask again in a moment; last=1: never 3), 0 not added
int addons_add_runtime(const char *file, char *msg, size_t mn);   // a new file in the folder: listed, then loaded
int addons_load_now(const char *file, int last, char *msg, size_t mn);   // a listed add-on, switched on, not loaded
int addons_update_runtime(const char *file, const wchar_t *newpath, int last, char *msg, size_t mn);   // newer file
int addons_session_allows(int gameplay, char *msg, size_t mn);   // addons_mp.c: as a client, the host's policy allows it
int addons_session_allows_quiet(int gameplay, char *msg, size_t mn);   // same, no log line (asked every frame)
// another add-on in the folder (not `skip`) with this content id or one of the outfits/weapon looks in a catalog
// entry's adds list; returns its file name, why = what matched
const char *addons_installed_like(const char *skip, const char *content_id, const char *adds, char *why, size_t wn);
void models_outfits_refresh(void);            // models.c: add outfits of add-ons mounted at runtime
void wlooks_refresh(void);                    // weaponlooks.c: same for weapon looks
// ~ window, Add-ons tab (looks.c): the add-ons in the folder (A[] slots; addons_info 1 = present, filled)
typedef struct {
    const char *file, *title, *version, *author, *desc, *why;   // why: it can't load ("" = it can)
    unsigned long long size, time;   // file size; installed = newest of created / written (100 ns since 1601)
    int on, on_at_start, mounted, valid, gameplay, outfits, weapons;
} AddonInfo;
int addons_slots(void);
int addons_info(int i, AddonInfo *o);
void addons_refresh(void);                    // addonlist.txt edited outside the game + file sizes/times (about 1/s)
int addons_look_buttons(const char *file, int after);   // its row: Wear or Take off (outfit) / Use or Stop (weapon look); 1 = drawn
int addons_wearing(const char *file, char *title, size_t n);   // 1 = you wear one of its outfits (title: which, if several)
void addons_tab_footer(void);                 // the folder and the "Load add-ons" switch, one line
int addons_tab_header(void);                  // warnings over the table (off, changes at restart); 1 = add-ons are on
// looks.c: the ~ window's "Add-ons" tab: one table of the shop's add-ons (shop.c) and the installed ones (addons.c),
// Host rules at its bottom (models.c, addons_mp.c). Replaced the Models, Add-ons and Browse tabs.
void looks_init(void);
// shop.c: the add-on shop (#36, docs/investigations/shop.md): its list, downloads, Add / Update / Remove / Undo
void shop_early(const wchar_t *dir);          // DllMain (addons_scan): pending removals/updates from the last session
int shop_forget(const char *file);            // addons_scan: 1 = removed at this start, drop its addonlist.txt line
void shop_init(void);
void shop_tick(float dt);
int shop_live(const char *key, const char *val);
int shop_cmd(const char *verb, char *rest, Out *o);  // dev builds: `shop status|fetch|add <id>|remove <id|file>|undo <id|file>`
int shop_enabled(void);                       // ini shop (0: no list, no downloads)
// the shop's list for the ~ window's Add-ons tab (looks.c; game thread)
enum { SHOP_ADD, SHOP_INSTALLED, SHOP_REMOVING, SHOP_UPDATING, SHOP_NEWER, SHOP_DOWNLOADING, SHOP_INSTALLED_AS };
typedef struct {
    const char *id, *name, *version, *author, *desc, *license;
    const char *like, *like_why;     // SHOP_INSTALLED_AS: that file, and why ("same content", "outfit batman")
    char file[48], too_old[80];      // "<id>.pak"; "needs b4bcoop X or newer (Updates tab)" or ""
    unsigned long long size, got;    // got: bytes so far (SHOP_DOWNLOADING)
    int state, tex, tw, th;          // tex: thumbnail (ov_image; 0 none yet), its size in pixels
    int msg_err; char msg[200];      // the last message about it (download, loaded or why not); msg_err: a failure
} ShopRow;
int shop_rows(void);                          // entries in the list (0: not fetched yet, or shop=0)
int shop_row(int i, ShopRow *r);              // 1 = filled
int shop_index_of(const char *file);          // the entry whose file is `file`, -1 none
const char *shop_name(int i);                 // its name (NULL: no such entry)
int shop_phase(char *msg, size_t n);          // 0 idle, 1 getting the list, 2 ready, 3 failed; msg: its last status line
int shop_busy(void);                          // a list fetch or a download runs
void shop_show_thumbs(void);                  // the tab shows the list: its thumbnails are fetched (once) and shown
#define SHOP_PREVIEW_SIDE 320
int shop_preview(int i, int *w, int *h);      // entry i's large picture for a tooltip (one texture; its w x h top left)
const char *shop_fetch(void);                 // Refresh: get the list again; NULL or why not
const char *shop_add(int i);                  // Add, or Update when installed: NULL (downloading) or why not
const char *shop_remove(const char *file);    // Remove (any add-on): off now, deleted at the next start; NULL or why not
const char *shop_undo(const char *file);      // a Remove / Update waiting for the next start taken back
int shop_pending(const char *file);           // this session's click: 0 none, 1 removed at the next start, 2 updated
int shop_pending_live(const char *file);      // that update is loaded already
// addons_mp.c: add-ons in multiplayer (#22, private #35): the host announces addons_policy, joiners check themselves
int addons_policy_set(const char *v);         // any|cosmetic|none|match; -1 if unknown
const char *addons_policy_name(void);
void addons_presence_token(char *out, size_t n);        // host: " addons:<policy>[:<ids>]" for the rich presence
void addons_note_host(const char *targets, const char *tok);  // joiner: the host's announced policy (tok NULL = older host)
int addons_join_refused(const char *target, char *msg, size_t n);  // joiner: 1 = our add-ons fail the host's policy
const char *addons_login_option(const char *target);    // "?b4bcoopaddonsok=<policy>" (checked) or "" (cmds.c)
int addons_on_refusal(const char *error, const char *target, char *msg, size_t n);  // 0 not ours, 1 join again, 2 stop
int addons_login_check(const char *claim, const char *summary, const char *name, const char *key, char *err, size_t en);  // host: 1 = refuse
int addons_mp_slash(const char *sub, char *arg, Out *o);  // /addons policy (players: gone); 1 if handled
void addons_mp_panel(void);                  // ~ window: the policy part of Host rules (looks.c draws the tab)
// models.c: runtime model swaps (#19, docs/investigations/model-swap.md)
int models_init(void);
void models_tick(float dt);
int models_cmd(const char *verb, char *rest, Out *o);  // dev builds: `model`, `models`, `mdl ...`
void models_slash(const char *verb, char *rest, Out *o); // chat /model, /models
void models_host_notice(const char *text);               // chat.c: a host notice arrived (client)
int models_locked(void);                                 // host: /models off
int models_off(void);                                    // host: /models off; client: as the host's notices said
UObject *models_load_asset(const char *path);            // LoadAsset_Blocking of an object path, NULL if it failed
int models_hero_pawns(UObject **out, int max);           // pawns of the hero-team slots, slot order
// ~ window: an add-on outfit on your current survivor, saved like the customization screen's choice
// (b4bcoop-outfits.txt); NULL outfit = take it off. NULL = done, else why not
const char *models_wear(const char *outfit);
const char *models_wear_why_not(void);                   // NULL: Wear works now
const char *models_wearing(void);                        // your survivor's saved add-on outfit, NULL none
int models_wear_held(void);                              // 1: saved, but model swaps are off / the host refused it
void models_host_panel(void);                            // ~ window: the model swaps part of Host rules
// weaponlooks.c: added weapon looks (/model <weapon look>, docs/investigations/new-assets.md §9)
int wlooks_init(void);
void wlooks_tick(float dt);
int wlooks_pick(const char *name, Out *o);               // /model <name>: 1 if it is a weapon look (handled)
void wlooks_reset(Out *o);                               // /model reset: your weapons back to your own skins
void wlooks_reset_code(const char *code, Out *o);        // one weapon type (NULL = all) back to your own skin
const char *wlooks_current(const char *code);            // your look for that weapon type (NULL = your own skin)
                                                         // (blocked: Use greyed; hdr: heading of a search; = shown)
void wlooks_list(Out *o);                                // /model list weapons
void wlooks_overview(Out *o);                            // the /model list overview line
void wlooks_status(Out *o);
int wlooks_lock_reset(void);                             // host, /models off: every weapon look undone; count
void wlooks_host_notice(const char *text);
void wlooks_host_data(const char *text);                 // chat.c: a data line from the host ("wlook ...", client)
int wlooks_cmd(const char *verb, char *rest, Out *o);    // dev: wlook dump

// admin.c helpers shared with cheats.c
int admin_player_array(UObject ***arr);                 // GameState.PlayerArray (humans and bots), /players order
UObject *admin_find_player(const char *arg, Out *o);    // "#n", name or unique prefix -> PlayerState (NULL + message)
void admin_display_name(UObject *ps, char *buf, size_t n);   // player name, or a bot's hero name
int admin_is_client(void);                              // connected to someone else's session
void admin_notice(const char *fmt, ...);                // "[b4bcoop] <text>" to every player (host notice)
int admin_data_to_w(UObject *pc, const wchar_t *text, int len); // host: a data line of any length (wide text as is)

// handoff.c: host hand-off (docs/investigations/host-handoff.md): the next host and the campaign run reach every
// client ahead of time; on /handoff or when the host is gone, the next host hosts and continues the run
int handoff_init(void);
void handoff_tick(float dt);
int handoff_live(const char *key, const char *val);      // ini host_handoff=0: no automatic take-over
void handoff_host_data(const wchar_t *text, int len);    // chat.c: a data line from the host starting "handoff "
void handoff_request(const char *why, int old_host_joins); // client: act on the plan on the next tick
void handoff_host_lost(const char *url);                 // travel.c: the connection to our host ended (not ours, no kick)
void handoff_slash(const char *rest, Out *o);            // chat /handoff [<name|#>]
void handoff_players_panel(void);                        // admin.c's Players tab: who hosts next
int handoff_cmd(const char *verb, char *rest, Out *o);   // dev builds: `handoff [runs|run <id>|resume <id>|go|<player>]`
void cmds_become_host(void);                             // cmds.c: host our camp from now on, no join targets
int steamnet_peer_known(uint64_t id);                    // steamnet.c: a Steam P2P peer we exchanged packets with
int steamnet_listen_port(void);                          // steamnet.c: the game's listen port (0 = not listening yet)

// Who may run a chat command; admin.c checks it before any handler runs (admin.c CMDS, cheats.c VERBS).
enum {
    CMD_ANYONE,   // every player, host or client: acts on their own game only (own camera, own light, lists, join/leave)
    CMD_HOST,     // this machine must be the server (listen host or standalone); a client gets "host only"
    CMD_CHEAT,    // host, and cheats on (/cheats on)
    CMD_OWNSAVE,  // own save, cheats on in this session: the host with its cheats on, or a client whose host has cheats on
                  // (the host's data line, cheats.c); writes only this machine's own profile, never sent anywhere
};

// cheats.c: opt-in host-only sandbox (#14) + own-save cheats for everyone in a cheats-on session (#49; docs/COMMANDS.md "Cheats")
int cheats_init(void);                                  // the overlay's Cheats tab
int cheats_perm(const char *verb);                      // CMD_* for a cheats.c verb, -1 if not one
int cheats_enabled(void);                               // /cheats on
int cheats_own_save_ok(const char **why);               // CMD_OWNSAVE allowed now (else 0 and why)
void cheats_host_data(const char *text);                // chat.c: a data line from the host ("cheats on|off", client)
void cheats_slash(const char *verb, char *rest, Out *o); // run a cheats.c verb (permission already checked)
void cheats_tick(float dt);
int rewards_execute_local(UObject *ppc, void *cmd);     // rewards.c: a profile command on this machine's own profile
int cheats_cmd(const char *verb, char *rest, Out *o);   // dev builds: `cheat <cmd> ...`, `cheatprobe ...`

// rules.c: the host's run rules (~ window Run tab, ini keys; host-side only, docs/investigations/run-rules.md)
int rules_init(void);
int rules_live(const char *key, const char *val);       // ini free_burncards=, baseline_cards=, baseline_cards_bots=
void rules_slash(char *rest, Out *o);                   // chat /rules [free_burncards on|off | baseline ...] (CMD_HOST)
int rules_free_burncards(void);                         // burncards.c: drop every burn-card charge
int rules_baseline_count(void);                         // runui.c: the baseline list
int rules_baseline_rows(char (*out)[64], int max);
int rules_baseline_has(const char *row);
// runui.c: the run rules on the game's create-run screen (MatchmakingSettingsPanel), built from the game's widgets
int runui_init(void);
void runui_tick(float dt);
int runui_live(const char *key, const char *val);       // ini runui=0
int runui_cmd(const char *verb, char *rest, Out *o);    // dev builds: `runui [rebuild|click <n>]`
void rules_note_free_charge(const char *row);           // burncards.c: a charge was dropped by the rule (notice)
// cheats.c card helpers shared with rules.c: a card = {its DataTable, its row name} (FDataTableRowHandle's first 2 fields)
typedef struct { UObject *table; FName row; } CardRef;
UObject *cheats_card_manager(void);                     // GameState.GameplayCardManager, NULL when there is none
UObject *cheats_slot_for_ps(UObject *ps);               // the PlayerState's UPlayerSlot (OwnedPlayerSlot, else its pawn's)
UObject *cheats_slot_by_index(int32_t id, uint8_t team); // the UPlayerSlot with that SlotIndex {SlotId, Team} (PlayerSlotManager)
int cheats_card_each(UObject *gcm, void (*fn)(const CardRef *c, const char *row, const char *shown, void *ctx), void *ctx);  // GCM.AllCards; -1 none
int cheats_card_find(UObject *gcm, const char *name, CardRef *out);   // exact row/shown name, or a unique substring; 1 found
int cheats_card_add(UObject *gcm, UObject *slot, const CardRef *card); // GCM.AddGameplayCardToSlotByRowHandle; 1 called
// every row of a card DataTable by object name (e.g. "PlayerCards_MASTER_DT"), with the card's shown name and its
// GameplayCardRow flags (from the table's RowStruct; -1 = unknown field); -1 no table
typedef struct { const char *row, *shown; int category, hidden, deckbuilding, pve_disabled; UObject *table; FName name; } CardRowInfo;
int cheats_card_table_each(const char *table, void (*fn)(const CardRowInfo *c, void *ctx), void *ctx);
UObject *cheats_new_object(UClass *cls, UObject *outer);  // StaticConstructObject_Internal(cls, outer), NULL if unavailable

// updater.c: in-game updates from the ~ window's Updates tab (#34, docs/investigations/updater.md)
int updater_early(void *module);        // DllMain: count a fresh update's starts, revert one that failed; 1 = start nothing
void updater_init(void);
void updater_tick(float dt);
int updater_live(const char *key, const char *val);          // ini updates=0 hides the tab
void updater_note_host(const char *ver, const char *proto);  // a host with another protocol refused us (any thread)
const char *updater_hint(void);                              // " Press ~, tab Updates, ..." or "" (updates=0)
int updater_cmd(const char *verb, char *rest, Out *o);       // dev builds: `update status|check [ver]|install|goback`

// poststats.c: remote players' post-round stats (docs/investigations/post-round-stats.md)
int poststats_cmd(const char *verb, char *rest, Out *o);   // dev builds: `poststats` dumps every player's values
void poststats_tick(float dt);                              // logs every player's values once per post-round screen
