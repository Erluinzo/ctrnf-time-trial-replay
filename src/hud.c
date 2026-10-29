/* Spectator HUD: copies of the game's own HUD pieces, added to the HUD's top level (as the reserves
 * meter does): the watched ghost's race time, its name and the playback speed, and a virtual
 * controller with the ghost's inputs. The game's timer and lap counter (the locked player's) are
 * hidden. */
#include "spectate.h"

extern void *game_spawn_placeable(void *project, void *source, void *parent) GAME;
extern void game_set_text(void *placeable, const char *text) GAME;
extern void game_set_position(void *placeable, const float *v) GAME;
extern void game_set_scale(void *placeable, const float *v) GAME;
extern void game_set_pivot(void *placeable, const float *v) GAME;
extern void game_set_opacity(void *placeable, float opacity) GAME;
extern void game_set_hidden(void *placeable, int hidden) GAME;
extern const char g_composition_vtable[] GAME;   /* igGuiComposition */
extern const char g_minimap_player_vtable[] GAME;    /* CGuiBehaviorMinimapPlayer */
extern void *g_race_system_meta GAME;   /* CWorldRaceSystemComponent */
/* The world entity's handle and the entity table, as 0xeaf5f6 reads them */
extern uint8_t g_world_ready GAME;
extern uint32_t g_world_id GAME, g_world_no_id GAME;
extern char *g_entity_table GAME;

#define TIME_TEMPLATE "[Timer]"
#define TEXT_TEMPLATE "[LapCounterText]"
#define LABEL_TEMPLATE "Laps Label 1"           /* small text in the lap times: has the button symbols */
#define BAR_TEMPLATE "[octane_ui_hud_boost_bg.tga]"
#define FILL_SPRITE "[octane_ui_hud_boost_fill.tga]"
#define FILL_FULL 2.77f                         /* the game's fill scale for a full classic turbo meter */
#define BAR_TEXTURE_WIDTH 244.0f                /* children are placed from the bar texture's width */
#define DIM 0.4f                                /* opacity of a button that is not pressed */
static const char HIDE[][40] = {"Timer", "Octane_RacerHud_LapDisplay"};     /* compositions */

/* Virtual controller, laid out like the game pad: button symbols (Unicode private use, see the
 * port's button_prompts.py), each placed by its middle in HUD space. The left stick leans with the
 * steering; buttons a ghost never records (L2, R2, the touchpad, Triangle, Circle, the right stick)
 * stay dim, so the pad looks whole. */
enum { PAD_L2, PAD_R2, PAD_L1, PAD_R1, PAD_TOUCHPAD, PAD_LEFT_STICK, PAD_RIGHT_STICK, PAD_TRIANGLE,
       PAD_SQUARE, PAD_CIRCLE, PAD_CROSS, PAD_COUNT };
static const char PAD_TEXT[PAD_COUNT][4] = {"\xee\x80\x87", "\xee\x80\x88", "\xee\x80\x84", "\xee\x80\x86",
    "\xee\x80\x92", "\xee\x80\x89", "\xee\x80\x8a", "\xee\x80\x82", "\xee\x80\x83", "\xee\x80\x81", "\xee\x80\x80"};
/* middle x, middle y, symbol width in the font's button image (a fraction of it, igCharMetrics) */
static const float PAD_AT[PAD_COUNT][3] = {
    {120, 770, 0.195f}, {380, 770, 0.195f}, {120, 830, 0.285f}, {380, 830, 0.285f}, {250, 880, 0.18f},
    {120, 905, 0.164f}, {305, 990, 0.164f}, {380, 885, 0.18f}, {335, 930, 0.179f}, {425, 930, 0.18f},
    {380, 975, 0.164f}};
#define PAD_SCALE 2.0f
#define PAD_WIDTH 270.0f        /* HUD pixels per font image width at PAD_SCALE: symbol width = this x fraction */
#define STICK_LEAN 22.0f        /* the left stick moves this far at full steering */
#define STEER_X 250.0f
#define STEER_Y 1040.0f
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

static void build(void *project, void *root)
{
    sp.root = root;
    sp.time_text = text_piece(project, find(root, TIME_TEMPLATE, 0, 0), root, 60.0f, 60.0f);
    sp.status_text = text_piece(project, find(root, TEXT_TEMPLATE, 0, 0), root, 60.0f, 135.0f);
    void *label = find(root, LABEL_TEMPLATE, 0, 0);
    for (int i = 0; i < PAD_COUNT; i++) {
        sp.pad[i] = text_piece(project, label, root, PAD_AT[i][0] - PAD_AT[i][2] * PAD_WIDTH / 2, PAD_AT[i][1]);
        if (!sp.pad[i]) continue;
        game_set_text(sp.pad[i], PAD_TEXT[i]);
        float scale[3] = {PAD_SCALE, PAD_SCALE, 1.0f};
        game_set_scale(sp.pad[i], scale);
    }
    /* Steering: a classic turbo meter whose fill grows from the middle to the side of the turn */
    sp.steer_bar = copy(project, find(root, BAR_TEMPLATE, 1, 0), root);
    sp.steer_fill = sp.steer_bar ? find(sp.steer_bar, FILL_SPRITE, 1, 0) : 0;
    if (sp.steer_bar) {
        float bar[3] = {STEER_X - AT(root, 0x90, float) / 2, STEER_Y - AT(root, 0x94, float) / 2, 0.0f};
        game_set_position(sp.steer_bar, bar);   /* placed from the root's middle (anchor 2) */
    }
    /* The game's fill has its pivot on its right end: a full fill spans [x - width, x] */
    if (sp.steer_fill)
        sp.steer_center = AT(sp.steer_fill, 0x60, float) - AT(sp.steer_fill, 0x90, float) * FILL_FULL / 2;
}

/* The fill from the middle of the bar: steer -1 (full left) to 1 (full right) */
static void show_steering(float steer)
{
    void *fill = sp.steer_fill;
    if (!fill) return;
    float pivot[3] = {steer < 0 ? 1.0f : 0.0f, AT(fill, 0x70, float), AT(fill, 0x74, float)};
    float position[3] = {sp.steer_center, AT(fill, 0x64, float), AT(fill, 0x68, float)};
    float scale[3] = {FILL_FULL * (steer < 0 ? -steer : steer) / 2, AT(fill, 0x88, float), AT(fill, 0x8c, float)};
    game_set_pivot(fill, pivot);
    game_set_position(fill, position);
    game_set_scale(fill, scale);
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

/* "DriverBigNorm" -> "BIG NORM", "DriverNTropy" -> "N. TROPY" */
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

/* The race start of a ghost: the time of its first input key with the race start bit
 * (CGhostSnapshotKey: +0xc bits, +0xe time in 1/100 s) */
static float race_start(void *ghost)
{
    void *keys = AT(ghost, 0x38, void *);
    for (int i = 0; i < count(keys); i++)
        if (AT(item(keys, i), 0xc, uint16_t) & IN_RACE_START) return AT(item(keys, i), 0xe, uint16_t) / 100.0f;
    return 0.0f;
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

/* Every frame of a spectator run */
void hud_frame(void *manager)
{
    void *project = sp.hud ? handle_object(&AT(sp.hud, 0x38, void *)) : 0;
    void *root = project && AT(project, 0x10, void *) ? content(AT(project, 0x10, void *)) : 0;
    if (!root) return;
    if (root != sp.root || !sp.time_text || !is_child(root, sp.time_text)) build(project, root);
    hide_player_icon();
    for (int i = 0; i < child_count(root); i++) {
        void *a = asset(child(root, i));
        if (!a || AT(a, 0, const void *) != g_composition_vtable) continue;
        for (int h = 0; h < (int)(sizeof HIDE / sizeof *HIDE); h++)
            if (same_text(AT(a, 0x10, const char *), HIDE[h])) game_set_hidden(child(root, i), 1);
    }

    void *list = AT(manager, 0x40, void *);
    void *ghost = sp.watch < count(list) ? item(list, sp.watch) : 0;
    if (!ghost) return;
    if (sp.go_watch != sp.watch) {
        sp.go = race_start(ghost);
        sp.go_watch = sp.watch;
    }

    char text[80], *at = text, *end = text + sizeof text - 16;
    int centis = (int)((sp.time - sp.go) * 100.0f + 0.5f);
    if (centis < 0) centis = 0;
    at = put_number(at, centis / 6000, 1);
    *at++ = ':';
    at = put_number(at, centis / 100 % 60, 2);
    *at++ = ':';
    at = put_number(at, centis % 100, 2);
    *at = 0;
    if (sp.time_text) game_set_text(sp.time_text, text);

    at = put_driver(text, AT(ghost, 0x20, const char *), end);
    at = put(at, "  ");
    at = put_number(at, sp.watch + 1, 1);
    *at++ = '/';
    at = put_number(at, count(list), 1);
    at = put(at, "  ");
    static const char SPEED_TEXT[][8] = {"x0.1", "x0.5", "x1", "x2", "x3", "x4"};
    at = put(at, sp.scrub < 0 ? "<< x4" : sp.scrub > 0 ? ">> x4" : sp.paused ? "PAUSED" : SPEED_TEXT[sp.speed]);
    *at = 0;
    if (sp.status_text) game_set_text(sp.status_text, text);

    int bits = inputs(ghost);
    float steer = (int8_t)(bits & 0xff) / 127.0f;
    for (int i = 0; i < PAD_COUNT; i++) {
        if (!sp.pad[i]) continue;
        int pressed = i == PAD_L1 ? bits & IN_SECOND_JUMP : i == PAD_R1 ? bits & IN_FIRST_JUMP
                    : i == PAD_SQUARE ? bits & (IN_BRAKE | IN_REVERSE) : i == PAD_CROSS ? bits & IN_ACCELERATE
                    : i == PAD_LEFT_STICK ? steer > 0.05f || steer < -0.05f : 0;
        game_set_hidden(sp.pad[i], !sp.show_pad);
        game_set_opacity(sp.pad[i], pressed ? 1.0f : DIM);
    }
    if (sp.pad[PAD_LEFT_STICK]) {
        float at[3] = {PAD_AT[PAD_LEFT_STICK][0] - PAD_AT[PAD_LEFT_STICK][2] * PAD_WIDTH / 2 + steer * STICK_LEAN,
                       PAD_AT[PAD_LEFT_STICK][1], 0.0f};
        game_set_position(sp.pad[PAD_LEFT_STICK], at);
    }
    if (sp.steer_bar) game_set_hidden(sp.steer_bar, !sp.show_pad);
    show_steering(steer);
}
