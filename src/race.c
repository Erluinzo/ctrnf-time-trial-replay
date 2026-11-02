/* Exact ghosts from full race recordings. Replay Mode (another mod of the same author) records every race in a
 * .crr file in its save folder RaceReplay (about 60 samples a second of each kart) and keeps every Time Trial
 * personal best; Ghost Sharing exports the run's .crr next to a shared ghost. When a playing ghost has one (same
 * track, a local racer who finished with the ghost's time, same driver), its kart is placed from the recording
 * instead of the ghost's move keys (about 14 a second, times rounded to 1/100 s). In spectator runs and in normal
 * Time Trials. The ghost itself is never changed: without a .crr, or without this mod, it plays as before.
 * File format: ~/Desktop/CTRNF-RaceReplay src/replay.h and src/files.c (rr_unpack). */
#include "spectate.h"

extern int sce_open(const char *path, int flags, int mode) GAME;
extern int64_t sce_pread(int fd, void *buffer, uint64_t size, int64_t offset) GAME;
extern int sce_close(int fd) GAME;
extern int64_t sce_getdents(int fd, char *buffer, uint64_t size) GAME;
extern int sce_initial_user(int *user) GAME;
extern int sce_save_mount2(const void *mount, void *result) GAME;
extern int sce_save_umount(const void *point) GAME;
extern void *game_current_zone(void *zones) GAME;   /* CZoneInfo: +0x10 level name */
extern void *g_zone_system GAME;
extern void *(*const game_malloc)(unsigned long size) GAME;
extern void (*const game_free)(void *memory) GAME;

/* ---- The .crr file (Replay Mode's replay.h, versions 4 and 5) ---- */
struct rr_racer {
    char name[32], driver[32], kart[32];
    int8_t player_id;
    uint8_t kind;               /* 1 local */
    uint8_t finished;           /* rank, 0 = did not finish */
    uint8_t look_filled;
    uint16_t look[6];
    float finish_time;          /* race time at the line, s */
};
struct rr_header {
    char magic[8];
    uint32_t version, header_size, frame_size, frames;
    float go_time;              /* recording time of GO, s */
    float duration;
    int32_t mode;               /* 2 Time Trial */
    int32_t laps;
    char level[64];
    char player[32];
    uint8_t date[8];
    struct rr_racer racer[8];
    uint32_t packed;            /* bytes of packed frames after the header */
};
struct rr_sample {              /* 52 bytes */
    int32_t position[3];        /* x 16 */
    int16_t velocity[3];        /* x 4 (units/s) */
    int16_t angles[3];          /* 65536 = one turn */
    uint8_t rest1[8];
    int16_t steer;
    uint8_t rest2[14];
    uint16_t ghost_bits;
    uint8_t rest3[2];
};
struct rr_frame {
    float time, race_time;
    struct rr_sample racer[8];
};
_Static_assert(sizeof(struct rr_racer) == 116 && __builtin_offsetof(struct rr_header, racer) == 0x90, "header layout");
_Static_assert(sizeof(struct rr_sample) == 52 && __builtin_offsetof(struct rr_sample, steer) == 32
               && __builtin_offsetof(struct rr_sample, ghost_bits) == 48, "sample layout");
#define MAX_FRAMES (60 * 60 * 11)
#define REST_OFFSET 24
#define GHOST_GO 3.80f          /* ghost time of GO in the game's ghosts */
#define ANGLE_UNIT 10430.06f

/* ---- What is kept: the matched racer's poses, in ghost time ---- */
struct pose {
    float time;
    int32_t position[3];
    int16_t velocity[3];
    int16_t angles[3];
};
struct race {
    void *ghost;                /* the CGhost it belongs to, 0 = none */
    float duration;             /* its _duration (+0x28), checked with the ghost */
    float end;                  /* its last move key's time: after it the game places it as before */
    int n;
    struct pose *pose;
    float error;                /* for tests (tools/peek.py): mean distance to the ghost's own keys */
};
struct races {
    int wanted;                 /* a race started: look for recordings when the ghosts play */
    int found;                  /* for tests: ghosts with a recording */
    struct race race[HISTORY_GHOSTS];
};
#define R ((struct races *)sp.races)

static int same(const char *a, const char *b)
{
    int i = 0;
    while (a[i] && a[i] == b[i]) i++;
    return a[i] == b[i];
}

static int get_varint(const uint8_t **in, const uint8_t *end, int32_t *v)
{
    uint32_t u = 0;
    for (int shift = 0; shift < 35; shift += 7) {
        if (*in >= end) return 0;
        uint8_t c = *(*in)++;
        u |= (uint32_t)(c & 0x7f) << shift;
        if (!(c & 0x80)) {
            *v = (int32_t)(u >> 1) ^ -(int32_t)(u & 1);
            return 1;
        }
    }
    return 0;
}

static int get_bytes(const uint8_t **in, const uint8_t *end, uint8_t *now, const uint8_t *before, int size)
{
    for (int i = 0; i < size; i++) now[i] = before[i];
    int o = 0;
    while (o < size) {
        if (*in >= end) return 0;
        int c = *(*in)++;
        if (c >= 0x80) {
            o += c - 0x7f;
            continue;
        }
        if (*in + c + 1 > end || o + c + 1 > size) return 0;
        for (int k = 0; k <= c; k++) now[o++] ^= *(*in)++;
    }
    return o == size;
}

/* One frame, as Replay Mode's rr_unpack (p1, p2: the two frames before). 0 = bad data. */
static int unpack(struct rr_frame *f, const struct rr_frame *p1, const struct rr_frame *p2, const uint8_t **in, const uint8_t *end, int version)
{
    if (!get_bytes(in, end, (uint8_t *)f, (const uint8_t *)p1, 8)) return 0;
    for (int r = 0; r < 8; r++) {
        struct rr_sample *s = &f->racer[r];
        const struct rr_sample *a = &p1->racer[r], *b = &p2->racer[r];
        int32_t v, steer, bits;
        for (int k = 0; k < 3; k++) {
            if (!get_varint(in, end, &v)) return 0;
            s->position[k] = (int32_t)((uint32_t)v + 2u * (uint32_t)a->position[k] - (uint32_t)b->position[k]);
        }
        for (int k = 0; k < 3; k++) {
            if (!get_varint(in, end, &v)) return 0;
            s->angles[k] = (int16_t)(v + 2 * a->angles[k] - b->angles[k]);
        }
        for (int k = 0; k < 3 && version > 4; k++) {
            if (!get_varint(in, end, &v)) return 0;
            s->velocity[k] = (int16_t)(v + a->velocity[k]);
        }
        if (!get_varint(in, end, &steer) || !get_varint(in, end, &bits)) return 0;
        if (!get_bytes(in, end, (uint8_t *)s + REST_OFFSET, (const uint8_t *)a + REST_OFFSET, sizeof *s - REST_OFFSET)) return 0;
        s->steer = (int16_t)(steer + a->steer);
        s->ghost_bits = (uint16_t)(bits ^ a->ghost_bits);
        float dt = f->time - p1->time;      /* version 4: velocity from the positions */
        for (int k = 0; k < 3 && version == 4; k++) {
            float speed = dt > 0.0f ? (float)(s->position[k] - a->position[k]) / 16.0f / dt * 4.0f : 0.0f;
            s->velocity[k] = (int16_t)(speed > 32767.0f ? 32767 : speed < -32767.0f ? -32767 : speed);
        }
    }
    return 1;
}

/* Pose of race r at ghost time t (position, velocity in units/s, angles in radians); 0 = not covered */
static int pose_at(const struct race *r, float t, float *position, float *velocity, float *angles)
{
    if (!r->n || t < r->pose[0].time || t > r->pose[r->n - 1].time) return 0;
    int lo = 0, hi = r->n - 1;
    while (hi - lo > 1) {
        int mid = (lo + hi) / 2;
        if (r->pose[mid].time <= t) lo = mid; else hi = mid;
    }
    const struct pose *a = &r->pose[lo], *b = &r->pose[hi];
    float f = b->time > a->time ? (t - a->time) / (b->time - a->time) : 0.0f, d = 0.0f;
    for (int k = 0; k < 3; k++) d += (float)(b->position[k] - a->position[k]) * (float)(b->position[k] - a->position[k]);
    if (d > 1000.0f * 16 * 1000.0f * 16) f = f < 0.5f ? 0.0f : 1.0f;    /* a respawn: no point in between */
    for (int k = 0; k < 3; k++) {
        position[k] = ((float)a->position[k] + (float)(b->position[k] - a->position[k]) * f) / 16.0f;
        velocity[k] = ((float)a->velocity[k] + (float)(b->velocity[k] - a->velocity[k]) * f) / 4.0f;
        angles[k] = ((float)a->angles[k] + (float)(int16_t)(b->angles[k] - a->angles[k]) * f) / ANGLE_UNIT;
    }
    return 1;
}

/* Mean distance between the race's poses (shifted by `shift` s) and up to 40 of the ghost's move keys after GO */
static float key_error(const struct race *r, void *keys, float shift)
{
    int n = count(keys), used = 0;
    float sum = 0.0f;
    for (int i = 0; i < 40; i++) {
        void *key = item(keys, 1 + (int)((long)(n - 2) * i / 40));
        float t = AT(key, 0x18, uint16_t) / 100.0f - shift, at[3], v[3], a[3], d = 0.0f;
        if (t < GHOST_GO + 0.5f || !pose_at(r, t, at, v, a)) continue;
        for (int k = 0; k < 3; k++) d += (at[k] - AT(key, 0xc + 4 * k, float)) * (at[k] - AT(key, 0xc + 4 * k, float));
        __asm__("sqrtss %1, %0" : "=x"(d) : "x"(d));
        sum += d;
        used++;
    }
    return used >= 20 ? sum / used : 1e9f;
}

#define MAX_ERROR 60.0f         /* units: a kart is about 100 long */
/* Reads racer `who` of the open file into r and checks it against the ghost's keys. 1 = kept. */
static int load(struct race *r, int fd, const struct rr_header *h, int who, void *ghost)
{
    uint8_t *packed = game_malloc((uint64_t)h->packed + 16);
    struct rr_frame *f = game_malloc(3 * sizeof *f);
    r->pose = game_malloc((uint64_t)h->frames * sizeof(struct pose));
    int ok = packed && f && r->pose && sce_pread(fd, packed, h->packed, h->header_size) == (int64_t)h->packed;
    const uint8_t *in = packed, *end = packed + h->packed;
    __builtin_memset(f, 0, 3 * sizeof *f);
    r->n = 0;
    for (uint32_t i = 0; ok && i < h->frames; i++) {
        struct rr_frame *now = &f[i % 3], *p1 = &f[(i + 2) % 3], *p2 = &f[(i + 1) % 3];
        static const struct rr_frame zero;
        if (!(ok = unpack(now, i ? p1 : &zero, i > 1 ? p2 : &zero, &in, end, h->version))) break;
        const struct rr_sample *s = &now->racer[who];
        if (!(s->rest1[0] & 1)) continue;       /* flags: RS_PRESENT */
        struct pose *p = &r->pose[r->n];
        p->time = now->time - h->go_time + GHOST_GO;
        if (r->n && p->time <= r->pose[r->n - 1].time) continue;
        for (int k = 0; k < 3; k++) p->position[k] = s->position[k], p->velocity[k] = s->velocity[k], p->angles[k] = s->angles[k];
        r->n++;
    }
    if (packed) game_free(packed);
    if (f) game_free(f);
    /* The recording's GO and the ghost's are a few 1/100 s apart: the best shift, checked with the keys */
    void *keys = AT(ghost, 0x30, void *);
    float best = 1e9f, shift = 0.0f;
    for (int i = -20; ok && count(keys) > 40 && i <= 20; i++) {
        float e = key_error(r, keys, i * 0.005f);
        if (e < best) best = e, shift = i * 0.005f;
    }
    r->error = best;
    if (!ok || best > MAX_ERROR) {
        if (r->pose) game_free(r->pose);
        r->pose = 0;
        r->n = 0;
        return 0;
    }
    for (int i = 0; i < r->n; i++) r->pose[i].time += shift;
    r->ghost = ghost;
    r->duration = AT(ghost, 0x28, float);
    r->end = key_time(keys, count(keys) - 1);
    return 1;
}

/* Racer of header h that is the ghost's run (-1 = none) */
static int racer_of(const struct rr_header *h, void *ghost, const char *level)
{
    const char *driver = AT(ghost, 0x20, const char *);
    float time = AT(ghost, 0x28, float);
    if (__builtin_memcmp(h->magic, "CTRRPLAY", 8) || (h->version != 5 && h->version != 4) || h->frame_size != sizeof(struct rr_frame)
        || h->frames < 2 || h->frames > MAX_FRAMES || h->packed > (uint64_t)MAX_FRAMES * sizeof(struct rr_frame) || h->mode != 2
        || h->header_size < sizeof *h || h->header_size > 4096 || !driver || !same(h->level, level))
        return -1;
    for (int i = 0; i < 8; i++) {
        const struct rr_racer *r = &h->racer[i];
        if (r->kind == 1 && r->finished && r->finish_time - time > -0.006f && r->finish_time - time < 0.006f && same(r->driver, driver)) return i;
    }
    return -1;
}

/* Finds and reads the recordings of the playing ghosts */
static void find(void *list)
{
    void *zone = g_zone_system ? game_current_zone(g_zone_system) : 0;
    const char *level = zone ? AT(zone, 0x10, const char *) : 0;
    struct { int32_t user, pad; const char *dir; uint64_t blocks; uint32_t mode; uint8_t reserved[36]; } mount;
    struct { char point[16]; uint8_t rest[48]; } result;
    char dir[16] = "RaceReplay", path[96], buffer[2048];
    __builtin_memset(&mount, 0, sizeof mount);
    __builtin_memset(&result, 0, sizeof result);
    mount.dir = dir;
    mount.blocks = 0x8000;
    mount.mode = 1;                                     /* read only */
    if (!level || sce_initial_user(&mount.user) < 0 || sce_save_mount2(&mount, &result) < 0) return;
    result.point[15] = 0;
    int folder = sce_open(result.point, 0x20000, 0);    /* O_DIRECTORY */
    int64_t n;
    while (folder >= 0 && (n = sce_getdents(folder, buffer, sizeof buffer)) > 0 && n <= (int64_t)sizeof buffer)
        for (int64_t at = 0; at + 8 <= n;) {
            /* each record: u32 id, u16 record length, u8 type, u8 name length, name at +8 */
            uint16_t length = AT(buffer + at, 4, uint16_t);
            uint8_t name_length = AT(buffer + at, 7, uint8_t);
            if (length < 8 || at + length > n || 8 + name_length > length) break;
            const char *name = buffer + at + 8;
            at += length;
            if (name_length < 5 || name_length > 60 || name[name_length - 4] != '.' || name[name_length - 3] != 'c'
                || name[name_length - 2] != 'r' || name[name_length - 1] != 'r') continue;
            int p = 0;
            for (; result.point[p]; p++) path[p] = result.point[p];
            path[p++] = '/';
            for (int i = 0; i < name_length; i++) path[p++] = name[i];
            path[p] = 0;
            struct rr_header h;
            int fd = sce_open(path, 0, 0);
            if (fd < 0) continue;
            if (sce_pread(fd, &h, sizeof h, 0) == (int64_t)sizeof h) {
                h.level[63] = 0;
                for (int i = 0; i < 8; i++) h.racer[i].driver[31] = 0;
                for (int g = 0; g < count(list) && g < HISTORY_GHOSTS; g++) {
                    void *ghost = item(list, g);
                    int who = ghost && !R->race[g].ghost ? racer_of(&h, ghost, level) : -1;
                    if (who >= 0) R->found += load(&R->race[g], fd, &h, who, ghost);
                }
            }
            sce_close(fd);
        }
    if (folder >= 0) sce_close(folder);
    sce_save_umount(result.point);
}

/* A race starts (sp_race_start): the recordings of the last one go; in a Time Trial, look again */
void recordings_start(void)
{
    if (!sp.races && game_malloc && (sp.races = game_malloc(sizeof(struct races)))) __builtin_memset(sp.races, 0, sizeof(struct races));
    if (!sp.races) return;
    for (int i = 0; i < HISTORY_GHOSTS; i++) {
        if (R->race[i].pose) game_free(R->race[i].pose);
        R->race[i].pose = 0;
        R->race[i].ghost = 0;
        R->race[i].n = 0;
    }
    R->found = 0;
    R->wanted = g_game && AT(g_game, 0xd4c, int32_t) == 2;
}

/* Every frame of the ghosts (sp_clock): once they are there, their recordings are read */
void recordings_frame(void *manager)
{
    void *list = manager ? AT(manager, 0x40, void *) : 0;
    if (!sp.races || !R->wanted || !count(list)) return;
    R->wanted = 0;
    find(list);
}

/* The ghost's kart at ghost time t from its recording: 1 = position (units), velocity (units/s) and angles (radians) set */
int recorded_pose(void *ghost, float t, float *position, float *velocity, float *angles)
{
    for (int i = 0; sp.races && ghost && i < HISTORY_GHOSTS; i++)
        if (R->race[i].ghost == ghost && R->race[i].duration == AT(ghost, 0x28, float))
            return t <= R->race[i].end && pose_at(&R->race[i], t, position, velocity, angles);
    return 0;
}
