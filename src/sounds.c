/* Sounds of the watched ghost, as the player's own kart would make them.
 *
 * The game already plays every kart sound and voice line of a ghost itself (hops, drifts, boosts,
 * jumps, surfaces, engine...), but with the "_NPC" version, which Time Trial keeps silent.
 * - sp_play, at the start of the game's lowest play call (0x28a6b0), starts the "_PLR" twin instead
 *   when the sound is for the watched ghost's kart or driver. So the events, rules, cooldowns and
 *   variants are the game's own.
 * - The engine graph (CVehicleAudioComponent) plays its "_PLR" set when _isLocalPlayer is set: on for
 *   the watched ghost; the others fade out and go back to their silent "_NPC" set.
 * - Ghosts have no collisions, so the game has no wall hits for them: those are found in the ghost's
 *   movement with the game's own limits (wall_hit) and get the game's impact sound and, 0.2 s later,
 *   the "Ouch" line, as the game's voice script does (notes/sounds-progress.md).
 * Nothing plays while paused or while the D-pad rewinds or fast forwards. */
#include "spectate.h"

extern void game_play_sound(void *entity, const char *name, void **instance, void *callback) GAME;
extern void *game_sound_system(void) GAME;
extern void game_play_at(void **instance, void *entity, const float *position, void *sound, void *callback) GAME;
extern void *game_play_original(void **out, void *system, void **sound, void *entity, const float *position, float volume) GAME;

#define IN_FIRST_JUMP 0x800    /* held while drifting */
/* The game's wall hit (0xe91e45, CKartCollisionResponseComponent data): speed >= 1000 and the kart
 * going into the wall within 35 degrees of head-on (15 while drifting). For a ghost the wall is not
 * known: its direction is taken from the speed change, which points out of the wall. */
#define HIT_SPEED 1000.0f
#define HIT_COS -0.819f         /* cos 35 degrees */
#define HIT_COS_DRIFT -0.966f   /* cos 15 degrees */
#define HIT_CHANGE 900.0f       /* units/s of speed change in 0.08 s; measured on real hits (2400 -> 0) */
#define WINDOW 0.08f            /* s: speeds before and after a moment */
#define TELEPORT 6000.0f        /* faster than any kart: a respawn, not driving */
#define GRID 0.02f              /* movement is checked every 0.02 s of ghost time */
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

static float length(float x, float y)
{
    float s = x * x + y * y, r = s > 1.0f ? s : 1.0f;
    for (int i = 0; i < 20; i++) r = 0.5f * (r + s / r);     /* square root */
    return s > 0.0f ? r : 0.0f;
}

/* Input bits at ghost time t (CGhostSnapshotKey: +0xc bits, +0xe time in 1/100 s) */
static int input_at(void *ghost, float t)
{
    void *keys = AT(ghost, 0x38, void *);
    int lo = 0, hi = count(keys);
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (AT(item(keys, mid), 0xe, uint16_t) / 100.0f <= t) lo = mid + 1; else hi = mid;
    }
    return lo > 0 ? AT(item(keys, lo - 1), 0xc, uint16_t) : 0;
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

/* A wall hit at ghost time g (see HIT_*): speeds in the 0.08 s before and after (z is up). Not a hit:
 * a ramp (the speed turns upward) or an out-of-bounds pick-up (the kart then stands frozen). */
static int wall_hit(void *ghost, float g)
{
    float a[3], b[3], c[3], d[3], e[3];
    position(ghost, g - WINDOW, a);
    position(ghost, g, b);
    position(ghost, g + WINDOW, c);
    float v1x = (b[0] - a[0]) / WINDOW, v1y = (b[1] - a[1]) / WINDOW;
    float v2x = (c[0] - b[0]) / WINDOW, v2y = (c[1] - b[1]) / WINDOW;
    float s1 = length(v1x, v1y), s2 = length(v2x, v2y), change = length(v2x - v1x, v2y - v1y);
    if (s1 < HIT_SPEED || change < HIT_CHANGE || s1 > TELEPORT || s2 > TELEPORT) return 0;
    float cos = (v1x * (v2x - v1x) + v1y * (v2y - v1y)) / (s1 * change);
    if (cos > (input_at(ghost, g) & IN_FIRST_JUMP ? HIT_COS_DRIFT : HIT_COS)) return 0;
    float climb = ((c[2] - b[2]) - (b[2] - a[2])) / WINDOW;
    if (climb > 400.0f || climb < -400.0f) return 0;
    position(ghost, g + 0.1f, d);
    position(ghost, g + 0.4f, e);
    float moved = (d[0] - e[0]) * (d[0] - e[0]) + (d[1] - e[1]) * (d[1] - e[1]) + (d[2] - e[2]) * (d[2] - e[2]);
    return moved >= 1.0f;
}

/* The game's sounds for a wall hit: the impact at once, the "Ouch" line 0.2 s later (voice script) */
static void wall_hits(void *ghost, float from, float to)
{
    for (float g = (float)((int)(from / GRID) + 1) * GRID; g <= to; g += GRID) {
        if (g < sp.go + 1.0f || g - sp.last_hit < 0.5f || !wall_hit(ghost, g)) continue;
        sp.last_hit = g;
        play(ghost, "COM_VEH_Collision_impact_PLR_set", 0);
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
    if (sp.quiet) {         /* a boost made again after a rewind (boosts.c) */
        if (out) *out = 0;
        return 0;
    }
    void *started = sp.screen ? handle_object(sound) : 0;
    /* the character screen's countdown ticks (spectate.c presses Ready until it runs) */
    if (started && same_name(AT(started, 0x30, const char *), "ui_customization_countdown"))
        sp.countdown_ticks++;
    if (sp.screen == 3 && started && same_name(AT(started, 0x30, const char *), "ui_menu_back"))
        sp.backs_heard++;
    if (started && same_name(AT(started, 0x30, const char *), "ui_customization_countdown_go"))
        sp_countdown_go();
    /* only in a spectator run while ghosts play (the manager lives for the whole game; +0x14 _isPlaying) */
    int on = sp.active && sp.manager && sp.manager == g_ghost_manager && AT(sp.manager, 0x14, uint8_t);
    if (on && entity && sp.kart && (entity == sp.kart || entity == game_get_driver(sp.kart))) {
        if (out) *out = 0;      /* the player's hidden kart, moved with the ghost, stays silent */
        return 0;
    }
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
        /* _isLocalPlayer also makes the surface sounds "_PLR": only the watched ghost keeps it. The
         * others keep it until their "_PLR" engine faded out (+0x88 current volume), or it would
         * stay at its last volume. */
        int watched = i == sp.watch;
        AT(audio, 0x79, uint8_t) = watched || AT(audio, 0x88, float) > 0.001f;
        AT(audio, 0x7c, float) = watched && forward() ? 1.0f : 0.0f;     /* wanted volume */
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
