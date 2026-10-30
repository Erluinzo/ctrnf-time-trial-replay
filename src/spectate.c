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
static void send(void *(*factory)(void *pool), void *kart);

/* ---- Ghost menu ---- */

void sp_menu_focus(void *behavior, void *placeable, int gained)
{
    (void)behavior;
    if (gained) {
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
 * spectator run stays one. Back from the character screen after L1, the character is shown again. */
void *sp_menu_open(void *level, void **list)
{
    sp.armed = 0;
    sp.screen = 0;
    show_preview();
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
    sp.unghosted = 0;
    sp.screen = 1;              /* sp_input presses Ready on the next screen */
    sp.screen_frames = 0;
    sp.screen_project = 0;
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
    boosts_reset();
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
    sp.target_placed = 0;
    sp.lock = sp.hud = 0;
    sp.show_pad = 1;
    sp.go_watch = -1;
    sp.root = 0;
    sp.bar_kart = 0;
    sp.pad_at = -10.0f;
    sp.skip_to = 0.0f;
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
    if (sp.screen_project) screen_gui(sp.screen_project, sp.screen < 3);
    if (sp.screen == 1) {
        if (sp.countdown_ticks) {
            sp.screen = 2;
            sp.screen_frames = 1;
        } else if (back) {
            sp.screen = 0;      /* the player goes back: shown again in the ghost menu (sp_menu_open) */
        } else if (sp.screen_frames > 600) {
            sp.screen = 0;      /* the screen never came */
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
     * "back" sound is heard after it (sounds.c), so it never goes back twice. The character stays
     * hidden while the screen goes: the ghost menu shows it again (sp_menu_open). */
    if (sp.backs_heard || sp.screen_frames > 150) {
        sp.screen = 0;
    } else if (sp.screen_frames % 30 == 0) {
        press(B_CIRCLE);
    }
}

/* Called in place of the game's "add a button to a screen's legend" (0xa09d30): the character screen
 * adds its own when it opens, which gives the patch its GUI project */
void sp_legend_add(void *project, int button, char **text, int always)
{
    if (sp.screen) {
        sp.screen_project = project;
        kart_hidden(preview_kart(), 1);     /* the screen opens again after a cancel: no character */
    }
    game_legend_add(project, button, text, always);
}

/* Called in place of the input manager's update (0xa3b350), every frame */
void *sp_input(void *manager)
{
    g_spectate_kart = sp.shared_next;
    sp.shared_next = 0;
    /* The run is over (level exit clears _isPlaying, +0x14): the game's speed back to normal */
    if (sp.game_speed_set && !(sp.active && sp.manager && AT(sp.manager, 0x14, uint8_t)) && g_frame) {
        AT(g_frame, 0x2c, float) = 1.0f;
        sp.game_speed_set = 0;
    }
    void *ctrl = sp.screen || sp.forced ? controller() : 0;
    if (ctrl) screen_frame(ctrl);
    /* The first watched ghost looks like a normal kart from the start: its ghost look fades out in
     * about 0.7 s, so it is ended as soon as its kart is there, during the track's intro */
    void *ghosts = sp.armed && !sp.active && g_ghost_manager ? AT(g_ghost_manager, 0x40, void *) : 0;
    void *first = count(ghosts) ? ghost_kart(item(ghosts, 0)) : 0;
    if (first && first != sp.unghosted) {
        send(game_ghost_look_end_new, first);
        sp.unghosted = first;
    }
    hud_late();
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
    if (kart) game_kart_immobile(kart, 0);      /* no physics: carried with the ghost (carry_player) */
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

/* L2 / R2: to the start of this lap (of the lap before within LAP_BACK real s of playing) / of the
 * next lap. Into time played before it is a jump (the boosts are put back, boosts.c); past it a fast
 * forward from there (skip_to, sp_clock), so the ghosts' boosts come as in the race. */
#define LAP_BACK 1.0f     /* real s: at x4 that is 4 s of the race */
static void lap_skip(int step)
{
    if (sp.go_watch != sp.watch) return;
    float target = -1.0f;
    if (step < 0) {
        target = sp.go;
        for (int i = 0; i < sp.lap_count; i++)
            if (sp.lap_starts[i] < sp.time - LAP_BACK * (SPEEDS[sp.speed] > 1.0f ? SPEEDS[sp.speed] : 1.0f)) target = sp.lap_starts[i];
    } else {
        for (int i = sp.lap_count - 1; i >= 0; i--)
            if (sp.lap_starts[i] > sp.time + 0.05f) target = sp.lap_starts[i];
    }
    if (target < 0.0f) return;
    float saved = boosts_saved_until();
    sp.skip_to = target > saved ? target : 0.0f;
    sp.time = target > saved ? (saved > sp.time ? saved : sp.time) : target;
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
    if (PRESSED(B_UP) && sp.speed < SPEED_COUNT - 1) sp.speed++;
    if (PRESSED(B_DOWN) && sp.speed > 0) sp.speed--;
    if (PRESSED(B_UP) || PRESSED(B_DOWN)) sp.speed_shown = 2.0f;     /* the HUD shows it for 2 s */
    if (PRESSED(B_L2)) lap_skip(-1);
    if (PRESSED(B_R2)) lap_skip(1);
    sp.scrub = (int)HELD(B_RIGHT) - (int)HELD(B_LEFT);
    if (sp.scrub) sp.skip_to = 0.0f;   /* the D-pad ends a lap skip */
    if (sp.skip_to > 0.0f) sp.scrub = 1;
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
    int on = sp.active && manager == sp.manager;
    if (on && time > ghost_end(ghost)) AT(manager, 0x18, float) = ghost_end(ghost);
    sp.placing = on ? ghost : 0;
    sp.placing_time = time;
    update(manager, ghost);
    sp.placing = 0;
    AT(manager, 0x18, float) = time;
}

/* Smooth movement. Move keys are about 0.07 s apart (up to 0.35 s in the air) and their times are
 * rounded to 1/100 s, so the game's straight lines between them make corners and change speed by
 * up to 20% at every key: the kart stutters. In a spectator run the position follows a curve
 * through the keys (Catmull-Rom: the direction at a key comes from its neighbours), averaged over
 * +-SMOOTH s around the play time (triangle weights) for the rounded times; angles are averaged
 * only. Past and future keys are used, so there is no delay. A jump (respawn) is not smoothed. CGhostMoveKey: +0xc position, +0x18 time (1/100 s), +0x1a/+0x1c/+0x1e
 * angles (65536 = one turn). */
#define SMOOTH 0.15f
#define JUMP 6000.0f        /* units in 2 x SMOOTH (0.3 s): 20000 units/s, faster than any kart (falls reach 11000) */
#define TURN 6.2831853f
#define ANGLE_UNIT 10430.06f

static float wrap(float a)      /* to -pi..pi */
{
    while (a > TURN / 2) a -= TURN;
    while (a < -TURN / 2) a += TURN;
    return a;
}

/* Key pair at time t: lo, and the fraction towards lo + 1 */
static int key_at(void *keys, float t, float *f)
{
    int lo = 0, hi = count(keys) - 1;
    while (hi - lo > 1) {
        int mid = (lo + hi) / 2;
        if (key_time(keys, mid) <= t) lo = mid; else hi = mid;
    }
    float t0 = key_time(keys, lo), t1 = key_time(keys, hi);
    *f = t1 > t0 ? (t - t0) / (t1 - t0) : 0.0f;
    if (*f < 0.0f) *f = 0.0f;
    if (*f > 1.0f) *f = 1.0f;
    return lo;
}

static float key_pos(void *keys, int k, int i);

/* A ghost's speed in its race between times a and b: path length over its move keys (units/s) */
float sp_path_speed(void *ghost, float a, float b)
{
    void *keys = AT(ghost, 0x30, void *);
    float f, length = 0.0f;
    if (count(keys) < 2 || b <= a) return 0.0f;
    int k = key_at(keys, a, &f) + 1, first = k;
    for (; k < count(keys) && key_time(keys, k) <= b; k++) {
        float d = 0.0f;
        for (int i = 0; i < 3; i++) d += (key_pos(keys, k, i) - key_pos(keys, k - 1, i)) * (key_pos(keys, k, i) - key_pos(keys, k - 1, i));
        __asm__("sqrtss %1, %0" : "=x"(d) : "x"(d));
        length += d;
    }
    float span = k - 1 > first ? key_time(keys, k - 1) - key_time(keys, first - 1) : 0.0f;
    return span > 0.0f ? length / span : 0.0f;
}

/* Direction (units per second, axis i) at key k: from the key before to the key after */
static float slope(void *keys, int k, int i)
{
    int a = k > 0 ? k - 1 : k, b = k + 1 < count(keys) ? k + 1 : k;
    float dt = key_time(keys, b) - key_time(keys, a);
    return dt > 0.0f ? (AT(item(keys, b), 0xc + 4 * i, float) - AT(item(keys, a), 0xc + 4 * i, float)) / dt : 0.0f;
}

/* Axis i of the curve between key lo and lo + 1, at fraction f (cubic Hermite) */
static float curve(void *keys, int lo, float f, int i)
{
    int hi = lo + 1 < count(keys) ? lo + 1 : lo;
    float h = key_time(keys, hi) - key_time(keys, lo);
    float p0 = AT(item(keys, lo), 0xc + 4 * i, float), p1 = AT(item(keys, hi), 0xc + 4 * i, float);
    if (h <= 0.0f) return p0;
    float f2 = f * f, f3 = f2 * f;
    return (2 * f3 - 3 * f2 + 1) * p0 + (f3 - 2 * f2 + f) * h * slope(keys, lo, i)
         + (3 * f2 - 2 * f3) * p1 + (f3 - f2) * h * slope(keys, hi, i);
}

/* The ghost's position (pose 0) or angles (pose 1) at time t, smoothed; 0 when it cannot be */
static int smooth_pose(void *ghost, float t, int pose, float *out)
{
    void *keys = AT(ghost, 0x30, void *);
    int n = count(keys);
    if (n < 3) return 0;
    float first = key_time(keys, 1), last = ghost_end(ghost), sum = 0.0f, ref[3] = {0};
    float ends[2][3];
    out[0] = out[1] = out[2] = 0.0f;
    for (int k = -4; k <= 4; k++) {
        float s = t + k * SMOOTH / 4, f, v[3];
        if (s < first) s = first;
        if (s > last) s = last;
        int lo = key_at(keys, s, &f);
        void *a = item(keys, lo), *b = item(keys, lo + 1 < n ? lo + 1 : lo);
        for (int i = 0; i < 3; i++) {
            if (pose) {
                float x = AT(a, 0x1a + 2 * i, int16_t) / ANGLE_UNIT, y = AT(b, 0x1a + 2 * i, int16_t) / ANGLE_UNIT;
                v[i] = x + f * wrap(y - x);
            } else {
                v[i] = curve(keys, lo, f, i);
            }
        }
        if (k == -4 || k == 4)
            for (int i = 0; i < 3; i++) ends[k > 0][i] = v[i];
        if (k == -4)
            for (int i = 0; i < 3; i++) ref[i] = v[i];
        float w = (float)(5 - (k < 0 ? -k : k));
        for (int i = 0; i < 3; i++) out[i] += w * (pose ? wrap(v[i] - ref[i]) : v[i]);   /* angles: no jump at +-pi */
        sum += w;
    }
    for (int i = 0; i < 3; i++) out[i] = pose ? wrap(ref[i] + out[i] / sum) : out[i] / sum;
    if (!pose) {
        float dx = ends[1][0] - ends[0][0], dy = ends[1][1] - ends[0][1], dz = ends[1][2] - ends[0][2];
        if (dx * dx + dy * dy + dz * dz > JUMP * JUMP) return 0;
    }
    return 1;
}

/* Called in place of "set position" where the game gives a kart its physics body's position after
 * the physics step (0xe96691). The body of a ghost kart can touch the track (e.g. Electron Avenue's
 * big jump at 0:07.39) and be pushed up to 100 units for a frame: the watched kart gets where it is on
 * the path instead: its placement at this frame's play time (sp_place_position). Not moved on by the
 * frame time as the physics step does: after an uneven frame (a 23 ms stall at 7.37) that put it up
 * to 90 units off for a frame. */
void sp_body_sync(void *entity, const float *position)
{
    if (sp.active && entity && entity == sp.target && sp.target_placed) position = sp.placed;
    void *list = sp.active && entity && sp.manager ? AT(sp.manager, 0x40, void *) : 0;
    for (int i = 0; i < count(list) && i < HISTORY_GHOSTS; i++)
        if (sp.held[i] && ghost_kart(item(list, i)) == entity) position = sp.hold[i];
    game_set_entity_position(entity, position);
}

/* Falls and crushes. A ghost file has neither: the recording stops while the kart falls or is crushed.
 * A fall: the keys stand still at the stop key for about 1.5 s, then jump to where the game put the kart
 * back (the respawn) and follow Aku Aku's descent; the kart now falls on from the stop key with its
 * speed there until the jump. A crush (Clockwork Wumpa's crusher blocks): a stop of CRUSH_GAP s or more
 * with no jump after it; the game's squash plays (respawns). */
#define RESPAWN_JUMP 1000.0f    /* units in one key step (< 0.05 s): no kart moves that far */
#define STOP_GAP 0.8f           /* s with no key before the jump */
#define GRAVITY 4000.0f         /* units/s/s */
#define FALL_MAX 1000.0f        /* units down at most */
#define FALL_OUT 0.3f           /* s it goes on forward, then it falls straight down (2500 units forward
                                 * into the level, the game crashed at 0x13c54ef) */
#define CRUSHED 8               /* EKartDamageType eKDT_Crushed */
#define CRUSH_GAP 0.95f         /* s with no key and almost no move (< CRUSH_MOVE units) */
#define CRUSH_MOVE 400.0f

static float key_pos(void *keys, int k, int i)
{
    return AT(item(keys, k), 0xc + 4 * i, float);
}

/* Key k is a respawn jump: a long move in one key step. Keys at speed can be 0.7 s and 1900 units apart
 * (Hot Air Skyway), a jump takes 0.01 s */
static int respawn_jump(void *keys, int k)
{
    if (k < 1 || k >= count(keys) || key_time(keys, k) - key_time(keys, k - 1) > 0.05f) return 0;
    float d = 0.0f;
    for (int i = 0; i < 3; i++) d += (key_pos(keys, k, i) - key_pos(keys, k - 1, i)) * (key_pos(keys, k, i) - key_pos(keys, k - 1, i));
    return d > RESPAWN_JUMP * RESPAWN_JUMP;
}

/* Key k + 2 is a respawn jump after a stop at key k */
static int stop_before_jump(void *keys, int k)
{
    return k >= 1 && k + 2 < count(keys) && key_time(keys, k + 1) - key_time(keys, k) >= STOP_GAP && respawn_jump(keys, k + 2);
}

/* The kart was crushed at key k: it stopped there with no respawn after (and not just put back: Aku Aku
 * holds it about 1.5 s while it comes down about 400 units) */
static int crush_stop(void *keys, int k)
{
    if (k < 1 || k + 1 >= count(keys) || stop_before_jump(keys, k) || respawn_jump(keys, k) || key_time(keys, k + 1) - key_time(keys, k) < CRUSH_GAP) return 0;
    float d = 0.0f;
    for (int i = 0; i < 3; i++) d += (key_pos(keys, k + 1, i) - key_pos(keys, k, i)) * (key_pos(keys, k + 1, i) - key_pos(keys, k, i));
    return d < CRUSH_MOVE * CRUSH_MOVE;
}

/* The kart's position and speed while it falls at time t, or 0 when it does not */
static int falling(void *ghost, float t, float *at, float *v)
{
    void *keys = AT(ghost, 0x30, void *);
    float f;
    int k = count(keys) > 3 ? key_at(keys, t, &f) : 0;
    if (!stop_before_jump(keys, k) || t >= key_time(keys, k + 1)) return 0;
    float dt = t - key_time(keys, k), step = key_time(keys, k) - key_time(keys, k - 1);
    for (int i = 0; i < 3; i++) {
        v[i] = step > 0.0f ? (key_pos(keys, k, i) - key_pos(keys, k - 1, i)) / step : 0.0f;
        at[i] = key_pos(keys, k, i) + v[i] * (dt < FALL_OUT ? dt : FALL_OUT);
        if (dt > FALL_OUT) v[i] = 0.0f;
    }
    float drop = GRAVITY * dt * dt / 2;
    at[2] -= drop < FALL_MAX ? drop : FALL_MAX;     /* z is up */
    v[2] -= drop < FALL_MAX ? GRAVITY * dt : 0.0f;
    return 1;
}

/* The watched ghost falls now (hud.c: no WRONG WAY then) */
int sp_watched_falling(void)
{
    void *list = sp.manager ? AT(sp.manager, 0x40, void *) : 0;
    float at[3], v[3];
    return sp.watch < count(list) && falling(item(list, sp.watch), sp.time, at, v);
}

/* Called in place of the game's "set position" and "set angles" when it places a ghost kart (0xfce836,
 * 0xfce997, in 0xfce6a0) */
void sp_place_position(void *entity, const float *position)
{
    float smooth[3], ahead[3];
    if (!sp.placing || !smooth_pose(sp.placing, sp.placing_time, 0, smooth)) {
        game_set_entity_position(entity, position);
        return;
    }
    void *list = AT(sp.manager, 0x40, void *);
    int watched = sp.watch < count(list) && item(list, sp.watch) == sp.placing;
    /* Its speed (+0x60) along the same path: the game's own speed value jumps at the keys. The speed
     * always points forward, as the camera looks along it (backwards while rewinding, it swung to the
     * front of the kart). */
    float v[3];
    if (falling(sp.placing, sp.placing_time, smooth, v)) {
        for (int i = 0; i < 3; i++) AT(entity, 0x60 + 4 * i, float) = v[i];
    } else if (smooth_pose(sp.placing, sp.placing_time + 0.01f, 0, ahead)) {
        for (int i = 0; i < 3; i++) AT(entity, 0x60 + 4 * i, float) = (ahead[i] - smooth[i]) / 0.01f;
    }
    game_set_entity_position(entity, smooth);
    if (watched) {      /* for sp_body_sync */
        for (int i = 0; i < 3; i++) sp.placed[i] = smooth[i];
        sp.target_placed = 1;
    }
}

void sp_place_angles(void *entity, const float *angles, int zero)
{
    float smooth[3];
    if (sp.placing && smooth_pose(sp.placing, sp.placing_time, 1, smooth)) angles = smooth;
    game_set_entity_angles(entity, angles, zero);
}

/* Called in place of the game's two calls for each playing ghost (0xfce60e, 0xfce619) */
#define FALL_SPEED 1000.0f      /* units/s down: a finished kart that goes down faster falls */
void sp_ghost_move(void *manager, void *ghost)
{
    /* Past its last key (it finished before the others) the kart is not placed: the game's physics
     * drives it on, with collisions (sp_collision) and no inputs (sp_ghost_inputs). Owner: "If they
     * fall, just stop them there": it then stays where it left the road (sp_body_sync). */
    if (sp.active && manager == sp.manager) {
        void *list = AT(manager, 0x40, void *), *kart = ghost_kart(ghost);
        int i = 0;
        while (i < count(list) && i < HISTORY_GHOSTS && item(list, i) != ghost) i++;
        int finished = AT(manager, 0x18, float) > ghost_end(ghost);
        if (i < HISTORY_GHOSTS && (!finished || !kart)) sp.held[i] = 0;
        if (finished) {
            if (kart == sp.target) sp.target_placed = 0;      /* sp_body_sync leaves it */
            if (i == HISTORY_GHOSTS || !kart) return;
            if (!sp.held[i] && AT(kart, 0x68, float) > -FALL_SPEED) game_entity_position(sp.hold[i], kart);
            else sp.held[i] = 1;
            if (sp.held[i]) {
                AT(kart, 0x60, float) = AT(kart, 0x64, float) = AT(kart, 0x68, float) = 0.0f;
                game_set_entity_position(kart, sp.hold[i]);
            }
            return;
        }
    }
    ghost_update(game_ghost_move, manager, ghost);
}

/* Faster than x1 a frame can jump over a short input (a boost tap in a slide: the kart misses the
 * boost and loses its sacred fire, seen at x4). So the inputs never skip a key: when this frame would
 * pass the next key (+0x44 is the key in effect), they stop on it and catch up on the next frames.
 * Not after a jump in time (rewind, start): only for a step of up to SKIP_MAX s. */
#define SKIP_MAX 0.25f
void sp_ghost_inputs(void *manager, void *ghost)
{
    void *keys = AT(ghost, 0x38, void *);
    int next = AT(ghost, 0x44, int32_t) + 1;
    float time = AT(manager, 0x18, float);
    /* Past its last key: its last input key, which lets go of every button */
    if (sp.active && manager == sp.manager && time > ghost_end(ghost) && count(keys)) {
        AT(manager, 0x18, float) = AT(item(keys, count(keys) - 1), 0xe, uint16_t) / 100.0f;
        game_ghost_inputs(manager, ghost);
        AT(manager, 0x18, float) = time;
        return;
    }
    if (sp.active && manager == sp.manager && next >= 1 && next + 1 < count(keys)) {
        float t1 = AT(item(keys, next), 0xe, uint16_t) / 100.0f, t2 = AT(item(keys, next + 1), 0xe, uint16_t) / 100.0f;
        /* A key in the same 1/100 s as the one after it is never in effect in the game's search, e.g.
         * the "start drift" input (bit 13, read at 0xe301b5), which starts a ghost's slide: a third of
         * them were lost, with the slide's boosts (Hot Air Skyway 34.38: no slide, the fire went out).
         * It is the key in effect for one frame (not the last keys: the final one releases every button). */
        if (time >= t1 && t2 <= t1 && time - t1 < SKIP_MAX && t1 < ghost_end(ghost)) {
            AT(ghost, 0x44, int32_t) = next;
            return;
        }
        if (time >= t2 && time - t1 < SKIP_MAX) AT(manager, 0x18, float) = t1;
    }
    ghost_update(game_ghost_inputs, manager, ghost);
    AT(manager, 0x18, float) = time;
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
    /* While rewinding the kart goes backwards at x4: the camera follows too slowly, the kart passes it
     * and is seen from the front. So the camera is put in its place behind the kart every frame. */
    if (sp.scrub < 0) AT(camera, 0x11d, uint8_t) = 1;
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

/* The level's triggers show parts of the track (e.g. Electron Avenue's road at 0:17 and 0:25) as the
 * player's kart comes near, so the hidden kart is put where the watched ghost will be in CARRY_AHEAD s,
 * CARRY_UNDER units under it: the road ahead is shown in time (just behind the camera it came too late),
 * and nothing of the kart can be seen. It does not move by itself (immobile, speed 0, air time 0), makes
 * no sound (sounds.c), no controller rumble (sp_rumble) and no respawn (sp_respawn), its laps and race
 * end are ignored (sp_player_lap, sp_player_finished), and its WRONG WAY check looks at the watched
 * ghost (sp_wrong_way_position and _angles, hud.c). */
#define CARRY_AHEAD 0.3f
#define CARRY_UNDER 400.0f
static void carry_player(void *manager)
{
    void *list = AT(manager, 0x40, void *);
    float at[3];
    if (!sp.kart || sp.watch >= count(list) || !smooth_pose(item(list, sp.watch), sp.time + CARRY_AHEAD, 0, at)) return;
    at[2] -= CARRY_UNDER;      /* z is up */
    game_set_entity_position(sp.kart, at);
    AT(sp.kart, 0x60, float) = AT(sp.kart, 0x64, float) = AT(sp.kart, 0x68, float) = 0.0f;
    void *move = g_kart_move_meta ? game_find_component(sp.kart, g_kart_move_meta, 0) : 0;
    if (move) AT(move, 0x248, float) = 0.0f;       /* air time: no wheel flames */
    void *audio = g_vehicle_audio_meta ? game_find_component(sp.kart, g_vehicle_audio_meta, 0) : 0;
    if (audio) AT(audio, 0x7c, float) = 0.0f;      /* the engine's wanted volume */
}

/* Called in place of the game's "start a controller rumble" (0xc04ed0): ghosts never rumble, so in
 * a spectator run every rumble comes from the hidden player kart */
void *sp_rumble(void *a, void *b, int c, void *d, void *e, void *f)
{
    if (sp.active && sp.manager && AT(sp.manager, 0x14, uint8_t)) return 0;
    return game_rumble_original(a, b, c, d, e, f);
}

/* Called in place of the game's "respawn a kart" (0xe7dd00; used by the fall check 0xe7dac0, respawn
 * triggers and the respawnKart script): ghost karts follow their keys (a fall in the race is a jump in
 * them), and rewinding or fast forwarding could send one through a trigger, so no kart respawns in a
 * spectator run */
void sp_respawn(void *component, int kind, void *at)
{
    if (sp.active && sp.manager && AT(sp.manager, 0x14, uint8_t) && !sp.respawning) return;
    game_respawn_original(component, kind, at);
}

/* The ghost file has no fall: after the kart left the track its keys stand still (about 1.5 s), then
 * jump to where the game put it back (the respawn) and follow Aku Aku's descent. When the watched
 * ghost plays past such a stop, the game's own respawn runs on its kart (respawnKart_VSC 0x1041a10)
 * for the Aku Aku / Uka Uka look; the kart stays on its keys. */
static void respawns(void *manager, float old, float time)
{
    void *list = AT(manager, 0x40, void *);
    void *ghost = sp.watch < count(list) ? item(list, sp.watch) : 0;
    void *kart = ghost_kart(ghost), *keys = ghost ? AT(ghost, 0x30, void *) : 0;
    if (!kart || sp.scrub || time <= old || time - old > 0.5f || count(keys) < 2) return;
    float f;
    for (int k = key_at(keys, old, &f) + 1; k < count(keys) && key_time(keys, k) <= time; k++) {
        /* not while still flat from the crush before (Clockwork Wumpa: crushed again 1.7 s later): the
         * game squashed it again and the first squash's sound never ended (owner: "the sound stays there
         * forever"). COverrideRenderMatrixComponent +0xc4: the kart's height scale (0.33 flat). */
        void *matrix = g_render_matrix_meta ? game_find_component(kart, g_render_matrix_meta, 0) : 0;
        int flat = matrix && AT(matrix, 0xc4, float) < 0.99f;
        if (key_time(keys, k) > old && crush_stop(keys, k) && !flat) {
            /* the game's squash; it refuses ghosts (0xe346f0: controller +0x260 _isGhost) */
            void *control = g_kart_move_meta ? game_find_component(kart, g_kart_move_meta, 0) : 0;
            uint8_t done = 0, other = 0;
            if (control) AT(control, 0x260, uint8_t) = 0;
            game_kart_damage(kart, CRUSHED, kart, &done, &other);
            if (control) AT(control, 0x260, uint8_t) = 1;
        }
        if (key_time(keys, k) <= old) continue;
        /* The game's respawn hides the kart about 1.6 s before Aku Aku brings it down: as in the race,
         * it starts where the keys stop before the jump (else the kart was hidden while its keys
         * already came down with Aku Aku, Hot Air Skyway 137.9-139.6 s) */
        int jump = stop_before_jump(keys, k) ? k + 2 : respawn_jump(keys, k) && !stop_before_jump(keys, k - 2) ? k : 0;
        if (respawn_jump(keys, k) && sp.camera) AT(sp.camera, 0x11d, uint8_t) = 1;  /* the camera jumps with the kart (no sweep through the floor) */
        if (!jump) continue;
        /* from where the game puts it back: from a fall the game's respawn found no place (crash 0x13c54ef) */
        game_set_entity_position(kart, &AT(item(keys, jump), 0xc, float));
        sp.respawning = 1;
        game_respawn_kart(kart, 0, 0);
        sp.respawning = 0;
    }
}

/* Crates: ghost karts never enter a crate's trigger (0xeebfa0), so the "!" crate the ghost broke
 * (Clockwork Wumpa ~71 s) stayed and its hole never opened. When the watched ghost goes forward past a
 * crate that can still break (+0x2c), the game's "kart entered the crate" (0xeec470) runs with the ghost
 * kart's crate collector (the player-only part, Wumpa fruit, needs a racer: ghosts have none). Its path
 * since the last frame is checked, so fast forward and lap skips break it too. A rewind does not bring a
 * broken crate back. */
#define CRATE_REACH 300.0f      /* units from its middle (the ghost that broke it passed at 249) */
#define CRATE_STEP 0.02f        /* s between the path points checked */
static void crates(void *manager, float old, float time)
{
    void *ghosts = AT(manager, 0x40, void *);
    void *ghost = sp.watch < count(ghosts) ? item(ghosts, sp.watch) : 0, *kart = ghost_kart(ghost);
    void *list = g_crates ? AT(g_crates, 0x10, void *) : 0;
    void *collector = kart && g_crate_collector_meta ? game_find_component(kart, g_crate_collector_meta, 0) : 0;
    if (!collector || time <= old || !count(list)) return;
    float step = (time - old) / 4000.0f > CRATE_STEP ? (time - old) / 4000.0f : CRATE_STEP;
    for (float t = old + step; t < time + step; t += step) {
        float at[3], crate[3];
        if (!smooth_pose(ghost, t < time ? t : time, 0, at)) return;
        for (int i = 0; i < count(list); i++) {
            void *c = item(list, i);
            if (!c || !AT(c, 0x2c, uint8_t) || !AT(c, 0x18, void *)) continue;
            game_entity_position(crate, AT(c, 0x18, void *));
            float d = 0.0f;
            for (int j = 0; j < 3; j++) d += (crate[j] - at[j]) * (crate[j] - at[j]);
            if (d < CRATE_REACH * CRATE_REACH) game_crate_enter(c, collector);
        }
    }
}

/* Called in place of the game's start boost at GO (0xeb6a37). A kart charges it by pressing accelerate
 * in the countdown (0xe7cd90: +0x24 charge, +0x20 level), but ghost files keep no input before GO, so
 * a ghost kart never gets it. A ghost that started with one moved fast at once (measured: about 700
 * units in the first 0.5 s, about 200 without): those get it here. The level (how full the turbo bar
 * is) cannot be told from the file: the best one. */
#define START_BOOST_MOVE 400.0f
void sp_turbo_start(void *turbo)
{
    void *list = sp.active && sp.manager && turbo ? AT(sp.manager, 0x40, void *) : 0;
    char *settings = list ? AT(game_kart_settings(), 0x100, char *) : 0;
    float a[3], b[3];
    for (int i = 0; settings && i < count(list); i++) {
        void *ghost = item(list, i);
        if (ghost_kart(ghost) != AT(turbo, 0x10, void *) || !smooth_pose(ghost, sp.time, 0, a)
            || !smooth_pose(ghost, sp.time + 0.5f, 0, b)) continue;
        float dx = b[0] - a[0], dy = b[1] - a[1], dz = b[2] - a[2];
        if (dx * dx + dy * dy + dz * dz < START_BOOST_MOVE * START_BOOST_MOVE) continue;
        AT(turbo, 0x20, int32_t) = count(AT(settings, 0x10, void *)) - 1;
        AT(turbo, 0x24, float) = AT(settings, 0x18, float) + 1.0f;    /* above the threshold */
    }
    game_turbo_start(turbo);
}

/* Called in place of the kart's "boost from the boost pad under it" (0xe56595, 0xe57120), made when the
 * ground under the kart changes. Slower than x1 the ghost kart's ground contact can drop for a frame on
 * a pad, which gave a second boost (seen at x0.1). Slow playback should look like x1: no new pad boost
 * for the watched kart within PAD_REPEAT s of ghost time (pads at x1 were 0.83 s apart or more). The
 * hidden player kart gets none (it filled the Reserves Meter mod's bar). */
#define PAD_REPEAT 0.6f
void sp_pad_boost(void **out, void *controller, void **material)
{
    int watched = sp.active && controller && AT(controller, 0x18, void *) == sp.target;
    int slow = !sp.scrub && SPEEDS[sp.speed] < 1.0f;
    int player = sp.active && controller && AT(controller, 0x18, void *) == sp.kart;  /* carried under the road */
    if (player || (watched && slow && sp.time > sp.pad_at && sp.time - sp.pad_at < PAD_REPEAT)) {
        *out = 0;
        return;
    }
    game_pad_boost(out, controller, material);
    if (watched && *out) sp.pad_at = sp.time;
}

/* Called in place of the kart camera's collision handling (0xc8b160): on Electron Avenue the camera
 * hits an overhead sign (2:51.9 and 1:48.6, ghost time 175.6 and 112.5) and ends inside the ghost's kart
 * for a moment. The spectator's camera ignores collisions (in a whole run they happen only there). */
void sp_camera_contacts(void *camera, void **contacts, int n)
{
    if (sp.active && camera == sp.camera) return;
    game_camera_contacts_original(camera, contacts, n);
}

/* Called in place of a kart's collision response (0xe916b0, a callback for its body's contacts): the
 * ghosts had no collisions in the race, but their karts' bodies still touch the level when placed (e.g.
 * an overhead sign at 112.4 s on Electron Avenue), and a hit while boosting ended all the kart's boosts
 * and its sacred fire. In a spectator run ghost karts (until their last key: then they drive on, as
 * after a race) and the hidden player kart get no response. */
long sp_collision(void *response, void *contacts, void *pairs)
{
    void *entity = sp.active && response ? AT(response, 0x18, void *) : 0;
    void *list = entity && sp.manager ? AT(sp.manager, 0x40, void *) : 0;
    if (entity && entity == sp.kart) return 0;
    for (int i = 0; i < count(list); i++)
        if (ghost_kart(item(list, i)) == entity && sp.time <= ghost_end(item(list, i))) return 0;     /* finished: it collides */
    return game_collision_original(response, contacts, pairs);
}

/* Called in place of the game's "lap changed" for the racer HUD (0xe61045) and "racer finished"
 * (0xf350b8): in a spectator run the HUD shows the ghost's laps and the player never finishes */
void sp_player_lap(void *hud, int lap)
{
    if (sp.active && hud == sp.hud) return;
    game_hud_set_lap(hud, lap);
}

void sp_player_finished(void *system, void *racer)
{
    if (sp.active) return;
    game_racer_finished(system, racer);
}

/* The game's speed for a stopped playback: not 0, as quitting from a level whose last frame had no
 * game time at all left the main thread waiting for the animation thread forever (0x99c63f) on the
 * loading screen. The game clock still stands (its ticks are whole: 8192 per second x frame time x
 * speed rounds down to 0). */
#define STOPPED 1.0e-6f

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
    /* The countdown runs in real time (frame times +0x18): faster than x1 the ghost would reach its GO
     * before the game lets the karts go, and its first slides and boosts would be lost; slower, it
     * would still wait when the lights are green. So the playback is x1 until the game's GO (a speed
     * chosen before applies then), but fast forward: then the real time of this frame is made as long
     * for the game too, so the lights and the countdown go fast with the ghost. */
    int started = race_started();
    int fast_start = !started && sp.scrub > 0 && !paused;
    if (rate > 0.0f && !started && !fast_start) rate = 1.0f;
    if (fast_start && g_frame) AT(g_frame, 0x18, float) *= SCRUB_SPEED;
    float time = sp.time + dt * rate;
    if (sp.skip_to > 0.0f && time >= sp.skip_to) {
        time = sp.skip_to;
        sp.skip_to = 0.0f;
    }
    sp.end = end_time(manager);
    if (time > sp.end) time = sp.end;   /* the run stops at the end, and can be rewound */
    /* Rewinding stops at the race start (GO): before it the karts wait at the line */
    float start = sp.go_watch == sp.watch ? sp.go : 0.0f;
    if (time < old && time < start) time = old < start ? old : start;
    if (time < 0.0f) time = 0.0f;
    if (time < old) rewind(manager, time);
    sp.time = time;
    /* The game's own speed (frame times +0x2c _gameSpeed) follows the playback, so animations,
     * effects and the world stop and change speed with the karts: STOPPED while paused or at the end
     * of the run (a ghost that finished before drives on), the playback speed while playing, x1 while
     * rewinding */
    int stopped = sp.paused || time >= sp.end;
    if (g_frame && !paused) {
        float speed = sp.scrub < 0 ? 1.0f : sp.scrub > 0 ? SCRUB_SPEED : stopped ? STOPPED : SPEEDS[sp.speed];
        AT(g_frame, 0x2c, float) = speed > STOPPED && !started && !fast_start ? 1.0f : speed;
        sp.game_speed_set = 1;
    }
    follow(manager);
    ghost_look(manager);
    hide_player();
    carry_player(manager);
    hud_frame(manager);
    respawns(manager, old, time);
    crates(manager, old, time);
    boosts_frame(manager, old, time);
    sounds_frame(manager, old, time);
    return time;
}

