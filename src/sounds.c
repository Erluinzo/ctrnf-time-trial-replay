/* Kart sounds for the watched ghost. Ghost files keep only the kart's movement and its inputs, so
 * the events are found in them: hop, drift start, drift boosts (an L1 tap in the drift meter's
 * window), brakes, wall hits (a sudden stop), boosts without a tap (a sudden speed gain: pads, the
 * start boost) and big jumps (long air time). Each event plays the game's own sound by name with
 * the player's version ("_PLR") of it; the sound data then applies the game's own play chance,
 * cooldown and variants (notes/kart-events.md). Voice lines follow the game's voice script:
 * boost lines only when no other line plays, the hit line 0.2 s after the hit, big air 0.3 s after
 * the jump.
 * Sounds play only while the clock runs forward at a chosen speed (not while paused, rewinding or
 * fast forwarding with the D-pad). */
#include "spectate.h"

extern void game_play_sound(void *entity, const char *name, void **instance, void *callback) GAME;
extern void *game_get_driver(void *kart) GAME;
extern void *game_sound_system(void) GAME;
extern void game_find_sound(void **handle, void *system, void *entity, char **name) GAME;
extern void game_play_at(void **instance, void *entity, const float *position, void *sound, void *callback) GAME;
extern char *game_string(void *pool, const char *text) GAME;
extern void game_string_release(void *pool, char *header) GAME;
extern void *g_string_pool GAME;

#define IN_ACCELERATE 0x100
#define IN_BRAKE 0x400
#define IN_FIRST_JUMP 0x800             /* hop and drift (held while drifting) */
#define IN_SECOND_JUMP 0x1000           /* the other shoulder: a boost tap while drifting */
#define IN_START_DRIFT 0x2000           /* set when the hop lands into a drift */
/* Thresholds measured on recorded ghosts (Crash Cove: player ghost and N. Tropy) */
#define TAP_EARLY 0.5f                  /* a boost tap works this long after the drift start or the last boost... */
#define TAP_LATE 1.05f                  /* ...until this long (experts tap at 0.75-0.9 s) */
#define HOP_LAND 0.36f                  /* a hop lands this long after the press on flat ground (or at the drift start) */
#define SPEED_WINDOW 0.3f               /* s: speeds are averaged over this */
#define SPEED_GAIN 300.0f               /* units/s gained in 0.3 s without a tap: a boost */
#define SPEED_LOSS -700.0f              /* lost in 0.3 s without braking: a wall hit */
#define HARD_LOSS -1200.0f
#define FAST_BRAKE 1200.0f              /* the game's brake sound rule: speed ratio >= 0.4 */
#define TAKEOFF_SPEED 600.0f            /* units/s up, not from a hop: leaving a ramp */
#define BIG_AIR 0.7f                    /* s in the air for the jump sounds and the big air line */
#define GRID 0.05f                      /* movement is checked every 0.05 s of ghost time */
#define VO_BUSY 2.0f                    /* a voice line counts as playing for this long */

enum { P_NONE, P_HOP_LAND, P_LAND, P_VO_HIT, P_VO_AIR };

static void *bank_sound(const char *name);
static void position(void *ghost, float t, float *out);

/* Voice lines by name on the ghost's driver (its character bank). Kart sounds come from the vehicle
 * banks, which the game's name search does not look in: they are found in the bank and played on
 * the ghost kart at its place, as the play by name call (0x1024340) does. */
static void play(void *ghost, const char *name, int voice)
{
    void *kart = ghost_kart(ghost);
    void *entity = kart && voice ? game_get_driver(kart) : kart;
    if (!entity) return;
    void *instance = 0, *callback[4] = {0};   /* "finished" delegate: two handles (0x263210), none here */
    void *sound = voice ? 0 : bank_sound(name);
    if (sound) {
        float at[3];
        position(ghost, sp.time, at);
        game_play_at(&instance, kart, at, sound, callback);
    } else {
        game_play_sound(entity, name, &instance, callback);
    }
    int i = sp.log_n++ % 24;
    sp.log[i].name = name;
    sp.log[i].at = sp.time;
    sp.log[i].ok = instance != 0;
    if (instance) {
        sp.sounds_played++;
        game_release(instance);
    } else {
        sp.sounds_failed++;
    }
}

static void later(int what, float at)
{
    for (int i = 0; i < 4; i++)
        if (sp.pending[i].what == P_NONE) {
            sp.pending[i].what = what;
            sp.pending[i].at = at;
            return;
        }
}

/* Returns 1 if `what` was waiting */
static int cancel(int what)
{
    int found = 0;
    for (int i = 0; i < 4; i++)
        if (sp.pending[i].what == what) {
            sp.pending[i].what = P_NONE;
            found = 1;
        }
    return found;
}

/* Kart position at ghost time t (lerp of the move keys, as the game places ghosts) */
static void position(void *ghost, float t, float *out)
{
    void *keys = AT(ghost, 0x30, void *);
    int n = count(keys), lo = 0, hi = n - 1;
    if (n < 2) {
        out[0] = out[1] = out[2] = 0.0f;
        return;
    }
    while (hi - lo > 1) {
        int mid = (lo + hi) / 2;
        if (key_time(keys, mid) <= t) lo = mid; else hi = mid;
    }
    float t0 = key_time(keys, lo), t1 = key_time(keys, hi);
    float f = t1 > t0 ? (t - t0) / (t1 - t0) : 0.0f;
    if (f < 0.0f) f = 0.0f;
    if (f > 1.0f) f = 1.0f;
    const float *a = &AT(item(keys, lo), 0xc, float), *b = &AT(item(keys, hi), 0xc, float);
    for (int i = 0; i < 3; i++) out[i] = a[i] + (b[i] - a[i]) * f;
}

static float sqrt_approx(float x)
{
    if (x <= 0.0f) return 0.0f;
    float r = x > 1.0f ? x : 1.0f;
    for (int i = 0; i < 20; i++) r = 0.5f * (r + x / r);
    return r;
}

/* Ground speed around ghost time t, units/s */
static float speed(void *ghost, float t)
{
    float a[3], b[3];
    position(ghost, t - SPEED_WINDOW / 2, a);
    position(ghost, t + SPEED_WINDOW / 2, b);
    float dx = b[0] - a[0], dy = b[1] - a[1];
    return sqrt_approx(dx * dx + dy * dy) / SPEED_WINDOW;
}

/* Vertical speed around ghost time t (z is up) */
static float climb(void *ghost, float t)
{
    float a[3], b[3];
    position(ghost, t - 0.05f, a);
    position(ghost, t + 0.05f, b);
    return (b[2] - a[2]) / 0.1f;
}

/* Input keys (CGhostSnapshotKey: +0xc bits, +0xe time in 1/100 s) */
static float input_time(void *keys, int i)
{
    return AT(item(keys, i), 0xe, uint16_t) / 100.0f;
}

static int input_bits(void *keys, int i)
{
    return i >= 0 ? AT(item(keys, i), 0xc, uint16_t) : 0;
}

/* First input key later than t */
static int first_after(void *keys, float t)
{
    int lo = 0, hi = count(keys);
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (input_time(keys, mid) <= t) lo = mid + 1; else hi = mid;
    }
    return lo;
}

static void input_events(void *ghost, float from, float to)
{
    void *keys = AT(ghost, 0x38, void *);
    for (int i = first_after(keys, from); i < count(keys) && input_time(keys, i) <= to; i++) {
        float t = input_time(keys, i);
        int bits = input_bits(keys, i), before = input_bits(keys, i - 1);
        int rise = bits & ~before, fall = before & ~bits;
        if ((rise & IN_FIRST_JUMP) && t >= sp.air_until) {     /* no hop in the air */
            play(ghost, "COM_VEH_Hop_Jump_PLR_set", 0);
            sp.last_hop = t;
            cancel(P_HOP_LAND);
            later(P_HOP_LAND, t + HOP_LAND);
        }
        if (rise & IN_START_DRIFT) {
            if (cancel(P_HOP_LAND)) play(ghost, "COM_VEH_Hop_Land_PLR_set", 0);  /* landed before the usual time */
            play(ghost, "COM_VEH_Skid_drift_PLR_set", 0);
            sp.drift_start = sp.last_boost = t;
        }
        if (fall & IN_FIRST_JUMP) sp.drift_start = -1.0f;
        if (rise & IN_SECOND_JUMP) {
            sp.last_tap = t;
            float since = t - sp.last_boost;
            if (sp.drift_start >= 0.0f && (bits & IN_FIRST_JUMP) && since >= TAP_EARLY && since <= TAP_LATE) {
                sp.last_boost = t;
                play(ghost, "COM_VEH_Drift_flame_burst_PLR_set", 0);
                play(ghost, "COM_VEH_Boost_Sweetener_PLR", 0);
                if (t >= sp.vo_until) {         /* a drift boost is well over the voice rule's speed 200 */
                    play(ghost, "VO2_PLR_Boosting", 1);
                    sp.vo_until = t + VO_BUSY;
                }
            }
        }
        if (rise & IN_BRAKE)
            play(ghost, speed(ghost, t) >= FAST_BRAKE ? "COM_VEH_Brake_Fast_PLR_set" : "COM_VEH_Brake_Slow_PLR_set", 0);
    }
}

/* When a ramp jump at `takeoff` lands (vertical speed back near 0 after falling), or -1 */
static float landing(void *ghost, float takeoff)
{
    for (float t = takeoff + 0.1f; t < takeoff + 3.0f; t += 0.02f)
        if (climb(ghost, t) > -100.0f && climb(ghost, t - 0.1f) < -500.0f) return t;
    return -1.0f;
}

static void movement_events(void *ghost, float from, float to)
{
    float g = (float)((int)(from / GRID) + 1) * GRID;
    for (; g <= to; g += GRID) {
        if (g < sp.go) continue;
        if (g - sp.last_move_event >= 0.5f) {
            float change = speed(ghost, g + 0.15f) - speed(ghost, g - 0.15f);
            if (change > SPEED_GAIN && g - sp.last_tap > 0.6f && g > sp.air_until) {
                sp.last_move_event = g;
                play(ghost, g - sp.go < 1.5f ? "PU_TurboBoost_In_PLR" : "GPI_BoostPad_PLR", 0);
                play(ghost, "COM_VEH_Boost_Sweetener_PLR", 0);
                if (g >= sp.vo_until) {
                    play(ghost, "VO2_PLR_Boosting", 1);
                    sp.vo_until = g + VO_BUSY;
                }
            } else if (change < SPEED_LOSS && g > sp.air_until) {
                int braking = 0;
                void *keys = AT(ghost, 0x38, void *);
                int i = first_after(keys, g) - 1;
                braking = input_bits(keys, i) & IN_BRAKE;
                if (!braking) {
                    sp.last_move_event = g;
                    play(ghost, change < HARD_LOSS ? "COM_VEH_Impact_Hard_PLR_set" : "COM_VEH_Impact_Med_PLR_set", 0);
                    later(P_VO_HIT, g + 0.2f);
                }
            }
        }
        if (g > sp.air_until && g - sp.last_hop > 0.6f && climb(ghost, g) > TAKEOFF_SPEED) {
            float land = landing(ghost, g);
            if (land > 0.0f && land - g >= BIG_AIR) {
                play(ghost, "COM_VEH_jump_PLR_set", 0);
                later(P_VO_AIR, g + 0.3f);
                later(P_LAND, land);
            }
            sp.air_until = land > 0.0f ? land : g + 0.5f;
        }
    }
}

static void pending_events(void *ghost, float to)
{
    for (int i = 0; i < 4; i++) {
        int what = sp.pending[i].what;
        if (what == P_NONE || sp.pending[i].at > to) continue;
        sp.pending[i].what = P_NONE;
        if (what == P_HOP_LAND) play(ghost, "COM_VEH_Hop_Land_PLR_set", 0);
        if (what == P_LAND) play(ghost, "COM_VEH_land_PLR_set", 0);
        if (what == P_VO_HIT) {
            play(ghost, "VO1_PLR_BeenHit_Light", 1);
            sp.vo_until = sp.pending[i].at + VO_BUSY;
        }
        if (what == P_VO_AIR) play(ghost, "VO2_PLR_BigAir", 1);
    }
}

/* Start again from ghost time t without sounds (new run, other ghost, a jump in time) */
static void restart(float t)
{
    sp.heard = t;
    sp.heard_watch = sp.watch;
    sp.drift_start = -1.0f;
    sp.last_hop = sp.last_tap = sp.last_boost = sp.last_move_event = sp.air_until = -10.0f;
    sp.vo_until = 0.0f;
    for (int i = 0; i < 4; i++) sp.pending[i].what = P_NONE;
}

/* Same text, ignoring case (bank sound names are lower case) */
static int same_name(const char *a, const char *b)
{
    if (!a) return 0;
    for (;; a++, b++) {
        char x = *a >= 'A' && *a <= 'Z' ? (char)(*a + 32) : *a, y = *b >= 'A' && *b <= 'Z' ? (char)(*b + 32) : *b;
        if (x != y) return 0;
        if (!x) return 1;
    }
}

/* A kart sound (CSound) by name, from the vehicle banks of the sound system's bank list (+0xd8). The
 * game's own name search does not look there, and its bank search (0x283c10) fails on these banks, so
 * the list is read directly: CSoundBank +0x10 name, +0x18 sounds; CSound +0x30 name. */
static void *bank_sound(const char *name)
{
    void *list = AT(game_sound_system(), 0xd8, void *);
    for (int i = 0; i < count(list); i++) {
        void *bank = item(list, i);
        const char *bank_name = bank ? AT(bank, 0x10, const char *) : 0;
        if (!same_name(bank_name, "veh_common_bank") && !same_name(bank_name, "veh_common_powerups_bank")) continue;
        void *sounds = AT(bank, 0x18, void *);
        for (int k = 0; k < count(sounds); k++)
            if (item(sounds, k) && same_name(AT(item(sounds, k), 0x30, const char *), name)) return item(sounds, k);
    }
    return 0;
}

static void sound_test(void *ghost)
{
    if (!sp.test_go) return;
    sp.test_go = 0;
    void *kart = sp.test_entity >= 2 ? sp.kart : ghost_kart(ghost);
    void *entity = kart && sp.test_entity % 2 ? game_get_driver(kart) : kart;
    sp.test_used = entity;
    sp.test_result = 0;
    if (!entity) return;
    void *instance = 0, *callback[4] = {0};   /* "finished" delegate: two handles (0x263210), none here */
    if (sp.test_mode == 0) {
        game_play_sound(entity, sp.test_name, &instance, callback);
        sp.test_result = instance;
        if (instance) game_release(instance);
        return;
    }
    if (sp.test_mode >= 3) {        /* our own search in the kart sound banks, play at the ghost */
        void *sound = bank_sound(sp.test_name);
        sp.test_used = sound;
        if (sound) {
            float at[3];
            position(ghost, sp.time, at);
            /* 3: no entity, 4: ghost kart, 5: player kart, 6: ghost kart with position 0 (attached) */
            void *with = sp.test_mode == 4 || sp.test_mode == 6 ? ghost_kart(ghost) : sp.test_mode == 5 ? sp.kart : 0;
            if (sp.test_mode == 6) at[0] = at[1] = at[2] = 0.0f;
            game_play_at(&instance, with, at, sound, callback);
            sp.test_result = instance;
            if (instance) game_release(instance);
        }
        return;
    }
    /* As 0x1024340 does, step by step */
    void *handle = 0;
    char *name = game_string(g_string_pool, sp.test_name);
    game_find_sound(&handle, game_sound_system(), entity, &name);
    if (name) game_string_release(g_string_pool, name - 0x10);
    sp.test_result = handle;
    if (!handle || sp.test_mode == 1) {
        game_make_handle(&handle, 0);
        return;
    }
    int32_t flags = *(int32_t *)handle;
    sp.test_used = (void *)(intptr_t)flags;
    void *sound = (flags & 0x10000000) ? 0 : AT(handle, 0x28, void *);
    if (sound) {
        float at[3];
        position(ghost, sp.time, at);
        game_play_at(&instance, 0, at, sound, callback);
        sp.test_result = instance;
        if (instance) game_release(instance);
    }
    game_make_handle(&handle, 0);
}

/* Plays the sounds of the watched ghost like the player's own kart.
 * The game already plays every kart sound and voice line of a ghost itself, but with the "_NPC"
 * version, which Time Trial keeps silent. sp_play (the start of the game's play call 0x28a6b0) starts
 * the "_PLR" twin instead when the sound is for the watched ghost: so the events, rules, cooldowns
 * and variants are the game's own. The engine and the drift and boost loops come from the kart's
 * engine graph (CVehicleAudioComponent), which plays the "_PLR" set when its _isLocalPlayer is set. */

extern void *game_play_original(void **out, void *system, void **sound, void *entity, const float *position, float volume) GAME;

/* The "_PLR" sound of the same bank for an "_NPC" one ("VO2_NPC_Boosting" -> "VO2_PLR_Boosting",
 * "COM_VEH_Hop_Jump_NPC_set" -> "COM_VEH_Hop_Jump_PLR_set"), or 0 */
static void *plr_twin(void *sound)
{
    const char *name = AT(sound, 0x30, const char *);
    char twin[96];
    int at = -1, n = 0;
    for (; name && name[n] && n < 95; n++) {
        twin[n] = name[n];
        if (at < 0 && n >= 1 && name[n - 1] == '_' && (name[n] | 32) == 'n' && (name[n + 1] | 32) == 'p'
            && (name[n + 2] | 32) == 'c' && (name[n + 3] == '_' || !name[n + 3]))
            at = n;
    }
    if (at < 0 || n >= 95) return 0;
    twin[n] = 0;
    int upper = name[at] == 'N';
    twin[at] = upper ? 'P' : 'p';
    twin[at + 1] = upper ? 'L' : 'l';
    twin[at + 2] = upper ? 'R' : 'r';
    void *sounds = AT(AT(sound, 0xd8, void *), 0x18, void *);     /* CSound +0xd8: its bank */
    for (int i = 0; i < count(sounds); i++)
        if (item(sounds, i) && same_name(AT(item(sounds, i), 0x30, const char *), twin)) return item(sounds, i);
    return 0;
}

void *sp_play(void **out, void *system, void **sound, void *entity, const float *position, float volume)
{
    /* only in a spectator run while ghosts play (the manager lives for the whole game; +0x14 _isPlaying) */
    int on = sp.active && sp.manager && sp.manager == g_ghost_manager && AT(sp.manager, 0x14, uint8_t);
    void *list = on && entity && !sp.paused && !sp.scrub ? AT(sp.manager, 0x40, void *) : 0;
    void *kart = sp.watch < count(list) ? ghost_kart(item(list, sp.watch)) : 0;
    void *npc = kart && (entity == kart || entity == game_get_driver(kart)) ? handle_object(sound) : 0;
    void *plr = npc ? plr_twin(npc) : 0;
    if (!plr) return game_play_original(out, system, sound, entity, position, volume);
    void *slot = 0;
    game_make_handle(&slot, plr);
    void *result = game_play_original(out, system, &slot, entity, position, volume);
    game_make_handle(&slot, 0);
    sp.sounds_played++;
    return result;
}

/* Engine graphs: the watched ghost's plays the "_PLR" engine at full volume while the clock runs
 * forward; the other ghosts' are silent (only the watched kart is heard). */
static void engines(void *list)
{
    int forward = !sp.paused && !sp.scrub;
    for (int i = 0; i < count(list); i++) {
        void *kart = ghost_kart(item(list, i));
        void *audio = kart ? game_find_component(kart, g_vehicle_audio_meta, 0) : 0;
        if (!audio) continue;
        AT(audio, 0x79, uint8_t) = 1;                                      /* _isLocalPlayer */
        AT(audio, 0x7c, float) = i == sp.watch && forward ? 1.0f : 0.0f;  /* wanted volume */
        if (i == sp.watch) sp.audio = audio;
    }
}

#define GUESS_EVENTS 0      /* TEST: own event detection, off while the game's own sounds are used */

/* Every frame of a spectator run, after the clock moved from `from` to `to` */
void sounds_frame(void *manager, float from, float to)
{
    void *list = AT(manager, 0x40, void *);
    void *ghost = sp.watch < count(list) ? item(list, sp.watch) : 0;
    if (!ghost || !ghost_kart(ghost)) return;
    sound_test(ghost);
    engines(list);
    sp.player_audio = sp.kart ? game_find_component(sp.kart, g_vehicle_audio_meta, 0) : 0;
    if (!GUESS_EVENTS) return;
    int forward = to >= from && !sp.scrub && to - from < 0.5f && sp.heard_watch == sp.watch && sp.heard == from;
    if (!forward) {
        restart(to);
        return;
    }
    if (to == from) return;
    input_events(ghost, from, to);
    movement_events(ghost, from, to);
    pending_events(ghost, to);
    sp.heard = to;
}
