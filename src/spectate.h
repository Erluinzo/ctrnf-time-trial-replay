/* Shared by spectate.c and hud.c: game symbols (set by build.py), the state and small helpers. */
#include <stdint.h>

#define GAME __attribute__((visibility("hidden")))

/* Game code */
extern void game_handle_resolve(void **slot) GAME;
extern void game_make_handle(void *slot, void *object) GAME;            /* object 0 clears the handle */
extern void game_release(void *object) GAME;
extern void *game_find_component(void *entity, void *meta, int zero) GAME;
extern void game_ghost_reset(void *manager, void *ghost) GAME;           /* kart to key 0, indexes 0 */
extern void game_ghost_move(void *manager, void *ghost) GAME;            /* move keys at the play time */
extern void game_ghost_inputs(void *manager, void *ghost) GAME;          /* input keys at the play time */
extern void game_set_entity_position(void *entity, const float *position) GAME;
extern void game_set_entity_angles(void *entity, const float *angles, int zero) GAME;
extern void game_start_recording(void *manager, void *racer) GAME;
extern void *game_player_kart(void *kart_manager, int player, int one) GAME;
extern void *game_camera_instance(void) GAME;                            /* first local player's */
extern void *game_active_camera(void *boxes) GAME;
extern void game_camera_refresh(void *camera) GAME;
extern void game_lock_request(void **request, void *lock) GAME;
extern void game_legend_add(void *project, int button, char **text, int always) GAME;
extern char *game_string(void *pool, const char *text) GAME;             /* new reference */
extern void game_string_release(void *pool, char *header) GAME;
extern void game_menu_prepare(void *settings, int zero) GAME;
extern void *game_fill_ghosts_original(void *level, void **list) GAME;   /* hooks.s */
extern void *game_input_original(void *input_manager) GAME;             /* hooks.s */
extern uint64_t game_pad_original(void *device, void *a, void *b, void *c) GAME;    /* hooks.s */
extern void *game_continue_event_new(void *pool) GAME;
extern void *game_ghost_look_end_new(void *pool) GAME;      /* CKartEndTemporaryGhostMessage */
extern void *game_ghost_look_start_new(void *pool) GAME;    /* CKartStartTemporaryGhostMessage */
extern void game_send_message(void *message, void *target, void *sender) GAME;
extern void game_set_entity_hidden(void *entity, int hidden) GAME;   /* drawing only */
extern void *game_get_driver(void *kart) GAME;
/* Game data */
extern char *g_ghost_manager GAME;
extern char *g_frame GAME;               /* frame times: +0x18 real, +0x24 game, +0x28 paused */
extern char *g_input_manager GAME;
extern char *g_players GAME;
extern void *g_kart_manager GAME;
extern void *g_kart_camera_meta GAME;
extern void *g_hud_meta GAME;            /* CRacerHudComponent */
extern void *g_vehicle_audio_meta GAME;  /* CVehicleAudioComponent */
extern void *g_child_manager_meta GAME;  /* CChildManagerComponent */
extern void *g_race_settings GAME;
extern void *g_string_pool GAME;
extern char *g_gui_manager GAME;
extern char *g_game GAME;                /* +0xd4c game mode, 2 = Time Trial */

#define LEGEND_L1 5             /* EButtonLegendButton */
#define SCRUB_SPEED 4.0f        /* D-pad left/right: rewind and fast forward at x4 */
static const float SPEEDS[] __attribute__((unused)) = {0.1f, 0.5f, 1.0f, 2.0f, 3.0f, 4.0f};
#define SPEED_COUNT (int)(sizeof SPEEDS / sizeof SPEEDS[0])
#define NORMAL_SPEED 2

/* Spectator buttons: device signals (CController._buttonMap values) and the default bits */
enum { B_L1, B_R1, B_SQUARE, B_TRIANGLE, B_UP, B_DOWN, B_LEFT, B_RIGHT, B_CIRCLE, B_COUNT };
static const uint8_t SIGNALS[B_COUNT] __attribute__((unused)) = {8, 10, 5, 6, 0, 1, 2, 3, 4};
static const uint8_t DEFAULT_BITS[B_COUNT] __attribute__((unused)) = {11, 13, 8, 6, 14, 15, 17, 16, 5};

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
    /* HUD (hud.c) */
    int show_pad;           /* the virtual controller is shown (Triangle) */
    float go;               /* the watched ghost's race start (its raceStart input), s */
    int go_watch;           /* the ghost go belongs to, -1 = none yet */
    void *root;             /* HUD root our pieces were added to (only compared) */
    void *time_text, *status_text, *steer_bar, *steer_fill;
    float steer_center;     /* x of the steering bar's middle, in the fill's parent */
    /* Sounds (sounds.c), in ghost time */
    float heard;            /* wall hits up to this time were checked */
    int heard_watch;        /* for this ghost; -1 = none */
    float last_hit;         /* the last wall hit */
    float voice_at;         /* its "Ouch" line plays at this time; -1 = none */
    int sounds_played;      /* sounds changed to the player's version or played for wall hits */
    /* Last wall-hit sounds (tools/sounds.py): name, ghost time, instance or 0 */
    int log_n;
    struct { const char *name; float at; int ok; } log[16];
    void *pad[11];          /* virtual controller buttons (hud.c) */
    void *unghosted;        /* the ghost kart shown without its ghost look */
    float status_extent;    /* the status text template's box width (hud.c) */
    /* The character screen after L1 (spectate.c): 1 = pressing Ready, 2 = its countdown runs,
     * 3 = cancelled: pressing Back to the ghost menu */
    int screen;
    int screen_frames;      /* frames in this step */
    int countdown_ticks;    /* countdown sounds heard (sounds.c) */
    uint64_t forced;        /* 1 << B_ button the patch pressed this frame (released on the next), 0 = none */
    int backs_heard;        /* "back" sounds heard in step 3 (sounds.c) */
    void *preview;          /* 1 = the kart on the character screen is hidden */
    void *placing;          /* the ghost the game places now (sp_ghost_move), 0 = none */
    float placing_time;     /* at this time */
    int game_speed_set;     /* the patch changed the game's speed (restored after the run) */
    int debug_focus;        /* DEBUG: ghost menu focus gains while sp.screen */
    int debug_n;            /* DEBUG: names of the last sounds started (tools) */
    const char *debug_names[32];
};
_Static_assert(sizeof(struct state) <= 0x500, "state must fit 0x2beea80-0x2beef80");
_Static_assert(__builtin_offsetof(struct state, heard) == 180 && __builtin_offsetof(struct state, sounds_played) == 196
               && __builtin_offsetof(struct state, log_n) == 200 && __builtin_offsetof(struct state, log) == 208,
               "tools/peek.py and tools/sounds.py read these offsets");
extern struct state sp GAME;

/* Field access on game objects */
#define AT(object, offset, type) (*(type *)((char *)(object) + (offset)))

/* The object a handle field points to, or 0 (as the game reads them, e.g. 0xe32076) */
static inline void *handle_object(void **slot)
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
static inline int count(void *list)
{
    return list ? AT(list, 0xc, int32_t) : 0;
}

static inline void *item(void *list, int i)
{
    return AT(list, 0x20, void **)[i];
}

/* CGhost: +0x10 _kart handle, +0x30 move keys, +0x38 input keys, +0x40/+0x44 play indexes */
static inline void *ghost_kart(void *ghost)
{
    return ghost ? handle_object(&AT(ghost, 0x10, void *)) : 0;
}

/* CGhostMoveKey: +0x18 time in 1/100 s */
static inline float key_time(void *keys, int i)
{
    return AT(item(keys, i), 0x18, uint16_t) / 100.0f;
}

/* hud.c */
void hud_frame(void *manager);
/* sounds.c */
void sounds_frame(void *manager, float from, float to);
/* spectate.c */
void sp_countdown_go(void);
