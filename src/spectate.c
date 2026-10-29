/* Ghost spectator: runs inside the game (USA CUSA13795 1.21), called from the hooks in hooks.s.
 *
 * - Ghost menu: L1 on an entry starts the race like Continue, with spectating switched on.
 * - Race start: nothing is recorded, so the run saves no ghost and unlocks nothing.
 * - Every frame (the ghost update): the player's kart is locked, the camera follows the watched
 *   ghost, and the patch sets the ghost clock itself: pause, rewind, fast forward and speeds.
 * Notes: ../notes/ (ghost-playback.md, camera-input.md).
 */
#include <stdint.h>

#define GAME __attribute__((visibility("hidden")))

/* Game code */
extern void game_handle_resolve(void **slot) GAME;
extern void game_make_handle(void *slot, void *object) GAME;            /* object 0 clears the handle */
extern void game_release(void *object) GAME;
extern void *game_find_component(void *entity, void *meta, int zero) GAME;
extern void game_ghost_reset(void *manager, void *ghost) GAME;           /* kart to key 0, indexes 0 */
extern void game_start_recording(void *manager, void *kart) GAME;
extern void *game_camera_instance(void) GAME;                            /* first local player's */
extern void *game_active_camera(void *boxes) GAME;
extern void game_camera_refresh(void *camera) GAME;
extern void game_lock_request(void **request, void *lock) GAME;
extern void game_legend_add(void *project, int button, char **text, int always) GAME;
extern char *game_string(void *pool, const char *text) GAME;             /* new reference */
extern void game_string_release(void *pool, char *header) GAME;
extern void game_menu_prepare(void *settings, int zero) GAME;
extern void *game_continue_event_new(void *pool) GAME;
/* Game data */
extern char *g_ghost_manager GAME;
extern char *g_frame GAME;               /* frame times: +0x18 real, +0x24 game, +0x28 paused */
extern char *g_input_manager GAME;
extern char *g_players GAME;
extern void *g_kart_camera_meta GAME;
extern void *g_hud_meta GAME;            /* CRacerHudComponent */
extern void *g_race_settings GAME;
extern void *g_string_pool GAME;
extern char *g_gui_manager GAME;

#define LEGEND_L1 5             /* EButtonLegendButton */
#define SCRUB_SPEED 4.0f        /* D-pad left/right: rewind and fast forward at x4 */
static const float SPEEDS[] = {0.1f, 0.5f, 1.0f, 2.0f, 3.0f, 4.0f};
#define SPEED_COUNT (int)(sizeof SPEEDS / sizeof SPEEDS[0])
#define NORMAL_SPEED 2

/* Spectator buttons: device signals (CController._buttonMap values) and the default bits */
enum { B_L1, B_R1, B_SQUARE, B_TRIANGLE, B_UP, B_DOWN, B_LEFT, B_RIGHT, B_COUNT };
static const uint8_t SIGNALS[B_COUNT] = {8, 10, 5, 6, 0, 1, 2, 3};
static const uint8_t DEFAULT_BITS[B_COUNT] = {11, 13, 8, 6, 14, 15, 17, 16};

struct state {
    int armed;              /* the last start from the ghost menu was L1 */
    int active;             /* this race is a spectator run */
    int set_up;             /* the player's kart is locked */
    int paused;
    int speed;              /* index in SPEEDS */
    int scrub;              /* -1 rewind, 1 fast forward, 0 none (D-pad held) */
    float time;             /* ghost clock, s */
    float end;              /* the last key of the longest ghost */
    int watch;              /* index of the watched ghost in the playing list */
    int ghosts;             /* playing ghosts */
    uint64_t buttons;       /* last frame's button bits */
    void *manager, *kart, *lock, *hud;
    void *camera, *target;  /* the camera we pointed at target (only compared, never used) */
    uint8_t bit[B_COUNT];
    /* For tests (tools/peek.py) */
    int frames, menu_presses, camera_sets;
};
_Static_assert(sizeof(struct state) <= 0x500, "state must fit 0x2beea80-0x2beef80");
extern struct state sp GAME;

/* Field access on game objects */
#define AT(object, offset, type) (*(type *)((char *)(object) + (offset)))

/* The object a handle field points to, or 0 (as the game reads them, e.g. 0xe32076) */
static void *handle_object(void **slot)
{
    for (int depth = 0; depth < 8; depth++) {
        int32_t *h = *slot;
        if (!h) return 0;
        if (*h & 0x10000000) {
            game_handle_resolve(slot);
            h = *slot;
            if (!h) return 0;
        }
        void *object = AT(h, 0x28, void *);
        if (!(*h & 0x20000000) || !object) return object;
        slot = (void **)((char *)object + 0x10);    /* the handle points to another handle */
    }
    return 0;
}

/* igObjectList: +0xc count, +0x20 items */
static int count(void *list)
{
    return list ? AT(list, 0xc, int32_t) : 0;
}

static void *item(void *list, int i)
{
    return AT(list, 0x20, void **)[i];
}

/* CGhost: +0x10 _kart handle, +0x30 move keys (key +0x18: time in 1/100 s), +0x40/+0x44 play indexes */
static void *ghost_kart(void *ghost)
{
    return ghost ? handle_object(&AT(ghost, 0x10, void *)) : 0;
}

static float key_time(void *keys, int i)
{
    return AT(item(keys, i), 0x18, uint16_t) / 100.0f;
}

/* ---- Ghost menu ---- */

void sp_menu_focus(void *behavior, void *placeable, int gained)
{
    (void)behavior;
    if (gained) sp.armed = 0;   /* back in the menu: a later Continue is a normal race */
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
    game_menu_prepare(g_race_settings, 0);
    void *event = game_continue_event_new(0);
    if (!event) return;
    AT(event, 0x18, uint8_t) = 1;
    typedef void handler(void *project, void *event);
    AT(*(void **)project, 0xb0, handler *)(project, event);
    game_release(event);
}

/* ---- Race ---- */

/* Called in place of the game's "start recording the player's kart" at the race start (0xea6390). */
void sp_race_start(void *manager, void *kart)
{
    game_start_recording(manager, kart);
    sp.active = sp.armed && manager;
    if (!sp.active) return;
    AT(manager, 0x0c, uint8_t) = 0;     /* _isRecording: the race end saves nothing (0xfcefe0) */
    sp.manager = manager;
    sp.kart = kart;
    sp.set_up = 0;
    sp.paused = 0;
    sp.speed = NORMAL_SPEED;
    sp.scrub = 0;
    sp.time = 0.0f;
    sp.end = 0.0f;
    sp.watch = 0;
    sp.camera = sp.target = 0;
    sp.lock = sp.hud = 0;
}

/* The first local player's controller (as 0x123a050), or 0 */
static void *controller(void)
{
    void *list = g_players ? AT(g_players, 0x10, void *) : 0;
    if (!g_input_manager) return 0;
    for (int i = 0; i < count(list); i++) {
        void *player = item(list, i);
        if (!player || AT(player, 0x20, int32_t) > 7) continue;
        int id = AT(player, 0xb0, int32_t);
        if (id < 0 || id > 7) return 0;
        int index = AT(g_input_manager, 0x38 + 4 * id, int32_t);
        void *controllers = AT(g_input_manager, 0x28, void *);
        return index >= 0 && index < count(controllers) ? item(controllers, index) : 0;
    }
    return 0;
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

/* Lock the player's kart (as the race end does, 0xe7eeb0) and the boost bar style button. */
static void set_up(void *manager)
{
    sp.set_up = 1;
    void *kart = sp.kart;
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
    if ((*(meta_fn **)camera)(camera) != g_kart_camera_meta) return;   /* intro and countdown cameras */
    if (camera == sp.camera && target == sp.target) return;
    game_make_handle((char *)camera + 0x278, target);
    game_camera_refresh(camera);
    AT(camera, 0x11d, uint8_t) = 1;     /* _resetOnNextUpdate: jump to the new kart, no swoop */
    sp.camera = camera;
    sp.target = target;
    sp.camera_sets++;
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
    return time;
}
