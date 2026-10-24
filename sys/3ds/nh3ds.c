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
 * Build:  make CROSS_TO_3DS=1 all        (see sys/unix/hints/include/
 *         make CROSS_TO_3DS=1 3dspkg      cross-pre2.501 for the flags)
 */

#include "hack.h"       /* svp.plname, iflags, flags, NHW_*, ... */
#include "func_tab.h"   /* extcmdlist, CMD_* flags (not in hack.h) */

#include <3ds.h>

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
static const char *nh3ds_plname = "Wizard";

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

/* bottom-screen layout: message tail, then the touch verb grid (see
 * the touch section below) */
#define TBOT_MSGROWS 14

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
    "pad=hjkl 2=diag L+pad=run R+pad=travel",
    "A=rest B=inv X=grab Y=search L=shift",
    "START=save SELECT=help",
    "NetHack 3DS",
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
    if (nh3ds_msg_dirty) {
        nh3ds_draw_messages();
        /* the bottom screen is the message-history tail; redraw it in
         * step.  (No guard for popups/menus needed: those waits are
         * synchronous inside our own handlers, so nh_poskey -- the only
         * refresh() caller -- cannot fire while the bottom is owned;
         * the perm_invent window keeps a MENU winid alive forever,
         * which would block the tail forever if we tested pop_win.) */
        nh3ds_draw_bot_msgs();
    }
    if (nh3ds_map_dirty)
        nh3ds_draw_map();
    if (nh3ds_st_dirty)
        nh3ds_draw_status();
}

/* full redraw after something else owned a screen (menus render on the
 * top screen and wipe the map) */
static void
nh3ds_full_redraw(void)
{
    nh3ds_msg_dirty = TRUE;
    nh3ds_map_dirty = TRUE;
    nh3ds_st_dirty = TRUE;
    nh3ds_refresh();
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

/* verb buttons: below the message tail during play -- 4 rows x 5
 * columns of 8x2-cell buttons (label row + spacer row, one gap row
 * between button rows).  Kick is ^D; locking/unlocking a door is
 * apply-a-key/lockpick ('a') or #force via the command menu. */
#define TBOT_ROW0 15            /* 0-based console row of the first button */
#define TBOT_STEP 3             /* 2-row button + 1 gap row */
#define TBOT_COLW 8
struct nh3ds_verb {
    int key;
    const char label[9];
};
static const struct nh3ds_verb nh3ds_verbs[] = {
    { 'a', "a:apply " }, { 'o', "o:open  " }, { 'c', "c:close " },
    { 0x04, "^D:kick " }, { 'q', "q:quaff " },
    { 'z', "z:zap   " }, { 'Z', "Z:spell " }, { 'e', "e:eat   " },
    { 'r', "r:read  " }, { 'w', "w:wield " },
    { 't', "t:throw " }, { 'f', "f:fire  " }, { 'd', "d:drop  " },
    { 'W', "W:wear  " }, { 'P', "P:puton " },
    { 'T', "T:tkoff " }, { 'R', "R:rmv   " }, { 'C', "C:call  " },
    { '*', "*:equip " }, { '#', "#:cmd   " },
};

/* key for the verb button under a tapped cell, or 0 */
static int
nh3ds_verb_at(int cx, int cy)
{
    int r, c;

    if (cy < TBOT_ROW0 || cy >= TBOT_ROW0 + 4 * TBOT_STEP)
        return 0;
    if ((cy - TBOT_ROW0) % TBOT_STEP >= 2)
        return 0; /* gap row between button rows */
    r = (cy - TBOT_ROW0) / TBOT_STEP;
    c = cx / TBOT_COLW;
    if (c >= 5)
        return 0;
    return nh3ds_verbs[r * 5 + c].key;
}

/* draw the verb grid + a hint line (bottom screen rows 16..28) */
static void
nh3ds_bot_buttons(void)
{
    int r, c;

    nh3ds_scr(0);
    for (r = 0; r < 4; ++r)
        for (c = 0; c < 5; ++c)
            printf("\x1b[%d;%dH\x1b[7m%-8.8s\x1b[0m",
                   TBOT_ROW0 + r * TBOT_STEP + 1, c * TBOT_COLW + 1,
                   nh3ds_verbs[r * 5 + c].label);
    printf("\x1b[28;1H\x1b[36m%-40.40s",
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
    int r, c;

    nh3ds_scr(0);
    for (r = 0; r < 3; ++r)
        for (c = 0; c < 10; ++c)
            printf("\x1b[%d;%dH\x1b[7m %c \x1b[0m",
                   KBD_ROW0 + r * KBD_STEP + 3, c * KBD_COLW + 1,
                   nh3ds_keys[r * 10 + c].base);
    printf("\x1b[29;1H\x1b[36m%-40.40s", "L=shift");
}

/* character under a tapped cell (shift applied), or 0 */
static int
nh3ds_kbd_map(int cx, int cy)
{
    int r, c, i;

    if (cy < KBD_ROW0 || cy >= KBD_ROW0 + 3 * KBD_STEP)
        return 0;
    r = (cy - KBD_ROW0) / KBD_STEP;
    c = cx / KBD_COLW;
    i = r * 10 + c;
    if (c >= 10 || i >= 30)
        return 0;
    return (hidKeysHeld() & KEY_L) ? nh3ds_keys[i].shifted
                                   : nh3ds_keys[i].base;
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
/* hid input (phase 2b)                                                   */
/* ----------------------------------------------------------------------*/

static boolean nh3ds_demo_toggle; /* canned m/. alternation in demo mode */

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
    } else if (btns) {
        if (btns & KEY_A)
            key = '.', repeatable = TRUE;
        else if (btns & KEY_B)
            key = 'i';
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

    if (!key) {
        int tcol, trow;

        if (nh3ds_tap(&tcol, &trow)) {
            key = nh3ds_verb_at(tcol, trow);
            if (key) {
                last_key = key;
                last_mask = 0;
                frames_since = 0;
                nh3ds_any_button_ever = TRUE;
                return key; /* touch verb: edge-triggered */
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

/* blocking read used by both input events */
static int
nh3ds_read_key(void)
{
    int key;

    for (;;) {
        key = nh3ds_next_key();
        if (key)
            return key;
        /* demo mode: nobody has ever touched the controls, keep the
         * game moving with the forced no-op rest ('m' prefix + '.');
         * once any button is seen, real input takes over for good */
        if (!nh3ds_any_button_ever) {
            nh3ds_demo_toggle = !nh3ds_demo_toggle;
            return nh3ds_demo_toggle ? '.' : 'm';
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
    return nh3ds_menu.item[i].ch != '\0';
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
    int i, row, top;

    if (cursor >= 0) {
        if (cursor < nh3ds_menu.top)
            nh3ds_menu.top = cursor;
        if (cursor >= nh3ds_menu.top + rows)
            nh3ds_menu.top = cursor - rows + 1;
    }
    top = nh3ds_menu.top;

    nh3ds_scr(1);
    printf("\x1b[2J\x1b[0m");
    printf("\x1b[1;1H\x1b[37;1m%-50.50s", nh3ds_menu.prompt);
    for (row = 0, i = top; i < nh3ds_menu.n && row < rows; ++i, ++row) {
        const struct nh3ds_menuitem *m = &nh3ds_menu.item[i];
        char line[128];
        const char *tag = "  ";

        if (nh3ds_menu_selectable(i)) {
            tag = (cursor == i) ? "> " : "  ";
            if (m->itemflags & MENU_ITEMFLAGS_SELECTED)
                tag = "* ";
        }
        if (m->ch && m->ch != ' ')
            (void) sprintf(line, "%s%c) %s", tag, m->ch, m->str);
        else
            (void) sprintf(line, "%s%s", tag, m->str);
        if (nh3ds_menu_selectable(i))
            printf("\x1b[%d;1H\x1b[0m%-47.47s", row + 2, line);
        else
            printf("\x1b[%d;1H\x1b[36;1m%-47.47s", row + 2, line);
    }
    printf("\x1b[%d;1H\x1b[36m%-50.50s", TOP_ROWS - 1,
           "pad=move A=ok B=cancel letter=select");
    if (cursor < 0) {
        nh3ds_scr(0);
        printf("\x1b[2J\x1b[0m\x1b[29;1H\x1b[0m%-40.40s", "[A] continue");
    } else {
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
                return 0; /* tap: caller reads *tcol/*trow */
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
        } else if (down & KEY_DDOWN) {
            for (i = cursor + 1; i < nh3ds_menu.n; ++i)
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
        }
        /* SELECT/Y/X: ignored for now */
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

#define NH3DS_EXTMAX 128

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
    /* available commands as extcmdlist[] indices */
    int idx[NH3DS_EXTMAX];
    const struct ext_func_tab *efp;
    char filter[24];
    int n = 0, flen = 0, mtop = 0, cursor = 0, i;

    for (efp = extcmdlist; efp->ef_txt && n < NH3DS_EXTMAX; ++efp) {
        if ((efp->flags & (CMD_NOT_AVAILABLE | INTERNALCMD))
            || !(efp->flags & AUTOCOMPLETE)
            || (!wizard && (efp->flags & WIZMODECMD)))
            continue;
        idx[n++] = (int) (efp - extcmdlist);
    }
    if (!n)
        return -1;
    filter[0] = '\0';

    for (;;) {
        int match[NH3DS_EXTMAX];
        int nm = 0;
        boolean filter_changed = FALSE;
        u32 down;
        int tcol, trow;

        for (i = 0; i < n; ++i)
            if (!flen
                || nh3ds_prefix_ci(extcmdlist[idx[i]].ef_txt, filter, flen))
                match[nm++] = idx[i];

        if (filter_changed) { /* new filter: jump to the first match */
            cursor = 0;
            mtop = 0;
        }
        /* clamp the cursor into the current match list */
        if (cursor >= nm)
            cursor = nm - 1;
        if (cursor < 0)
            cursor = 0;
        if (mtop > cursor)
            mtop = cursor;
        if (mtop > nm - 1)
            mtop = nm - 1;
        if (mtop < 0)
            mtop = 0;
        nh3ds_scr(1);
        printf("\x1b[2J\x1b[0m");
        printf("\x1b[1;1H\x1b[37;1m#%.22s_\x1b[0m", filter);
        for (i = mtop; i < nm && i < mtop + 26; ++i) {
            const struct ext_func_tab *ef = &extcmdlist[match[i]];

            printf("\x1b[%d;1H\x1b[0m%s%-14.14s %.32s", i - mtop + 2,
                   (i == cursor) ? "> " : "  ", ef->ef_txt,
                   ef->ef_desc ? ef->ef_desc : "");
        }
        if (!nm)
            printf("\x1b[2;1H\x1b[31m  (no matching command)");
        printf("\x1b[%d;1H\x1b[36m%-50.50s", TOP_ROWS,
               "A=do B=bsp START=cancel pad=move");

        /* bottom screen: description of the highlighted command over
         * the letter keyboard */
        nh3ds_detail(nm ? (extcmdlist[match[cursor]].ef_desc
                           ? extcmdlist[match[cursor]].ef_desc
                           : extcmdlist[match[cursor]].ef_txt)
                        : "no matching command");
        nh3ds_kbd_render();

        down = nh3ds_menu_input(&tcol, &trow);
        if (tcol >= 0) {
            int ch = nh3ds_kbd_map(tcol, trow);

            if (ch && flen < (int) sizeof filter - 1) {
                filter[flen++] = nh3ds_lower((char) ch);
                filter_changed = TRUE;
            }
        } else if (down & KEY_DUP) {
            if (cursor > 0)
                --cursor;
        } else if (down & KEY_DDOWN) {
            if (cursor < nm - 1)
                ++cursor;
        } else if (down & KEY_B) {
            if (flen) {
                filter[--flen] = '\0';
                filter_changed = TRUE;
            } else {
                return -1;
            }
        } else if (down & KEY_START) {
            return -1;
        } else if (down & KEY_A) {
            if (nm) {
                nh3ds_full_redraw();
                return match[cursor];
            }
        }
    }
}

/* at character-creation prompts: give a real player a short window to
 * press a button; unattended runs fall through to the canned answers */
static void
nh3ds_detect_player(int frames)
{
    int f;

    if (nh3ds_any_button_ever)
        return;
    for (f = 0; f < frames; ++f) {
        hidScanInput();
        gspWaitForVBlank();
        if (hidKeysHeld() != 0) {
            nh3ds_any_button_ever = TRUE;
            return;
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
        nh3ds_log("[init_nhwindows] window_inited=TRUE");
    } else if (!strcmp(name, "shim_player_selection")) {
        nh3ds_detect_player(180); /* 3s to press a button */
        if (nh3ds_any_button_ever) {
            /* real play: generic role/race/gender/alignment menus (the
             * core's genl setup drives this windowport's own menus) */
            (void) genl_player_setup(80);
            nh3ds_log("[player_selection] genl setup done");
        } else {
            /* demo mode: role_init() randomizes unspecified facets */
            nh3ds_log("[player_selection] canned: role/race/gend/align random");
        }
    } else if (!strcmp(name, "shim_player_selection_or_tty")) {
        rint = 0; /* FALSE: don't run the generic tty-style setup */
    } else if (!strcmp(name, "shim_askname")) {
        nh3ds_detect_player(180); /* 3s to press a button */
        if (nh3ds_any_button_ever) {
            /* real play: software keyboard */
            if (nh3ds_swkbd("Who are you?", svp.plname,
                            sizeof svp.plname))
                nh3ds_log("[askname] swkbd plname=\"%s\"", svp.plname);
            else {
                (void) strncpy(svp.plname, nh3ds_plname,
                               sizeof svp.plname - 1);
                svp.plname[sizeof svp.plname - 1] = '\0';
                nh3ds_log("[askname] swkbd cancelled, using \"%s\"",
                          svp.plname);
            }
        } else {
            /* demo mode: canned name */
            (void) strncpy(svp.plname, nh3ds_plname,
                           sizeof svp.plname - 1);
            svp.plname[sizeof svp.plname - 1] = '\0';
            nh3ds_log("[askname] canned plname=\"%s\"", svp.plname);
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

        if (nh3ds_menu.active && w == nh3ds_menu.win
            && nh3ds_menu.n < NH3DS_MAXMENU) {
            struct nh3ds_menuitem *m = &nh3ds_menu.item[nh3ds_menu.n];

            m->id = *(const ANY_P *) (const void *) (uintptr_t) args[2].i;
            m->ch = ch;
            m->gch = (char) args[4].i;
            m->attr = args[5].i;
            m->clr = args[6].i;
            (void) strncpy(m->str, str, sizeof m->str - 1);
            m->str[sizeof m->str - 1] = '\0';
            m->itemflags = (unsigned) args[8].i;
            ++nh3ds_menu.n;
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
            /* display-only list: show it and wait for a key (tty-like) */
            if (nh3ds_any_button_ever) {
                nh3ds_menu_render(-1);
                nh3ds_wait_button();
                nh3ds_full_redraw();
            }
            nh3ds_menu.active = FALSE;
            rint = 0;
        } else if (!nh3ds_any_button_ever) {
            /* demo mode: cancel every interactive menu so unattended
             * runs keep flowing */
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

        /* relaunch friendliness in demo mode: a stale lock file from a
         * killed run prompts "There is already a game in progress under
         * your name.  Destroy old game?" -- answer yes automatically so
         * unattended save/restore testing keeps working */
        if (!nh3ds_any_button_ever) {
            if (strstr(query, "Destroy old game"))
                ans = 'y';
            else
                ans = def ? def : 'n';
            rint = ans;
            nh3ds_log("[yn demo def='%c'] %s -> '%c'", def ? def : '?',
                      query, ans);
        } else {
            /* real play: B = no/escape, Y = explicit yes, A (or any
             * action button) = default / first allowed answer;
             * directions are ignored */
            nh3ds_scr(0);
            printf("\x1b[2J\x1b[0m");
            printf("\x1b[1;1H\x1b[37;1m%-40.40s", query);
            printf("\x1b[2;1H\x1b[0m%-40.40s",
                   "[A]=default [B]=no/esc [Y]=yes");
            for (;;) {
                int key = nh3ds_read_key();
                char c = (char) key;

                if (c == 'i') { /* B: no / escape */
                    ans = (resp && strchr(resp, 'n')) ? 'n' : '\033';
                    break;
                }
                /* Y ('s' in the game mapping): explicit yes, but only
                 * when the prompt actually allows 'y' (e.g. "Really
                 * save?" defaults to no and A alone can't accept it) */
                if (c == 's' && resp && strchr(resp, 'y')) {
                    ans = 'y';
                    break;
                }
                if (c == '.' || c == 's' || c == ',' || c == 'S'
                    || c == '?') {
                    if (def && resp && strchr(resp, def))
                        ans = def;
                    else if (resp && *resp)
                        ans = *resp;
                    else
                        ans = 'y';
                    break;
                }
            }
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
     * acknowledges it (the console is wiped on return to the launcher) */
    nh3ds_scr(1);
    printf("\x1b[0m\x1b[26;1H\x1b[K");
    if (nh3ds_lastwords[0])
        printf("\x1b[27;1H\x1b[31m%.180s\x1b[0m", nh3ds_lastwords);
    printf("\x1b[29;1H\x1b[36mexit -- press START to return to the "
           "launcher\x1b[0m");
    (void) fflush(stdout);
    for (;;) {
        u32 kdown;

        hidScanInput();
        kdown = hidKeysDown();
        if (kdown & (KEY_START | KEY_SELECT | KEY_A | KEY_B))
            break;
        svcSleepThread(50 * 1000 * 1000LL); /* 50 ms */
    }
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
        (void) snprintf(nh3ds_lastwords, sizeof nh3ds_lastwords,
                        "cannot chdir to %s -- is the nethack/ data folder "
                        "(from the zip) on the SD card?", nh3ds_hackdir);
    } else {
        nh3ds_trace = fopen("nh3ds.log", "w");
        if (nh3ds_trace)
            nh3ds_log("[start] NetHack 3DS phase 2 (P2c+save), hackdir=%s",
                      nh3ds_hackdir);
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

    shim_graphics_set_callback(shim_cb);
    (void) nhmain(1, nh_argv); /* moveloop() only returns at game over */

    nh3ds_scr(1);
    printf("NetHack 3DS: game over.\n");
    nh3ds_log("[game over] nhmain returned");
    return 0;
}

/*nh3ds.c*/
