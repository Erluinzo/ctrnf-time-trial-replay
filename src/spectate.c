/* Time Trial Spectator Mode: runs inside the game (USA CUSA13795 1.21), called from the hooks in hooks.s.
 *
 * - Ghost menu: L1 on an entry starts the race like Continue, with spectating switched on.
 * - Race start: nothing is recorded, so the run saves no ghost and unlocks nothing.
 * - Every frame (the ghost update): the player's kart is locked, the camera follows the watched
 *   ghost, and the patch sets the ghost clock itself: pause, rewind, fast forward and speeds.
 *   The HUD (hud.c) shows the ghost's time, name, speed and inputs; sounds.c plays its kart sounds.
 * Notes: ../notes/ (ghost-playback.md, camera-input.md).
 */
#include "spectate.h"

static void show_preview(void);
static void kart_hidden(void *kart, int hidden);

/* ---- Ghost menu ---- */

void sp_menu_focus(void *behavior, void *placeable, int gained)
{
    (void)behavior;
    if (gained) {
        if (sp.screen) sp.debug_focus++;
        sp.armed = 0;           /* back in the menu: a later Continue is a normal race */
        sp.screen = 0;
        show_preview();
    }
    void *project = placeable ? AT(placeable, 0x20, void *) : 0;
    if (!project || !g_gui_manager) return;
    if (gained) {
        char *text = game_string(g_string_pool, "Spectate");
        game_legend_add(project, LEGEND_L1, &text, 1);
        game_string_release(g_string_pool, text - 0x10);
        return;
    }
    /* As removeButtonFromLegend_VSC (0x110bcb0): the GUI manager's remove event, sent to the project */
    void *event = AT(g_gui_manager, 0xf0, void *);
    if (!event) return;
    AT(event, 0x18, int32_t) = LEGEND_L1;
    typedef void handler(void *project, void *event);
    AT(*(void **)project, 0xb0, handler *)(project, event);
}

/* The ghost menu fills its list (0xfceac0, called when the menu opens): a race started from this menu
 * is a normal one unless L1 is pressed in it. Restart in the pause menu does not come here, so a
 * spectator run stays one. */
void *sp_menu_open(void *level, void **list)
{
    sp.armed = 0;
    return game_fill_ghosts_original(level, list);
}

/* L1 on an entry: if ghosts are chosen, start the race as the Continue button does (0x127ee57). */
void sp_menu_spectate(void *behavior, void *placeable)
{
    (void)behavior;
    sp.menu_presses++;
    void *project = placeable ? AT(placeable, 0x20, void *) : 0;
    /* Chosen: the player's ghost (+0x48) or challenge/shared ghosts in the list at +0x50 */
    if (!g_ghost_manager || !project) return;
    if (!AT(g_ghost_manager, 0x48, uint8_t) && !count(AT(g_ghost_manager, 0x50, void *))) return;
    sp.armed = 1;
    sp.screen = 1;              /* sp_input presses Ready on the next screen */
    sp.screen_frames = 0;
    sp.countdown_ticks = 0;
    game_menu_prepare(g_race_settings, 0);
    void *event = game_continue_event_new(0);
    if (!event) return;
    AT(event, 0x18, uint8_t) = 1;
    typedef void handler(void *project, void *event);
    AT(*(void **)project, 0xb0, handler *)(project, event);
    game_release(event);
}

/* ---- Race ---- */

/* Called in place of the game's "start recording the player" at the race start (0xea6390). */
void sp_race_start(void *manager, void *racer)
{
    game_start_recording(manager, racer);
    /* Time Trial only: the game's mode, as the race tick checks it (0xea2d90) */
    sp.active = sp.armed && manager && g_game && AT(g_game, 0xd4c, int32_t) == 2;
    sp.screen = 0;
    if (!sp.active) return;
    AT(manager, 0x0c, uint8_t) = 0;     /* _isRecording: the race end saves nothing (0xfcefe0) */
    sp.manager = manager;
    sp.kart = 0;
    sp.set_up = 0;
    sp.paused = 0;
    sp.speed = NORMAL_SPEED;
    sp.scrub = 0;
    sp.time = 0.0f;
    sp.end = 0.0f;
    sp.watch = 0;
    sp.camera = sp.target = 0;
    sp.unghosted = 0;
    sp.lock = sp.hud = 0;
    sp.show_pad = 1;
    sp.go_watch = -1;
    sp.root = 0;
    sp.heard_watch = -1;
    sp.sounds_played = 0;
}

/* The first local player's id (CPlayer: +0x20 type, local <= 7; +0xb0 id), or -1 */
static int local_player(void)
{
    void *list = g_players ? AT(g_players, 0x10, void *) : 0;
    for (int i = 0; i < count(list); i++) {
        void *player = item(list, i);
        if (!player || AT(player, 0x20, int32_t) > 7) continue;
        int id = AT(player, 0xb0, int32_t);
        return id >= 0 && id <= 7 ? id : -1;
    }
    return -1;
}

/* The first local player's controller (as 0x123a050), or 0 */
static void *controller(void)
{
    int id = local_player();
    if (id < 0 || !g_input_manager) return 0;
    int index = AT(g_input_manager, 0x38 + 4 * id, int32_t);
    void *controllers = AT(g_input_manager, 0x28, void *);
    return index >= 0 && index < count(controllers) ? item(controllers, index) : 0;
}

/* Bit of each spectator button in _buttonState: the first game button mapped to its signal */
static void find_bits(void *ctrl)
{
    for (int b = 0; b < B_COUNT; b++) {
        sp.bit[b] = DEFAULT_BITS[b];
        for (int i = 0; i < 51; i++)
            if (AT(ctrl, 0x18 + 4 * i, int32_t) == SIGNALS[b]) {
                sp.bit[b] = (uint8_t)i;
                break;
            }
    }
}

/* The player's kart, which the character screen shows on its platform */
static void *preview_kart(void)
{
    int id = local_player();
    return id >= 0 && g_kart_manager ? game_player_kart(g_kart_manager, id, 1) : 0;
}

static void show_preview(void)
{
    if (sp.preview) kart_hidden(preview_kart(), 0);
    sp.preview = 0;
}

/* The countdown's GO: the level is about to load, the kart stays hidden and is not used again */
void sp_countdown_go(void)
{
    sp.screen = 0;
    sp.preview = 0;
}

/* A button pressed for one frame: sp_pad_update gives it to the pad device after the device read the
 * pad, as the menus read the device, not the controller's bits. Released on the next frame. */
static void press(int b)
{
    sp.forced = 1ull << b;
}

/* Called in place of the pad device's update (0x3d4dd0): igInputDevice +0x18 values (float per
 * signal), +0x20 changed flags (byte per signal), +0x10 something changed. */
uint64_t sp_pad_update(void *device, void *a, void *b, void *c)
{
    uint64_t result = game_pad_original(device, a, b, c);
    void *ctrl = sp.forced ? controller() : 0;
    if (!ctrl || AT(ctrl, 0x10, void *) != device) return result;
    void *values = AT(device, 0x18, void *), *changed = AT(device, 0x20, void *);
    for (int k = 0; k < B_COUNT; k++)
        if (sp.forced >> k & 1 && SIGNALS[k] < count(values) && SIGNALS[k] < count(changed)) {
            AT(values, 0x20, float *)[SIGNALS[k]] = 1.0f;
            AT(changed, 0x20, uint8_t *)[SIGNALS[k]] = 1;
            AT(device, 0x10, uint8_t) = 1;
        }
    return result;
}

/* Every frame from L1 until the race starts. The character screen after L1 shows no character
 * (its kart is hidden) and starts its countdown at once: the patch presses Ready (Square, the
 * screen's shortcut) every 20 frames until the countdown ticks are heard (sounds.c). Back during the
 * countdown cancels it as usual; the patch then presses Back once more, to the ghost menu. Frames
 * are counted because the menus do not update the game's frame times. */
static void screen_frame(void *ctrl)
{
    sp.forced = 0;      /* the patch's press is released */
    if (!sp.screen) return;
    if (!sp.screen_frames++) find_bits(ctrl);
    /* Back pressed by the player: on any game bit of its signal (the default map has copies) */
    uint64_t pressed = AT(ctrl, 0xe8, uint64_t) & ~AT(ctrl, 0xf0, uint64_t);
    int back = 0;
    for (int i = 0; i < 51; i++)
        if (AT(ctrl, 0x18 + 4 * i, int32_t) == SIGNALS[B_CIRCLE] && pressed >> i & 1) back = 1;
    /* every frame (the screen shows it again when it opens), looked up again each time: it is
     * freed when the level loads, after the countdown's GO (sp_countdown_go) */
    sp.preview = (void *)1;
    kart_hidden(preview_kart(), 1);
    if (sp.screen == 1) {
        if (sp.countdown_ticks) {
            sp.screen = 2;
            sp.screen_frames = 1;
        } else if (back || sp.screen_frames > 600) {
            sp.screen = 0;      /* the player goes back, or the screen never came */
            show_preview();
        } else if (sp.screen_frames >= 30 && sp.screen_frames % 20 == 0) {
            press(B_SQUARE);
        }
        return;
    }
    if (sp.screen == 2) {
        if (back) {             /* the game cancels the countdown: go on to the ghost menu */
            sp.screen = 3;
            sp.screen_frames = 1;
            sp.backs_heard = 0;
        }
        return;
    }
    /* Back again every 30 frames (the screen ignores it just after the cancel) until the game's
     * "back" sound is heard after it (sounds.c), so it never goes back twice */
    if (sp.backs_heard || sp.screen_frames > 150) {
        sp.screen = 0;
        show_preview();
    } else if (sp.screen_frames % 30 == 0) {
        press(B_CIRCLE);
    }
}

/* Called in place of the input manager's update (0xa3b350), every frame */
void *sp_input(void *manager)
{
    void *ctrl = sp.screen || sp.forced ? controller() : 0;
    if (ctrl) screen_frame(ctrl);
    return game_input_original(manager);
}

/* Lock the player's kart (as the race end does, 0xe7eeb0) and the boost bar style button. */
static void set_up(void *manager)
{
    sp.set_up = 1;
    int id = local_player();
    void *kart = sp.kart = id >= 0 && g_kart_manager ? game_player_kart(g_kart_manager, id, 1) : 0;
    void *input = kart ? AT(kart, 0x1a0, void *) : 0;
    if (input && AT(input, 0x58, void *)) {
        void *request = 0;
        game_lock_request(&request, AT(input, 0x58, void *));
        sp.lock = request;      /* kept for the whole race: the lock goes with the level */
    }
    sp.hud = kart && g_hud_meta ? game_find_component(kart, g_hud_meta, 0) : 0;
    if (sp.hud) AT(sp.hud, 0x8a, uint8_t) = 0;    /* _allowSpeedometerToggle: Triangle does nothing */
    void *ctrl = controller();
    if (ctrl) {
        find_bits(ctrl);
        sp.buttons = AT(ctrl, 0xe8, uint64_t);  /* buttons held now (L1 from the menu) are not presses */
    }
    sp.ghosts = count(AT(manager, 0x40, void *));
}

/* Next (step 1) or previous (-1) playing ghost with a kart, from the watched one */
static void switch_ghost(void *manager, int step)
{
    void *list = AT(manager, 0x40, void *);
    int n = count(list);
    for (int i = 1; i <= n; i++) {
        int next = ((sp.watch + step * i) % n + n) % n;
        if (ghost_kart(item(list, next))) {
            sp.watch = next;
            return;
        }
    }
}

static void read_buttons(void *manager)
{
    void *ctrl = controller();
    if (!ctrl) return;
    uint64_t now = AT(ctrl, 0xe8, uint64_t), pressed = now & ~sp.buttons;
    sp.buttons = now;
#define PRESSED(b) (pressed >> sp.bit[b] & 1)
#define HELD(b) (now >> sp.bit[b] & 1)
    if (PRESSED(B_L1)) switch_ghost(manager, -1);
    if (PRESSED(B_R1)) switch_ghost(manager, 1);
    if (PRESSED(B_SQUARE)) sp.paused = !sp.paused;
    if (PRESSED(B_TRIANGLE)) sp.show_pad = !sp.show_pad;
    if (PRESSED(B_UP) && sp.speed < SPEED_COUNT - 1) sp.speed++;
    if (PRESSED(B_DOWN) && sp.speed > 0) sp.speed--;
    sp.scrub = (int)HELD(B_RIGHT) - (int)HELD(B_LEFT);
}

/* The latest key time of all playing ghosts */
static float end_time(void *manager)
{
    void *list = AT(manager, 0x40, void *);
    float end = 0.0f;
    for (int i = 0; i < count(list); i++) {
        void *keys = item(list, i) ? AT(item(list, i), 0x30, void *) : 0;
        int n = count(keys);
        if (n && key_time(keys, n - 1) > end) end = key_time(keys, n - 1);
    }
    return end;
}

/* The game only searches ghost keys forward from the last index (0xfce6a0, 0xfcea00): going back
 * in time needs the indexes reset. Between key 0 and key 1 the game does not move the kart, so it
 * is put on key 0 (0xfce410). */
static void rewind(void *manager, float time)
{
    void *list = AT(manager, 0x40, void *);
    for (int i = 0; i < count(list); i++) {
        void *ghost = item(list, i);
        void *keys = ghost ? AT(ghost, 0x30, void *) : 0;
        if (!ghost_kart(ghost) || !count(keys)) continue;
        AT(ghost, 0x40, int64_t) = 0;   /* _playMoveKeyIndex and _playSnapshotKeyIndex */
        if (count(keys) < 2 || time < key_time(keys, 1)) game_ghost_reset(manager, ghost);
    }
}

/* Past its last key the game no longer places a ghost's kart: it falls (ghosts have no collisions)
 * and keeps a broken state (flames, no engine sound) after a rewind. So in a spectator run each ghost
 * is moved and read at most just before its own last move and input key (times are in 1/100 s). */
static float ghost_end(void *ghost)
{
    void *moves = AT(ghost, 0x30, void *), *inputs = AT(ghost, 0x38, void *);
    int n = count(moves), k = count(inputs);
    float end = n ? key_time(moves, n - 1) : 0.0f;
    if (k && AT(item(inputs, k - 1), 0xe, uint16_t) / 100.0f < end) end = AT(item(inputs, k - 1), 0xe, uint16_t) / 100.0f;
    return end - 0.005f;
}

static void ghost_update(void (*update)(void *, void *), void *manager, void *ghost)
{
    float time = AT(manager, 0x18, float);
    if (sp.active && manager == sp.manager && time > ghost_end(ghost)) {
        AT(manager, 0x18, float) = ghost_end(ghost);
        update(manager, ghost);
        AT(manager, 0x18, float) = time;
        return;
    }
    update(manager, ghost);
}

/* Called in place of the game's two calls for each playing ghost (0xfce60e, 0xfce619) */
void sp_ghost_move(void *manager, void *ghost)
{
    ghost_update(game_ghost_move, manager, ghost);
}

void sp_ghost_inputs(void *manager, void *ghost)
{
    ghost_update(game_ghost_inputs, manager, ghost);
}

/* Point the race camera at the watched ghost's kart (CKartCamera._overrideFocusedEntity, +0x278). */
static void follow(void *manager)
{
    void *list = AT(manager, 0x40, void *);
    if (sp.watch >= count(list)) sp.watch = 0;
    void *target = count(list) ? ghost_kart(item(list, sp.watch)) : 0;
    if (!target) return;
    void *instance = game_camera_instance();
    void *boxes = instance ? AT(instance, 0x58, void *) : 0;
    void *camera = boxes ? game_active_camera(boxes) : 0;
    if (!camera || !g_kart_camera_meta) return;
    typedef void *meta_fn(void *);
    meta_fn **vtable = *(meta_fn ***)camera;
    if (vtable[0](camera) != g_kart_camera_meta) return;   /* not a kart camera: intro and countdown */
    if (camera == sp.camera && target == sp.target) return;
    game_make_handle((char *)camera + 0x278, target);
    game_camera_refresh(camera);
    AT(camera, 0x11d, uint8_t) = 1;     /* _resetOnNextUpdate: jump to the new kart, no swoop */
    sp.camera = camera;
    sp.target = target;
    sp.camera_sets++;
}

/* The watched ghost looks like a normal kart: the game's "temporary ghost" messages remove and spawn
 * again the ghost look effects (common_kart_ghost_timetrial.igz: the same effect list for both). Each
 * kart gets End and Start in turn only, or the look would be spawned twice. */
static void send(void *(*factory)(void *pool), void *kart)
{
    void *message = factory(0);
    if (!message) return;
    game_send_message(message, kart, kart);
    game_release(message);
}

static void ghost_look(void *manager)
{
    void *list = AT(manager, 0x40, void *);
    void *target = sp.watch < count(list) ? ghost_kart(item(list, sp.watch)) : 0;
    if (target == sp.unghosted) return;
    for (int i = 0; i < count(list); i++)       /* only if that kart still plays */
        if (sp.unghosted && ghost_kart(item(list, i)) == sp.unghosted) send(game_ghost_look_start_new, sp.unghosted);
    if (target) send(game_ghost_look_end_new, target);
    sp.unghosted = target;
}

/* A kart, its driver and its child entities (wheels) drawn or not (igEntity "set hidden" 0x3def00
 * changes the drawing only; CChildManagerComponent +0x28 handles) */
static void kart_hidden(void *kart, int hidden)
{
    if (!kart) return;
    game_set_entity_hidden(kart, hidden);
    void *driver = game_get_driver(kart);
    if (driver) game_set_entity_hidden(driver, hidden);
    void *children = g_child_manager_meta ? game_find_component(kart, g_child_manager_meta, 0) : 0;
    void *list = children ? AT(children, 0x28, void *) : 0;
    for (int i = 0; i < count(list); i++) {
        void *entity = handle_object(&AT(list, 0x20, void **)[i]);
        if (entity) game_set_entity_hidden(entity, hidden);
    }
}

/* The player is not seen: the locked kart is hidden every frame, as game code may show it again. The
 * level is loaded again after the run. */
static void hide_player(void)
{
    kart_hidden(sp.kart, 1);
}

/* Every frame while ghosts play: returns the new ghost play time (the game adds the frame time). */
float sp_clock(void *manager, float game_time)
{
    float old = AT(manager, 0x18, float);
    if (!sp.active || manager != sp.manager) return old + game_time;
    sp.frames++;
    if (!sp.set_up) set_up(manager);
    int paused = g_frame && AT(g_frame, 0x28, uint8_t);    /* the game's pause menu */
    float dt = 0.0f;
    if (!paused) {
        read_buttons(manager);
        dt = g_frame ? AT(g_frame, 0x18, float) : 0.0f;     /* real time: speeds are the patch's own */
        if (dt < 0.001f) dt = 0.001f;
        if (dt > 0.1f) dt = 0.1f;
    }
    float rate = sp.scrub ? sp.scrub * SCRUB_SPEED : sp.paused ? 0.0f : SPEEDS[sp.speed];
    float time = sp.time + dt * rate;
    sp.end = end_time(manager);
    if (time > sp.end) time = sp.end;   /* the run stops at the end, and can be rewound */
    if (time < 0.0f) time = 0.0f;
    if (time < old) rewind(manager, time);
    sp.time = time;
    follow(manager);
    ghost_look(manager);
    hide_player();
    hud_frame(manager);
    sounds_frame(manager, old, time);
    return time;
}
