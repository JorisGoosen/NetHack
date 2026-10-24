/* NetHack 5.0 nh3ds.c    $NHDT-Date: $  $NHDT-Branch: 3ds $:$NHDT-Revision: $ */
/* Copyright (c) the NetHack 3DS port contributors, 2026.                */
/* NetHack may be freely redistributed.  See license for details.        */

/* Nintendo 3DS front-end for NetHack 5.0.1 (libctru + shim windowport).
 *
 * The game core never touches the 3DS: it runs through win/shim/winshim.c,
 * which forwards every windowing call to the single variadic callback
 * implemented here.  This file owns the 3DS: console rendering, hid input,
 * the software keyboard, and the process lifecycle.
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
 * Build:  make CROSS_TO_3DS=1 all        (see sys/unix/hints/include/
 *         make CROSS_TO_3DS=1 3dspkg      cross-pre2.501 for the flags)
 */

#include "hack.h"       /* svp.plname, iflags, flags, NHW_*, ... */

#include <3ds.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
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
static const char *nh3ds_plname = "Wizard";

/* ----------------------------------------------------------------------*/
/* trace log                                                              */
/* ----------------------------------------------------------------------*/

static FILE *nh3ds_trace;

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

static winid nh3ds_nextwin = 1;   /* synthetic winid counter */

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

/* map glyph buffer: the whole level as print_glyph delivered it */
struct nh3ds_cell {
    char c;                     /* ttychar from glyph_info */
    signed char color;          /* gm.sym.color, -1 = default */
};
static struct nh3ds_cell nh3ds_map[ROWNO][COLNO];
static boolean nh3ds_map_dirty;
static coordxy nh3ds_vx, nh3ds_vy; /* viewport origin (top-left cell) */

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
static winid nh3ds_pop_win = -1;
static char nh3ds_pop_lines[NH3DS_POPLINES][NH3DS_POPLINELEN];
static int nh3ds_pop_n;
static char nh3ds_pop_prompt[NH3DS_POPLINELEN];

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
/* console helpers                                                        */
/* ----------------------------------------------------------------------*/

static PrintConsole nh3ds_topcon, nh3ds_botcon;
static PrintConsole *nh3ds_top, *nh3ds_bot;

static void
nh3ds_scr(int top) /* select which screen subsequent printf()s go to */
{
    consoleSelect(top ? nh3ds_top : nh3ds_bot);
}

/* NetHack CLR_* color to an ANSI SGR sequence; CLR_BLACK is forced to
 * bright black (dark gray) because the console background is black */
static const char *
nh3ds_esc(int clr)
{
    static char buf[12];

    if (clr <= 0)               /* NO_COLOR and CLR_BLACK */
        return clr == 0 ? "\x1b[30;1m" : "\x1b[0m";
    if (clr < 8) {
        buf[0] = '\0';
        (void) sprintf(buf, "\x1b[3%dm", clr);
    } else {
        buf[0] = '\0';
        (void) sprintf(buf, "\x1b[3%d;1m", clr - 8);
    }
    return buf;
}

/* ----------------------------------------------------------------------*/
/* renderers                                                              */
/* ----------------------------------------------------------------------*/

static void
nh3ds_draw_map(void)
{
    char line[MAP_VIEW_COLS * 10 + 24];
    int cy, mx;
    int lastclr = -99;

    nh3ds_scr(1);
    for (cy = 0; cy < MAP_ROWS; ++cy) {
        char *p = line;
        int my = (int) nh3ds_vy + cy;

        p += sprintf(p, "\x1b[%d;1H", MAP_ROW0 + cy + 1);
        lastclr = -99;
        for (mx = 0; mx < MAP_VIEW_COLS; ++mx) {
            int cx = (int) nh3ds_vx + mx;
            char c = ' ';
            int clr = -1;

            if (my >= 0 && my < ROWNO && cx >= 0 && cx < COLNO) {
                c = nh3ds_map[my][cx].c ? nh3ds_map[my][cx].c : ' ';
                clr = nh3ds_map[my][cx].color;
            }
            if (clr != lastclr) {
                p += sprintf(p, "%s", nh3ds_esc(clr));
                lastclr = clr;
            }
            *p++ = c;
        }
        *p++ = '\0';
        printf("%s\x1b[0m", line);
    }
    nh3ds_map_dirty = FALSE;
}

static void
nh3ds_draw_messages(void)
{
    int i, oldest, n;

    nh3ds_scr(1);
    oldest = (nh3ds_msg_head - nh3ds_msg_count + NH3DS_NMSGS) % NH3DS_NMSGS;
    oldest = (oldest + nh3ds_msg_count
              - (nh3ds_msg_count < MSG_ROWS ? nh3ds_msg_count : MSG_ROWS))
             % NH3DS_NMSGS;
    n = (nh3ds_msg_count < MSG_ROWS) ? nh3ds_msg_count : MSG_ROWS;
    for (i = 0; i < MSG_ROWS; ++i) {
        const char *s = "";

        if (i < n)
            s = nh3ds_msgs[(oldest + i) % NH3DS_NMSGS];
        printf("\x1b[%d;1H\x1b[0m%-50.50s", MSG_ROW0 + i + 1, s);
    }
    nh3ds_msg_dirty = FALSE;
}

/* append one message line to the ring */
static void
nh3ds_msg_add(const char *s)
{
    if (!s)
        s = "";
    strncpy(nh3ds_msgs[nh3ds_msg_head], s, NH3DS_MSGLEN - 1);
    nh3ds_msgs[nh3ds_msg_head][NH3DS_MSGLEN - 1] = '\0';
    nh3ds_msg_head = (nh3ds_msg_head + 1) % NH3DS_NMSGS;
    if (nh3ds_msg_count < NH3DS_NMSGS)
        ++nh3ds_msg_count;
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
    char escbuf[TOP_COLS * 12 + 16];
    char *p = escbuf;
    int len = 0, i;

    p += sprintf(p, "\x1b[%d;1H", ST_ROW0 + row + 1);
    for (i = 0; i < nflds; ++i) {
        int fld = flds[i];
        char seg[TOP_COLS + 1];

        if (fld < 0 || fld >= MAXBLSTATS || !nh3ds_st_active[fld])
            continue;
        if (!nh3ds_st_val[fld][0])
            continue; /* field disabled or not (yet) reported */
        if (nh3ds_st_fmt[fld][0])
            (void) sprintf(seg, nh3ds_st_fmt[fld], nh3ds_st_val[fld]);
        else
            (void) sprintf(seg, "%s", nh3ds_st_val[fld]);
        if (!*seg || !strcmp(seg, " "))
            continue;
        if (len + (int) strlen(seg) > TOP_COLS)
            break;
        p += sprintf(p, "%s%s", nh3ds_esc(nh3ds_st_color[fld]), seg);
        len += (int) strlen(seg);
    }
    if (use_cond) {
        int k;

        for (k = 0; k < (int) (sizeof nh3ds_cond / sizeof nh3ds_cond[0]);
             ++k) {
            char seg[16];

            if ((nh3ds_st_cond & nh3ds_cond[k].mask) == 0L)
                continue;
            (void) sprintf(seg, " %s", nh3ds_cond[k].tag);
            if (len + (int) strlen(seg) > TOP_COLS)
                break;
            p += sprintf(p, "%s%s", nh3ds_esc(11 /* yellow */), seg);
            len += (int) strlen(seg);
        }
    }
    printf("%s\x1b[0m%-*s", escbuf, TOP_COLS - len, "");
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

    nh3ds_scr(1);
    nh3ds_st_row(0, row1flds, (int) (sizeof row1flds / sizeof row1flds[0]),
                 FALSE);
    nh3ds_st_row(1, row2flds, (int) (sizeof row2flds / sizeof row2flds[0]),
                 TRUE);
    nh3ds_st_dirty = FALSE;
}

static const char *const nh3ds_legend[TOP_ROWS - LEG_ROW0] = {
    "pad=hjkl two=diag L/R=run",
    "A=rest B=inv X=grab Y=search",
    "START=save SELECT=help",
    "NetHack 3DS -- phase 2a",
};

static void
nh3ds_draw_legend(void)
{
    int i;

    nh3ds_scr(1);
    for (i = 0; i < TOP_ROWS - LEG_ROW0; ++i)
        printf("\x1b[%d;1H\x1b[0m%-*s", LEG_ROW0 + i + 1, TOP_COLS,
               nh3ds_legend[i]);
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

/* bottom screen default view: message-history tail */
static void
nh3ds_draw_bot_msgs(void)
{
    int i, n, oldest;

    nh3ds_scr(0);
    printf("\x1b[2J\x1b[0m");
    n = (nh3ds_msg_count < BOT_ROWS - 1) ? nh3ds_msg_count : BOT_ROWS - 1;
    oldest = (nh3ds_msg_head - nh3ds_msg_count + NH3DS_NMSGS) % NH3DS_NMSGS;
    oldest = (oldest + nh3ds_msg_count - n) % NH3DS_NMSGS;
    for (i = 0; i < n; ++i)
        printf("\x1b[%d;1H\x1b[0m%-40.40s", i + 1,
               nh3ds_msgs[(oldest + i) % NH3DS_NMSGS]);
}

/* wait for any button press (edge-triggered: must be released first).
 * Demo mode (no button ever pressed since boot): continue on our own
 * after ~1.5s so unattended runs flow through blocking displays. */
static boolean nh3ds_any_button_ever;

static void
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
            return;
        }
        if (nh3ds_any_button_ever)
            continue; /* a real player is present: wait indefinitely */
        if (++frames >= 90)
            return; /* demo mode: auto-continue */
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
    if (nh3ds_msg_dirty)
        nh3ds_draw_messages();
    if (nh3ds_map_dirty)
        nh3ds_draw_map();
    if (nh3ds_st_dirty)
        nh3ds_draw_status();
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
        nh3ds_log("[init_nhwindows] window_inited=TRUE");
    } else if (!strcmp(name, "shim_player_selection")) {
        /* phase 2c: role dialog on the bottom screen; phase 1/2a:
         * role_init() randomizes unspecified facets */
        nh3ds_log("[player_selection] canned: role/race/gend/align random");
    } else if (!strcmp(name, "shim_player_selection_or_tty")) {
        rint = 0; /* FALSE: don't run the generic tty-style setup */
    } else if (!strcmp(name, "shim_askname")) {
        (void) strncpy(svp.plname, nh3ds_plname,
                       sizeof svp.plname - 1);
        svp.plname[sizeof svp.plname - 1] = '\0';
        nh3ds_log("[askname] canned plname=\"%s\"", svp.plname);
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

        rint = (int) nh3ds_nextwin++;
        if (rint > 0 && rint < NH3DS_MAXWINID)
            nh3ds_wintype[rint] = (unsigned char) (type + 1);
        if (type == NHW_MENU || type == NHW_TEXT) {
            nh3ds_pop_win = (winid) rint;
            nh3ds_pop_n = 0;
            nh3ds_pop_prompt[0] = '\0';
        }
        nh3ds_log("[create_nhwindow] type=%s -> winid=%d",
                  nh3ds_wintypename(type), rint);
    } else if (!strcmp(name, "shim_clear_nhwindow")) {
        winid w = (winid) args[0].i;
        int ty = (w > 0 && w < NH3DS_MAXWINID) ? nh3ds_wintype[w] - 1 : -1;

        if (ty == NHW_MAP) {
            memset(nh3ds_map, 0, sizeof nh3ds_map);
            nh3ds_map_dirty = TRUE;
        }
    } else if (!strcmp(name, "shim_display_nhwindow")) {
        winid w = (winid) args[0].i;
        boolean blocking = (args[1].i != 0);
        int ty = (w > 0 && w < NH3DS_MAXWINID) ? nh3ds_wintype[w] - 1 : -1;

        nh3ds_refresh();
        if (blocking) {
            if ((ty == NHW_MENU || ty == NHW_TEXT) && w == nh3ds_pop_win) {
                nh3ds_draw_popup();
                nh3ds_wait_button();
                nh3ds_draw_bot_msgs();
            } else {
                /* --More-- style pause (message window, map, ...) */
                nh3ds_wait_button();
            }
        }
    } else if (!strcmp(name, "shim_destroy_nhwindow")) {
        winid w = (winid) args[0].i;

        if (w == nh3ds_pop_win)
            nh3ds_pop_win = -1;
        if (w > 0 && w < NH3DS_MAXWINID)
            nh3ds_wintype[w] = 0;
    } else if (!strcmp(name, "shim_curs")) {
        /* phase 2: position indicator; the hero glyph already shows */
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
        }
    } else if (!strcmp(name, "shim_display_file")) {
        nh3ds_log("[display_file] %s", args[0].s ? args[0].s : "");
    } else if (!strcmp(name, "shim_start_menu")) {
        /* nothing yet (phase 2c: menu buffering per winid) */
    } else if (!strcmp(name, "shim_add_menu")) {
        /* phase 2c: real menu items; phase 2a: append "ch) str" to the
         * popup text so blocking menus are readable.  fmt is
         * "vipi00iisi": 0=win, 2=identifier, 3=ch, 6=clr, 7=str,
         * 8=itemflags (the shim forwards plain varargs). */
        winid w = (winid) args[0].i;
        char ch = (char) args[3].i;
        const char *str = args[7].s ? args[7].s : "";

        if (w == nh3ds_pop_win && nh3ds_pop_n < NH3DS_POPLINES) {
            if (ch && ch != ' ')
                (void) sprintf(nh3ds_pop_lines[nh3ds_pop_n], "%c) %s", ch,
                               str);
            else
                (void) sprintf(nh3ds_pop_lines[nh3ds_pop_n], "  %s", str);
            ++nh3ds_pop_n;
        }
    } else if (!strcmp(name, "shim_end_menu")) {
        const char *prompt = args[1].s;

        if (prompt && *prompt) {
            strncpy(nh3ds_pop_prompt, prompt, NH3DS_POPLINELEN - 1);
            nh3ds_pop_prompt[NH3DS_POPLINELEN - 1] = '\0';
        }
    } else if (!strcmp(name, "shim_select_menu")) {
        /* phase 2c: bottom-screen menu selection.  Until then: report
         * "cancelled" (-1 == ESC).  Returning 0 would mean "empty menu,
         * nothing selected"; core loops such as ask_do_tutorial()
         * (options.c) re-issue the menu until they get a non-zero
         * result, so 0 caused an infinite create_nhwindow flood
         * (~6400/s).  -1 ends every such loop the ESC way. */
        if (args[2].p)
            *(MENU_ITEM_P **) args[2].p = (MENU_ITEM_P *) 0;
        rint = -1;
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
        const glyph_info *gi = (const glyph_info *) args[3].p;

        if (x >= 0 && x < COLNO && y >= 0 && y < ROWNO && gi) {
            nh3ds_map[y][x].c = (char) gi->ttychar;
            nh3ds_map[y][x].color =
                (signed char) (gi->gm.sym.color <= 0 ? -1
                                                     : gi->gm.sym.color);
            nh3ds_map_dirty = TRUE;
        }
    } else if (!strcmp(name, "shim_raw_print")
               || !strcmp(name, "shim_raw_print_bold")) {
        /* error()/panic() surface here -- always show and log */
        printf("%s\n", args[0].s ? args[0].s : "");
        nh3ds_log("[raw_print] %s", args[0].s ? args[0].s : "");
    } else if (!strcmp(name, "shim_nhgetch")) {
        /* rest one turn: keeps moveloop ticking */
        rint = '.';
        ++nh3ds_getch_n;
        if (nh3ds_getch_n == 1 || nh3ds_getch_n % 2048 == 0)
            nh3ds_log("[nhgetch n=%lu] returning '.'", nh3ds_getch_n);
        gspWaitForVBlank();
    } else if (!strcmp(name, "shim_nh_poskey")) {
        /* P1 canned input: alternate the 'm' (#reqmenu) prefix with '.'
         * (rest).  A plain '.' is refused by safe_wait when a monster is
         * nearby ("Are you waiting to get hit?  Use 'm' prefix to force
         * a no-op") which would freeze the turn counter; the m/. pair is
         * exactly the forced no-op rest the game itself suggests, so the
         * moveloop turn counter advances.  Phase 2b: real button input. */
        rint = (nh3ds_poskey_n & 1) ? '.' : 'm';
        ++nh3ds_poskey_n;
        if (nh3ds_poskey_n == 1 || nh3ds_poskey_n % 16384 == 0)
            nh3ds_log("[nh_poskey n=%lu] moves=%ld returning '%c'",
                      nh3ds_poskey_n, svm.moves, (char) rint);
        nh3ds_refresh();
        gspWaitForVBlank();
    } else if (!strcmp(name, "shim_nhbell")) {
        /* nothing */
    } else if (!strcmp(name, "shim_doprev_message")) {
        rint = 0;
    } else if (!strcmp(name, "shim_yn_function")) {
        const char *query = args[0].s ? args[0].s : "";
        char def = (char) args[2].i;

        /* relaunch friendliness: a stale lock file from a killed run
         * prompts "There is already a game in progress under your name.
         * Destroy old game?" -- answer yes so save/restore can be tested
         * repeatedly (phase 2b: real prompt via buttons) */
        if (strstr(query, "Destroy old game"))
            rint = 'y';
        else
            rint = def ? def : 'n';
        nh3ds_log("[yn_function def='%c'] %s -> '%c'",
                  def ? def : '?', query, (char) rint);
    } else if (!strcmp(name, "shim_getlin")) {
        /* cancel-style answer; phase 2c: software keyboard */
        if (args[1].p)
            strcpy((char *) args[1].p, "\033");
        nh3ds_log("[getlin] %s -> ESC", args[0].s ? args[0].s : "");
    } else if (!strcmp(name, "shim_get_ext_cmd")) {
        rint = -1; /* cancelled */
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
    gfxExit();
}

int
main(int argc, char *argv[])
{
    char *nh_argv[2];

    gfxInitDefault();
    /* Top screen 50x30, bottom 40x30 with the default 8x8 font.  The
     * top screen is the game view; the bottom screen shows message
     * history and (phase 2c) menus and the software keyboard. */
    /* Separate PrintConsole structs: consoleInit(..., NULL) would hand
     * back the same static default console for both screens, and the
     * second init would retarget it to the bottom screen. */
    nh3ds_top = consoleInit(GFX_TOP, &nh3ds_topcon);
    nh3ds_bot = consoleInit(GFX_BOTTOM, &nh3ds_botcon);
    nh3ds_st_init();
    nh3ds_scr(1);
    printf("\x1b[2J\x1b[0m");
    nh3ds_draw_legend();
    nh3ds_scr(0);
    printf("\x1b[2J\x1b[0m\x1b[1;1HNetHack 3DS booting...\x1b[0m");
    nh3ds_scr(1);

    if (chdir(nh3ds_hackdir) < 0) {
        printf("nh3ds: cannot chdir to %s\n", nh3ds_hackdir);
    } else {
        nh3ds_trace = fopen("nh3ds.log", "w");
        if (nh3ds_trace)
            nh3ds_log("[start] NetHack 3DS phase 2a, hackdir=%s",
                      nh3ds_hackdir);
        /* CHDIR is disabled in the SHIM_GRAPHICS configuration, so
         * check_recordfile() never runs; create the variable files. */
        nh3ds_ensure_file("record");
        nh3ds_ensure_file("logfile");
        nh3ds_ensure_file("xlogfile");
    }

    (void) atexit(nh3ds_cleanup);

    nh_argv[0] = (char *) "NetHack3ds";
    nh_argv[1] = (char *) 0;
    (void) argc;
    (void) argv;

    shim_graphics_set_callback(shim_cb);
    (void) nhmain(1, nh_argv); /* moveloop() only returns at game over */

    nh3ds_scr(1);
    printf("NetHack 3DS: game over.\n");
    nh3ds_log("[game over] nhmain returned");
    return 0;
}

/*nh3ds.c*/
