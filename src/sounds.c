/* Sounds of the watched ghost, as the player's own kart would make them.
 *
 * The game already plays every kart sound and voice line of a ghost itself (hops, drifts, boosts,
 * jumps, surfaces, engine...), but with the "_NPC" version, which Time Trial keeps silent.
 * - sp_play, at the start of the game's lowest play call (0x28a6b0), starts the "_PLR" twin instead
 *   when the sound is for the watched ghost's kart or driver. So the events, rules, cooldowns and
 *   variants are the game's own.
 * - The engine graph (CVehicleAudioComponent) plays its "_PLR" set when _isLocalPlayer is set: on for
 *   every ghost, at full volume for the watched one and silent for the others.
 * - Ghosts have no collisions, so the game has no wall hits for them: those are found in the ghost's
 *   movement (a sudden speed loss without braking) and get the impact sound and, 0.2 s later, the
 *   "Ouch" line, as the game's voice script does (notes/kart-events.md).
 * Nothing plays while paused or while the D-pad rewinds or fast forwards. */
#include "spectate.h"

extern void game_play_sound(void *entity, const char *name, void **instance, void *callback) GAME;
extern void *game_get_driver(void *kart) GAME;
extern void *game_sound_system(void) GAME;
extern void game_play_at(void **instance, void *entity, const float *position, void *sound, void *callback) GAME;
extern void *game_play_original(void **out, void *system, void **sound, void *entity, const float *position, float volume) GAME;

#define IN_BRAKE 0x400
#define WINDOW 0.3f             /* s: speeds are averaged over this */
#define WALL_LOSS -700.0f       /* units/s lost within 0.3 s without braking: a wall hit */
#define HARD_LOSS -1200.0f
#define TELEPORT 6000.0f        /* faster than any kart: a respawn, not driving */
#define GRID 0.05f              /* movement is checked every 0.05 s of ghost time */
#define VOICE_DELAY 0.2f        /* the voice script plays the hit line 0.2 s after the hit */

static int forward(void)
{
    return !sp.paused && !sp.scrub;
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

/* Ground speed around ghost time t, units/s (z is up) */
static float speed(void *ghost, float t)
{
    float a[3], b[3];
    position(ghost, t - WINDOW / 2, a);
    position(ghost, t + WINDOW / 2, b);
    float x = (b[0] - a[0]) * (b[0] - a[0]) + (b[1] - a[1]) * (b[1] - a[1]);
    float r = x > 1.0f ? x : 1.0f;
    for (int i = 0; i < 20; i++) r = 0.5f * (r + x / r);    /* square root */
    return r / WINDOW;
}

/* The brake input at ghost time t (CGhostSnapshotKey: +0xc bits, +0xe time in 1/100 s) */
static int braking(void *ghost, float t)
{
    void *keys = AT(ghost, 0x38, void *);
    int lo = 0, hi = count(keys);
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (AT(item(keys, mid), 0xe, uint16_t) / 100.0f <= t) lo = mid + 1; else hi = mid;
    }
    return lo > 0 && (AT(item(keys, lo - 1), 0xc, uint16_t) & IN_BRAKE);
}

/* A kart sound (CSound) by name from the vehicle bank in the sound system's bank list (+0xd8): the
 * game's name search does not look there. CSoundBank +0x10 name, +0x18 sounds; CSound +0x30 name. */
static void *bank_sound(const char *name)
{
    void *list = AT(game_sound_system(), 0xd8, void *);
    for (int i = 0; i < count(list); i++) {
        void *bank = item(list, i);
        if (!bank || !same_name(AT(bank, 0x10, const char *), "veh_common_bank")) continue;
        void *sounds = AT(bank, 0x18, void *);
        for (int k = 0; k < count(sounds); k++)
            if (item(sounds, k) && same_name(AT(item(sounds, k), 0x30, const char *), name)) return item(sounds, k);
    }
    return 0;
}

/* Wall-hit sounds: kart sounds on the ghost kart at its place (as the play by name call 0x1024340
 * does), voice lines by name on its driver (their character's bank) */
static void play(void *ghost, const char *name, int voice)
{
    void *kart = ghost_kart(ghost);
    void *instance = 0, *callback[4] = {0};   /* "finished" delegate: two handles (0x263210), none here */
    if (voice) {
        void *driver = kart ? game_get_driver(kart) : 0;
        if (driver) game_play_sound(driver, name, &instance, callback);
    } else {
        void *sound = kart ? bank_sound(name) : 0;
        float at[3];
        position(ghost, sp.time, at);
        if (sound) game_play_at(&instance, kart, at, sound, callback);
    }
    int i = sp.log_n++ % 16;
    sp.log[i].name = name;
    sp.log[i].at = sp.time;
    sp.log[i].ok = instance != 0;     /* delayed voice lines give none, but play */
    sp.sounds_played++;
    if (instance) game_release(instance);
}

static void wall_hits(void *ghost, float from, float to)
{
    for (float g = (float)((int)(from / GRID) + 1) * GRID; g <= to; g += GRID) {
        if (g < sp.go + 1.0f || g - sp.last_hit < 0.5f) continue;
        float before = speed(ghost, g - 0.15f), after = speed(ghost, g + 0.15f);
        if (after - before >= WALL_LOSS || before > TELEPORT || after > TELEPORT || braking(ghost, g)) continue;
        sp.last_hit = g;
        play(ghost, after - before < HARD_LOSS ? "COM_VEH_Impact_Hard_PLR_set" : "COM_VEH_Impact_Med_PLR_set", 0);
        sp.voice_at = g + VOICE_DELAY;
    }
    if (sp.voice_at >= 0.0f && sp.voice_at <= to) {
        sp.voice_at = -1.0f;
        play(ghost, "VO1_PLR_BeenHit_Light", 1);
    }
}

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
    void *list = on && entity && forward() ? AT(sp.manager, 0x40, void *) : 0;
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

/* Every frame of a spectator run, after the clock moved from `from` to `to` */
void sounds_frame(void *manager, float from, float to)
{
    void *list = AT(manager, 0x40, void *);
    for (int i = 0; i < count(list); i++) {
        void *kart = ghost_kart(item(list, i));
        void *audio = kart ? game_find_component(kart, g_vehicle_audio_meta, 0) : 0;
        if (!audio) continue;
        AT(audio, 0x79, uint8_t) = 1;                                        /* _isLocalPlayer */
        AT(audio, 0x7c, float) = i == sp.watch && forward() ? 1.0f : 0.0f;  /* wanted volume */
    }
    void *ghost = sp.watch < count(list) ? item(list, sp.watch) : 0;
    if (!ghost || !ghost_kart(ghost)) return;
    /* A jump in time, another ghost, or the D-pad: start again from here without sounds */
    if (!forward() || to < from || to - from > 0.5f || sp.heard_watch != sp.watch || sp.heard != from) {
        sp.heard_watch = sp.watch;
        sp.last_hit = -10.0f;
        sp.voice_at = -1.0f;
    } else {
        wall_hits(ghost, from, to);
    }
    sp.heard = to;
}
