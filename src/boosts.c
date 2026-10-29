/* Boosts after a rewind.
 * A kart's boosts (its reserves and sacred fire) are live game state that only runs forward: rewinding
 * left them as they were, or with boosts the kart got on the way back. So while the play goes forward
 * past the time it reached before, every ghost kart's boost list is saved every HIST_STEP s of ghost
 * time; in saved time (while rewinding, and when playing it again) each list is made the saved one at
 * every step, as the karts' slides are not rewound and would start other boosts.
 * A boost is its data (+0x10, the kart's own copy made when it started) and its times: the data of every
 * saved boost is kept (one reference, never given back: it lives in the race's memory), so a boost
 * that has ended since can be made again from it (0xe2e6b0, as the game does). The history lives in
 * memory from the game's malloc, freed at the next race. */
#include "spectate.h"

extern void *(*const game_malloc)(unsigned long size) GAME;     /* the game's malloc and free (import slots) */
extern void (*const game_free)(void *memory) GAME;
extern void game_boost_create(void *component, void *data, void *source) GAME;
extern void *g_boost_meta GAME;                                 /* CKartBoostComponent */

#define HIST_STEP 0.1f
#define HIST_BOOSTS 4
#define KEPT 8192           /* boost data kept (5 ghosts start about 1500 boosts in a long run) */
#define EXPIRED 1.0e6f      /* elapsed time that ends a boost: the game's boost update then removes it */
#define RESTORE_FRAMES 4    /* frames the boosts are restored after a rewind: the karts' first frames there may start boosts */

/* CKartBoost: +0x10 data, +0x18 elapsed, +0x1c, +0x28 sustain, +0x2c, +0x30 flag (1: lasts while its
 * source, e.g. a boost pad, holds it), +0x34 and +0x38 (start) game clock times (frame times +0x20, 8192
 * per second; the clock goes on during a rewind, so these are saved from the clock), +0x48 blending out */
struct saved_boost {
    void *data;
    float elapsed, f1c, sustain, f2c;
    int32_t flag, since34, since38;     /* sinces: game clock ticks before the save (-1 kept as -1) */
    uint8_t blending;
};
struct sample {
    int n;
    struct saved_boost boosts[HIST_BOOSTS];
};
/* The history memory: the kept boost data, then ghosts x length samples */
struct history {
    void *kept[KEPT];
    struct sample samples[];
};

/* The boost data is kept alive (one more reference), once */
static int keep(void *data)
{
    struct history *h = sp.history;
    for (int i = sp.kept_n - 1; i >= 0; i--)        /* the newest first: most boosts are recent */
        if (h->kept[i] == data) return 1;
    if (sp.kept_n == KEPT) return 0;
    __atomic_add_fetch(&AT(data, 8, int32_t), 1, __ATOMIC_SEQ_CST);     /* as the game: lock inc [object + 8] */
    h->kept[sp.kept_n++] = data;
    return 1;
}

/* The game clock in ticks */
static int32_t ticks(void)
{
    return g_frame ? AT(g_frame, 0x20, int32_t) : 0;
}

static void *boost_component(void *ghost)
{
    void *kart = ghost_kart(ghost);
    return kart && g_boost_meta ? game_find_component(kart, g_boost_meta, 0) : 0;
}

static void save(void *component, struct sample *s)
{
    void *list = AT(component, 0x28, void *);
    s->n = 0;
    for (int i = 0; i < count(list) && s->n < HIST_BOOSTS; i++) {
        void *boost = item(list, i);
        void *data = boost ? AT(boost, 0x10, void *) : 0;
        if (!data || !keep(data)) continue;
        struct saved_boost *b = &s->boosts[s->n++];
        b->data = data;
        b->elapsed = AT(boost, 0x18, float);
        b->f1c = AT(boost, 0x1c, float);
        b->sustain = AT(boost, 0x28, float);
        b->f2c = AT(boost, 0x2c, float);
        b->flag = AT(boost, 0x30, int32_t);
        b->since34 = AT(boost, 0x34, int32_t) == -1 ? -1 : ticks() - AT(boost, 0x34, int32_t);
        b->since38 = AT(boost, 0x38, int32_t) == -1 ? -1 : ticks() - AT(boost, 0x38, int32_t);
        b->blending = AT(boost, 0x48, uint8_t);
    }
}

static void put(void *boost, const struct saved_boost *b)
{
    AT(boost, 0x18, float) = b->elapsed;
    AT(boost, 0x1c, float) = b->f1c;
    AT(boost, 0x28, float) = b->sustain;
    AT(boost, 0x2c, float) = b->f2c;
    AT(boost, 0x30, int32_t) = b->flag;
    AT(boost, 0x34, int32_t) = b->since34 == -1 ? -1 : ticks() - b->since34;
    AT(boost, 0x38, int32_t) = b->since38 == -1 ? -1 : ticks() - b->since38;
    AT(boost, 0x48, uint8_t) = b->blending;
}

/* The kart's boost with this data, or 0 */
static void *find(void *list, void *data)
{
    for (int i = count(list) - 1; i >= 0; i--)
        if (item(list, i) && AT(item(list, i), 0x10, void *) == data) return item(list, i);
    return 0;
}

/* The kart's boosts become the saved ones: the same boosts take the saved times, the others end, and
 * saved boosts the kart no longer has are made again, in the saved order */
static void restore(void *component, const struct sample *s)
{
    void *list = AT(component, 0x28, void *);
    for (int i = 0; i < count(list); i++) {
        void *boost = item(list, i);
        if (!boost) continue;
        int m = 0;
        while (m < s->n && s->boosts[m].data != AT(boost, 0x10, void *)) m++;
        if (m < s->n) {
            put(boost, &s->boosts[m]);
        } else {
            AT(boost, 0x18, float) = EXPIRED;
            AT(boost, 0x28, float) = 0.0f;
        }
    }
    for (int m = 0; m < s->n; m++) {
        if (find(list, s->boosts[m].data)) continue;
        game_boost_create(component, s->boosts[m].data, 0);
        list = AT(component, 0x28, void *);
        void *boost = find(list, s->boosts[m].data);
        if (boost) put(boost, &s->boosts[m]);
    }
    /* The saved order: of equal boosts the game runs the last one (0xe2e946) */
    void **items = count(list) ? AT(list, 0x20, void **) : 0;
    for (int m = 0, at = 0; m < s->n; m++)
        for (int j = at; j < count(list); j++)
            if (items[j] && AT(items[j], 0x10, void *) == s->boosts[m].data) {
                void *swap = items[at];
                items[at++] = items[j];
                items[j] = swap;
                break;
            }
}

/* Every frame of a spectator run, after the play time moved from old to time */
void boosts_frame(void *manager, float old, float time)
{
    void *list = AT(manager, 0x40, void *);
    if (!sp.history && sp.end > 0.0f && game_malloc) {
        sp.history_ghosts = count(list) < HISTORY_GHOSTS ? count(list) : HISTORY_GHOSTS;
        sp.history_length = (int)(sp.end / HIST_STEP) + 2;
        sp.history = game_malloc(sizeof(struct history) + sizeof(struct sample) * (unsigned long)(sp.history_ghosts * sp.history_length));
        sp.kept_n = 0;
        for (int i = 0; i < HISTORY_GHOSTS; i++) sp.saved_up_to[i] = -1;
    }
    if (!sp.history) return;
    int k = (int)(time / HIST_STEP);
    if (k >= sp.history_length) k = sp.history_length - 1;
    int scrubbing = sp.scrub != 0;
    if (sp.was_scrubbing && !scrubbing) sp.restore_frames = RESTORE_FRAMES;
    for (int i = 0; i < sp.history_ghosts && i < count(list); i++) {
        void *component = boost_component(item(list, i));
        if (!component) continue;
        struct sample *samples = ((struct history *)sp.history)->samples + i * sp.history_length;
        if (k <= sp.saved_up_to[i]) {
            if (k != sp.restored_step || sp.restore_frames) restore(component, &samples[k]);
        } else if (time > old) {
            for (int j = sp.saved_up_to[i] + 1; j <= k; j++) save(component, &samples[j]);
            sp.saved_up_to[i] = k;
        }
    }
    sp.was_scrubbing = scrubbing;
    sp.restored_step = k;
    if (sp.restore_frames) sp.restore_frames--;
}

/* At every race start: the history of the run before is freed */
void boosts_reset(void)
{
    if (sp.history && game_free) game_free(sp.history);
    sp.history = 0;
    sp.was_scrubbing = sp.restore_frames = 0;
}
