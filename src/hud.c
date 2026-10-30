/* Spectator HUD: the game's own timer, lap display and boost bar show the watched ghost, and copies
 * of the game's HUD pieces, added to the HUD's top level (as the reserves meter does), show its
 * name and the playback speed, and a virtual controller with the ghost's inputs. */
#include "spectate.h"

extern void *game_spawn_placeable(void *project, void *source, void *parent) GAME;
extern void game_set_text(void *placeable, const char *text) GAME;
extern void game_set_position(void *placeable, const float *v) GAME;
extern void game_set_scale(void *placeable, const float *v) GAME;
extern void game_set_pivot(void *placeable, const float *v) GAME;
extern void game_set_opacity(void *placeable, float opacity) GAME;
extern void game_set_rotation(void *placeable, const float *degrees) GAME;     /* +0x78: x, y, z */
extern void game_set_color(void *placeable, const uint8_t *rgba) GAME;
extern void game_set_hidden(void *placeable, int hidden) GAME;
extern void *game_find_driver(void *drivers, const char *name) GAME;   /* CRacerDriverData by _name, or 0 */
extern void game_hud_set_lap(void *hud, int lap) GAME;
extern void game_hud_bar(void *hud, int style) GAME;
extern void *g_race_clock_meta GAME;    /* CRaceClockComponent (world entity): +0x28 _raceTimer */
static float ghost_end_time(void *ghost);
void sp_boost_update(void *hud, int flag);
static void *world(void);
static void *watched_ghost(void);
extern void game_boost_update_original(void *hud, int flag) GAME;              /* hooks.s */
extern void game_hud_boost_stop(void *hud) GAME;    /* the meter's "stop boost update" */
extern float game_spline_nearest(void *spline, const float *pos, float *out) GAME;           /* 0xe49470 */
extern void game_spline_direction(void *spline, const float *pos, float *dir, long flag) GAME;  /* 0xe490c0 */
extern float game_cos(float a) GAME;    /* the math functions the WRONG WAY check calls */
extern float game_sin(float a) GAME;
extern void game_entity_angles(float *out, void *entity) GAME;
extern void game_speed_original(void *kart, float *speed, int32_t *max) GAME;   /* hooks.s */
extern void game_speed_ratio_original(void *kart, float *ratio) GAME;           /* hooks.s */
extern void game_air_original(void *kart, void *a, void *b, void *c, void *d) GAME;   /* hooks.s */
extern void *g_kart_move_meta GAME;     /* the kart component with the slide timer (+0xe0 state, +0xf8 timer) and air time (+0x248) */
extern char *g_racer_manager GAME;      /* +0x18 driver list */
extern const char g_composition_vtable[] GAME;   /* igGuiComposition */
extern const char g_minimap_player_vtable[] GAME;    /* CGuiBehaviorMinimapPlayer */
extern void *g_race_system_meta GAME;   /* CWorldRaceSystemComponent */
/* The world entity's handle and the entity table, as 0xeaf5f6 reads them */
extern uint8_t g_world_ready GAME;
extern uint32_t g_world_id GAME, g_world_no_id GAME;

#define LAP_TIMER "Lap Timer %"                 /* % = 1..7: the game timer's lap time lines */
#define TEXT_TEMPLATE "[LapCounterText]"
#define LABEL_TEMPLATE "Laps Label 1"           /* small text in the lap times: has the button symbols */
#define ARROW_TEMPLATE "[octane_ui_hud_boost_nf_arrow.tga]"   /* a small arrow, pointing down at 0 degrees */
#define STATUS_BOX 0.11f                        /* see hud_frame */
#define HELP_SCALE 0.55f                        /* the help lines: smaller than the status line */

/* Virtual controller, laid out like the game pad: button symbols (Unicode private use, see the
 * port's button_prompts.py), each placed by its middle in HUD space, and a D-pad of four game
 * arrows (steering shows on left / right, reversing on down; the sticks are not shown). Pressed
 * buttons are bright and a bit bigger, the others dark; buttons a ghost never records (L2, R2,
 * Triangle, Circle, D-pad up) stay dark, so the pad looks whole. */
enum { PAD_L2, PAD_R2, PAD_L1, PAD_R1, PAD_TRIANGLE,
       PAD_SQUARE, PAD_CIRCLE, PAD_CROSS, PAD_UP, PAD_DOWN, PAD_LEFT, PAD_RIGHT, PAD_COUNT, PAD_SYMBOLS = PAD_UP };
static const char PAD_TEXT[PAD_SYMBOLS][4] = {"\xee\x80\x87", "\xee\x80\x88", "\xee\x80\x84", "\xee\x80\x86",
    "\xee\x80\x82", "\xee\x80\x83", "\xee\x80\x81", "\xee\x80\x80"};
/* middle x, middle y, and: the width the symbol is drawn with (a fraction of the font image width;
 * measured in game: L1/R1 are drawn narrower than their cells)
 * or the arrow's rotation in degrees */
#define PAD_X 70.0f
#define PAD_Y 765.0f
static const float PAD_AT[PAD_COUNT][3] = {
    {60, 30, 0.195f}, {340, 30, 0.195f}, {60, 88, 0.222f}, {342, 88, 0.222f},
    {341.5f, 130, 0.18f}, {295, 175, 0.18f}, {385, 175, 0.18f},
    {339.5f, 220, 0.164f},
    {66, 129, 180}, {63.5f, 211, 0}, {24, 170, 90}, {106, 170, 270}};
#define PAD_SCALE 2.0f
#define PAD_WIDTH 270.0f        /* HUD pixels per font image width at PAD_SCALE: symbol width = this x fraction */
#define PRESSED_SCALE 1.2f
#define ARROW_SCALE 2.0f
static const uint8_t BRIGHT[4] = {255, 255, 255, 255}, DARK[4] = {150, 150, 150, 255};
#define DARK_OPACITY 0.45f
/* The help lines (the button symbols are in the font too, but not the D-pad: its arrows are drawn over
 * the spaces at the start of lines 1 and 2) */
static const char HELP[5][48] = {"\xee\x80\x83 : PAUSE", "     /     : REWIND / FORWARD",
    "     /     : SPEED", "\xee\x80\x87 / \xee\x80\x88 : PREVIOUS / NEXT LAP", "\xee\x80\x84 / \xee\x80\x86 : OTHER GHOST"};
/* Help arrows: line, x, y change, rotation (as the controller's); the up arrow's image sits low */
static const float HELP_ARROW[4][4] = {{1, 70, -1, 90}, {1, 113, -1, 270}, {2, 69, -2, 180}, {2, 113, 0, 0}};
#define HELP_ARROW_SCALE 0.7f
/* Box width change per help line: the box only guesses the text's width (see hud_frame), so the
 * spaces under the arrows move with each line's letters; measured in game */
static const float HELP_BOX_FIX[5] = {0, 0, -37, 0, 0};
/* Left end of each help line: the D-pad lines start with the arrows' pointed ends, a bit to the left */
static const float HELP_X[5] = {60, 55, 55, 60, 60};
#define HELP_Y 522.0f
#define HELP_STEP 34.0f

/* CGhostSnapshotKey bits: 0-7 steering (s8), 8 accelerate, 9 reverse, 10 brake, 11 first jump,
 * 12 second jump, 13 start drift, 14 race start */
#define IN_ACCELERATE 0x100
#define IN_REVERSE 0x200
#define IN_BRAKE 0x400
#define IN_FIRST_JUMP 0x800
#define IN_SECOND_JUMP 0x1000
#define IN_RACE_START 0x4000

static int same_text(const char *a, const char *b)
{
    if (!a) return 0;
    while (*a && *a == *b) a++, b++;
    return *a == *b;
}

/* igGuiPlaceable children: +0x48 list */
static void *child(void *placeable, int i)
{
    return item(AT(placeable, 0x48, void *), i);
}

static int child_count(void *placeable)
{
    return count(AT(placeable, 0x48, void *));
}

static void *asset(void *placeable)
{
    return handle_object(&AT(placeable, 0x38, void *));
}

/* A placeable that shows a composition has no children: its content is the composition's _placeable */
static void *content(void *placeable)
{
    void *a = asset(placeable);
    return a && AT(a, 0, const void *) == g_composition_vtable ? AT(a, 0x20, void *) : 0;
}

/* First placeable below `placeable` named `name` (by_asset: its asset's name: sprite or composition) */
static void *find(void *placeable, const char *name, int by_asset, int depth)
{
    if (!placeable || depth > 24) return 0;
    void *a = by_asset ? asset(placeable) : 0;
    if (same_text(by_asset ? (a ? AT(a, 0x10, const char *) : 0) : AT(placeable, 0x28, const char *), name))
        return placeable;
    for (int i = 0; i < child_count(placeable); i++) {
        void *found = find(child(placeable, i), name, by_asset, depth + 1);
        if (found) return found;
    }
    return find(content(placeable), name, by_asset, depth + 1);
}

/* A copy of a template placeable at the end of `parent`, shown */
static void *copy(void *project, void *template, void *parent)
{
    if (!template) return 0;
    void *source = AT(template, 0x18, void *);      /* _sourcePlaceable, as the game spawns from */
    void *placeable = game_spawn_placeable(project, source ? source : template, parent);
    if (!placeable) return 0;
    game_set_opacity(placeable, 1.0f);
    game_set_hidden(placeable, 0);
    return placeable;
}

/* Text piece placed by its left middle, in HUD space (anchor top left) */
static void *text_piece(void *project, void *template, void *root, float x, float y)
{
    void *piece = copy(project, template, root);
    if (!piece) return 0;
    AT(piece, 0xa0, int32_t) = AT(piece, 0xa4, int32_t) = 0;
    float pivot[3] = {0.0f, 0.5f, 0.5f}, position[3] = {x, y, 0.0f};
    game_set_pivot(piece, pivot);
    game_set_position(piece, position);
    return piece;
}

/* The game timer's lap line `i` (0..6), or 0 */
static void *game_lap_line(void *root, int i)
{
    char name[] = LAP_TIMER;
    name[sizeof name - 2] = (char)('1' + i);
    return find(root, name, 0, 0);
}

/* The placeable that holds the game timer's pieces: the content of the "Timer" composition */
static void *timer_content(void *root)
{
    for (int i = 0; i < child_count(root); i++) {
        void *a = asset(child(root, i));
        if (a && AT(a, 0, const void *) == g_composition_vtable && same_text(AT(a, 0x10, const char *), "Timer"))
            return content(child(root, i));
    }
    return 0;
}

static void build(void *project, void *root)
{
    sp.root = root;
    /* The game's script writes its own current lap line every frame (from lap times it gets only in a
     * real race), so the lap lines shown are copies, at the same place; the game's stay hidden */
    void *timer = timer_content(root);
    for (int i = 0; i < 7; i++) {
        void *line = timer ? game_lap_line(root, i) : 0;
        sp.lap_line[i] = line ? copy(project, line, timer) : 0;
        if (sp.lap_line[i]) game_set_position(sp.lap_line[i], &AT(line, 0x60, float));
    }
    sp.status_text = text_piece(project, find(root, TEXT_TEMPLATE, 0, 0), root, 60.0f, 700.0f);   /* above the controller */
    if (sp.status_text) sp.status_extent = AT(sp.status_text, 0x90, float);
    void *label = find(root, LABEL_TEMPLATE, 0, 0), *arrow = find(root, ARROW_TEMPLATE, 1, 0);
    for (int i = 0; i < PAD_COUNT; i++) {
        float x = PAD_X + PAD_AT[i][0], y = PAD_Y + PAD_AT[i][1];
        if (i < PAD_SYMBOLS) {
            sp.pad[i] = text_piece(project, label, root, x - PAD_AT[i][2] * PAD_WIDTH / 2, y);
            if (sp.pad[i]) game_set_text(sp.pad[i], PAD_TEXT[i]);
            continue;
        }
        void *piece = sp.pad[i] = copy(project, arrow, root);      /* D-pad: an arrow by its middle */
        if (!piece) continue;
        AT(piece, 0xa0, int32_t) = AT(piece, 0xa4, int32_t) = 0;
        float pivot[3] = {0.5f, 0.5f, 0.5f}, position[3] = {x, y, 0.0f}, turn[3] = {0.0f, 0.0f, PAD_AT[i][2]};
        game_set_pivot(piece, pivot);
        game_set_position(piece, position);
        game_set_rotation(piece, turn);
    }
    for (int i = 0; i < 5; i++) {
        sp.help[i] = text_piece(project, find(root, TEXT_TEMPLATE, 0, 0), root, HELP_X[i], HELP_Y + HELP_STEP * i);
        if (!sp.help[i]) continue;
        float scale[3] = {HELP_SCALE, HELP_SCALE, 1.0f};
        game_set_scale(sp.help[i], scale);
        int n = 0;
        while (HELP[i][n]) n++;
        AT(sp.help[i], 0x90, float) = sp.status_extent * STATUS_BOX * (float)n + HELP_BOX_FIX[i];   /* see hud_frame */
        game_set_text(sp.help[i], HELP[i]);
    }
    for (int i = 0; i < 4; i++) {
        void *piece = sp.help_arrow[i] = copy(project, arrow, root);
        if (!piece) continue;
        AT(piece, 0xa0, int32_t) = AT(piece, 0xa4, int32_t) = 0;
        float pivot[3] = {0.5f, 0.5f, 0.5f}, turn[3] = {0.0f, 0.0f, HELP_ARROW[i][3]};
        float scale[3] = {HELP_ARROW_SCALE, HELP_ARROW_SCALE, 1.0f};       /* placed in hud_frame */
        game_set_pivot(piece, pivot);
        game_set_rotation(piece, turn);
        game_set_scale(piece, scale);
        game_set_color(piece, BRIGHT);
        game_set_opacity(piece, 1.0f);
    }
}

/* A controller piece pressed (bright, a bit bigger) or not (dark); x: the left stick's lean */
static void show_button(int i, int pressed, float x)
{
    void *piece = sp.pad[i];
    if (!piece) return;
    float size = pressed ? PRESSED_SCALE : 1.0f;
    game_set_hidden(piece, !sp.show_pad);
    game_set_opacity(piece, pressed ? 1.0f : DARK_OPACITY);
    game_set_color(piece, pressed ? BRIGHT : DARK);
    if (i >= PAD_SYMBOLS) {         /* arrows keep their size: bigger ones would touch each other */
        float scale[3] = {ARROW_SCALE, ARROW_SCALE, 1.0f};
        game_set_scale(piece, scale);
        return;
    }
    float scale[3] = {PAD_SCALE * size, PAD_SCALE * size, 1.0f};
    float position[3] = {PAD_X + PAD_AT[i][0] + x - PAD_AT[i][2] * PAD_WIDTH * size / 2, PAD_Y + PAD_AT[i][1], 0.0f};
    game_set_scale(piece, scale);
    game_set_position(piece, position);
}

static char *put(char *at, const char *text)
{
    while (*text) *at++ = *text++;
    return at;
}

static char *put_number(char *at, int n, int digits)
{
    char d[12];
    int k = 0;
    do d[k++] = (char)('0' + n % 10), n /= 10; while (n || k < digits);
    while (k) *at++ = d[--k];
    return at;
}

/* "1:02:13" (minutes, seconds, hundredths), as the game's timer */
static void put_time(char *at, float seconds)
{
    int centis = (int)(seconds * 100.0f + 0.5f);
    if (centis < 0) centis = 0;
    at = put_number(at, centis / 6000, 1);
    *at++ = ':';
    at = put_number(at, centis / 100 % 60, 2);
    *at++ = ':';
    at = put_number(at, centis % 100, 2);
    *at = 0;
}

/* "Dr. N. Tropy" -> "DR. N. TROPY"; driver ids too: "DriverBigNorm" -> "BIG NORM", "DriverNTropy" -> "N. TROPY" */
static char *put_driver(char *at, const char *name, char *end)
{
    if (!name) return at;
    const char *p = name;
    if (p[0] == 'D' && p[1] == 'r' && p[2] == 'i' && p[3] == 'v' && p[4] == 'e' && p[5] == 'r') p += 6;
    for (int i = 0; p[i] && at < end; i++) {
        char c = p[i];
        if (i && c >= 'A' && c <= 'Z' && p[i - 1] >= 'a' && p[i - 1] <= 'z' && at < end) *at++ = ' ';
        if (i == 1 && c >= 'A' && c <= 'Z' && p[0] >= 'A' && p[0] <= 'Z' && p[2] >= 'a' && p[2] <= 'z') at = put(at, ". ");
        if (at < end) *at++ = c >= 'a' && c <= 'z' ? (char)(c - 32) : c;
    }
    return at;
}

/* The name the ghost menu shows for playing ghost i: its driver's display name ("Dr. N. Tropy",
 * "William P."). A challenge ghost's file may use another character, so its driver is the one of its
 * header: the playing list is the player's ghost (if chosen, manager +0x48), then the chosen headers
 * (manager +0x50, CGhostHeaderData +0x18 _driverName) in order. */
static const char *ghost_name(void *manager, int i, void *ghost)
{
    const char *id = AT(ghost, 0x20, const char *);
    int player = AT(manager, 0x48, uint8_t) != 0;
    void *headers = AT(manager, 0x50, void *);
    if (i >= player && i - player < count(headers) && item(headers, i - player))
        id = AT(item(headers, i - player), 0x18, const char *);
    void *driver = id && g_racer_manager ? game_find_driver(AT(g_racer_manager, 0x18, void *), id) : 0;
    const char *name = driver ? AT(driver, 0x60, const char *) : 0;    /* +0x60 _displayName */
    return name ? name : id;
}

/* The race start of a ghost: the time of its first input key with the race start bit
 * (CGhostSnapshotKey: +0xc bits, +0xe time in 1/100 s) */
static float race_start(void *ghost)
{
    void *keys = AT(ghost, 0x38, void *);
    for (int i = 0; i < count(keys); i++)
        if (AT(item(keys, i), 0xc, uint16_t) & IN_RACE_START) return AT(item(keys, i), 0xe, uint16_t) / 100.0f;
    return 0.0f;
}

/* Lap starts of a ghost: ghost files keep no laps, but the last move key is on the finish line, so
 * a lap starts each time the ghost passes close to it (LINE units) going the same way, at least
 * 10 s after the race start and the last lap start. */
#define LINE 1500.0f
static void find_laps(void *ghost, float go)
{
    void *keys = AT(ghost, 0x30, void *);
    int n = count(keys);
    sp.lap_count = 0;
    sp.shown_lap = 0;
    if (n < 3) return;
    const float *end = &AT(item(keys, n - 1), 0xc, float), *before = &AT(item(keys, n - 2), 0xc, float);
    float way[3] = {end[0] - before[0], end[1] - before[1], end[2] - before[2]}, last = go, best = 0.0f;
    int found = -1;
    for (int i = 1; i < n - 1; i++) {
        float t = key_time(keys, i);
        const float *p = &AT(item(keys, i), 0xc, float), *q = &AT(item(keys, i - 1), 0xc, float);
        float dx = p[0] - end[0], dy = p[1] - end[1], dz = p[2] - end[2], d = dx * dx + dy * dy + dz * dz;
        int near = t > last + 10.0f && t < key_time(keys, n - 1) - 10.0f && d < LINE * LINE
                   && (p[0] - q[0]) * way[0] + (p[1] - q[1]) * way[1] + (p[2] - q[2]) * way[2] > 0.0f;
        if (near && (found < 0 || d < best)) {
            found = i;
            best = d;
        } else if (!near && found >= 0) {   /* left the line: the closest key was the crossing */
            if (sp.lap_count < 8) sp.lap_starts[sp.lap_count++] = key_time(keys, found);
            last = key_time(keys, found);
            found = -1;
        }
    }
}

/* The watched ghost's input bits now (0 past its end) */
static int inputs(void *ghost)
{
    void *keys = AT(ghost, 0x38, void *);
    int i = AT(ghost, 0x44, int32_t);
    return i >= 0 && i < count(keys) ? AT(item(keys, i), 0xc, uint16_t) : 0;
}

static int is_child(void *parent, void *placeable)
{
    for (int i = 0; i < child_count(parent); i++)
        if (child(parent, i) == placeable) return 1;
    return 0;
}

/* The world entity (CWorldEntity), as 0xeaf5f6 finds it: the id's index in the entity table, checked
 * against the table's generations (u16 index | 0x8000) */
static void *world(void)
{
    uint32_t id = g_world_id;
    if (!g_world_ready || id == g_world_no_id || !(id & 0x8000) || !g_entity_table) return 0;
    int i = id & 0x7fff;
    void *generations = AT(g_entity_table, 0x28, void *);
    if (i >= count(generations) || AT(generations, 0x20, uint16_t *)[i] != (i | 0x8000)) return 0;
    return AT(AT(g_entity_table, 0x20, void *), 8 * i, void *);
}

/* Hides the player's icon below `placeable`: the Racer placeable whose CGuiBehaviorMinimapPlayer
 * (in _behaviors +0xe8) has the player's kart as _racer (+0x28 handle -> CRacer, +0x20 handle -> kart).
 * Its content is hidden, not the Racer placeable: the behavior shows that one again every frame. */
static void hide_icon(void *placeable, int depth)
{
    if (!placeable || depth > 24) return;
    void *behaviors = AT(placeable, 0xe8, void *);
    for (int i = 0; i < count(behaviors); i++) {
        void *b = item(behaviors, i);
        if (!b || AT(b, 0, const void *) != g_minimap_player_vtable) continue;
        void *racer = handle_object(&AT(b, 0x28, void *));
        if (racer && handle_object(&AT(racer, 0x20, void *)) == sp.kart && content(placeable))
            game_set_hidden(content(placeable), 1);
    }
    for (int i = 0; i < child_count(placeable); i++) hide_icon(child(placeable, i), depth + 1);
    hide_icon(content(placeable), depth + 1);
}

/* The minimap is the world race system's project (+0x90 _minimapHud), not part of the racer HUD */
static void hide_player_icon(void)
{
    void *w = world();
    void *system = w && g_race_system_meta ? game_find_component(w, g_race_system_meta, 0) : 0;
    void *project = system ? handle_object(&AT(system, 0x90, void *)) : 0;
    if (project && sp.kart) hide_icon(AT(project, 0x10, void *), 0);
}

/* The race clock (CRaceClockComponent on the world entity, +0x28 _raceTimer) to `seconds`. The timer
 * counts ticks of 1/8192 s: elapsed = (now, or its stop tick +0x10 when not running) - start tick
 * (+0xc); +0x14 running, +0x15 paused; vtable +0xb0 = now (as the slide meter reads its timer, 0xe8c740) */
/* The race clock's timer (+0x28: 1/8192 s ticks; +0xc start, +0x10 stop, +0x14 running, +0x15 paused,
 * vtable +0xb0 now), or 0 */
static void *race_timer(void)
{
    void *w = world();
    void *clock = w && g_race_clock_meta ? game_find_component(w, g_race_clock_meta, 0) : 0;
    void *timer = clock ? AT(clock, 0x28, void *) : 0;
    return timer && AT(timer, 0, void *) ? timer : 0;
}

/* The game's GO has come: its race clock runs */
int race_started(void)
{
    void *timer = race_timer();
    return timer && AT(timer, 0x14, uint8_t);
}

static void set_race_clock(float seconds)
{
    void *timer = race_timer();
    if (!timer) return;
    typedef int32_t now_fn(void *timer);
    int32_t now = AT(timer, 0x14, uint8_t) && !AT(timer, 0x15, uint8_t)
                ? AT(*(void **)timer, 0xb0, now_fn *)(timer) : AT(timer, 0x10, int32_t);
    AT(timer, 0xc, int32_t) = now - (int32_t)(seconds * 8192.0f);
}

/* The game timer's lap lines: the times of the finished laps and the current one, from the lap
 * starts at the play time (so a rewind takes lines away). After the end, the last lap is finished. */
static void lap_lines(void *root, void *ghost)
{
    char text[16];
    float lines[8], last = ghost_end_time(ghost), start = sp.go;
    int shown = 0;
    for (int i = 0; i < sp.lap_count && sp.time >= sp.lap_starts[i] && shown < 7; i++) {
        lines[shown++] = sp.lap_starts[i] - start;
        start = sp.lap_starts[i];
    }
    if (sp.time > sp.go && shown < 7) lines[shown++] = (sp.time < last ? sp.time : last) - start;
    for (int i = 0; i < 7; i++) {
        void *game = game_lap_line(root, i);
        if (game && game != sp.lap_line[i]) game_set_hidden(game, 1);
        if (!sp.lap_line[i]) continue;
        game_set_hidden(sp.lap_line[i], i >= shown);
        if (i < shown) {
            put_time(text, lines[i]);
            game_set_text(sp.lap_line[i], text);
        }
    }
}

/* A new name for a copied piece: game scripts find pieces by name, and would write into the copy */
static void rename(void *placeable, const char *name)
{
    char *old = AT(placeable, 0x28, char *);
    AT(placeable, 0x28, char *) = game_string(g_string_pool, name);
    if (old) game_string_release(g_string_pool, old - 0x10);
}

/* The character screen after L1 (spectate.c): no loading circle where the (hidden) character would
 * be, and in its place "SPECTATOR MODE" in the font of the screen's "READY!". show 0: the message is
 * taken away (the screen is used for normal races too). */
void screen_gui(void *project, int show)
{
    void *top = project ? AT(project, 0x10, void *) : 0;
    void *root = top ? content(top) : 0;
    if (!root) return;
    void *load = find(root, "Octane_Customization_LoadIcon", 1, 0);
    if (load && show) game_set_hidden(load, 1);
    if (show && (!sp.screen_text || !is_child(root, sp.screen_text))) {
        void *template = find(root, "[Ready]", 0, 0);      /* "READY!": the same font, not animated */
        sp.screen_text = template ? copy(project, template, root) : 0;
        if (sp.screen_text) {
            rename(sp.screen_text, "[SpectatorModeText]");
            AT(sp.screen_text, 0xa0, int32_t) = AT(sp.screen_text, 0xa4, int32_t) = 0;    /* from the top left */
            float pivot[3] = {0.5f, 0.5f, 0.5f}, at[3] = {500.0f, 580.0f, 0.0f};
            game_set_pivot(sp.screen_text, pivot);
            game_set_position(sp.screen_text, at);
        }
    }
    if (sp.screen_text && is_child(root, sp.screen_text)) {
        game_set_hidden(sp.screen_text, !show);
        game_set_text(sp.screen_text, "SPECTATOR MODE");
    }
}

/* Every frame from sp_input, after the game's timer script wrote its own current lap line: write
 * the lap lines again (only while ghosts play: the HUD is gone after the level) */
void hud_late(void)
{
    if (!sp.active || !sp.manager || !AT(sp.manager, 0x14, uint8_t) || !sp.hud || sp.go_watch != sp.watch) return;
    void *project = handle_object(&AT(sp.hud, 0x38, void *));
    void *root = project && AT(project, 0x10, void *) ? content(AT(project, 0x10, void *)) : 0;
    void *ghost = watched_ghost();
    if (root && root == sp.root && ghost) lap_lines(root, ghost);
}

/* The kart is in a slide: its slide timer runs (move component +0xf8, timer +0x14) */
static int sliding(void *kart)
{
    void *move = g_kart_move_meta ? game_find_component(kart, g_kart_move_meta, 0) : 0;
    void *timer = move ? AT(move, 0xf8, void *) : 0;
    return timer && AT(timer, 0x14, uint8_t);
}

/* Every frame of a spectator run */
void hud_frame(void *manager)
{
    void *project = sp.hud ? handle_object(&AT(sp.hud, 0x38, void *)) : 0;
    void *root = project && AT(project, 0x10, void *) ? content(AT(project, 0x10, void *)) : 0;
    if (!root) return;
    if (root != sp.root || !sp.status_text || !is_child(root, sp.status_text)) build(project, root);
    hide_player_icon();

    void *list = AT(manager, 0x40, void *);
    void *ghost = sp.watch < count(list) ? item(list, sp.watch) : 0;
    if (!ghost) return;
    if (sp.go_watch != sp.watch) {
        sp.go = race_start(ghost);
        sp.go_watch = sp.watch;
        find_laps(ghost, sp.go);
    }
    /* The boost bar (powerslide and jump meters) reads the watched ghost's kart (sp_hud_bar) */
    void *kart = ghost_kart(ghost);
    sp.shared_next = kart;                  /* for other mods: see g_spectate_kart */
    if (sp.hud && kart && kart != sp.bar_kart) {
        game_hud_bar(sp.hud, AT(sp.hud, 0x8c, int32_t));     /* _boostBarIndex */
        sp.bar_kart = kart;
        /* The powerslide meter follows the kart's slide events: after a switch it is told whether the
         * new kart is in a slide now. "Stop" empties the meter with an animation on game time, which does
         * not run while paused: then the meter gets the new kart's (empty) slide and the stop when the
         * play goes on. */
        if (sliding(kart) || sp.paused) sp_boost_update(sp.hud, 1);
        else game_hud_boost_stop(sp.hud);
        sp.bar_stop = sp.paused;
    }
    if (sp.bar_stop && !sp.paused) {
        if (!sliding(kart)) game_hud_boost_stop(sp.hud);
        sp.bar_stop = 0;
    }
    /* The game's lap display shows the watched ghost's lap. Its final lap effect plays once per
     * entry into the last lap (+0x88 cleared below it, e.g. after a rewind). */
    int lap = 1, laps = g_race_settings ? AT(g_race_settings, 0xc, int32_t) : 0;
    for (int i = 0; i < sp.lap_count; i++) lap += sp.time >= sp.lap_starts[i];
    if (laps > 0 && lap > laps) lap = laps;
    if (sp.hud && lap != sp.shown_lap) {
        if (lap < laps) AT(sp.hud, 0x88, uint8_t) = 0;
        game_hud_set_lap(sp.hud, lap);
        sp.shown_lap = lap;
    }

    /* The game's timer shows the watched ghost's race time: its race clock is set every frame
     * (it counts game time, which the playback speeds change) */
    float last = ghost_end_time(ghost), now = (sp.time < last ? sp.time : last) - sp.go;
    set_race_clock(now > 0.0f ? now : 0.0f);
    lap_lines(root, ghost);
    char text[80], *at, *end = text + sizeof text - 16;

    at = put_driver(text, ghost_name(manager, sp.watch, ghost), end);
    at = put(at, "  ");
    if (count(list) > 1) {      /* "2/3": which ghost, only when there is a choice */
        at = put_number(at, sp.watch + 1, 1);
        *at++ = '/';
        at = put_number(at, count(list), 1);
        at = put(at, "  ");
    }
    /* the speed while it is not x1, x1 only for a moment after a change; pause, rewind and forward
     * while they last */
    static const char SPEED_TEXT[][8] = {"x0.1", "x0.5", "x1", "x2", "x3", "x4"};
    float dt = g_frame ? AT(g_frame, 0x18, float) : 0.0f;
    if (sp.speed_shown > 0.0f) sp.speed_shown -= dt > 0.1f ? 0.1f : dt;
    at = put(at, sp.scrub < 0 ? "REWIND x4" : sp.scrub > 0 ? "FORWARD x4" : sp.paused ? "PAUSED"   /* the font has no < > */
                 : sp.speed != NORMAL_SPEED || sp.speed_shown > 0.0f ? SPEED_TEXT[sp.speed] : "");
    *at = 0;
    /* The text is right-aligned in its box and shrinks when it does not fit (the template's asset, shared
     * with the game's lap counter, sets this): the box gets about the width of this text, so the line
     * starts at the left with full-size letters. STATUS_BOX: box width per character, as the template's. */
    if (sp.status_text) {
        AT(sp.status_text, 0x90, float) = sp.status_extent * STATUS_BOX * (float)(at - text);
        game_set_text(sp.status_text, text);
    }
    /* "OTHER GHOST" only with ghosts to switch to; without it the lines move down one line, so the
     * last one stays just above the status line */
    int one = count(list) < 2;
    float top = HELP_Y + (one ? HELP_STEP : 0.0f);
    for (int i = 0; i < 5; i++) {
        if (!sp.help[i]) continue;
        game_set_hidden(sp.help[i], i == 4 && one);
        float at[3] = {HELP_X[i], top + HELP_STEP * i, 0.0f};
        game_set_position(sp.help[i], at);
    }
    for (int i = 0; i < 4; i++) {
        if (!sp.help_arrow[i]) continue;
        float at[3] = {HELP_ARROW[i][1], top + HELP_STEP * HELP_ARROW[i][0] + HELP_ARROW[i][2], 0.0f};
        game_set_position(sp.help_arrow[i], at);
    }

    int bits = inputs(ghost);
    float steer = (int8_t)(bits & 0xff) / 127.0f;
    for (int i = 0; i < PAD_COUNT; i++) {
        show_button(i, i == PAD_L1 ? bits & IN_SECOND_JUMP : i == PAD_R1 ? bits & IN_FIRST_JUMP
                    : i == PAD_SQUARE ? bits & IN_BRAKE : i == PAD_CROSS ? bits & IN_ACCELERATE
                    : i == PAD_DOWN ? bits & IN_REVERSE
                    : i == PAD_LEFT ? steer < -0.05f : i == PAD_RIGHT ? steer > 0.05f : 0, 0.0f);
    }
}

/* ---- Powerslide and jump meters ----
 * The HUD's boost bar reads the watched ghost's kart (hud_frame). Its kart runs the game's slide
 * logic from the ghost's inputs (slides, jumps and air time), so the meters only need the kart's
 * HUD events and the HUD's kart (below). */


static void *watched_kart(void)
{
    void *list = sp.active && sp.manager ? AT(sp.manager, 0x40, void *) : 0;
    return sp.watch < count(list) ? ghost_kart(item(list, sp.watch)) : 0;
}

static void *watched_ghost(void)
{
    void *list = sp.active && sp.manager ? AT(sp.manager, 0x40, void *) : 0;
    return sp.watch < count(list) ? item(list, sp.watch) : 0;
}

/* ---- Wheel glow ----
 * Near the end of a slide (meter almost full) the player's wheels glow (slide level 2, 0xe8e720 starts
 * 4 wheel effects). Three game rules keep that from a ghost kart; the watched kart is let through. */
extern int game_slide_level_original(void *effects, void *move) GAME;   /* hooks.s */
extern float game_drift_ratio(void *move) GAME;                         /* the slide meter, 0..1 */
extern void game_effect_version(void **out, void *effects, void *asset) GAME;
extern int game_entity_player(void *entity) GAME;

/* Called in place of the kart effects' slide level (0xe8c810): 0 = no slide, 1 = slide, 2 = slide meter
 * near full. The game gives 2 only to the local player; the watched kart gets the player's rule.
 * Effects +0x10 data (+0xa0 the meter level for 2), +0x18 kart; move +0xf8 slide timer. */
int sp_slide_level(void *effects, void *move)
{
    if (!sp.active || AT(effects, 0x18, void *) != watched_kart() || !sliding(AT(effects, 0x18, void *)))
        return game_slide_level_original(effects, move);
    return game_drift_ratio(move) > AT(AT(effects, 0x10, void *), 0xa0, float) ? 2 : 1;
}

/* Called in place of a kart effect's "pick the version" (0xe284d0, from the effect start 0xe27230):
 * effect assets have a version for the local player (+0x10) and one for other karts (+0x18), picked by
 * effects +0x25. The wheel glow has no other-kart version, so the watched kart gets the player's. */
void sp_effect_version(void **out, void *effects, void *asset)
{
    uint8_t local = AT(effects, 0x25, uint8_t);
    if (sp.active && AT(effects, 0x18, void *) == watched_kart()) AT(effects, 0x25, uint8_t) = 1;
    game_effect_version(out, effects, asset);
    AT(effects, 0x25, uint8_t) = local;
}

/* Called in place of "the local player of this entity" (0xcb8b30) where a player version effect is
 * started (0xb95d46): it is drawn in that player's view only, and a ghost kart has no player (-1), so
 * it was never started. The watched kart answers as the hidden player kart. */
int sp_effect_player(void *entity)
{
    return game_entity_player(sp.active && sp.kart && entity && entity == watched_kart() ? sp.kart : entity);
}

/* Called in place of the game's call that gives the boost bar's script the HUD's kart (0xe32050; the
 * game makes it again at the race start): in a spectator run it gets the watched ghost's kart */
void sp_hud_bar(void *hud, int style)
{
    void *kart = hud && hud == sp.hud ? watched_kart() : 0;
    if (!kart) {
        game_hud_bar(hud, style);
        return;
    }
    void *own = AT(hud, 0x18, void *);
    AT(hud, 0x18, void *) = kart;
    game_hud_bar(hud, style);
    AT(hud, 0x18, void *) = own;
}

/* Called in place of the racer HUD's "start boost update" (0xe437b0), which sends its kart to the
 * meter's script: in a spectator run the watched ghost's kart */
void sp_boost_update(void *hud, int flag)
{
    void *kart = hud && hud == sp.hud ? watched_kart() : 0;
    if (!kart) {
        game_boost_update_original(hud, flag);
        return;
    }
    void *own = AT(hud, 0x18, void *);
    AT(hud, 0x18, void *) = kart;
    game_boost_update_original(hud, flag);
    AT(hud, 0x18, void *) = own;
}

/* Called in place of the kart code's "find the HUD component on this kart" where a slide starts and
 * ends and a slide boost is made or fails (0xe32826, 0xe2d5cb, 0xe32bad, 0xe2fe7b): the game then
 * fills the powerslide meter and shows its ratings itself (events to the HUD). A ghost kart's own HUD component shows
 * nothing: the watched one gets the player's. */
void *sp_kart_hud(void *entity, void *meta, int zero)
{
    return entity && entity == watched_kart() && sp.hud ? sp.hud : game_find_component(entity, meta, zero);
}

/* Called in place of "an entity's position" and "an entity's angles" in the racer's WRONG WAY check
 * (0xe48cc1, 0xe48cdf in 0xe48c20: the kart's heading against the track's direction there): for the
 * hidden player kart (under the road, ahead of the ghost) the watched ghost's kart, so the message and
 * its sound come when the ghost drives the wrong way */
void sp_wrong_way_position(float *out, void *entity)
{
    game_entity_position(out, sp.active && entity == sp.kart && watched_kart() ? watched_kart() : entity);
}

void sp_wrong_way_angles(float *out, void *entity)
{
    game_entity_angles(out, sp.active && entity == sp.kart && watched_kart() ? watched_kart() : entity);
}

/* The track's direction at the watched ghost (called in place of 0xe490c0 at 0xe48cd2). The check
 * takes it from the racer's track segment (CTrackSplineSegment), which changes only when the hidden
 * kart passes a transition: after a rewind, a lap skip or a ghost switch it stayed on an old one (Hot
 * Air Skyway: a whole lap), so a ghost driving the right way got WRONG WAY or a wrong one got none.
 * So the segment nearest the ghost is used: from the racer's, along the segments before and after it
 * (segment +0x18 start and +0x20 end transition; transition +0x38 segments ending, +0x40 starting
 * there), or of all of them when that stops far away. While rewinding no WRONG WAY: the direction is
 * where the kart faces, made from its angles as the check does (0xe48ce4). */
#define SEGMENTS 128
#define SEGMENT_NEAR 1500.0f    /* units: a segment this near is the ghost's */
static float segment_distance(void *segment, const float *pos)
{
    float near[4], d = 0.0f;
    game_spline_nearest(AT(segment, 0x10, void *), pos, near);
    for (int i = 0; i < 3; i++) d += (near[i] - pos[i]) * (near[i] - pos[i]);
    return d;
}

/* The segment nearest pos: from `segment` to nearer neighbours (all = 0), or of every segment reached */
static void *nearest_segment(void *segment, const float *pos, int all)
{
    void *seen[SEGMENTS];
    seen[0] = segment;
    int n = 1;
    float best = segment_distance(segment, pos);
    for (int at = 0; at < n; at++) {
        void *from = all ? seen[at] : segment;
        for (int side = 0; side < 2; side++) {
            void *transition = AT(from, side ? 0x20 : 0x18, void *);
            void *list = transition ? AT(transition, side ? 0x40 : 0x38, void *) : 0;
            for (int i = 0; i < count(list) && n < SEGMENTS; i++) {
                void *other = item(list, i);
                int known = !other;
                for (int j = 0; j < n && !known; j++) known = seen[j] == other;
                if (known) continue;
                seen[n++] = other;
                float d = segment_distance(other, pos);
                if (d < best) {
                    best = d;
                    segment = other;
                }
            }
        }
        if (!all && segment == from) break;     /* no nearer neighbour */
    }
    return all || best < SEGMENT_NEAR * SEGMENT_NEAR ? segment : nearest_segment(segment, pos, 1);
}

void sp_wrong_way_direction(void *spline, const float *pos, float *dir, long flag)
{
    void *kart = sp.active ? watched_kart() : 0;
    void *segments = spline ? AT(spline, 0x38, void *) : 0;
    if (kart && sp.scrub < 0) {
        float a[3];
        game_entity_angles(a, kart);
        dir[0] = game_cos(a[1]) * game_cos(a[2]);
        dir[1] = game_cos(a[1]) * game_sin(a[2]);
        dir[2] = -game_sin(a[1]);
        return;
    }
    if (kart && count(segments)) spline = AT(nearest_segment(item(segments, 0), pos, 0), 0x10, void *);
    game_spline_direction(spline, pos, dir, flag);
}

/* Called in place of the game's speed values for the speedometer (0x1041ca0 speed ratio, 0x1041ce0
 * forward speed), which it reads for the HUD's kart: in a spectator run the watched ghost's kart
 * instead of the player's still one */
static void *speed_kart(void *kart)
{
    void *watched = kart && kart == sp.kart ? watched_kart() : 0;
    return watched ? watched : kart;
}

void sp_speed_ratio(void *kart, float *ratio)
{
    game_speed_ratio_original(speed_kart(kart), ratio);
}

void sp_forward_speed(void *kart, float *speed, int32_t *max)
{
    game_speed_original(speed_kart(kart), speed, max);
}

/* Called in place of the game's air time values (0x1041d90): the speedometer style's air time
 * number asks for the HUD's kart (the jump meter already reads the watched kart's own air time) */
void sp_air_data(void *kart, void *a, void *b, void *c, void *d)
{
    game_air_original(speed_kart(kart), a, b, c, d);
}

/* The ghost's race end: its last move key (as ghost_end in spectate.c, without the margin) */
static float ghost_end_time(void *ghost)
{
    void *keys = AT(ghost, 0x30, void *);
    return count(keys) ? key_time(keys, count(keys) - 1) : 0.0f;
}

