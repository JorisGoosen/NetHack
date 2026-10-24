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
 * Phase 1 (this version): stub front-end.  Every shim event is accepted
 * with a safe default answer so that the game reaches moveloop(); a trace
 * log (nh3ds.log in the playground directory) records how far it gets.
 * Rendering and real input arrive in phase 2.
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
/* P2 state (initialized here, used by later phases)                       */
/* ----------------------------------------------------------------------*/

static winid nh3ds_nextwin = 1;   /* synthetic winid counter */

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
        /* phase 1: no dialog; role_init() randomizes unspecified facets */
        nh3ds_log("[player_selection] canned: role/race/gend/align random");
    } else if (!strcmp(name, "shim_player_selection_or_tty")) {
        rint = 0; /* FALSE: don't run the generic tty-style setup */
    } else if (!strcmp(name, "shim_askname")) {
        (void) strncpy(svp.plname, nh3ds_plname,
                       sizeof svp.plname - 1);
        svp.plname[sizeof svp.plname - 1] = '\0';
        nh3ds_log("[askname] canned plname=\"%s\"", svp.plname);
    } else if (!strcmp(name, "shim_get_nh_event")) {
        /* phase 2: poll hid here */
    } else if (!strcmp(name, "shim_exit_nhwindows")) {
        nh3ds_log("[exit_nhwindows] %s", args[0].s ? args[0].s : "");
    } else if (!strcmp(name, "shim_suspend_nhwindows")) {
        nh3ds_log("[suspend_nhwindows] %s", args[0].s ? args[0].s : "");
    } else if (!strcmp(name, "shim_resume_nhwindows")) {
        /* nothing */
    } else if (!strcmp(name, "shim_create_nhwindow")) {
        rint = (int) nh3ds_nextwin++;
        nh3ds_log("[create_nhwindow] type=%s -> winid=%d",
                  nh3ds_wintypename(args[0].i), rint);
    } else if (!strcmp(name, "shim_clear_nhwindow")) {
        /* nothing yet */
    } else if (!strcmp(name, "shim_display_nhwindow")) {
        /* nothing yet (phase 2: flush the map) */
    } else if (!strcmp(name, "shim_destroy_nhwindow")) {
        /* nothing yet */
    } else if (!strcmp(name, "shim_curs")) {
        /* nothing yet */
    } else if (!strcmp(name, "shim_putstr")) {
        static unsigned long putstr_n;
        ++putstr_n;
        if (putstr_n <= 30 || putstr_n % 200 == 0)
            nh3ds_log("[putstr winid=%d n=%lu] %s", args[0].i, putstr_n,
                      args[2].s ? args[2].s : "");
    } else if (!strcmp(name, "shim_display_file")) {
        nh3ds_log("[display_file] %s", args[0].s ? args[0].s : "");
    } else if (!strcmp(name, "shim_start_menu")) {
        /* nothing yet */
    } else if (!strcmp(name, "shim_add_menu")) {
        /* nothing yet */
    } else if (!strcmp(name, "shim_end_menu")) {
        /* nothing yet */
    } else if (!strcmp(name, "shim_select_menu")) {
        /* no menu UI yet: report "cancelled" (-1 == ESC).  Returning 0
         * would mean "empty menu, nothing selected"; core loops such as
         * ask_do_tutorial() (options.c) re-issue the menu until they get
         * a non-zero result, so 0 caused an infinite create_nhwindow
         * flood (~6400/s).  -1 ends every such loop the ESC way. */
        if (args[2].p)
            *(MENU_ITEM_P **) args[2].p = (MENU_ITEM_P *) 0;
        rint = -1;
    } else if (!strcmp(name, "shim_message_menu")) {
        /* acknowledge the message (equivalent of pressing the key) */
        rint = args[0].i;
    } else if (!strcmp(name, "shim_mark_synch")
               || !strcmp(name, "shim_wait_synch")) {
        /* nothing */
    } else if (!strcmp(name, "shim_cliparound")) {
        /* phase 2: viewport follows (args[0].i, args[1].i) */
    } else if (!strcmp(name, "shim_update_positionbar")) {
        /* nothing */
    } else if (!strcmp(name, "shim_print_glyph")) {
        /* phase 2: glyph grid update; counted, not logged */
    } else if (!strcmp(name, "shim_raw_print")
               || !strcmp(name, "shim_raw_print_bold")) {
        /* error()/panic() surface here -- always show and log */
        printf("%s\n", args[0].s ? args[0].s : "");
        nh3ds_log("[raw_print] %s", args[0].s ? args[0].s : "");
    } else if (!strcmp(name, "shim_nhgetch")) {
        rint = '.'; /* rest one turn: keeps moveloop ticking */
        ++nh3ds_getch_n;
        if (nh3ds_getch_n == 1 || nh3ds_getch_n % 2048 == 0)
            nh3ds_log("[nhgetch n=%lu] returning '.'", nh3ds_getch_n);
    } else if (!strcmp(name, "shim_nh_poskey")) {
        /* P1 canned input: alternate the 'm' (#reqmenu) prefix with '.'
         * (rest).  A plain '.' is refused by safe_wait when a monster is
         * nearby ("Are you waiting to get hit?  Use 'm' prefix to force
         * a no-op") which would freeze the turn counter; the m/. pair is
         * exactly the forced no-op rest the game itself suggests, so the
         * moveloop turn counter advances.  P2: real button mapping. */
        rint = (nh3ds_poskey_n & 1) ? '.' : 'm';
        ++nh3ds_poskey_n;
        if (nh3ds_poskey_n == 1 || nh3ds_poskey_n % 16384 == 0)
            nh3ds_log("[nh_poskey n=%lu] moves=%ld returning '%c'",
                      nh3ds_poskey_n, svm.moves, (char) rint);
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
         * repeatedly (phase 2: real prompt via buttons) */
        if (strstr(query, "Destroy old game"))
            rint = 'y';
        else
            rint = def ? def : 'n';
        nh3ds_log("[yn_function def='%c'] %s -> '%c'",
                  def ? def : '?', query, (char) rint);
    } else if (!strcmp(name, "shim_getlin")) {
        /* cancel-style answer; phase 2: software keyboard */
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
               || !strcmp(name, "shim_status_init")
               || !strcmp(name, "shim_status_enablefield")
               || !strcmp(name, "shim_status_update")
               || !strcmp(name, "shim_update_inventory")) {
        /* phase 2: status bar from status_update field events */
    } else if (!strcmp(name, "set_shim_font_name")) {
        rint = 0;
    } else if (!strcmp(name, "shim_get_color_string")) {
        rptr = (void *) "";
    } else if (!strcmp(name, "shim_getmsghistory")) {
        rptr = (void *) 0; /* empty message history */
    } else if (!strcmp(name, "shim_putmsghistory")) {
        /* phase 2: feed the message ring buffer */
    } else if (!strcmp(name, "shim_ctrl_nhwindow")) {
        rptr = (void *) 0;
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
    /* P1: both screens are diagnostic consoles; P2 turns the top screen
     * into the map viewport and the bottom one into the interaction
     * screen (50x30 and 40x30 with the 8x8 font). */
    {
        PrintConsole *top = consoleInit(GFX_TOP, (PrintConsole *) 0);
        (void) consoleInit(GFX_BOTTOM, (PrintConsole *) 0);
        consoleSelect(top);
    }

    if (chdir(nh3ds_hackdir) < 0) {
        printf("nh3ds: cannot chdir to %s\n", nh3ds_hackdir);
    } else {
        nh3ds_trace = fopen("nh3ds.log", "w");
        if (nh3ds_trace)
            nh3ds_log("[start] NetHack 3DS phase 1 stub, hackdir=%s",
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

    printf("NetHack 3DS: game over.\n");
    nh3ds_log("[game over] nhmain returned");
    return 0;
}

/*nh3ds.c*/
