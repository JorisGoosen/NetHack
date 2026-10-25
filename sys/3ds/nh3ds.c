/* NetHack 5.0 nh3ds.c    $NHDT-Date: 1792928538 2026/10/25 12:42:18 $  $NHDT-Branch: 3ds $:$NHDT-Revision: 1.18 $ */
/* Copyright (c) the NetHack 3DS port contributors, 2026.                */
/* NetHack may be freely redistributed.  See license for details.        */

/* Nintendo 3DS front-end for NetHack 5.0.1 (libctru + shim windowport).
 *
 * The game core never touches the 3DS: it runs through win/shim/winshim.c,
 * which forwards every windowing call to the single variadic callback
 * implemented here.  This file owns the 3DS: console rendering, hid input,
 * the software keyboard, and the process lifecycle.
 *
 * Phase 2c: menus and text entry.  add_menu streams are buffered per
 * window; select_menu renders them on the bottom screen (d-pad navigate,
 * A select/toggle, B cancel, START confirm; PICK_NONE lists wait for a
 * key like a tty).  getlin/askname use the libctru software keyboard;
 * #commands run through the core's own extcmd menu.  Character creation
 * uses the core's generic setup menus once a button press is seen.
 *
 * Phase 2b: hid input.  D-pad/circle-pad = hjkl (two held = diagonals,
 * L/R+direction = run), A='.' rest, B='i', X=',', Y='s', START='S',
 * SELECT='?'; yn prompts answer via buttons (B = no/esc, A = default).
 * Without any button press the stub keeps auto-playing forced rests so
 * unattended emulator runs still progress (demo mode).
 *
 * Phase 2a: console rendering.  The top screen (50x30 with the 8x8 font)
 * shows the message area (3 rows), the map viewport (21 rows x 50 cols,
 * horizontally scrolled via cliparound), a field-driven 2-row status bar
 * fed by status_update events, and a key legend.  The bottom screen (40x30)
 * shows the message-history tail, or the contents of a blocking menu/text
 * window while the game waits for a key.  Input is still the phase 1
 * canned answer (alternating 'm' prefix + '.' rest); real button input
 * arrives in phase 2b.
 *
 * Phase 4a: stereoscopic depth.  The top screen is now a citro2d stereo
 * viewport (the bottom screen keeps the console).  Map glyphs are drawn
 * as sprites from an atlas of the same 8x8 console font (white pixels
 * on transparent) on depth layers: wall tops on the screen plane,
 * entities (hero, monsters, objects) on an intermediate plane, floor
 * terrain deepest, with stairs down / pits below the floor plane and
 * stairs up raised above it.  Each layer's horizontal parallax scales
 * with the 3D slider (at 0 the picture collapses to the old flat text
 * view; only the left eye is drawn then).  Levitation or flight raises
 * the hero one plane; the last thing the hero's tile showed before the
 * hero glyph covered it (plus the terrain the core reports beneath the
 * hero via the background glyph) is drawn on its own layer just above
 * the floor.  A "3drig" file in the playground forces the slider to
 * 1.0, dumps both eyes as BMPs every 64 frames and puts demo mode on a
 * wander-and-take-stairs autopilot.
 *
 * Build:  make CROSS_TO_3DS=1 all        (see sys/unix/hints/include/
 *         make CROSS_TO_3DS=1 3dspkg      cross-pre2.501 for the flags)
 */

#include "hack.h"       /* svp.plname, iflags, flags, NHW_*, ... */
#include "func_tab.h"   /* extcmdlist, CMD_* flags (not in hack.h) */
#include "dlb.h"        /* dlb_fopen: help/data.base live in nhdat */

#include <3ds.h>
#include <citro2d.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* --- libnh API (sys/libnh/libnhmain.c; there is no header by design) --- */
extern int nhmain(int argc, char *argv[]);
typedef void (*shim_callback_t)(const char *name, void *ret_ptr,
                                const char *fmt, ...);
extern void shim_graphics_set_callback(shim_callback_t cb);

/* ----------------------------------------------------------------------*/
/* Configuration                                                          */
/* ----------------------------------------------------------------------*/

/* playground on the SD card; the front-end chdir()s here because the
 * SHIM_GRAPHICS configuration has no CHDIR (include/config.h) */
static const char *nh3ds_hackdir = "sdmc:/3ds/nethack";

/* canned answers for the phase 1 stub */
static const char *nh3ds_plname = "Adventurer";

/* ----------------------------------------------------------------------*/
/* trace log                                                              */
/* ----------------------------------------------------------------------*/

static FILE *nh3ds_trace;

/* first raw_print/error we saw, shown on the hold-on-exit screen -- on
 * real hardware the console is wiped the instant we return to the
 * Homebrew Menu, so a fatal error would otherwise flash by unread */
static char nh3ds_lastwords[192] = "";

static void
nh3ds_log(const char *fmt, ...)
{
    va_list ap;

    if (!nh3ds_trace)
        return;
    va_start(ap, fmt);
    (void) vfprintf(nh3ds_trace, fmt, ap);
    va_end(ap);
    (void) fputc('\n', nh3ds_trace);
    (void) fflush(nh3ds_trace);
}

/* event accounting: log each event name the first time it is seen and
 * keep counters for the final dump */
#define NHEV_MAX 48
static struct {
    const char *name;
    unsigned long n;
} nhev[NHEV_MAX];
static int nhev_n;
static unsigned long nhev_total;

static void
nhev_count(const char *name)
{
    int i;

    ++nhev_total;
    for (i = 0; i < nhev_n; ++i)
        if (!strcmp(nhev[i].name, name)) {
            ++nhev[i].n;
            return;
        }
    if (nhev_n < NHEV_MAX) {
        nhev[nhev_n].name = name;
        nhev[nhev_n].n = 1;
        ++nhev_n;
    }
    nh3ds_log("[evt first] %s", name);
}

/* periodic histogram so that a run killed from outside (pkill) still
 * leaves an audit trail; the final dump only happens on clean exit */
static void
nhev_periodic(void)
{
    int i;

    if (nhev_total % 1048576L != 0)
        return;
    nh3ds_log("[evt snapshot total=%lu]", nhev_total);
    for (i = 0; i < nhev_n; ++i)
        nh3ds_log("[evt n=%8lu] %s", nhev[i].n, nhev[i].name);
}

static void
nh3ds_dump_and_close(void)
{
    int i;

    if (!nh3ds_trace)
        return;
    nh3ds_log("[exit] total shim events=%lu", nhev_total);
    for (i = 0; i < nhev_n; ++i)
        nh3ds_log("[evt n=%8lu] %s", nhev[i].n, nhev[i].name);
    (void) fclose(nh3ds_trace);
    nh3ds_trace = (FILE *) 0;
}

/* ----------------------------------------------------------------------*/
/* screen geometry and render state                                       */
/* ----------------------------------------------------------------------*/

/* top console is 50x30 with the 8x8 font; bottom is 40x30 */
#define TOP_COLS 50
#define TOP_ROWS 30
#define BOT_COLS 40
#define BOT_ROWS 30

/* top-screen layout */
#define MSG_ROW0 0              /* 3 message rows */
#define MSG_ROWS 3
#define MAP_ROW0 3              /* 21 map rows: full ROWNO */
#define MAP_ROWS 21
#define MAP_VIEW_COLS 50        /* COLNO is 80: horizontal scroll */
#define ST_ROW0 24              /* 2 status rows */
#define ST_ROWS 2
#define LEG_ROW0 26             /* 4 legend rows */

/* bottom-screen layout: message tail, then the touch verb grid (see
 * the touch section below) */
#define TBOT_MSGROWS 14

/* map glyph buffer: the whole level as print_glyph delivered it */
struct nh3ds_cell {
    char c;                     /* ttychar from glyph_info */
    signed char color;          /* gm.sym.color, -1 = default */
    int glyph;                  /* the display entity (glyph_is_* tests) */
    unsigned short gflags;      /* gm.glyphflags (MG_HERO, ...) */
};
static struct nh3ds_cell nh3ds_map[ROWNO][COLNO];
/* background terrain per cell (print_glyph's bkglyphinfo; delivered
 * only when iflags.use_background_glyph is set -- we turn it on in
 * shim_init_nhwindows): the floor plane beneath objects, monsters and
 * the hero */
static struct nh3ds_cell nh3ds_mapbg[ROWNO][COLNO];
/* the last foreground glyph each cell showed while the hero was not
 * standing on it: on the hero's tile this is what the hero stands on */
static struct nh3ds_cell nh3ds_mapunder[ROWNO][COLNO];
static boolean nh3ds_map_dirty;
static coordxy nh3ds_vx, nh3ds_vy; /* viewport origin (top-left cell) */
static int nh3ds_cur_x = -1, nh3ds_cur_y = -1; /* map cursor (getpos:
                                                * farlook/travel); the
                                                * cube hides on the hero */
static unsigned long nh3ds_mapgen; /* bumped by map clears (level
                                    * changes): the sweep restarts */

/* ----------------------------------------------------------------------*/
/* stereoscopic depth rendering (P4a)                                     */
/* ----------------------------------------------------------------------*/

/* Depth layers, nearest (the screen plane) to deepest (behind it).
 * Depths are pixels of horizontal eye separation at 3D slider = 1.0;
 * the real slider scales all of them, so it acts as a depth fader from
 * the old flat view (0) to the full diorama (1). */
enum {
    NH3DS_D_WALL = 0,   /* wall tops, closed/open doors, trees, iron
                         * bars -- and all the UI text */
    NH3DS_D_LEV,        /* the hero while levitating or flying */
    NH3DS_D_ENTITY,     /* hero, monsters, objects (chests etc.) */
    NH3DS_D_FURNITURE,  /* altar, grave, throne, sink, fountain, webs,
                         * stairs/ladders up: raised above the floor */
    NH3DS_D_UNDER,      /* what the hero stands on */
    NH3DS_D_FLOOR,      /* floor terrain: room, corridor, ice, water,
                         * lava, air, cloud, open drawbridges */
    NH3DS_D_DOWN,       /* stairs/ladders down, pits, holes, trap doors:
                         * openings below the floor plane */
    NH3DS_D_LAYERS
};
static const unsigned char nh3ds_depth[NH3DS_D_LAYERS] = {
    0,  /* wall tops / UI: the screen plane everything else recedes from */
    2,  /* levitating hero */
    4,  /* entities */
    6,  /* furniture / stairs up */
    8,  /* under the hero */
    10, /* floor */
    15  /* stairs down / pits: a hole in the floor plane */
};

/* what the top screen currently paints */
enum {
    NH3DS_VM_GAME = 0,  /* map + messages + status + legend */
    NH3DS_VM_MENU,      /* a buffered menu owns the top screen */
    NH3DS_VM_EXT,       /* the ext-command list owns the top screen */
    NH3DS_VM_EXIT       /* hold-on-exit screen (over the game view) */
};
static int nh3ds_view = NH3DS_VM_GAME;

static boolean nh3ds_gpu_ok;
static C3D_RenderTarget *nh3ds_rt_left, *nh3ds_rt_right;
static boolean nh3ds_stereo;     /* whether gfxSet3D is currently on */
static boolean nh3ds_debug3d;    /* "3drig" flag file seen at boot */
static boolean nh3ds_debug_lev;  /* ...containing 'L': hero always on
                                  * the levitation plane (visual test) */
static boolean nh3ds_debug_sweep; /* ...containing 'M': magic-map +
                                   * farlook cursor-sweep autopilot */
static unsigned long nh3ds_frame_n;

/* glyph atlas: the libctru console font (8x8, 1bpp, 256 chars -- the
 * same font the old console view used) expanded to white pixels on
 * transparent, swizzled to the PICA hardware texture layout.  Tinting
 * per NetHack color turns the white pixels into colored glyphs while
 * the background stays transparent, so lower depth layers show
 * through. */
extern const unsigned char default_font_bin[]; /* libctru font.bin.o */
#define NH3DS_FONTCHARS 256
#define NH3DS_ATLAS_W 256              /* 32 chars per atlas row */
#define NH3DS_ATLAS_H 64               /* 8 atlas rows */
static C3D_Tex nh3ds_glyph_tex;
static Tex3DS_SubTexture nh3ds_glyph_sub[NH3DS_FONTCHARS];

/* NetHack CLR_* (stored value; -1 = default) -> RGBA, filled at init */
static u32 nh3ds_rgb[16];
static C2D_ImageTint nh3ds_tints[16];
static C2D_ImageTint nh3ds_tints_dim[16]; /* 30% brightness -- the
                                           * under-the-hero glyphs */

/* message ring: putstr(WIN_MESSAGE) + putmsghistory */
#define NH3DS_NMSGS 64
#define NH3DS_MSGLEN 128
static char nh3ds_msgs[NH3DS_NMSGS][NH3DS_MSGLEN];
static int nh3ds_msg_head;      /* next free slot */
static int nh3ds_msg_count;
static boolean nh3ds_msg_dirty;

/* per-window bookkeeping (winids are small sequential ints here) */
#define NH3DS_MAXWINID 64
static unsigned char nh3ds_wintype[NH3DS_MAXWINID]; /* NHW_* + 1, 0 = none */

/* popup content buffer: the one MENU/TEXT window whose blocking display
 * we render on the bottom screen */
#define NH3DS_POPLINES 80
#define NH3DS_POPLINELEN 128
/* display_file page budget: content rows that fit between the prompt
 * row and the "[A] continue" row of the bottom-screen popup */
#define NH3DS_PAGEROWS (BOT_ROWS - 3)
static winid nh3ds_pop_win = -1;
static char nh3ds_pop_lines[NH3DS_POPLINES][NH3DS_POPLINELEN];
static int nh3ds_pop_n;
static char nh3ds_pop_prompt[NH3DS_POPLINELEN];

/* one buffered menu window; items are the add_menu() stream between
 * start_menu and end_menu.  ch == 0 marks non-selectable text
 * (headings, blank filler).  (Declared here, before the render/refresh
 * code below needs nh3ds_menu.active.) */
#define NH3DS_MAXMENU 128
struct nh3ds_menuitem {
    ANY_P id;
    char ch, gch;
    int attr, clr;
    char str[96];
    unsigned itemflags;
};
static struct nh3ds_menu {
    winid win;
    boolean active;
    int n;
    int top;                     /* first visible item row */
    int cursor;                  /* highlighted item (-1 = display only) */
    struct nh3ds_menuitem item[NH3DS_MAXMENU];
    char prompt[128];
} nh3ds_menu;

/* status bar state, fed by status_update; field labels are ours (the
 * core's genl_status_enablefield stores fmts for its own use and never
 * forwards them to the window port -- curses and wintty keep their own
 * copies too) */
static char nh3ds_st_val[MAXBLSTATS][MAXVALWIDTH];
static char nh3ds_st_fmt[MAXBLSTATS][16];
static int nh3ds_st_color[MAXBLSTATS];
static boolean nh3ds_st_active[MAXBLSTATS];
static unsigned long nh3ds_st_cond;
static boolean nh3ds_st_dirty;

/* per-field printf format for the value string, mirroring
 * initblstats[] in src/botl.c */
static const char *const nh3ds_st_initfmt[MAXBLSTATS] = {
    "%s",       /* BL_TITLE (role/rank) */
    " St:%s",   /* BL_STR */
    " Dx:%s",   /* BL_DX */
    " Co:%s",   /* BL_CO */
    " In:%s",   /* BL_IN */
    " Wi:%s",   /* BL_WI */
    " Ch:%s",   /* BL_CH */
    " %s",      /* BL_ALIGN (value: "Chaotic") */
    " S:%s",    /* BL_SCORE */
    " %s",      /* BL_CAP (value: "Burdened") */
    " %s",      /* BL_GOLD (value: "$:0", already labelled) */
    " Pw:%s",   /* BL_ENE */
    "(%s)",     /* BL_ENEMAX (follows Pw) */
    " Xp:%s",   /* BL_XP */
    " AC:%s",   /* BL_AC */
    " HD:%s",   /* BL_HD */
    " T:%s",    /* BL_TIME */
    " %s",      /* BL_HUNGER (value: "Hungry") */
    " HP:%s",   /* BL_HP */
    "(%s)",     /* BL_HPMAX (follows HP) */
    "%s",       /* BL_LEVELDESC (value: "Dlvl:1") */
    "/%s",      /* BL_EXP (follows Xp) */
    "%s",       /* BL_CONDITION (via bitmask, not text) */
    " %s",      /* BL_VERS */
    " %s",      /* BL_WEAPON */
    " %s",      /* BL_ARMOR */
    " %s",      /* BL_TERRAIN */
};

/* ----------------------------------------------------------------------*/
/* console helpers (bottom screen only -- the top screen is citro2d)      */
/* ----------------------------------------------------------------------*/

static PrintConsole nh3ds_botcon;
static PrintConsole *nh3ds_bot;

static void
nh3ds_scr(int top) /* select the console for printf(); only the bottom
                    * screen has one (the top screen is the citro2d
                    * stereo viewport), so any argument is ignored */
{
    (void) top;
    consoleSelect(nh3ds_bot);
}

/* ----------------------------------------------------------------------*/
/* GPU renderer: glyph atlas, text, the layered stereo map                */
/* ----------------------------------------------------------------------*/

/* expand the libctru console font into a white-on-transparent RGBA
 * atlas and swizzle it into the PICA's 8x8-morton-tile texture layout
 * (reference: citra's texture_codec.h) */
static void
nh3ds_atlas_init(void)
{
    /* within an 8x8 tile, row y starts at morton_row[y] texels and
     * column x adds morton_x[x]; linear image row r lands on morton
     * row r (verified empirically: sprite row s samples morton row s,
     * so storing the glyph rows in order keeps them upright) */
    static const u32 morton_row[8] = { 0x00, 0x02, 0x08, 0x0A, 0x20, 0x22,
                                       0x28, 0x2A };
    static const u32 morton_x[8] = { 0x00, 0x01, 0x04, 0x05, 0x10, 0x11,
                                     0x14, 0x15 };
    static u32 linbuf[NH3DS_ATLAS_W * NH3DS_ATLAS_H];
    static u32 tiled[NH3DS_ATLAS_W * NH3DS_ATLAS_H];
    int ch, r, x, tx, ty;

    for (ch = 0; ch < NH3DS_FONTCHARS; ++ch) {
        int ax = (ch & 31) * 8, ay = (ch >> 5) * 8;

        for (r = 0; r < 8; ++r) {
            u8 bits = default_font_bin[ch * 8 + r];
            u32 *dst = &linbuf[(ay + r) * NH3DS_ATLAS_W + ax];

            for (x = 0; x < 8; ++x)
                dst[x] = (bits & (0x80u >> x)) ? 0xFFFFFFFFu : 0u;
        }
    }
    for (ty = 0; ty < NH3DS_ATLAS_H / 8; ++ty) {
        for (tx = 0; tx < NH3DS_ATLAS_W / 8; ++tx) {
            u32 *tile = &tiled[(ty * (NH3DS_ATLAS_W / 8) + tx) * 64];

            for (r = 0; r < 8; ++r) {
                const u32 *src =
                    &linbuf[(ty * 8 + r) * NH3DS_ATLAS_W + tx * 8];
                u32 moff = morton_row[r];

                for (x = 0; x < 8; ++x)
                    tile[moff + morton_x[x]] = src[x];
            }
        }
    }
    if (!C3D_TexInit(&nh3ds_glyph_tex, NH3DS_ATLAS_W, NH3DS_ATLAS_H,
                     GPU_RGBA8))
        return;
    C3D_TexUpload(&nh3ds_glyph_tex, tiled);
    C3D_TexSetFilter(&nh3ds_glyph_tex, GPU_NEAREST, GPU_NEAREST);
    C3D_TexSetWrap(&nh3ds_glyph_tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
    C3D_TexFlush(&nh3ds_glyph_tex);
    for (ch = 0; ch < NH3DS_FONTCHARS; ++ch) {
        int ax = (ch & 31) * 8, ay = (ch >> 5) * 8;

        nh3ds_glyph_sub[ch].width = 8;
        nh3ds_glyph_sub[ch].height = 8;
        nh3ds_glyph_sub[ch].left = (float) ax / NH3DS_ATLAS_W;
        nh3ds_glyph_sub[ch].right = (float) (ax + 8) / NH3DS_ATLAS_W;
        nh3ds_glyph_sub[ch].top = 1.0f - (float) ay / NH3DS_ATLAS_H;
        nh3ds_glyph_sub[ch].bottom =
            1.0f - (float) (ay + 8) / NH3DS_ATLAS_H;
    }
}

/* the NetHack classic palette; index by the stored color value
 * (-1/default maps to NO_COLOR's light gray, like the console did) */
static void
nh3ds_palette_init(void)
{
    nh3ds_rgb[0] = C2D_Color32(110, 110, 110, 255); /* CLR_BLACK */
    nh3ds_rgb[1] = C2D_Color32(255, 64, 64, 255);   /* CLR_RED */
    nh3ds_rgb[2] = C2D_Color32(0, 208, 0, 255);     /* CLR_GREEN */
    nh3ds_rgb[3] = C2D_Color32(176, 112, 32, 255);  /* CLR_BROWN */
    nh3ds_rgb[4] = C2D_Color32(96, 112, 255, 255);  /* CLR_BLUE */
    nh3ds_rgb[5] = C2D_Color32(255, 0, 255, 255);   /* CLR_MAGENTA */
    nh3ds_rgb[6] = C2D_Color32(0, 224, 224, 255);   /* CLR_CYAN */
    nh3ds_rgb[7] = C2D_Color32(176, 176, 176, 255); /* CLR_GRAY */
    nh3ds_rgb[8] = C2D_Color32(200, 200, 200, 255); /* NO_COLOR/default */
    nh3ds_rgb[9] = C2D_Color32(255, 144, 0, 255);   /* CLR_ORANGE */
    nh3ds_rgb[10] = C2D_Color32(0, 255, 0, 255);    /* BRIGHT_GREEN */
    nh3ds_rgb[11] = C2D_Color32(255, 232, 0, 255);  /* CLR_YELLOW */
    nh3ds_rgb[12] = C2D_Color32(136, 152, 255, 255);/* BRIGHT_BLUE */
    nh3ds_rgb[13] = C2D_Color32(255, 128, 255, 255);/* BRIGHT_MAGENTA */
    nh3ds_rgb[14] = C2D_Color32(128, 255, 255, 255);/* BRIGHT_CYAN */
    nh3ds_rgb[15] = C2D_Color32(255, 255, 255, 255);/* CLR_WHITE */
}

static int
nh3ds_tintidx(int clr)
{
    return (clr < 0 || clr > 15) ? 8 : clr;
}

/* the palette at greatly reduced brightness (C2D_Color32 packs ABGR):
 * the under-the-hero glyphs use it so they stay out of the hero glyph's
 * way when the slider is near the bottom and the layers overlap */
static u32
nh3ds_dim_rgb(u32 c)
{
    u32 r = (c & 0xFFu) * 3 / 10;
    u32 g = ((c >> 8) & 0xFFu) * 3 / 10;
    u32 b = ((c >> 16) & 0xFFu) * 3 / 10;

    return (c & 0xFF000000u) | (b << 16) | (g << 8) | r;
}

/* one 8x8 glyph sprite at pixel position (px,py); spaces and unknown
 * characters draw nothing (the atlas background is transparent, so
 * deeper layers show through the gaps); `dim` picks the reduced-
 * brightness tint (under-the-hero layer) */
static void
nh3ds_glyph(char chr, int clr, float px, float py, boolean dim)
{
    unsigned ch = (unsigned char) chr;
    C2D_Image img;

    if (ch == ' ' || ch == '\0')
        return;
    if (ch >= NH3DS_FONTCHARS)
        ch = '?';
    img.tex = &nh3ds_glyph_tex;
    img.subtex = &nh3ds_glyph_sub[ch];
    (void) C2D_DrawImageAt(img, px, py, 0.5f,
                           dim ? &nh3ds_tints_dim[nh3ds_tintidx(clr)]
                               : &nh3ds_tints[nh3ds_tintidx(clr)],
                           1.0f, 1.0f);
}

/* a text run on the UI plane (screen depth): col/row in 8x8 cells */
static void
nh3ds_text(int col, int row, const char *s, int clr)
{
    float px = (float) (col * 8);
    float py = (float) (row * 8);

    for (; *s; ++s, px += 8.0f)
        nh3ds_glyph(*s, clr, px, py, FALSE);
}

/* is this glyph flat terrain that belongs on the floor plane? */
static boolean
nh3ds_flat_glyph(int glyph)
{
    int c;

    if (!glyph_is_cmap(glyph))
        return FALSE;
    c = glyph_to_cmap(glyph);
    return is_cmap_room(c) || c == S_corr || c == S_litcorr || c == S_stone
           || c == S_engroom || c == S_engrcorr || c == S_ice || c == S_air
           || c == S_cloud || c == S_pool || c == S_water || c == S_lava
           || c == S_ndoor || c == S_vodbridge || c == S_hodbridge;
}

/* which depth layer does a foreground glyph belong to?  (The hero's
 * own glyph is decided per cell -- see nh3ds_cell_layer.) */
static int
nh3ds_glyph_layer(int glyph)
{
    int c, tt;

    if (glyph_is_cmap(glyph)) {
        c = glyph_to_cmap(glyph);
        if (is_cmap_stairs(c))
            return (c == S_upstair || c == S_upladder || c == S_brupstair
                    || c == S_brupladder) ? NH3DS_D_FURNITURE : NH3DS_D_DOWN;
        if (is_cmap_wall(c) || c == S_tree || c == S_bars || c == S_lavawall
            || c == S_vcdbridge || c == S_hcdbridge
            || c == S_hcdoor || c == S_vcdoor || c == S_hodoor
            || c == S_vodoor)
            return NH3DS_D_WALL;
        if (c == S_altar || c == S_grave || c == S_throne || c == S_sink
            || c == S_fountain)
            return NH3DS_D_FURNITURE;
        return NH3DS_D_FLOOR;
    }
    if (glyph_is_trap(glyph)) {
        tt = glyph_to_trap(glyph);
        if (is_pit(tt) || is_hole(tt))
            return NH3DS_D_DOWN;    /* an opening below the floor */
        return (tt == WEB) ? NH3DS_D_FURNITURE : NH3DS_D_FLOOR;
    }
    if (glyph_is_object(glyph) || glyph_is_monster(glyph)
        || glyph_is_swallow(glyph) || glyph_is_explosion(glyph)
        || glyph_is_warning(glyph) || glyph_is_invisible(glyph))
        return NH3DS_D_ENTITY;
    return NH3DS_D_WALL;
}

/* layer of a map cell; the hero's glyph rises a plane while
 * levitating or flying */
static int
nh3ds_cell_layer(const struct nh3ds_cell *cell)
{
    if (cell->gflags & MG_HERO) {
        if (nh3ds_debug_lev)
            return NH3DS_D_LEV; /* visual test of the levitation plane */
        return (nh3ds_st_cond & (BL_MASK_LEV | BL_MASK_FLY))
                   ? NH3DS_D_LEV : NH3DS_D_ENTITY;
    }
    return nh3ds_glyph_layer(cell->glyph);
}

/* the map viewport for one eye: every glyph drawn on its layer's
 * plane; `eye` is -1 (left) or +1 (right), `slider` scales the
 * parallax (0 collapses all layers onto the screen plane) */
static void
nh3ds_draw_map_eye(float eye, float slider)
{
    int layer;
    int hx = -1, hy = -1;

    /* painter's order: the deepest layer first */
    for (layer = NH3DS_D_DOWN; layer >= NH3DS_D_WALL; --layer) {
        float dx = eye * 0.5f * slider * (float) nh3ds_depth[layer];
        int cy, mx;

        if (layer == NH3DS_D_UNDER && slider < 0.05f)
            continue; /* flat mode: the under-glyphs would only smear
                       * into the hero glyph -- like tty, the hero
                       * hides what it stands on */

        for (cy = 0; cy < MAP_ROWS; ++cy) {
            int my = (int) nh3ds_vy + cy;
            float py = (float) ((MAP_ROW0 + cy) * 8);

            if (my < 0 || my >= ROWNO)
                continue;
            for (mx = 0; mx < MAP_VIEW_COLS; ++mx) {
                int cx = (int) nh3ds_vx + mx;
                struct nh3ds_cell *cell, *bg, *un;
                int lay;
                float px;

                if (cx < 0 || cx >= COLNO)
                    continue;
                cell = &nh3ds_map[my][cx];
                bg = &nh3ds_mapbg[my][cx];
                un = &nh3ds_mapunder[my][cx];
                if (cell->gflags & MG_HERO)
                    hx = cx, hy = my;
                if (!cell->c || cell->c == ' ')
                    continue;
                lay = nh3ds_cell_layer(cell);
                px = (float) (mx * 8) + dx;
                if (lay == layer)
                    nh3ds_glyph(cell->c, cell->color, px, py, FALSE);
                /* terrain behind whatever stands on it -- not behind
                 * walls (they have no floor footprint), not under
                 * the floor's own glyphs (that would double-draw) and
                 * not behind down-openings (stairs/pits are holes in
                 * the floor; the floor plane would cover them) */
                if (layer == NH3DS_D_FLOOR && lay != NH3DS_D_WALL
                    && lay != NH3DS_D_FLOOR && lay != NH3DS_D_DOWN
                    && bg->c && bg->c != ' ')
                    /* the tty shows the primary (above) glyph on this
                     * cell, so the terrain behind it is "under
                     * something" -- keep it well out of its way */
                    nh3ds_glyph(bg->c, bg->color, px, py, TRUE);
                /* what the hero stands on, one plane above the floor;
                 * stays put while the hero levitates away from it.
                 * Floor-like glyphs are skipped (the background terrain
                 * already shows them) and so are stale monsters and
                 * transient effect glyphs. */
                if (layer == NH3DS_D_UNDER && (cell->gflags & MG_HERO)
                    && un->c && un->c != ' ' && un->c != bg->c
                    && !nh3ds_flat_glyph(un->glyph)
                    && !glyph_is_monster(un->glyph)
                    && !glyph_is_swallow(un->glyph)
                    && !glyph_is_explosion(un->glyph)
                    && !glyph_is_warning(un->glyph))
                    nh3ds_glyph(un->c, un->color, px, py, TRUE);
            }
        }
    }

    /* the map cursor (getpos -- farlook/travel targeting): a
     * translucent lightblue cube around the targeted tile, two faces
     * one step of depth either side of the tile's own plane; hidden
     * while the cursor sits on the hero */
    if (hx >= 0 && nh3ds_cur_x >= 0 && nh3ds_cur_y >= 0
        && nh3ds_cur_y < ROWNO && nh3ds_cur_x < COLNO
        && !(nh3ds_cur_x == hx && nh3ds_cur_y == hy)) {
        struct nh3ds_cell *cur = &nh3ds_map[nh3ds_cur_y][nh3ds_cur_x];
        float cdepth = (cur->c && cur->c != ' ')
                          ? (float) nh3ds_depth[nh3ds_cell_layer(cur)]
                          : (float) nh3ds_depth[NH3DS_D_FLOOR];
        int cmx = nh3ds_cur_x - (int) nh3ds_vx;
        int cmy = nh3ds_cur_y - (int) nh3ds_vy;

        if (cmx >= 0 && cmx < MAP_VIEW_COLS && cmy >= 0 && cmy < MAP_ROWS) {
            float cpx = (float) (cmx * 8);
            float cpy = (float) ((MAP_ROW0 + cmy) * 8);

            C2D_DrawRectSolid(cpx + eye * 0.5f * slider * (cdepth + 3.0f),
                              cpy, 0.5f, 8.0f, 8.0f,
                              C2D_Color32(136, 152, 255, 77));
            C2D_DrawRectSolid(cpx + eye * 0.5f * slider * (cdepth - 3.0f),
                              cpy, 0.5f, 8.0f, 8.0f,
                              C2D_Color32(136, 152, 255, 77));
        }
    }
}

static void
nh3ds_draw_messages(void)
{
    int i, oldest, n;

    oldest = (nh3ds_msg_head - nh3ds_msg_count + NH3DS_NMSGS) % NH3DS_NMSGS;
    oldest = (oldest + nh3ds_msg_count
              - (nh3ds_msg_count < MSG_ROWS ? nh3ds_msg_count : MSG_ROWS))
             % NH3DS_NMSGS;
    n = (nh3ds_msg_count < MSG_ROWS) ? nh3ds_msg_count : MSG_ROWS;
    for (i = 0; i < MSG_ROWS; ++i) {
        const char *s = "";

        if (i < n)
            s = nh3ds_msgs[(oldest + i) % NH3DS_NMSGS];
        nh3ds_text(0, MSG_ROW0 + i, s, -1);
    }
}

/* append one message line to the ring; word-wrap s at the bottom-screen
 * width (40) so long messages stay readable on both screens instead of
 * being truncated */

/* shared by the 3ddebug walker helpers below */
static const char nh3ds_dirkey[] = "hjklyubn";
static const int nh3ds_dx8[8] = { -1, 1, 0, 0, -1, 1, -1, 1 };
static const int nh3ds_dy8[8] = { 0, 0, -1, 1, -1, -1, 1, 1 };
/* set/cleared by the message handler as hunger messages scroll by */
static boolean nh3ds_demo_hungry;

static void
nh3ds_msg_add(const char *s)
{
    char buf[NH3DS_MSGLEN];
    const char *p;

    if (!s)
        s = "";
    (void) strncpy(buf, s, NH3DS_MSGLEN - 1);
    buf[NH3DS_MSGLEN - 1] = '\0';
    /* the 3ddebug walker grazes on this: hunger messages scroll by
     * wrapped, but nh3ds_msg_add sees each full line once */
    if (strstr(buf, "feel hungry") || strstr(buf, "feel weak")
        || strstr(buf, "faint"))
        nh3ds_demo_hungry = TRUE;
    else if (strstr(buf, "no longer hungry"))
        nh3ds_demo_hungry = FALSE;
    p = buf;
    while (*p) {
        char chunk[BOT_COLS + 1];
        int len = (int) strlen(p);
        const char *cut;

        if (len <= BOT_COLS) {
            cut = p + len;
        } else {
            cut = p + BOT_COLS; /* break at the last space in range */
            while (cut > p && *cut != ' ')
                --cut;
            if (cut == p)       /* no space at all: hard break */
                cut = p + BOT_COLS;
        }
        len = (int) (cut - p);
        memcpy(chunk, p, (size_t) len);
        chunk[len] = '\0';
        p = cut;
        while (*p == ' ')
            ++p;
        strncpy(nh3ds_msgs[nh3ds_msg_head], chunk, NH3DS_MSGLEN - 1);
        nh3ds_msgs[nh3ds_msg_head][NH3DS_MSGLEN - 1] = '\0';
        nh3ds_msg_head = (nh3ds_msg_head + 1) % NH3DS_NMSGS;
        if (nh3ds_msg_count < NH3DS_NMSGS)
            ++nh3ds_msg_count;
    }
    nh3ds_msg_dirty = TRUE;
}

/* strip trailing blanks (the core blank-pads some field values) */
static void
nh3ds_trim(char *s)
{
    size_t n = strlen(s);

    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t'))
        s[--n] = '\0';
}

/* condition bits -> compact tags for the status line */static const struct {
    unsigned long mask;
    const char *tag;
} nh3ds_cond[] = {
    { BL_MASK_BAREH, "Bare" },       { BL_MASK_BLIND, "Blind" },
    { BL_MASK_BUSY, "Busy" },        { BL_MASK_CONF, "Conf" },
    { BL_MASK_DEAF, "Deaf" },        { BL_MASK_ELF_IRON, "ElfIron" },
    { BL_MASK_FLY, "Fly" },          { BL_MASK_FOODPOIS, "FoodPois" },
    { BL_MASK_GLOWHANDS, "Glow" },   { BL_MASK_GRAB, "Grab" },
    { BL_MASK_HALLU, "Hallu" },      { BL_MASK_HELD, "Held" },
    { BL_MASK_ICY, "Icy" },          { BL_MASK_INLAVA, "Lava" },
    { BL_MASK_LEV, "Lev" },          { BL_MASK_PARLYZ, "Parlyz" },
    { BL_MASK_RIDE, "Ride" },        { BL_MASK_SLEEPING, "Sleep" },
    { BL_MASK_SLIME, "Slime" },      { BL_MASK_SLIPPERY, "Slip" },
    { BL_MASK_STONE, "Stone" },      { BL_MASK_STRNGL, "Strngl" },
    { BL_MASK_STUN, "Stun" },        { BL_MASK_SUBMERGED, "Subm" },
    { BL_MASK_TERMILL, "TermIll" },  { BL_MASK_TETHERED, "Teth" },
    { BL_MASK_TRAPPED, "Trap" },     { BL_MASK_UNCONSC, "Uncon" },
    { BL_MASK_WOUNDEDL, "WoundedL" },{ BL_MASK_HOLDING, "Hold" },
};

/* build one status row from an ordered list of fields; a field only
 * lands if its formatted form fits in the remaining space */
static void
nh3ds_st_row(int row, const int *flds, int nflds, boolean use_cond)
{
    int col = 0, i;

    for (i = 0; i < nflds; ++i) {
        int fld = flds[i];
        char seg[TOP_COLS + 1];

        if (fld < 0 || fld >= MAXBLSTATS || !nh3ds_st_active[fld])
            continue;
        if (!nh3ds_st_val[fld][0])
            continue; /* field disabled or not (yet) reported */
        if (nh3ds_st_fmt[fld][0])
            (void) snprintf(seg, sizeof seg, nh3ds_st_fmt[fld],
                            nh3ds_st_val[fld]);
        else
            (void) snprintf(seg, sizeof seg, "%s", nh3ds_st_val[fld]);
        if (!*seg || !strcmp(seg, " "))
            continue;
        if (col + (int) strlen(seg) > TOP_COLS)
            break;
        nh3ds_text(col, ST_ROW0 + row, seg, nh3ds_st_color[fld]);
        col += (int) strlen(seg);
    }
    if (use_cond) {
        int k;

        for (k = 0; k < (int) (sizeof nh3ds_cond / sizeof nh3ds_cond[0]);
             ++k) {
            char seg[16];

            if ((nh3ds_st_cond & nh3ds_cond[k].mask) == 0L)
                continue;
            (void) snprintf(seg, sizeof seg, " %s", nh3ds_cond[k].tag);
            if (col + (int) strlen(seg) > TOP_COLS)
                break;
            nh3ds_text(col, ST_ROW0 + row, seg, 11 /* yellow */);
            col += (int) strlen(seg);
        }
    }
}

static void
nh3ds_draw_status(void)
{
    /* row 1: vitals; row 2: world + attributes + conditions */
    static const int row1flds[] = {
        BL_TITLE, BL_HP, BL_HPMAX, BL_ENE, BL_ENEMAX, BL_XP, BL_EXP,
        BL_AC, BL_HD, BL_GOLD, BL_ALIGN, BL_SCORE, BL_CAP,
    };
    static const int row2flds[] = {
        BL_LEVELDESC, BL_STR, BL_DX, BL_CO, BL_IN, BL_WI, BL_CH,
        BL_TIME, BL_HUNGER, BL_WEAPON, BL_ARMOR, BL_TERRAIN,
    };
    static unsigned long nh3ds_st_draw_n;
    int i;

    ++nh3ds_st_draw_n;
    if (nh3ds_st_draw_n % 4096 == 0) {
        for (i = 0; i < MAXBLSTATS; ++i)
            if (nh3ds_st_val[i][0])
                nh3ds_log("[st draw#%lu fld=%d] '%s'", nh3ds_st_draw_n, i,
                          nh3ds_st_val[i]);
    }

    nh3ds_st_row(0, row1flds, (int) (sizeof row1flds / sizeof row1flds[0]),
                 FALSE);
    nh3ds_st_row(1, row2flds, (int) (sizeof row2flds / sizeof row2flds[0]),
                 TRUE);
}

static const char *const nh3ds_legend[TOP_ROWS - LEG_ROW0] = {
    "pad=hjkl 2=diag L+pad=run R+pad=travel",
    "A=rest B=inv X=grab Y=search L=shift",
    "START=save SELECT=help touch=buttons",
    "k:keys = full keyboard digits ^cmds",
};

static void
nh3ds_draw_legend(void)
{
    int i;

    for (i = 0; i < TOP_ROWS - LEG_ROW0; ++i)
        nh3ds_text(0, LEG_ROW0 + i, nh3ds_legend[i], -1);
}

/* ----------------------------------------------------------------------*/
/* top-screen view compositor and the citro3d frame loop                  */
/* ----------------------------------------------------------------------*/

static int
nh3ds_menu_selectable(int i); /* defined with the menu code below */

/* the ext-command list state (drawn from the top-screen renderer) */
#define NH3DS_EXTMAX 128
static struct {
    int idx[NH3DS_EXTMAX];   /* all available commands */
    int match[NH3DS_EXTMAX]; /* visible after the prefix filter */
    int n, nm, mtop, cursor, flen;
    char filter[24];
} nh3ds_ext;

/* buffered menu on the top screen (rows: 0 prompt, 1..26 items,
 * 28 footer) */
static void
nh3ds_draw_menu(void)
{
    int rows = 26, row, i, top = nh3ds_menu.top;
    char line[128];

    nh3ds_text(0, 0, nh3ds_menu.prompt, 15);
    for (row = 0, i = top; i < nh3ds_menu.n && row < rows; ++i, ++row) {
        const struct nh3ds_menuitem *m = &nh3ds_menu.item[i];
        const char *tag = "  ";

        if (nh3ds_menu_selectable(i)) {
            tag = (nh3ds_menu.cursor == i) ? "> " : "  ";
            if (m->itemflags & MENU_ITEMFLAGS_SELECTED)
                tag = "* ";
        }
        if (m->ch && m->ch != ' ')
            (void) snprintf(line, sizeof line, "%s%c) %s", tag, m->ch,
                            m->str);
        else
            (void) snprintf(line, sizeof line, "%s%s", tag, m->str);
        nh3ds_text(0, row + 1, line, nh3ds_menu_selectable(i) ? -1 : 14);
    }
    nh3ds_text(0, TOP_ROWS - 2, "pad=move A=ok B=cancel letter=select", 6);
}

/* ext-command list (rows: 0 "#filter_", 1..26 matches, 28 footer) */
static void
nh3ds_draw_ext(void)
{
    char line[64];
    int i;

    (void) snprintf(line, sizeof line, "#%.22s_", nh3ds_ext.filter);
    nh3ds_text(0, 0, line, 15);
    for (i = nh3ds_ext.mtop; i < nh3ds_ext.nm && i < nh3ds_ext.mtop + 26;
         ++i) {
        const struct ext_func_tab *ef = &extcmdlist[nh3ds_ext.match[i]];

        (void) snprintf(line, sizeof line, "%s%-14.14s %.32s",
                        (i == nh3ds_ext.cursor) ? "> " : "  ", ef->ef_txt,
                        ef->ef_desc ? ef->ef_desc : "");
        nh3ds_text(0, i - nh3ds_ext.mtop + 1, line, -1);
    }
    if (!nh3ds_ext.nm)
        nh3ds_text(0, 1, "  (no matching command)", 1);
    nh3ds_text(0, TOP_ROWS - 2, "A=do B=bsp START=cancel pad=move", 6);
}

/* hold-on-exit lines, written over the (frozen) game view */
static void
nh3ds_draw_exit(void)
{
    char line[TOP_COLS + 1];

    if (nh3ds_lastwords[0]) {
        (void) snprintf(line, sizeof line, "%.50s", nh3ds_lastwords);
        nh3ds_text(0, 26, line, 1);
    }
    nh3ds_text(0, 28, "exit -- press START to return to the launcher", 6);
}

static void
nh3ds_draw_view(float eye, float slider)
{
    switch (nh3ds_view) {
    case NH3DS_VM_EXIT:
        nh3ds_draw_messages();
        nh3ds_draw_map_eye(eye, slider);
        nh3ds_draw_status();
        nh3ds_draw_exit();
        break;
    case NH3DS_VM_MENU:
        nh3ds_draw_menu();
        break;
    case NH3DS_VM_EXT:
        nh3ds_draw_ext();
        break;
    default: /* NH3DS_VM_GAME */
        nh3ds_draw_messages();
        nh3ds_draw_map_eye(eye, slider);
        nh3ds_draw_status();
        nh3ds_draw_legend();
        break;
    }
}

/* debug: write both eyes' top-screen framebuffers as 400x240 BMPs into
 * a 32-slot ring (enabled by a "3drig" file in the playground) */
static void
nh3ds_debug_dump(void)
{
    static const char *const eye[2] = { "L", "R" };
    gfx3dSide_t sides[2] = { GFX_LEFT, GFX_RIGHT };
    char fn[24];
    int i;

    gspWaitForPPF(); /* wait for the display transfer into the LCD fb */
    for (i = 0; i < 2; ++i) {
        u8 *fb = (u8 *) gfxGetFramebuffer(GFX_TOP, sides[i], NULL, NULL);
        u8 hdr[54], row[400 * 3];
        u32 sz = 54u + 400u * 240u * 3u;
        FILE *f;
        int sx, sy;

        if (!fb)
            continue;
        (void) snprintf(fn, sizeof fn, "dump3d_%02lu_%s.bmp",
                        (unsigned long) (nh3ds_frame_n / 64) % 32, eye[i]);
        (void) memset(hdr, 0, sizeof hdr);
        hdr[0] = 'B', hdr[1] = 'M';
        hdr[2] = (u8) sz, hdr[3] = (u8) (sz >> 8);
        hdr[4] = (u8) (sz >> 16), hdr[5] = (u8) (sz >> 24);
        hdr[10] = 54;                /* pixel data offset */
        hdr[14] = 40;                /* BITMAPINFOHEADER size */
        hdr[18] = (u8) 400;          /* width, little-endian */
        hdr[19] = (u8) (400 >> 8);
        hdr[22] = (u8) 240;          /* height, little-endian */
        hdr[23] = (u8) (240 >> 8);
        hdr[26] = 1;                 /* planes */
        hdr[28] = 24;                /* bits per pixel */
        f = fopen(fn, "wb");
        if (!f)
            continue;
        (void) fwrite(hdr, 1, sizeof hdr, f);
        /* the framebuffer is 240x400, column-major with its origin at
         * the screen's bottom-left: fb pixel p of column sx is screen
         * row (239 - p); BMP rows are bottom-up (file row 0 = image
         * bottom = screen bottom = framebuffer pixel 0), so ascending
         * framebuffer rows land the image upright */
        for (sy = 0; sy < 240; ++sy) {
            for (sx = 0; sx < 400; ++sx) {
                const u8 *px = fb + ((u32) sx * 240 + (u32) sy) * 3;

                row[sx * 3] = px[0];
                row[sx * 3 + 1] = px[1];
                row[sx * 3 + 2] = px[2];
            }
            (void) fwrite(row, 1, sizeof row, f);
        }
        (void) fclose(f);
    }
}

/* one citro3d frame: both eyes when the slider is up, else only the
 * left one (a flat picture) */
static void
nh3ds_frame_draw(void)
{
    float slider = nh3ds_debug3d ? 1.0f : (float) osGet3DSliderState();
    boolean stereo = slider > 0.001f;

    if (stereo != nh3ds_stereo) {
        gfxSet3D(stereo);
        nh3ds_stereo = stereo;
    }
    C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
    C2D_TargetClear(nh3ds_rt_left, C2D_Color32(0, 0, 0, 255));
    C2D_SceneBegin(nh3ds_rt_left);
    C2D_Prepare();
    nh3ds_draw_view(-1.0f, slider);
    if (stereo) {
        C2D_TargetClear(nh3ds_rt_right, C2D_Color32(0, 0, 0, 255));
        C2D_SceneBegin(nh3ds_rt_right);
        C2D_Prepare();
        nh3ds_draw_view(1.0f, slider);
    }
    C3D_FrameEnd(C3D_FRAME_SYNCDRAW);
}

/* redraw the top screen; called whenever anything is dirty.  The LCD
 * keeps scanning out the last transferred frame, so turns without
 * visible changes cost nothing. */
static void
nh3ds_render_frame(void)
{
    if (!nh3ds_gpu_ok)
        return;
    nh3ds_frame_draw();
    ++nh3ds_frame_n;
    if (nh3ds_debug3d && nh3ds_frame_n % 64 == 0)
        nh3ds_debug_dump();
}

static void
nh3ds_gpu_init(void)
{
    int i;

    nh3ds_gpu_ok = FALSE;
    if (!C3D_Init(C3D_DEFAULT_CMDBUF_SIZE)) {
        nh3ds_log("[gpu] C3D_Init failed");
        return;
    }
    if (!C2D_Init(C2D_DEFAULT_MAX_OBJECTS)) {
        C3D_Fini();
        nh3ds_log("[gpu] C2D_Init failed");
        return;
    }
    nh3ds_rt_left = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
    nh3ds_rt_right = C2D_CreateScreenTarget(GFX_TOP, GFX_RIGHT);
    if (!nh3ds_rt_left || !nh3ds_rt_right) {
        C2D_Fini();
        C3D_Fini();
        nh3ds_log("[gpu] screen target creation failed");
        return;
    }
    (void) C2D_SetTintMode(C2D_TintMult);
    nh3ds_palette_init();
    for (i = 0; i < 16; ++i) {
        C2D_PlainImageTint(&nh3ds_tints[i], nh3ds_rgb[i], 1.0f);
        C2D_PlainImageTint(&nh3ds_tints_dim[i], nh3ds_dim_rgb(nh3ds_rgb[i]),
                           1.0f);
    }
    nh3ds_atlas_init();
    gfxSet3D(FALSE);
    nh3ds_stereo = FALSE;
    nh3ds_gpu_ok = TRUE;
}

/* render the popup buffer (wrapped to BOT_COLS) onto the bottom screen */
static void
nh3ds_draw_popup(void)
{
    int row = 0, i;

    nh3ds_scr(0);
    printf("\x1b[2J\x1b[0m");
    if (nh3ds_pop_prompt[0]) {
        printf("\x1b[1;1H\x1b[37;1m%-40.40s", nh3ds_pop_prompt);
        row = 1;
    }
    for (i = 0; i < nh3ds_pop_n && row < BOT_ROWS - 1; ++i) {
        const char *s = nh3ds_pop_lines[i];

        while (*s && row < BOT_ROWS - 1) {
            char chunk[NH3DS_POPLINELEN];
            int n = 0;

            while (s[n] && n < BOT_COLS)
                ++n;
            if (s[n]) { /* wrap at the last space within the chunk */
                int j = n;

                while (j > 0 && s[j] != ' ')
                    --j;
                if (j > 0)
                    n = j;
            }
            (void) memcpy(chunk, s, (size_t) n);
            chunk[n] = '\0';
            printf("\x1b[%d;1H\x1b[0m%-40.40s", row + 2, chunk);
            ++row;
            s += n;
            while (*s == ' ')
                ++s;
        }
    }
    printf("\x1b[%d;1H\x1b[0m%-40.40s", BOT_ROWS, "[A] continue");
}

static void nh3ds_bot_buttons(void); /* touch verb grid (defined below) */

/* bottom screen default view: message-history tail + verb buttons */
static void
nh3ds_draw_bot_msgs(void)
{
    int i, n, oldest;

    nh3ds_scr(0);
    printf("\x1b[2J\x1b[0m");
    n = (nh3ds_msg_count < TBOT_MSGROWS) ? nh3ds_msg_count : TBOT_MSGROWS;
    oldest = (nh3ds_msg_head - nh3ds_msg_count + NH3DS_NMSGS) % NH3DS_NMSGS;
    oldest = (oldest + nh3ds_msg_count - n) % NH3DS_NMSGS;
    for (i = 0; i < n; ++i)
        printf("\x1b[%d;1H\x1b[0m%-40.40s", i + 1,
               nh3ds_msgs[(oldest + i) % NH3DS_NMSGS]);
    nh3ds_bot_buttons();
}

/* the touch letter-keyboard overlay is open (it owns the taps) */
static boolean nh3ds_keys_open;

/* any button or tap since boot: once set, blocking displays wait for a
 * real press instead of auto-continuing after a few frames (the rig
 * relies on the auto-continue; a real player always touches something
 * during name entry and role setup first) */
static boolean nh3ds_any_button_ever;

/* wait for any button press (edge-triggered: must be released first);
 * returns the button bits, or 0 if it auto-continued because no
 * interaction has happened yet */
static u32
nh3ds_wait_button(void)
{
    u32 held;
    int frames = 0;

    do {
        hidScanInput();
        gspWaitForVBlank();
        held = hidKeysHeld();
    } while (held != 0);
    for (;;) {
        hidScanInput();
        gspWaitForVBlank();
        held = hidKeysHeld();
        if (held != 0) {
            nh3ds_any_button_ever = TRUE;
            return held;
        }
        if (nh3ds_any_button_ever)
            continue; /* a real player is present: wait indefinitely */
        if (++frames >= 6)
            return 0; /* no interaction yet: auto-continue (a page wait
                       * would cost seconds of wall time per --More--
                       * at the emulator's reduced speed; the rig
                       * depends on this) */
    }
}

/* ----------------------------------------------------------------------*/
/* helpers                                                                */
/* ----------------------------------------------------------------------*/

static void
nh3ds_ensure_file(const char *fn)
{
    FILE *f = fopen(fn, "ab");

    if (f)
        (void) fclose(f);
}

static const char *
nh3ds_wintypename(int type)
{
    switch (type) {
    case NHW_MESSAGE: return "MESSAGE";
    case NHW_STATUS:  return "STATUS";
    case NHW_MAP:     return "MAP";
    case NHW_MENU:    return "MENU";
    case NHW_TEXT:    return "TEXT";
    default:          return "?";
    }
}

/* refresh any dirty region; called once per turn before returning input */
static void
nh3ds_refresh(void)
{
    if (nh3ds_msg_dirty) {
        /* the bottom screen is the message-history tail; redraw it in
         * step.  (No guard for popups/menus needed: those waits are
         * synchronous inside our own handlers, so nh_poskey -- the only
         * refresh() caller -- cannot fire while the bottom is owned;
         * the perm_invent window keeps a MENU winid alive forever,
         * which would block the tail forever if we tested pop_win.) */
        nh3ds_draw_bot_msgs();
    }
    if (nh3ds_map_dirty || nh3ds_msg_dirty || nh3ds_st_dirty)
        nh3ds_render_frame();
    nh3ds_map_dirty = nh3ds_msg_dirty = nh3ds_st_dirty = FALSE;
}

/* full redraw after something else owned a screen (menus render on the
 * top screen and wipe the map); hands the top screen back to the game
 * view -- nothing else resets nh3ds_view once a menu or the ext-command
 * list has claimed it */
static void
nh3ds_full_redraw(void)
{
    nh3ds_view = NH3DS_VM_GAME;
    nh3ds_msg_dirty = TRUE;
    nh3ds_map_dirty = TRUE;
    nh3ds_st_dirty = TRUE;
    nh3ds_refresh();
}

/* rows the popup renderer will spend on s: same wrap rule as
 * nh3ds_draw_popup (chop at BOT_COLS, back up to the last space) */
static int
nh3ds_wrap_rows(const char *s)
{
    int rows = 0;

    while (*s) {
        int n = 0;

        while (s[n] && n < BOT_COLS)
            ++n;
        if (s[n]) { /* wrap at the last space within the chunk */
            int j = n;

            while (j > 0 && s[j] != ' ')
                --j;
            if (j > 0)
                n = j;
        }
        ++rows;
        s += n;
        while (*s == ' ')
            ++s;
    }
    return rows;
}

/* shim_display_file: page a data file (help, the data.base, history,
 * guidebook excerpts -- dlb reaches into nhdat) onto the bottom screen
 * through the popup buffer.  One screenful per button press, B skips
 * the rest.  This used to be a silent no-op, which left '?' (the
 * SELECT key) doing nothing at all. */
static void
nh3ds_display_file(const char *fname, boolean complain)
{
    dlb *fp;
    char line[NH3DS_POPLINELEN];
    boolean held = FALSE; /* line[] read but not shown yet */
    boolean stop = FALSE;

    fp = dlb_fopen(fname, "r");
    if (!fp) {
        if (complain)
            pline("Cannot open %s.", fname);
        nh3ds_log("[display_file] cannot open %s", fname);
        return;
    }
    nh3ds_keys_open = FALSE; /* the popup owns the bottom now */
    while (!stop) {
        int rows = 0;

        nh3ds_pop_n = 0;
        nh3ds_pop_prompt[0] = '\0';
        while (nh3ds_pop_n < NH3DS_POPLINES) {
            int r;

            if (!held) {
                if (!dlb_fgets(line, (int) sizeof line, fp))
                    break; /* end of file */
                line[strcspn(line, "\r\n")] = '\0';
                held = TRUE;
            }
            r = (line[0] != '\0') ? nh3ds_wrap_rows(line) : 1;
            if (rows + r > NH3DS_PAGEROWS && nh3ds_pop_n > 0)
                break; /* line[] stays held for the next page */
            rows += r;
            (void) strncpy(nh3ds_pop_lines[nh3ds_pop_n], line,
                           NH3DS_POPLINELEN - 1);
            nh3ds_pop_lines[nh3ds_pop_n][NH3DS_POPLINELEN - 1] = '\0';
            ++nh3ds_pop_n;
            held = FALSE;
        }
        if (!nh3ds_pop_n)
            break; /* nothing left to show */
        nh3ds_refresh();
        nh3ds_draw_popup();
        if (nh3ds_wait_button() & KEY_B)
            stop = TRUE; /* skip the rest of the file */
    }
    (void) dlb_fclose(fp);
    nh3ds_full_redraw();
}

/* prime the status formatter table (labels are port-owned; see the
 * comment above nh3ds_st_initfmt) */
static void
nh3ds_st_init(void)
{
    int i;

    for (i = 0; i < MAXBLSTATS; ++i) {
        (void) strncpy(nh3ds_st_fmt[i], nh3ds_st_initfmt[i],
                       sizeof nh3ds_st_fmt[i] - 1);
        nh3ds_st_fmt[i][sizeof nh3ds_st_fmt[i] - 1] = '\0';
        nh3ds_st_active[i] = TRUE;
        nh3ds_st_val[i][0] = '\0';
        nh3ds_st_color[i] = -1;
    }
}

/* ----------------------------------------------------------------------*/
/* touch: verb buttons (play) and letter keyboard (menus)                 */
/* ----------------------------------------------------------------------*/

/* the bottom screen is 40x30 console cells (8x8 px each); hidTouchRead
 * gives pixels, so the tapped cell is (px / 8, py / 8) */

/* edge-detected touch tap; returns TRUE once per fresh press and fills
 * the tapped console cell (0-based col/row) */
static boolean
nh3ds_tap(int *tcol, int *trow)
{
    static boolean was_down;
    touchPosition tp;
    boolean down;

    hidScanInput();
    down = (hidKeysHeld() & KEY_TOUCH) != 0;
    *tcol = *trow = -1;
    if (down) {
        hidTouchRead(&tp);
        if (!was_down) {
            *tcol = tp.px / 8;
            *trow = tp.py / 8;
        }
    }
    was_down = down;
    return *tcol >= 0;
}

/* verb buttons: below the message tail during play -- 5 rows x 5
 * columns of 8x2-cell buttons (label row + spacer row, one gap row
 * between button rows).  Kick is ^D; force opens the command menu
 * pre-filtered on "force" (it needs a wielded blade); k opens the
 * full keys overlay (digits, ^letters, all punctuation). */
#define TBOT_ROW0 15            /* 0-based console row of the first button */
#define TBOT_STEP 3             /* 2-row button + 1 gap row */
#define TBOT_COLW 8
#define NH3DS_VKEY_KEYS 0x100   /* sentinel: open the keys overlay */
struct nh3ds_verb {
    int key;
    const char label[9];
    const char *pre;            /* ext-menu prefill, or 0 */
};
static const struct nh3ds_verb nh3ds_verbs[] = {
    { 'a', "a:apply ", 0 }, { 'o', "o:open  ", 0 }, { 'c', "c:close ", 0 },
    { 0x04, "^D:kick ", 0 }, { 'q', "q:quaff ", 0 },
    { 'z', "z:zap   ", 0 }, { 'Z', "Z:spell ", 0 }, { 'e', "e:eat   ", 0 },
    { 'r', "r:read  ", 0 }, { 'w', "w:wield ", 0 },
    { 't', "t:throw ", 0 }, { 'f', "f:fire  ", 0 }, { 'd', "d:drop  ", 0 },
    { ';', ";:look  ", 0 }, { '#', "#:loot  ", "loot" },
    { 'T', "T:tkoff ", 0 }, { 'R', "R:rmv   ", 0 }, { 'C', "C:call  ", 0 },
    { '*', "*:equip ", 0 }, { '#', "#:cmd   ", 0 },
    { '>', ">:down  ", 0 }, { '<', "<:up    ", 0 },
    { '#', "F:force ", "force" },
    { NH3DS_VKEY_KEYS, "k:keys  ", 0 },
    { 'm', "m:sneak ", 0 },
};

/* ext-menu prefill queued by the F:force verb button */
static const char *nh3ds_ext_prefill;

/* index of the verb button under a tapped cell, or -1 */
static int
nh3ds_verb_at(int cx, int cy)
{
    int r, c;

    if (cy < TBOT_ROW0 || cy >= TBOT_ROW0 + 5 * TBOT_STEP)
        return -1;
    if ((cy - TBOT_ROW0) % TBOT_STEP >= 2)
        return -1; /* gap row between button rows */
    r = (cy - TBOT_ROW0) / TBOT_STEP;
    c = cx / TBOT_COLW;
    if (c >= 5)
        return -1;
    return r * 5 + c;
}

/* draw the verb grid + a hint line (bottom screen rows 16..30) */
static void
nh3ds_bot_buttons(void)
{
    int r, c;

    nh3ds_scr(0);
    for (r = 0; r < 5; ++r)
        for (c = 0; c < 5; ++c) {
            /* the drawn block covers exactly the pressable cells: a
             * 2-row x 8-col button, like the keyboard's keys */
            printf("\x1b[%d;%dH\x1b[44m        \x1b[0m",
                   TBOT_ROW0 + r * TBOT_STEP + 1, c * TBOT_COLW + 1);
            printf("\x1b[%d;%dH\x1b[44m\x1b[7m%-8.8s\x1b[0m",
                   TBOT_ROW0 + r * TBOT_STEP + 2, c * TBOT_COLW + 1,
                   nh3ds_verbs[r * 5 + c].label);
        }
    printf("\x1b[30;1H\x1b[36m%-40.40s",
           "L=shift R=travel  tap a button");
}

/* touch letter keyboard: 3 QWERTY rows x 10 keys of 4x5-cell keys on
 * the bottom screen (rows 13..27); the detail pane sits above (rows
 * 1..11) and a hint line below (row 29).  L (shift) uppercases letters
 * and shifts the punctuation (';'->':' ','->'<' '.'->'>' '/'->'?'),
 * so map() returns the character as typed.  (Self-contained component:
 * reusable for any future touch text entry.) */
#define KBD_ROW0 12             /* 0-based console row of the first key */
#define KBD_STEP 5
#define KBD_COLW 4
struct nh3ds_key {
    char base, shifted;
};
static const struct nh3ds_key nh3ds_keys[] = {
    { 'q', 'Q' }, { 'w', 'W' }, { 'e', 'E' }, { 'r', 'R' }, { 't', 'T' },
    { 'y', 'Y' }, { 'u', 'U' }, { 'i', 'I' }, { 'o', 'O' }, { 'p', 'P' },
    { 'a', 'A' }, { 's', 'S' }, { 'd', 'D' }, { 'f', 'F' }, { 'g', 'G' },
    { 'h', 'H' }, { 'j', 'J' }, { 'k', 'K' }, { 'l', 'L' }, { ';', ':' },
    { 'z', 'Z' }, { 'x', 'X' }, { 'c', 'C' }, { 'v', 'V' }, { 'b', 'B' },
    { 'n', 'N' }, { 'm', 'M' }, { ',', '<' }, { '.', '>' }, { '/', '?' },
};

static void
nh3ds_kbd_render(void)
{
    int r, c, i;

    nh3ds_scr(0);
    for (r = 0; r < 3; ++r)
        for (c = 0; c < 10; ++c) {
            const struct nh3ds_key *k = &nh3ds_keys[r * 10 + c];
            int row0 = KBD_ROW0 + r * KBD_STEP + 1;

            /* blue border rows top+bottom, blue left edge on the face
             * rows: each key draws its whole 4x5-cell area */
            printf("\x1b[%d;%dH\x1b[44m    \x1b[0m", row0, c * KBD_COLW + 1);
            printf("\x1b[%d;%dH\x1b[44m    \x1b[0m", row0 + KBD_STEP - 1,
                   c * KBD_COLW + 1);
            for (i = 1; i < KBD_STEP - 1; ++i)
                printf("\x1b[%d;%dH\x1b[44m \x1b[7m %c \x1b[0m",
                       row0 + i, c * KBD_COLW + 1, k->base);
        }
    printf("\x1b[29;1H\x1b[36m%-40.40s", "L=shift R=ctrl");
}

/* character under a tapped cell (L = shift, R = ctrl applied), or 0 */
static int
nh3ds_kbd_map(int cx, int cy)
{
    int r, c, i, ch;

    if (cy < KBD_ROW0 || cy >= KBD_ROW0 + 3 * KBD_STEP)
        return 0;
    r = (cy - KBD_ROW0) / KBD_STEP;
    c = cx / KBD_COLW;
    i = r * 10 + c;
    if (c >= 10 || i >= 30)
        return 0;
    ch = (hidKeysHeld() & KEY_L) ? nh3ds_keys[i].shifted : nh3ds_keys[i].base;
    if (hidKeysHeld() & KEY_R)
        ch &= 0x1f; /* ctrl */
    return ch;
}

/* print s word-wrapped into the detail pane above the keyboard */
static void
nh3ds_detail(const char *s)
{
    int row, col;

    nh3ds_scr(0);
    for (row = 1; row <= 11; ++row)
        printf("\x1b[%d;1H\x1b[0m\x1b[K", row);
    row = 2;
    col = 0;
    while (*s && row <= 11) {
        if (*s == ' ') {
            if (col && ++col >= BOT_COLS) {
                ++row;
                col = 0;
            }
            ++s;
            continue;
        }
        if (col >= BOT_COLS) {
            if (++row > 11)
                break;
            col = 0;
        }
        printf("\x1b[%d;%dH\x1b[0m%c", row, col + 1, *s);
        ++col;
        ++s;
    }
}

/* ----------------------------------------------------------------------*/
/* keys overlay: full bottom-screen keyboard during play                  */
/* ----------------------------------------------------------------------*/

/* Five rows x 10 keys of 4x4-cell blocks: digits, the qwerty rows,
 * then NetHack specials ('>' '<' '^' '_' '$' '\' '=' '@' '!' and Esc
 * shown as "Es").  L = shift, R = ctrl, B closes; the newest messages
 * stay visible below so the prompt being answered can be read. */
#define OV_ROW0 2               /* 0-based console row of the first key */
#define OV_STEP 4
static const char nh3ds_ov_rows[5][11] = {
    "1234567890",
    "qwertyuiop",
    "asdfghjkl;",
    "zxcvbnm,./",
    "><^_$\\=@!\033",
};

static void
nh3ds_overlay_draw(void)
{
    int r, c, i, n, oldest;

    nh3ds_scr(0);
    printf("\x1b[2J\x1b[0m");
    printf("\x1b[1;1H\x1b[36m%-40.40s",
           "L=shift R=ctrl B=close tap=key");
    for (r = 0; r < 5; ++r)
        for (c = 0; c < 10; ++c) {
            char k = nh3ds_ov_rows[r][c];
            int row0 = OV_ROW0 + r * OV_STEP + 1;

            printf("\x1b[%d;%dH\x1b[44m    \x1b[0m", row0, c * 4 + 1);
            printf("\x1b[%d;%dH\x1b[44m    \x1b[0m", row0 + OV_STEP - 1,
                   c * 4 + 1);
            for (i = 1; i < OV_STEP - 1; ++i) {
                if (k == '\033')
                    printf("\x1b[%d;%dH\x1b[44m \x1b[7m Es \x1b[0m",
                           row0 + i, c * 4 + 1);
                else
                    printf("\x1b[%d;%dH\x1b[44m \x1b[7m %c \x1b[0m",
                           row0 + i, c * 4 + 1, k);
            }
        }
    /* newest messages below the keys */
    n = (nh3ds_msg_count < 6) ? nh3ds_msg_count : 6;
    oldest = (nh3ds_msg_head - nh3ds_msg_count + NH3DS_NMSGS) % NH3DS_NMSGS;
    oldest = (oldest + nh3ds_msg_count - n) % NH3DS_NMSGS;
    for (i = 0; i < n; ++i)
        printf("\x1b[%d;1H\x1b[0m%-40.40s", OV_ROW0 + 5 * OV_STEP + 1 + i,
               nh3ds_msgs[(oldest + i) % NH3DS_NMSGS]);
}

/* character under a tapped overlay cell (L/R applied), or 0 */
static int
nh3ds_overlay_map(int cx, int cy)
{
    int r, c, ch;

    if (cy < OV_ROW0 || cy >= OV_ROW0 + 5 * OV_STEP)
        return 0;
    r = (cy - OV_ROW0) / OV_STEP;
    c = cx / 4;
    if (c >= 10)
        return 0;
    if (r >= 1 && r <= 3) /* qwerty rows share the menu keyboard table */
        ch = (hidKeysHeld() & KEY_L) ? nh3ds_keys[(r - 1) * 10 + c].shifted
                                     : nh3ds_keys[(r - 1) * 10 + c].base;
    else
        ch = nh3ds_ov_rows[r][c];
    if ((hidKeysHeld() & KEY_R) && ch != '\033')
        ch &= 0x1f; /* ctrl */
    return ch;
}

/* ----------------------------------------------------------------------*/
/* hid input (phase 2b)                                                   */
/* ----------------------------------------------------------------------*/

/* Button mapping (sys/3ds/PLAN.md "UI design"):
 *   d-pad / circle pad  hjkl (two held = diagonals yubn)
 *   L+dir = run (HJKL/YUBN); L alone = shift for the touch keyboard
 *   R+dir = 'g' + dir travel (move until something interesting)
 *   A '.' rest          B 'i' inventory      X ',' pick up
 *   Y 's' search        START 'S' save       SELECT '?' help
 *   touch               verb buttons on the bottom screen (nh3ds_verbs)
 * Directions and A auto-repeat while held; the rest are edge-triggered.
 */
static int
nh3ds_next_key(void) /* 0 = nothing to report this frame */
{
    static int last_key;
    static u32 last_mask;
    static int frames_since;
    static int travel_armed;
    u32 held, btns, dir;
    int key = 0;
    boolean repeatable = FALSE;
    static float last_slider = -1.0f;

    /* depth control without a keypress: re-render once when the 3D
     * slider moves (the usual per-turn redraw would wait for a move) */
    {
        float slider = nh3ds_debug3d ? 1.0f : (float) osGet3DSliderState();

        if (last_slider < 0.0f)
            last_slider = slider;
        if (slider != last_slider) {
            last_slider = slider;
            nh3ds_render_frame();
        }
    }

    hidScanInput();
    gspWaitForVBlank();
    held = hidKeysHeld(); /* KEY_UP.. include the circle pad */
    btns = held & (KEY_A | KEY_B | KEY_X | KEY_Y | KEY_START | KEY_SELECT);
    dir = held & (KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT);

    if (dir) {
        if ((dir & KEY_UP) && (dir & KEY_LEFT))
            key = 'y';
        else if ((dir & KEY_UP) && (dir & KEY_RIGHT))
            key = 'u';
        else if ((dir & KEY_DOWN) && (dir & KEY_LEFT))
            key = 'b';
        else if ((dir & KEY_DOWN) && (dir & KEY_RIGHT))
            key = 'n';
        else if (dir & KEY_LEFT)
            key = 'h';
        else if (dir & KEY_RIGHT)
            key = 'l';
        else if (dir & KEY_UP)
            key = 'k';
        else
            key = 'j';
        if (held & KEY_L)
            key = key - 'a' + 'A'; /* run: HJKL / YUBN (L = shift too) */
        else if (held & KEY_R && !travel_armed) {
            travel_armed = TRUE;
            last_key = 'g';
            last_mask = held;
            frames_since = 0;
            nh3ds_any_button_ever = TRUE;
            return 'g'; /* travel: move until something interesting; the
                         * direction follows while R stays held */
        }
        repeatable = TRUE;
    } else if (!nh3ds_keys_open && btns) {
        if (btns & KEY_A)
            key = '.', repeatable = TRUE;
        else if (btns & KEY_B)
            /* in getpos (farlook/travel cursor mode) B is the escape */
            key = (program_state.input_state == getposInp) ? '\033' : 'i';
        else if (btns & KEY_X)
            key = ',';
        else if (btns & KEY_Y)
            key = 's';
        else if (btns & KEY_START)
            key = 'S';
        else if (btns & KEY_SELECT)
            key = '?';
    }
    if (!dir)
        travel_armed = FALSE; /* R+dir re-arms from the start */

    if (nh3ds_keys_open) {
        /* keys overlay: taps type characters and B closes, but the
         * d-pad stays live -- a direction prompt ("In what
         * direction?") must be answerable with the pad while the
         * overlay is up */
        if (btns & KEY_B) {
            nh3ds_keys_open = FALSE;
            nh3ds_draw_bot_msgs();
            last_key = 0;
            last_mask = 0;
            frames_since = 0;
            return 0;
        }
        if (!key) {
            int tcol, trow;

            if (nh3ds_tap(&tcol, &trow)) {
                int ch = nh3ds_overlay_map(tcol, trow);

                if (ch) {
                    nh3ds_any_button_ever = TRUE;
                    last_key = 0;
                    last_mask = 0;
                    frames_since = 0;
                    return ch;
                }
            }
            last_key = 0;
            last_mask = 0;
            frames_since = 0;
            return 0;
        }
        /* a d-pad direction: fall through so it answers the prompt */
    }

    if (!key) {
        int tcol, trow;

        if (nh3ds_tap(&tcol, &trow)) {
            int vi = nh3ds_verb_at(tcol, trow);

            if (vi >= 0) {
                const struct nh3ds_verb *v = &nh3ds_verbs[vi];

                nh3ds_any_button_ever = TRUE;
                last_key = 0;
                last_mask = 0;
                frames_since = 0;
                if (v->key == NH3DS_VKEY_KEYS) {
                    nh3ds_keys_open = TRUE;
                    nh3ds_overlay_draw();
                    return 0;
                }
                if (v->pre)
                    nh3ds_ext_prefill = v->pre;
                return v->key;
            }
        }
        last_key = 0;
        last_mask = 0;
        frames_since = 0;
        return 0;
    }
    ++frames_since;
    if (key == last_key && held == last_mask && !repeatable)
        return 0; /* non-repeatable button still held */
    if (key == last_key && held == last_mask && frames_since < 8)
        return 0; /* auto-repeat throttle (~130 ms) */
    last_key = key;
    last_mask = held;
    frames_since = 0;
    nh3ds_any_button_ever = TRUE;
    return key;
}

/* walkable for the 3ddebug autopilot: unseen cells ('\0' -- dark
 * corridors do NOT reveal ahead, so the only way to traverse them is
 * stepping into the unseen; a step that turns out to be real rock
 * bumps harmlessly and the walker's failure feedback rotates away),
 * floor-ish glyphs, and open doors (which share their '|'/'-' ttychars
 * with walls, so tell them apart by the stored cmap glyph).  Walls,
 * closed doors, iron bars and solid rock need 'o' or a way around.
 * (strchr(" |-+", '\0') would wrongly report a match on the string
 * terminator.) */
static boolean
nh3ds_is_open(const struct nh3ds_cell *cp)
{
    if (cp->c == '\0')
        return TRUE; /* unseen: try it, bump-and-rotate if rock */
    if (glyph_is_cmap(cp->glyph)) {
        int cmap = glyph_to_cmap(cp->glyph);

        if (cmap == S_hodoor || cmap == S_vodoor)
            return TRUE; /* open door: walk through */
        if (cmap == S_bars)
            return FALSE; /* iron bars share '#' with corridors */
    }
    return !strchr(" |-+", cp->c);
}

/* one greedy step toward the nearest seen cell whose glyph char is
 * `want` (dominant axis first, the other axis when blocked); returns
 * the move key, or 0 when there is no such cell or no open way to it */
static int
nh3ds_demo_step_toward(char want, int hx, int hy)
{
    int tx = -1, ty = -1, x, y, dx, dy, nx, ny;

    for (y = 0; y < ROWNO; ++y)
        for (x = 0; x < COLNO; ++x)
            if (nh3ds_map[y][x].c == want
                && (tx < 0
                    || abs(x - hx) + abs(y - hy)
                       < abs(tx - hx) + abs(ty - hy)))
                tx = x, ty = y;
    if (tx < 0)
        return 0;
    dx = tx - hx, dy = ty - hy;
    if (abs(dx) >= abs(dy) && dx) {
        nx = hx + ((dx > 0) ? 1 : -1);
        if (nx >= 0 && nx < COLNO && nh3ds_is_open(&nh3ds_map[hy][nx]))
            return (dx > 0) ? 'l' : 'h';
    }
    if (dy) {
        ny = hy + ((dy > 0) ? 1 : -1);
        if (ny >= 0 && ny < ROWNO && nh3ds_is_open(&nh3ds_map[ny][hx]))
            return (dy > 0) ? 'j' : 'k';
    }
    return 0;
}

/* 3ddebug autopilot: wander randomly (momentum -- keep one heading
 * until blocked, then the next open direction), rest every 5th step,
 * every 9th step head for the nearest visible closed door and open it
 * once adjacent (rooms are inescapable for a random walker otherwise),
 * search when stuck in a doorless nook, and descend when standing on
 * '>'.  Deliberately dumb -- it only needs to steer the hero past
 * enough terrain to exercise the depth layers in unattended emulator
 * runs; real mapping and pathing live in the nethack MCP tooling, not
 * in the front-end. */
static int
nh3ds_demo_chase(void)
{
    static int step, dir = 1, pending_dir;
    static int last_hx = -1, last_hy = -1;
    static int last_eat_x = -1, last_eat_y = -1;
    static char last_sent;
    int hx = -1, hy = -1, x, y, d, tries, pass, ddx, ddy, nx, ny;
    char ch;

    if (pending_dir) { /* answer the 'o' direction prompt */
        d = pending_dir;
        pending_dir = 0;
        last_sent = nh3ds_dirkey[d];
        return nh3ds_dirkey[d];
    }
    for (y = 0; y < ROWNO; ++y)
        for (x = 0; x < COLNO; ++x)
            if (nh3ds_map[y][x].gflags & MG_HERO)
                hx = x, hy = y;
    if (hx < 0) {
        static boolean logged;

        if (!logged) {
            logged = TRUE;
            nh3ds_log("[walk] no MG_HERO cell in the map buffer");
        }
        return '.'; /* no hero seen yet: rest */
    }
    if (nh3ds_mapunder[hy][hx].c == '>') {
        last_sent = '>';
        return '>'; /* standing on the stairs: descend */
    }
    if (nh3ds_mapunder[hy][hx].c == '%'
        && !(last_sent == 'e' && hx == last_eat_x && hy == last_eat_y)) {
        /* standing on food or a corpse: graze.  Only one 'e' per cell:
         * if it produced no turn (the pack eat menu gets cancelled in
         * demo mode) do not spam the prompt -- walk off instead; the
         * stale '%' under-glyph corrects itself as soon as this cell
         * is redrawn without the hero on it */
        last_eat_x = hx, last_eat_y = hy;
        last_sent = 'e';
        return 'e';
    }
    ++step;
    /* failure feedback: if the hero did not move since the last call
     * and a move key was sent, that way is blocked in the real game
     * despite looking open in the buffer (iron bars, webs, a fighting
     * pet...) -- rotate the heading so the next momentum tries start
     * elsewhere instead of bumping forever */
    if (hx == last_hx && hy == last_hy && last_sent
        && !strchr(".>eos", last_sent))
        dir = (dir + 1) & 7;
    last_hx = hx, last_hy = hy;
    if (step % 5 == 0) {
        last_sent = '.';
        return '.'; /* pause: lets dumps catch the hero standing on
                     * items or stairs (under-the-hero layer) */
    }
    /* steer toward what it can see -- one greedy step per call, no
     * pathfinding: food first when a hunger message scrolled by
     * (starvation ends every run otherwise), then the down stairs */
    if (nh3ds_demo_hungry
        && (d = nh3ds_demo_step_toward('%', hx, hy)) != 0) {
        last_sent = (char) d;
        return d;
    }
    if ((d = nh3ds_demo_step_toward('>', hx, hy)) != 0) {
        last_sent = (char) d;
        return d;
    }
    /* every 9th step: head for the nearest visible closed door and
     * open it once adjacent */
    if (step % 9 == 0) {
        int bx = -1, by = -1;

        for (y = 0; y < ROWNO; ++y)
            for (x = 0; x < COLNO; ++x)
                if (nh3ds_map[y][x].c == '+'
                    && (bx < 0
                        || abs(x - hx) + abs(y - hy)
                           < abs(bx - hx) + abs(by - hy)))
                    bx = x, by = y;
        if (bx >= 0) {
            ddx = bx - hx, ddy = by - hy;
            if (abs(ddx) <= 1 && abs(ddy) <= 1 && (ddx || ddy)) {
                for (d = 0; d < 8; ++d)
                    if (nh3ds_dx8[d] == ddx && nh3ds_dy8[d] == ddy)
                        break;
                pending_dir = d & 7;
                last_sent = 'o';
                return 'o';
            }
            if (abs(ddx) >= abs(ddy) && ddx) {
                nx = hx + ((ddx > 0) ? 1 : -1);
                if (nx >= 0 && nx < COLNO
                    && nh3ds_is_open(&nh3ds_map[hy][nx])) {
                    d = (ddx > 0) ? 1 : 0;
                    dir = d;
                    last_sent = nh3ds_dirkey[d];
                    return nh3ds_dirkey[d];
                }
            }
            if (ddy) {
                ny = hy + ((ddy > 0) ? 1 : -1);
                if (ny >= 0 && ny < ROWNO
                    && nh3ds_is_open(&nh3ds_map[ny][hx])) {
                    d = (ddy > 0) ? 3 : 2;
                    dir = d;
                    last_sent = nh3ds_dirkey[d];
                    return nh3ds_dirkey[d];
                }
            }
        }
    }
    /* explore with momentum -- keep heading one way until blocked,
     * then take the next open direction; prefer quiet seen cells
     * (not letters = not monsters or the pet), then fights, and only
     * push into unseen cells (dark corridors) when nothing seen is
     * open -- a push into real rock bumps harmlessly and the failure
     * feedback above rotates away */
    for (pass = 0; pass < 3; ++pass) {
        for (tries = 0; tries < 8; ++tries) {
            d = (dir + tries) & 7;
            if (hx + nh3ds_dx8[d] >= 0 && hx + nh3ds_dx8[d] < COLNO
                && hy + nh3ds_dy8[d] >= 0 && hy + nh3ds_dy8[d] < ROWNO) {
                const struct nh3ds_cell *cp =
                    &nh3ds_map[hy + nh3ds_dy8[d]][hx + nh3ds_dx8[d]];

                if (!nh3ds_is_open(cp))
                    continue;
                ch = cp->c;
                if (pass == 0
                    && (ch == '\0' || (ch >= 'a' && ch <= 'z')
                        || (ch >= 'A' && ch <= 'Z')))
                    continue; /* unseen or a monster: later pass */
                if (pass == 1
                    && (ch == '\0' || !((ch >= 'a' && ch <= 'z')
                                        || (ch >= 'A' && ch <= 'Z'))))
                    continue; /* not a seen monster */
                if (pass == 2 && ch != '\0')
                    continue; /* seen: handled by earlier passes */
                dir = d;
                last_sent = nh3ds_dirkey[d];
                return nh3ds_dirkey[d];
            }
        }
    }
    {
        static int last_bx = -1, last_by = -1;

        if (hx != last_bx || hy != last_by) {
            last_bx = hx, last_by = hy;
            nh3ds_log("[walk] boxed in at (%d,%d), no door seen -- search",
                      hx, hy);
        }
    }
    last_sent = 's';
    return 's'; /* boxed in with no door: search (secret doors) */
}

/* 3ddebug sweep mode ("M" in the flag file): once the hero exists,
 * magic-map the level, enter farlook and sweep the look cursor across
 * it (the viewport follows the cursor; no turns pass while the cursor
 * moves, so nothing starves), then leave farlook and let the chase
 * walk the hero to the now-mapped '>'.  Restarts on every level
 * change.  Deliberately dumb: a hardcoded 'h'/'l' sequence, no
 * heuristics. */
static int
nh3ds_demo_sweep(void)
{
    static int phase;
    static unsigned long gen = ~0ul;
    static int swept;
    int hx = -1, x, y;

    for (y = 0; y < ROWNO; ++y)
        for (x = 0; x < COLNO; ++x)
            if (nh3ds_map[y][x].gflags & MG_HERO)
                hx = x;
    if (hx < 0)
        return '.'; /* not in the game yet: rest until the hero exists */

    if (nh3ds_mapgen != gen) { /* fresh level: start over */
        gen = nh3ds_mapgen;
        phase = 0;
        swept = 0;
    }
    switch (phase) {
    case 0:
        ++phase;
        return 0x06; /* ^F: magic mapping (debug mode) */
    case 1:
        ++phase;
        return ';'; /* farlook: enter the getpos cursor mode */
    case 2:
    case 3:
        ++phase;
        return ' '; /* get through the farlook info prompts */
    case 4:
        if (swept++ < 100)
            return 'h'; /* 100 steps left... */
        if (swept++ < 300)
            return 'l'; /* ...then 200 right (clamps at the edge) */
        ++phase;
        /* fall through */
    case 5:
        phase = 0; /* leave farlook and restart the sweep: no chase
                    * afterwards, unattended runs stay in cursor mode
                    * and keep producing fresh sweep dumps */
        return 0x1B;
    default:
        return '.';
    }
}

/* the 3ddebug autopilot dispatcher */
static int
nh3ds_demo_walk(void)
{
    if (nh3ds_debug_sweep)
        return nh3ds_demo_sweep();
    return nh3ds_demo_chase();
}

/* blocking read used by both input events */
static int
nh3ds_read_key(void)
{
    int key;

    for (;;) {
        key = nh3ds_next_key();
        if (key)
            return key;
        /* the debug rig's autopilot keeps unattended rig runs moving;
         * without the rig file the game simply waits for a real
         * player -- no autopilot, ever */
        if (!nh3ds_any_button_ever && nh3ds_debug3d) {
            int k = nh3ds_demo_walk();
            {
                static unsigned long dk_n;

                if ((++dk_n & 63) == 0)
                    nh3ds_log("[demo key n=%lu] '%c' moves=%ld",
                              dk_n, (char) k, svm.moves);
            }
            return k;
        }
    }
}

/* ----------------------------------------------------------------------*/
/* menus (phase 2c)                                                       */
/* ----------------------------------------------------------------------*/

/* (the menu state structs live near the top of the file, before the
 * refresh code that checks nh3ds_menu.active) */

static int
nh3ds_menu_selectable(int i)
{
    /* tty's rule: selectable iff the core gave the item an identifier
     * (headers/headings arrive with a zero id) */
    return nh3ds_menu.item[i].id.a_void != 0;
}

/* tty assigns keyboard accelerators itself to items that arrived
 * without one (the pickup menu passes ch = 0 for floor objects);
 * mirror its pool: 'a'..'z' then 'A'.. */
static void
nh3ds_menu_assign_accels(void)
{
    char menu_ch = 'a';
    int i;

    for (i = 0; i < nh3ds_menu.n; ++i) {
        struct nh3ds_menuitem *m = &nh3ds_menu.item[i];

        if (m->id.a_void && !m->ch) {
            m->ch = menu_ch;
            if (menu_ch++ == 'z')
                menu_ch = 'A';
        }
    }
}

/* render the buffered menu on the TOP screen (the map is paused while
 * a menu owns the screen); rows 2..27 list items, row 1 the prompt,
 * row 29 a hint footer.  The bottom screen shows the letter keyboard
 * plus the highlighted item's full text (top-screen lines truncate at
 * 50 columns).  cursor < 0 means display-only: no keyboard. */
static void
nh3ds_menu_render(int cursor)
{
    int rows = 26;

    if (cursor >= 0) {
        if (cursor < nh3ds_menu.top)
            nh3ds_menu.top = cursor;
        if (cursor >= nh3ds_menu.top + rows)
            nh3ds_menu.top = cursor - rows + 1;
    }
    nh3ds_menu.cursor = cursor;
    nh3ds_view = NH3DS_VM_MENU;
    nh3ds_render_frame();

    if (cursor < 0) {
        nh3ds_scr(0);
        printf("\x1b[2J\x1b[0m\x1b[29;1H\x1b[0m%-40.40s", "[A] continue");
    } else {
        nh3ds_scr(0);
        printf("\x1b[2J\x1b[0m");
        nh3ds_kbd_render();
        nh3ds_detail(nh3ds_menu.item[cursor].str);
    }
}

/* wait for one edge-triggered button press or touch tap; returns the
 * hid keys-down bits (0 for a tap, with the tapped cell in *tcol and
 * *trow).  L/R are excluded from the release wait: L is the shift
 * modifier for the touch keyboard and must stay holdable. */
#define NH3DS_MENUWAIT_MASK                                                     \
    (KEY_A | KEY_B | KEY_X | KEY_Y | KEY_START | KEY_SELECT                     \
     | KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT | KEY_TOUCH)
static u32
nh3ds_menu_input(int *tcol, int *trow)
{
    u32 down;

    *tcol = *trow = -1;
    do {
        do {
            hidScanInput();
            gspWaitForVBlank();
        } while (hidKeysHeld() & NH3DS_MENUWAIT_MASK);
        for (;;) {
            int tc, tr;

            if (nh3ds_tap(&tc, &tr)) {
                *tcol = tc;
                *trow = tr;
                nh3ds_any_button_ever = TRUE;
                return 0; /* tap: caller reads *tcol / *trow */
            }
            down = hidKeysDown();
            if (down)
                break;
        }
    } while (0);
    nh3ds_any_button_ever = TRUE;
    return down;
}

/* interactive menu selection; returns the number of picks (or -1) and
 * fills *menulist with a malloc'd menu_item array */
static int
nh3ds_menu_pick(int how, MENU_ITEM_P **menulist)
{
    int cursor = -1, i, n;
    MENU_ITEM_P *out;

    nh3ds_menu_assign_accels();
    for (i = 0; i < nh3ds_menu.n; ++i)
        if (nh3ds_menu_selectable(i)) {
            cursor = i;
            break;
        }
    if (cursor < 0)
        return -1; /* nothing selectable */

    for (;;) {
        u32 down;
        int tcol, trow;
        boolean done = FALSE;

        nh3ds_menu_render(cursor);
        down = nh3ds_menu_input(&tcol, &trow);
        if (tcol >= 0) {
            /* touch: a key selects by accelerator (shifted via L for
             * uppercase accelerators, items 27+), tapping a listed
             * row moves the cursor there */
            int ch = nh3ds_kbd_map(tcol, trow);

            if (ch) {
                for (i = 0; i < nh3ds_menu.n; ++i) {
                    if (nh3ds_menu_selectable(i)
                        && nh3ds_menu.item[i].ch == ch) {
                        cursor = i;
                        if (how == PICK_ONE) {
                            nh3ds_menu.item[i].itemflags |=
                                MENU_ITEMFLAGS_SELECTED;
                            done = TRUE;
                        } else {
                            nh3ds_menu.item[i].itemflags ^=
                                MENU_ITEMFLAGS_SELECTED;
                        }
                        break;
                    }
                }
            } else if (trow >= 2 && trow <= 27) {
                int j = nh3ds_menu.top + trow - 2;

                if (j >= 0 && j < nh3ds_menu.n && nh3ds_menu_selectable(j))
                    cursor = j;
            }
        } else if (down & KEY_DUP) {
            for (i = cursor - 1; i >= 0; --i)
                if (nh3ds_menu_selectable(i)) {
                    cursor = i;
                    break;
                }
            if (i < 0) /* top of the list: scroll over to the bottom */
                for (i = nh3ds_menu.n - 1; i > cursor; --i)
                    if (nh3ds_menu_selectable(i)) {
                        cursor = i;
                        break;
                    }
        } else if (down & KEY_DDOWN) {
            for (i = cursor + 1; i < nh3ds_menu.n; ++i)
                if (nh3ds_menu_selectable(i)) {
                    cursor = i;
                    break;
                }
            if (i == nh3ds_menu.n) /* bottom: scroll over to the top */
                for (i = 0; i < cursor; ++i)
                    if (nh3ds_menu_selectable(i)) {
                        cursor = i;
                        break;
                    }
        } else if (down & KEY_DLEFT) {
            cursor -= 26;
            if (cursor < 0)
                cursor = 0;
            while (cursor > 0 && !nh3ds_menu_selectable(cursor))
                --cursor;
        } else if (down & KEY_DRIGHT) {
            cursor += 26;
            if (cursor >= nh3ds_menu.n)
                cursor = nh3ds_menu.n - 1;
            while (cursor > 0 && !nh3ds_menu_selectable(cursor))
                --cursor;
        } else if (down & KEY_B) {
            if (how == PICK_ONE)
                return -1; /* cancel */
            break;         /* done, keep toggles */
        } else if (down & (KEY_A | KEY_START)) {
            if (how == PICK_ONE) {
                nh3ds_menu.item[cursor].itemflags |=
                    MENU_ITEMFLAGS_SELECTED;
                done = TRUE;
            } else if (down & KEY_A) {
                nh3ds_menu.item[cursor].itemflags ^=
                    MENU_ITEMFLAGS_SELECTED;
            } else {
                break; /* START: done, keep toggles */
            }
        } else if (down & KEY_X) {
            /* grab: in a multi-select menu ("Things that are here")
             * take everything selectable and confirm; PICK_ONE acts
             * like A (take the cursor item) */
            if (how == PICK_ONE) {
                nh3ds_menu.item[cursor].itemflags |=
                    MENU_ITEMFLAGS_SELECTED;
            } else {
                for (i = 0; i < nh3ds_menu.n; ++i)
                    if (nh3ds_menu_selectable(i))
                        nh3ds_menu.item[i].itemflags |=
                            MENU_ITEMFLAGS_SELECTED;
            }
            done = TRUE;
        }
        /* SELECT/Y: ignored for now */
        if (done)
            break;
    }
    n = 0;
    for (i = 0; i < nh3ds_menu.n; ++i)
        if (nh3ds_menu.item[i].itemflags & MENU_ITEMFLAGS_SELECTED)
            ++n;
    if (n == 0)
        return 0;
    out = (MENU_ITEM_P *) malloc((size_t) n * sizeof *out);
    if (!out)
        return -1;
    n = 0;
    for (i = 0; i < nh3ds_menu.n; ++i)
        if (nh3ds_menu.item[i].itemflags & MENU_ITEMFLAGS_SELECTED) {
            out[n].item = nh3ds_menu.item[i].id;
            out[n].count = -1L; /* "select all" (no count prompt yet) */
            out[n].itemflags = nh3ds_menu.item[i].itemflags;
            ++n;
        }
    *menulist = out;
    return n;
}

/* ----------------------------------------------------------------------*/
/* extended commands: top-screen list + touch letter keyboard             */
/* ----------------------------------------------------------------------*/

/* lowercase ASCII (the prefix filter is typed on the touch keyboard) */
static char
nh3ds_lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char) (c - 'A' + 'a') : c;
}

/* does txt start with the (already lowercase) prefix pre of length
 * plen, case-insensitively? */
static boolean
nh3ds_prefix_ci(const char *txt, const char *pre, int plen)
{
    int i;

    for (i = 0; i < plen; ++i)
        if (nh3ds_lower(txt[i]) != pre[i])
            return FALSE;
    return TRUE;
}

/* the ext-command menu, tty "#cmd" style: the available commands are
 * listed on the top screen and filtered live by the letters typed on
 * the bottom-screen touch keyboard (pad = move, A = run it, B =
 * backspace or cancel, START = cancel).  Returns an extcmdlist[]
 * index or -1.  (The core's extcmd_via_menu() is TTY/CURSES-only, and
 * the return must be a true extcmdlist index -- doextcmd() indexes
 * extcmdlist[] with it directly.) */
static int
nh3ds_ext_menu(void)
{
    const struct ext_func_tab *efp;
    int n = 0, i;

    for (efp = extcmdlist; efp->ef_txt && n < NH3DS_EXTMAX; ++efp) {
        if ((efp->flags & (CMD_NOT_AVAILABLE | INTERNALCMD))
            || !(efp->flags & AUTOCOMPLETE)
            || (!wizard && (efp->flags & WIZMODECMD)))
            continue;
        nh3ds_ext.idx[n++] = (int) (efp - extcmdlist);
    }
    if (!n)
        return -1;
    nh3ds_ext.n = n;
    nh3ds_ext.filter[0] = '\0';
    nh3ds_ext.flen = 0;
    nh3ds_ext.mtop = 0;
    nh3ds_ext.cursor = 0;
    nh3ds_keys_open = FALSE; /* the menu owns the bottom screen now */
    if (nh3ds_ext_prefill) { /* F:force verb button queued a filter */
        (void) strncpy(nh3ds_ext.filter, nh3ds_ext_prefill,
                       sizeof nh3ds_ext.filter - 1);
        nh3ds_ext.filter[sizeof nh3ds_ext.filter - 1] = '\0';
        nh3ds_ext.flen = (int) strlen(nh3ds_ext.filter);
        nh3ds_ext_prefill = 0;
    }

    for (;;) {
        boolean filter_changed = FALSE;
        u32 down;
        int tcol, trow;

        /* recompute the match list for the current prefix filter */
        nh3ds_ext.nm = 0;
        for (i = 0; i < nh3ds_ext.n; ++i)
            if (!nh3ds_ext.flen
                || nh3ds_prefix_ci(extcmdlist[nh3ds_ext.idx[i]].ef_txt,
                                   nh3ds_ext.filter, nh3ds_ext.flen))
                nh3ds_ext.match[nh3ds_ext.nm++] = nh3ds_ext.idx[i];

        /* clamp the cursor into the current match list */
        if (nh3ds_ext.cursor >= nh3ds_ext.nm)
            nh3ds_ext.cursor = nh3ds_ext.nm - 1;
        if (nh3ds_ext.cursor < 0)
            nh3ds_ext.cursor = 0;
        if (nh3ds_ext.mtop > nh3ds_ext.cursor)
            nh3ds_ext.mtop = nh3ds_ext.cursor;
        if (nh3ds_ext.mtop > nh3ds_ext.nm - 1)
            nh3ds_ext.mtop = nh3ds_ext.nm - 1;
        if (nh3ds_ext.mtop < 0)
            nh3ds_ext.mtop = 0;

        nh3ds_view = NH3DS_VM_EXT;
        nh3ds_render_frame();

        /* bottom screen: description of the highlighted command over
         * the letter keyboard */
        nh3ds_scr(0);
        printf("\x1b[2J\x1b[0m");
        nh3ds_detail(nh3ds_ext.nm
                         ? (extcmdlist[nh3ds_ext.match[nh3ds_ext.cursor]]
                                .ef_desc
                            ? extcmdlist[nh3ds_ext.match[nh3ds_ext.cursor]]
                                  .ef_desc
                            : extcmdlist[nh3ds_ext.match[nh3ds_ext.cursor]]
                                  .ef_txt)
                         : "no matching command");
        nh3ds_kbd_render();

        down = nh3ds_menu_input(&tcol, &trow);
        if (tcol >= 0) {
            int ch = nh3ds_kbd_map(tcol, trow);

            if (ch
                && nh3ds_ext.flen < (int) sizeof nh3ds_ext.filter - 1) {
                nh3ds_ext.filter[nh3ds_ext.flen++] =
                    nh3ds_lower((char) ch);
                filter_changed = TRUE;
            }
        } else if (down & KEY_DUP) {
            if (nh3ds_ext.cursor > 0)
                --nh3ds_ext.cursor;
            else
                nh3ds_ext.cursor = nh3ds_ext.nm - 1; /* wrap to bottom */
        } else if (down & KEY_DDOWN) {
            if (nh3ds_ext.cursor < nh3ds_ext.nm - 1)
                ++nh3ds_ext.cursor;
            else
                nh3ds_ext.cursor = 0; /* wrap to top */
        } else if (down & KEY_B) {
            if (nh3ds_ext.flen) {
                nh3ds_ext.filter[--nh3ds_ext.flen] = '\0';
                filter_changed = TRUE;
            } else {
                return -1;
            }
        } else if (down & KEY_START) {
            return -1;
        } else if (down & KEY_A) {
            if (nh3ds_ext.nm) {
                nh3ds_full_redraw();
                return nh3ds_ext.match[nh3ds_ext.cursor];
            }
        }
        if (filter_changed) { /* new filter: jump to the first match */
            nh3ds_ext.cursor = 0;
            nh3ds_ext.mtop = 0;
        }
    }
}

/* libctru software keyboard; returns FALSE when cancelled.  The text is
 * copied into buf (truncated to fit). */
static boolean
nh3ds_swkbd(const char *hint, char *buf, size_t buflen)
{
    SwkbdState sw;
    static char tmp[256];
    SwkbdButton res;

    if (buflen > sizeof tmp)
        buflen = sizeof tmp;
    swkbdInit(&sw, SWKBD_TYPE_NORMAL, 2, (int) buflen - 1);
    swkbdSetHintText(&sw, hint ? hint : "");
    swkbdSetValidation(&sw, SWKBD_ANYTHING, 0, 0);
    res = swkbdInputText(&sw, tmp, sizeof tmp);
    if (res != SWKBD_BUTTON_CONFIRM) {
        buf[0] = '\0';
        return FALSE;
    }
    (void) strncpy(buf, tmp, buflen - 1);
    buf[buflen - 1] = '\0';
    return TRUE;
}

/* ----------------------------------------------------------------------*/
/* the shim callback                                                      */
/* ----------------------------------------------------------------------*/

/* argument slots: fmt chars i/c/b/0/1/2 arrive as int (default argument
 * promotions), s/p as pointers.  fmt[0] is the return type. */
#define SHIM_MAXARGS 12
union shimarg {
    int i;
    void *p;
    const char *s;
};

static unsigned long nh3ds_getch_n;
static unsigned long nh3ds_poskey_n;

static void
shim_cb(const char *name, void *ret_ptr, const char *fmt, ...)
{
    union shimarg args[SHIM_MAXARGS];
    char rtype = fmt[0];
    int argn = 0;
    int rint = 0;
    void *rptr = (void *) 0;
    va_list ap;
    int i;

    nhev_count(name);
    nhev_periodic();

    va_start(ap, fmt);
    for (i = 1; fmt[i] && argn < SHIM_MAXARGS; ++i) {
        char t = fmt[i];
        if (t == 's' || t == 'p')
            args[argn++].p = va_arg(ap, void *);
        else
            args[argn++].i = va_arg(ap, int);
    }
    va_end(ap);

    /* ---- window lifecycle --------------------------------------------*/
    if (!strcmp(name, "shim_init_nhwindows")) {
        /* the shim windowport never sets this itself; without it
         * getlock() would take the raw getchar() path */
        iflags.window_inited = TRUE;
        /* ask the core for background glyphs: the floor terrain beneath
         * objects, monsters and the hero (the stereo floor plane and
         * the terrain under the hero) */
        iflags.use_background_glyph = TRUE;
        nh3ds_log("[init_nhwindows] window_inited=TRUE bg_glyph=TRUE");
    } else if (!strcmp(name, "shim_player_selection")) {
        if (nh3ds_debug3d) {
            /* rig: unattended -- role_init() randomizes unspecified
             * facets */
            nh3ds_log("[player_selection] rig: role/race/gend/align random");
        } else {
            /* generic role/race/gender/alignment menus (the core's
             * genl setup drives this windowport's own menus) */
            (void) genl_player_setup(80);
            nh3ds_log("[player_selection] genl setup done");
        }
    } else if (!strcmp(name, "shim_player_selection_or_tty")) {
        rint = 0; /* FALSE: don't run the generic tty-style setup */
    } else if (!strcmp(name, "shim_askname")) {
        if (nh3ds_debug3d) {
            /* rig: canned name, unattended */
            (void) strncpy(svp.plname, nh3ds_plname,
                           sizeof svp.plname - 1);
            svp.plname[sizeof svp.plname - 1] = '\0';
            nh3ds_log("[askname] rig canned plname=\"%s\"", svp.plname);
        } else if (nh3ds_swkbd("Who are you?", svp.plname,
                               sizeof svp.plname)) {
            nh3ds_log("[askname] swkbd plname=\"%s\"", svp.plname);
        } else {
            (void) strncpy(svp.plname, nh3ds_plname,
                           sizeof svp.plname - 1);
            svp.plname[sizeof svp.plname - 1] = '\0';
            nh3ds_log("[askname] swkbd cancelled, using \"%s\"",
                      svp.plname);
        }
    } else if (!strcmp(name, "shim_get_nh_event")) {
        /* phase 2b: poll hid here */
    } else if (!strcmp(name, "shim_exit_nhwindows")) {
        nh3ds_log("[exit_nhwindows] %s", args[0].s ? args[0].s : "");
    } else if (!strcmp(name, "shim_suspend_nhwindows")) {
        nh3ds_log("[suspend_nhwindows] %s", args[0].s ? args[0].s : "");
    } else if (!strcmp(name, "shim_resume_nhwindows")) {
        /* nothing */
    } else if (!strcmp(name, "shim_create_nhwindow")) {
        int type = args[0].i;
        winid w;

        /* take the lowest free id: destroyed popups hand their slot
         * back for reuse, mirroring tty_create_nhwindow -- the core
         * churns through popup menus, and a monotonic counter would
         * exhaust the table mid-session and silently kill every
         * menu from then on */
        for (w = 1; w < NH3DS_MAXWINID; ++w)
            if (!nh3ds_wintype[w])
                break;
        if (w == NH3DS_MAXWINID) {
            panic("No window slots!");
            /*NOTREACHED*/
            rint = 0;
        } else {
            rint = (int) w;
            nh3ds_wintype[w] = (unsigned char) (type + 1);
            if (type == NHW_MENU || type == NHW_TEXT) {
                nh3ds_pop_win = w;
                nh3ds_pop_n = 0;
                nh3ds_pop_prompt[0] = '\0';
            }
            nh3ds_log("[create_nhwindow] type=%s -> winid=%d",
                      nh3ds_wintypename(type), (int) w);
        }
    } else if (!strcmp(name, "shim_clear_nhwindow")) {
        winid w = (winid) args[0].i;
        int ty = (w > 0 && w < NH3DS_MAXWINID) ? nh3ds_wintype[w] - 1 : -1;

        if (ty == NHW_MAP) {
            memset(nh3ds_map, 0, sizeof nh3ds_map);
            memset(nh3ds_mapbg, 0, sizeof nh3ds_mapbg);
            memset(nh3ds_mapunder, 0, sizeof nh3ds_mapunder);
            nh3ds_cur_x = nh3ds_cur_y = -1;
            ++nh3ds_mapgen;
            nh3ds_map_dirty = TRUE;
        }
    } else if (!strcmp(name, "shim_display_nhwindow")) {
        winid w = (winid) args[0].i;
        boolean blocking = (args[1].i != 0);
        int ty = (w > 0 && w < NH3DS_MAXWINID) ? nh3ds_wintype[w] - 1 : -1;

        nh3ds_refresh();
        if (blocking) {
            nh3ds_keys_open = FALSE; /* the popup owns the bottom now */
            if ((ty == NHW_MENU || ty == NHW_TEXT) && w == nh3ds_pop_win) {
                if (nh3ds_menu.active && nh3ds_menu.win == w
                    && nh3ds_menu.n > 0) {
                    /* a menu that is displayed rather than selected */
                    nh3ds_menu_render(-1);
                } else {
                    nh3ds_draw_popup();
                }
                nh3ds_wait_button();
                nh3ds_full_redraw();
            } else {
                /* --More-- style pause (message window, map, ...) */
                nh3ds_wait_button();
            }
        }
    } else if (!strcmp(name, "shim_destroy_nhwindow")) {
        winid w = (winid) args[0].i;

        if (w == nh3ds_pop_win)
            nh3ds_pop_win = -1;
        if (nh3ds_menu.win == w)
            nh3ds_menu.active = FALSE;
        if (w > 0 && w < NH3DS_MAXWINID)
            nh3ds_wintype[w] = 0;
    } else if (!strcmp(name, "shim_curs")) {
        /* position indicator: on the map this is the getpos cursor
         * (farlook/travel targeting); the cube hides while it sits on
         * the hero -- which is where curs() puts it after every move */
        {
            winid w = (winid) args[0].i;
            int ty = (w > 0 && w < NH3DS_MAXWINID) ? nh3ds_wintype[w] - 1
                                                   : -1;

            if (ty == NHW_MAP) {
                nh3ds_cur_x = (int) args[1].i;
                nh3ds_cur_y = (int) args[2].i;
                /* getpos moves the cursor without passing turns, so
                 * refresh() (per-turn) will not fire: redraw now or
                 * the cursor cube is invisible while it moves */
                nh3ds_map_dirty = TRUE;
                nh3ds_render_frame();
            }
        }
    } else if (!strcmp(name, "shim_putstr")) {
        winid w = (winid) args[0].i;
        const char *str = args[2].s ? args[2].s : "";
        int ty = (w > 0 && w < NH3DS_MAXWINID) ? nh3ds_wintype[w] - 1 : -1;

        if (ty == NHW_MESSAGE) {
            nh3ds_msg_add(str);
        } else if ((ty == NHW_MENU || ty == NHW_TEXT) && w == nh3ds_pop_win
                   && nh3ds_pop_n < NH3DS_POPLINES) {
            strncpy(nh3ds_pop_lines[nh3ds_pop_n], str,
                    NH3DS_POPLINELEN - 1);
            nh3ds_pop_lines[nh3ds_pop_n][NH3DS_POPLINELEN - 1] = '\0';
            ++nh3ds_pop_n;
        } else if ((ty == NHW_MENU || ty == NHW_TEXT)
                   && w == nh3ds_pop_win) {
            nh3ds_log("[putstr] popup full (%d lines), dropping \"%s\"",
                      NH3DS_POPLINES, str);
        }
    } else if (!strcmp(name, "shim_display_file")) {
        nh3ds_display_file(args[0].s ? args[0].s : "", args[1].i != 0);
    } else if (!strcmp(name, "shim_start_menu")) {
        winid w = (winid) args[0].i;

        if (w > 0 && w < NH3DS_MAXWINID
            && nh3ds_wintype[w] - 1 == NHW_MENU) {
            nh3ds_menu.win = w;
            nh3ds_menu.active = TRUE;
            nh3ds_menu.n = 0;
            nh3ds_menu.top = 0;
            nh3ds_menu.prompt[0] = '\0';
        }
    } else if (!strcmp(name, "shim_add_menu")) {
        /* fmt "vipi00iisi": 0=win, 2=identifier (pointer bits in an int
         * slot), 3=ch, 4=gch, 5=attr, 6=clr, 7=str, 8=itemflags (the
         * shim forwards plain varargs). */
        winid w = (winid) args[0].i;
        char ch = (char) args[3].i;
        const char *str = args[7].s ? args[7].s : "";

        if (nh3ds_menu.active && w == nh3ds_menu.win) {
            if (nh3ds_menu.n >= NH3DS_MAXMENU) {
                nh3ds_log("[add_menu] full (%d), dropping \"%s\"",
                          NH3DS_MAXMENU, str);
            } else {
                struct nh3ds_menuitem *m = &nh3ds_menu.item[nh3ds_menu.n];

                m->id = *(const ANY_P *) (const void *) (uintptr_t)
                            args[2].i;
                m->ch = ch;
                m->gch = (char) args[4].i;
                m->attr = args[5].i;
                m->clr = args[6].i;
                (void) strncpy(m->str, str, sizeof m->str - 1);
                m->str[sizeof m->str - 1] = '\0';
                if (strlen(str) >= sizeof m->str)
                    nh3ds_log("[add_menu] item clipped: \"%s\"", str);
                m->itemflags = (unsigned) args[8].i;
                ++nh3ds_menu.n;
            }
        }
    } else if (!strcmp(name, "shim_end_menu")) {
        const char *prompt = args[1].s;

        if (prompt && *prompt) {
            if (nh3ds_menu.active) {
                (void) strncpy(nh3ds_menu.prompt, prompt,
                               sizeof nh3ds_menu.prompt - 1);
                nh3ds_menu.prompt[sizeof nh3ds_menu.prompt - 1] = '\0';
            }
            (void) strncpy(nh3ds_pop_prompt, prompt, NH3DS_POPLINELEN - 1);
            nh3ds_pop_prompt[NH3DS_POPLINELEN - 1] = '\0';
        }
    } else if (!strcmp(name, "shim_select_menu")) {
        winid w = (winid) args[0].i;
        int how = args[1].i;

        if (args[2].p)
            *(MENU_ITEM_P **) args[2].p = (MENU_ITEM_P *) 0;
        if (w != nh3ds_menu.win || !nh3ds_menu.active) {
            rint = -1; /* stale or unbuffered window: cancelled */
        } else if (how == PICK_NONE) {
            /* display-only list: show it and wait for a key (tty-like);
             * the rig dismisses it unattended */
            if (!nh3ds_debug3d) {
                nh3ds_menu_render(-1);
                nh3ds_wait_button();
                nh3ds_full_redraw();
            }
            nh3ds_menu.active = FALSE;
            rint = 0;
        } else if (nh3ds_debug3d) {
            /* rig: cancel every interactive menu so unattended runs
             * keep flowing */
            nh3ds_menu.active = FALSE;
            rint = -1;
        } else {
            rint = nh3ds_menu_pick(how, (MENU_ITEM_P **) args[2].p);
            nh3ds_menu.active = FALSE;
            nh3ds_full_redraw(); /* the menu owned the top screen */
            nh3ds_log("[select_menu win=%d how=%d] -> %d", w, how, rint);
        }
    } else if (!strcmp(name, "shim_message_menu")) {
        /* acknowledge the message (equivalent of pressing the key) */
        if (args[2].s)
            nh3ds_msg_add(args[2].s);
        rint = args[0].i;
    } else if (!strcmp(name, "shim_mark_synch")
               || !strcmp(name, "shim_wait_synch")) {
        /* nothing */
    } else if (!strcmp(name, "shim_cliparound")) {
        coordxy x = (coordxy) args[0].i;
        coordxy y = (coordxy) args[1].i;
        coordxy nvx = (x > MAP_VIEW_COLS / 2) ? x - MAP_VIEW_COLS / 2 : 0;
        coordxy nvy = (y > MAP_ROWS / 2) ? y - MAP_ROWS / 2 : 0;

        if (nvx > COLNO - MAP_VIEW_COLS)
            nvx = COLNO - MAP_VIEW_COLS;
        if (nvy > ROWNO - MAP_ROWS)
            nvy = ROWNO - MAP_ROWS;
        if (nvx != nh3ds_vx || nvy != nh3ds_vy) {
            nh3ds_vx = nvx;
            nh3ds_vy = nvy;
            nh3ds_map_dirty = TRUE;
        }
    } else if (!strcmp(name, "shim_update_positionbar")) {
        /* nothing */
    } else if (!strcmp(name, "shim_print_glyph")) {
        coordxy x = (coordxy) args[1].i;
        coordxy y = (coordxy) args[2].i;
        const glyph_info *gip = (const glyph_info *) args[3].p;
        const glyph_info *bgip = (const glyph_info *) args[4].p;
        struct nh3ds_cell *cell, *bg, *un;
        signed char clr;

        if (x >= 0 && x < COLNO && y >= 0 && y < ROWNO && gip) {
            cell = &nh3ds_map[y][x];
            bg = &nh3ds_mapbg[y][x];
            un = &nh3ds_mapunder[y][x];
            clr = (signed char) (gip->gm.sym.color <= 0 ? -1
                                                        : gip->gm.sym.color);
            /* keep the last non-hero foreground glyph per cell: on the
             * hero's tile it is what the hero stands on */
            if (!(gip->gm.glyphflags & MG_HERO)) {
                un->c = (char) gip->ttychar;
                un->color = clr;
                un->glyph = gip->glyph;
                un->gflags = gip->gm.glyphflags;
            }
            cell->c = (char) gip->ttychar;
            cell->color = clr;
            cell->glyph = gip->glyph;
            cell->gflags = gip->gm.glyphflags;
            {
                static unsigned long pg_n;
                static uchar pg_seen[256];

                ++pg_n;
                if (!pg_seen[(uchar) gip->ttychar & 0xff]) {
                    pg_seen[(uchar) gip->ttychar & 0xff] = 1;
                    nh3ds_log("[pg first n=%lu] '%c' (%d) glyph=%ld color=%d",
                              pg_n, (char) gip->ttychar,
                              (int) gip->ttychar, (long) gip->glyph,
                              (int) clr);
                }
                if ((pg_n & 8191) == 0)
                    nh3ds_log("[pg count] %lu", pg_n);
            }
            if (bgip) {
                bg->c = (char) bgip->ttychar;
                bg->color = (signed char) (bgip->gm.sym.color <= 0
                                               ? -1 : bgip->gm.sym.color);
                bg->glyph = bgip->glyph;
                bg->gflags = bgip->gm.glyphflags;
            }
            nh3ds_map_dirty = TRUE;
        }
    } else if (!strcmp(name, "shim_raw_print")
               || !strcmp(name, "shim_raw_print_bold")) {
        /* error()/panic() surface here -- into the wrapped message ring
         * (a raw printf would land at an arbitrary console position,
         * often off-screen) */
        nh3ds_msg_add(args[0].s ? args[0].s : "");
        nh3ds_log("[raw_print] %s", args[0].s ? args[0].s : "");
        /* remember the first error for the hold-on-exit screen */
        if (!nh3ds_lastwords[0] && args[0].s)
            (void) snprintf(nh3ds_lastwords, sizeof nh3ds_lastwords,
                            "%s", args[0].s);
    } else if (!strcmp(name, "shim_nhgetch")) {
        rint = nh3ds_read_key();
        ++nh3ds_getch_n;
        if (nh3ds_getch_n == 1 || nh3ds_getch_n % 2048 == 0)
            nh3ds_log("[nhgetch n=%lu] '%c'", nh3ds_getch_n, (char) rint);
    } else if (!strcmp(name, "shim_nh_poskey")) {
        rint = nh3ds_read_key();
        ++nh3ds_poskey_n;
        if (nh3ds_poskey_n == 1 || nh3ds_poskey_n % 2048 == 0)
            nh3ds_log("[nh_poskey n=%lu] moves=%ld '%c'", nh3ds_poskey_n,
                      svm.moves, (char) rint);
        nh3ds_refresh();
    } else if (!strcmp(name, "shim_nhbell")) {
        /* nothing */
    } else if (!strcmp(name, "shim_doprev_message")) {
        rint = 0;
    } else if (!strcmp(name, "shim_yn_function")) {
        const char *query = args[0].s ? args[0].s : "";
        const char *resp = args[1].s;
        char def = (char) args[2].i;
        char ans = 0;

        /* relaunch friendliness under the rig: a stale lock file from a
         * killed run prompts "There is already a game in progress under
         * your name.  Destroy old game?" -- answer yes automatically so
         * unattended save/restore testing keeps working */
        if (nh3ds_debug3d && (strstr(query, "Destroy old game")
                              || strstr(query, "eat it?"))) {
            rint = 'y'; /* rig heroes never shop, they graze */
            nh3ds_log("[yn rig] %s -> 'y'", query);
        } else {
            /* real play: the query wraps in the detail pane, the touch
             * keyboard answers with the exact choice letter, and the
             * physical buttons shortcut the common cases (A = default,
             * B = no/escape, Y = yes, X = quit-choice) */
            nh3ds_keys_open = FALSE;
            nh3ds_scr(0);
            printf("\x1b[2J\x1b[0m");
            nh3ds_detail(query);
            nh3ds_kbd_render();
            printf("\x1b[29;1H\x1b[36m%-40.40s",
                   "A=yes B=no START=def tap=key");
            for (;;) {
                u32 down;
                int tcol, trow;

                down = nh3ds_menu_input(&tcol, &trow);
                if (tcol >= 0) {
                    int ch = nh3ds_kbd_map(tcol, trow);
                    char c;

                    if (!ch)
                        continue;
                    c = (char) ch;
                    /* resp == NULL is the getdir() direction prompt:
                     * any tapped char is offered to the core, which
                     * validates it (help_dir on a bad one) */
                    if (!resp || strchr(resp, c)) {
                        ans = c;
                        break;
                    }
                    c = nh3ds_lower(c); /* shifted tap -> choice letter */
                    if (resp && strchr(resp, c)) {
                        ans = c;
                        break;
                    }
                } else if (!resp
                           && (down & (KEY_DUP | KEY_DDOWN | KEY_DLEFT
                                       | KEY_DRIGHT))) {
                    /* direction prompt (no response set): the d-pad
                     * answers directly with the direction char (same
                     * map as the main input).  Gated on !resp: on a
                     * yes/no or lettered prompt a direction char is an
                     * invalid answer and the core flags "program in
                     * disorder". */
                    ans = (char) ((down & KEY_DUP)
                                      ? ((down & KEY_DLEFT)
                                             ? 'y'
                                             : (down & KEY_DRIGHT) ? 'u'
                                                                   : 'k')
                                      : (down & KEY_DLEFT)
                                          ? ((down & KEY_DRIGHT) ? 'b' : 'h')
                                          : (down & KEY_DRIGHT) ? 'n' : 'j');
                    break;
                } else if (!resp && !strncmp(query, "Shall I pick", 12)
                           && (down & (KEY_A | KEY_START | KEY_Y | KEY_B))) {
                    /* the generic role-setup prompt ("Shall I pick a
                     * role, race, ...? [ynaq]") arrives with resp NULL
                     * like a direction prompt; here B must answer "no"
                     * (pick via menus) because esc quits role selection
                     * outright and the game would start with a random
                     * role.  A/START/Y = yes (random pick + confirm). */
                    ans = (down & KEY_B) ? 'n' : 'y';
                    break;
                } else if (down & KEY_A) {
                    /* yes/no prompts: A = yes (console convention);
                     * otherwise the default or first choice */
                    if (resp && strchr(resp, 'y'))
                        ans = 'y';
                    else
                        ans = (def && resp && strchr(resp, def))
                                  ? def
                                  : (resp && *resp) ? *resp
                                      : (resp ? 'y' : '.');
                    break;
                } else if (down & KEY_START) {
                    /* START keeps the default-answer shortcut */
                    ans = (def && resp && strchr(resp, def))
                              ? def
                              : (resp && *resp) ? *resp
                                  : (resp ? 'y' : '.');
                    break;
                } else if (down & KEY_B) {
                    ans = (resp && strchr(resp, 'n')) ? 'n' : '\033';
                    break;
                } else if (down & KEY_Y) {
                    if (resp && strchr(resp, 'y')) {
                        ans = 'y';
                        break;
                    }
                } else if (down & KEY_X) {
                    if (resp && strchr(resp, 'q')) {
                        ans = 'q';
                        break;
                    }
                }
            }
            nh3ds_full_redraw();
            rint = ans;
            nh3ds_log("[yn buttons def='%c'] %s -> '%c'", def ? def : '?',
                      query, ans);
        }
    } else if (!strcmp(name, "shim_getlin")) {
        const char *query = args[0].s ? args[0].s : "";

        if (nh3ds_any_button_ever) {
            /* real play: software keyboard; ESC/cancel -> "\033" */
            if (nh3ds_swkbd(query, (char *) args[1].p, BUFSZ))
                nh3ds_log("[getlin] %s -> \"%s\"", query,
                          (char *) args[1].p);
            else {
                strcpy((char *) args[1].p, "\033");
                nh3ds_log("[getlin] %s -> ESC", query);
            }
        } else {
            strcpy((char *) args[1].p, "\033");
            nh3ds_log("[getlin demo] %s -> ESC", query);
        }
    } else if (!strcmp(name, "shim_get_ext_cmd")) {
        if (nh3ds_any_button_ever) {
            /* our own ext-command menu: top-screen list, live prefix
             * filtering from the touch letter keyboard (the core's
             * extcmd_via_menu() is TTY/CURSES-only) */
            rint = nh3ds_ext_menu();
            nh3ds_full_redraw();
            nh3ds_log("[get_ext_cmd] -> %d", rint);
        } else {
            rint = -1; /* demo mode: cancelled */
        }
    } else if (!strcmp(name, "shim_number_pad")) {
        /* nothing */
    } else if (!strcmp(name, "shim_delay_output")) {
        gspWaitForVBlank();
    } else if (!strcmp(name, "shim_change_color")
               || !strcmp(name, "shim_change_background")
               || !strcmp(name, "shim_preference_update")
               || !strcmp(name, "shim_status_init")) {
        /* nothing (status field labels come from nh3ds_st_initfmt[]) */
    } else if (!strcmp(name, "shim_status_update")) {
        int fldidx = args[0].i;
        void *ptr = args[1].p;
        int color = args[4].i;

        if (fldidx == BL_FLUSH) {
            nh3ds_st_dirty = TRUE;
        } else if (fldidx == BL_RESET) {
            /* the core re-sends every field after a reset */
            nh3ds_st_dirty = TRUE;
        } else if (fldidx >= 0 && fldidx < MAXBLSTATS) {
            if (fldidx == BL_CONDITION) {
                if (ptr)
                    nh3ds_st_cond = *(unsigned long *) ptr;
            } else {
                const char *text = (const char *) ptr;

                if (text) {
                    strncpy(nh3ds_st_val[fldidx], text,
                            MAXVALWIDTH - 1);
                    nh3ds_st_val[fldidx][MAXVALWIDTH - 1] = '\0';
                } else {
                    nh3ds_st_val[fldidx][0] = '\0';
                }
                /* tty-oriented fields carry padding and glyph encodings:
                 * title/ldesc are blank-padded, gold is \GXXXXXXXX:NNN */
                nh3ds_trim(nh3ds_st_val[fldidx]);
                if (nh3ds_st_val[fldidx][0] == '\\' 
                    && nh3ds_st_val[fldidx][1] == 'G'
                    && strlen(nh3ds_st_val[fldidx]) >= 11
                    && nh3ds_st_val[fldidx][10] == ':') {
                    nh3ds_st_val[fldidx][0] = '$';
                    (void) memmove(nh3ds_st_val[fldidx] + 1,
                                   nh3ds_st_val[fldidx] + 10,
                                   strlen(nh3ds_st_val[fldidx] + 10) + 1);
                }
                nh3ds_st_color[fldidx] = color;
            }
        }
    } else if (!strcmp(name, "shim_update_inventory")) {
        /* phase 2c: the shim contract says the port maintains the
         * persistent inventory from this event; the stub ignores it */
    } else if (!strcmp(name, "shim_ctrl_nhwindow")) {
        rptr = (void *) 0;
    } else if (!strcmp(name, "shim_putmsghistory")) {
        const char *msg = args[0].s;

        nh3ds_msg_add(msg ? msg : "");
    } else if (!strcmp(name, "shim_getmsghistory")) {
        rptr = (void *) 0; /* empty message history */
    } else if (!strcmp(name, "set_shim_font_name")) {
        rint = 0;
    } else if (!strcmp(name, "shim_get_color_string")) {
        rptr = (void *) "";
    } else {
        nh3ds_log("[UNHANDLED event] %s fmt=%s", name, fmt);
    }

    /* ---- write the return value --------------------------------------*/
    if (ret_ptr) {
        switch (rtype) {
        case 'c':
        case '0':
            *(char *) ret_ptr = (char) rint;
            break;
        case 'b':
            *(unsigned char *) ret_ptr = (unsigned char) (rint ? 1 : 0);
            break;
        case '1':
        case '2': /* short in C (set_shim_font_name) */
            *(short *) ret_ptr = (short) rint;
            break;
        case 'i':
            *(int *) ret_ptr = rint;
            break;
        case 's':
            *(const char **) ret_ptr = (const char *) rptr;
            break;
        case 'p':
            *(void **) ret_ptr = rptr;
            break;
        default: /* 'v': ret_ptr is NULL for VDECLCB events */
            break;
        }
    }
}

/* ----------------------------------------------------------------------*/
/* 3DS entry point                                                        */
/* ----------------------------------------------------------------------*/

static void
nh3ds_cleanup(void)
{
    nh3ds_dump_and_close();
    /* hold-on-exit screen: keep the exit reason visible until the user
     * acknowledges it (the screen is wiped on return to the launcher) */
    nh3ds_view = NH3DS_VM_EXIT;
    nh3ds_render_frame();
    for (;;) {
        u32 kdown;

        hidScanInput();
        kdown = hidKeysDown();
        if (kdown & (KEY_START | KEY_SELECT | KEY_A | KEY_B))
            break;
        svcSleepThread(50 * 1000 * 1000LL); /* 50 ms */
    }
    if (nh3ds_gpu_ok) {
        C2D_Fini();
        C3D_Fini();
    }
    gfxExit();
}

int
main(int argc, char *argv[])
{
    char *nh_argv[2];

    gfxInitDefault();
    /* The bottom screen keeps the libctru console (message history,
     * touch verb grid, keyboards, menu detail pane); the top screen is
     * a citro2d stereo viewport (see the depth section above). */
    nh3ds_bot = consoleInit(GFX_BOTTOM, &nh3ds_botcon);
    nh3ds_gpu_init();
    nh3ds_st_init();
    nh3ds_scr(0);
    printf("\x1b[2J\x1b[0m\x1b[1;1HNetHack 3DS booting...\x1b[0m");

    if (chdir(nh3ds_hackdir) < 0) {
        printf("nh3ds: cannot chdir to %s\n", nh3ds_hackdir);
        (void) snprintf(nh3ds_lastwords, sizeof nh3ds_lastwords,
                        "cannot chdir to %s -- is the nethack/ data folder "
                        "(from the zip) on the SD card?", nh3ds_hackdir);
    } else {
        FILE *f;
        /* debug aid: force the 3D slider, dump both eyes as BMPs into
         * a ring, and let the rig autopilot hunt the down-stairs; a
         * file containing 'L' also pins the hero to the levitation
         * plane for a visual test of that layer */
        nh3ds_debug3d = (access("3drig", F_OK) == 0);
        if (nh3ds_debug3d) {
            /* the trace log is a rig aid: an unconditional log would
             * mean SD-card wear and clutter on every real player's
             * card */
            nh3ds_trace = fopen("nh3ds.log", "w");
            if (nh3ds_trace)
                nh3ds_log("[start] NetHack 3DS phase 4a (stereo map), "
                          "hackdir=%s", nh3ds_hackdir);
            f = fopen("3drig", "r");
            if (f) {
                char flagbuf[16];

                if (fgets(flagbuf, (int) sizeof flagbuf, f)) {
                    nh3ds_debug_lev = strchr(flagbuf, 'L') != NULL;
                    nh3ds_debug_sweep = strchr(flagbuf, 'M') != NULL;
                }
                (void) fclose(f);
            }
            /* the rig runs in debug/wizard mode (magic mapping via ^F)
             * without needing an OPTIONS=playmode:debug line in the
             * config files -- those stay player-clean */
            wizard = TRUE;
        }
        /* CHDIR is disabled in the SHIM_GRAPHICS configuration, so
         * check_recordfile() never runs; create the variable files. */
        nh3ds_ensure_file("record");
        nh3ds_ensure_file("logfile");
        nh3ds_ensure_file("xlogfile");
        /* set_savefile_name() builds "save/<uid><plname>" relative to the
         * playground directory; without the subdirectory create_savefile()
         * fails and #save can never write ("Cannot open save file."), nor
         * can the automatic restore on the next boot find anything. */
        (void) mkdir("save", 0777); /* EEXIST is fine */
    }

    (void) atexit(nh3ds_cleanup);

    nh_argv[0] = (char *) "NetHack3ds";
    nh_argv[1] = (char *) 0;
    (void) argc;
    (void) argv;

    /* first frame: legend over the empty map, so the top screen is
     * never showing an uninitialized framebuffer */
    nh3ds_render_frame();

    shim_graphics_set_callback(shim_cb);
    (void) nhmain(1, nh_argv); /* moveloop() only returns at game over */

    nh3ds_log("[game over] nhmain returned");
    return 0;
}

/*nh3ds.c*/
