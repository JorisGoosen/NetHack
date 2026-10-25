NetHack 5.0 for Nintendo 3DS -- Port Plan
=========================================

Status:   P0 + P1 + P2 MVP DONE 2026-10-24 (overnight build session): the
          committed build cross-compiles, boots in Azahar, plays (movement,
          menus, swkbd), saves/restores, dies to tombstone + topten, exits
          cleanly. P3/P4/P5 pending; real hardware untested.
Game:     NetHack 5.0.1 (VERSION_MAJOR 5, VERSION_MINOR 0, PATCHLEVEL 1)
Branch:   `3ds`
Host:     Arch Linux x86-64 (cross toolchain), authoring possible on any host
Target:   Nintendo 3DS / 2DS homebrew, `.3dsx` (Homebrew Launcher); `.cia` later
Toolchain: devkitARM + libctru (devkitPro)

Goal
----
A native 3DS port of current NetHack 5.0.1: the full game (Lua, dlbed data,
save/restore, bones, records) running as a homebrew `.3dsx`, with a handheld
UI designed for the two screens. MVP is a text interface; tiles and a
stereoscopic depth view are later phases.

Existing ports, and why this is a fresh port
--------------------------------------------
* NetHackDS (Stuart Pernsteiner, 2007) -- NetHack 3.4.3, Nintendo DS.
* NetHackDS (fancypantalons, active) -- NetHack 3.6.7, Nintendo DS,
  https://github.com/fancypantalons/NetHack (branch NetHackDS-3.6). Builds a
  `.nds` with devkitPro's nds-dev (libnds), custom touchscreen UI.

Neither is reusable directly:

1. Both target the DS (libnds, 256x192, 4 MB). A native 3DS app needs libctru.
   Their builds do run on a 3DS, but only in DS-compat mode (TWiLightMenu /
   nds-bootstrap) and stay 3.6.7.
2. The core APIs a porting layer hooks changed substantially 3.6 -> 5.0:
   print_glyph now passes `glyph_info` structs, the status window API was
   reworked, and the build system was overhauled (lev_comp/dgn_comp are gone;
   all levels/dungeon/quest are Lua processed at run time).
3. The DS ports are deep forks; merging their glue forward costs roughly the
   same as writing fresh glue, plus 3.6-vs-5.0 reconciliation.

What we DO take from the DS ports: the interaction design. Bottom screen for
interaction (menus, keyboard, help), top screen for the map; viewport
scrolling instead of showing the whole 80x21 level; software keyboard for
text prompts; button remapping. Credit where due -- fancypantalons' port is
the reference for what plays well on handheld hardware.

Architecture
------------
NetHack 5.0 was explicitly restructured to make cross-compiles like this
possible (see `Cross-compiling` at top level). The only host/target split
left is: host utilities run natively, the game itself is cross-compiled.

    HOST (Arch Linux, native gcc)              TARGET (3DS, arm-none-eabi-gcc)
    -----------------------------              -------------------------------
    util/makedefs  -> date.h, pm.h, ...        src/* (core game)
    util/dlb       -> nhdat (all data)         nhlua/lua/src/* (Lua 5.4)
    (also: uudecode,                               |
     tile tools later)                             v
                                            win/shim/winshim.c  ("shim"
    package:                                 pseudo-windowport, same as the
      targets/3ds/pkg/                       official WASM build)
        NetHack3ds.3dsx                          |
        nhdat                                    v
        sysconf, defaults.nh                shim callback events (one
        record, logfile, perm               variadic C function, see below)
                                                 |
                                                 v
                                            sys/3ds/nh3ds.c  (front-end:
                                            libctru console renderer, hid
                                            input, swkbd prompts, menus)

Key property: the game never touches the 3DS directly. Everything
3DS-specific lives in the front-end (`sys/3ds/nh3ds.c`) and a small set of
newlib gap-fillers. The shim boundary is also what the official web frontend
(nethack.js) runs on, so the event stream is known-good and complete.

Build system integration (spec, verified against the tree)
----------------------------------------------------------
Follow the four existing cross targets (msdos, amiga, wasm, mips). Three
files carry the per-target rules; all additions are `ifdef`-isolated so they
fold back upstream cleanly (per `Porting`: always add, never delete).

How the pieces connect (verified while writing this spec):

* `hints/linux.501` `#-INCLUDE`s cross-pre1.501 (line 66), cross-pre2.501
  (559) and cross-post.501 (604); setup.sh splices the hints into the top
  Makefile, src/Makefile and util/Makefile, so one ifdef block serves both
  the top-level and the src-level make.
* cross-pre1.501's generic `ifdef CROSS` tail clears PREGAME, BUILDMORE,
  CLEANMORE and PACKAGE; each per-target section re-appends with `+=`.
* Top-level `all:` depends on ALLDEP (Makefile.top lines 129-132). Each
  cross target overrides ALLDEP to "<dat/ deps> <phony>" where the phony
  (e.g. `wasm:` at Makefile.top line 249) cds into src and drives the real
  file rule in cross-post. `multiw-1.501` line 23 sets NHV=501 (zip names).
* `multiw-2.501` sorts out window ports: with no tty/curses/X11/Qt wanted,
  WINCFLAGS gets -DNOTTYGRAPHICS, and `WANT_LIBNH` sets
  `WANT_DEFAULT=shim` (lines 50-52). The shim windowport reaches the link
  through SYSOBJ (winshim.o) plus -DDEFAULT_WINDOW_SYS=\"shim\".
* Generic compile rules for winshim.o (Makefile.src line 1142),
  posixregex.o (line 932) and unixunix.o (line 943) already exist; only
  libnhmain.o and the new sys/3ds objects need rules of their own.
* `targets/*` is gitignored; the PREGAME mkdir creates targets/3ds/.

1. `sys/unix/hints/include/cross-pre1.501` -- target declaration block
   (modeled on the WASM block, lines 18-27):

       ifdef CROSS_TO_3DS
       CROSS=1
       BUILD_TARGET_LUA=1
       HACKDIR=sdmc:/3ds/nethack
       PREFIX=
       override TARGET = 3ds
       override TARGETDIR=../targets/$(TARGET)
       override TARGETPFX = $(TARGETDIR)/
       override TARGET_LIBS=
       endif

2. `sys/unix/hints/include/cross-pre2.501` -- toolchain + flags section
   (new `ifdef CROSS_TO_3DS` right after the WASM section, which ends at
   line 424):

       # Host setup (below) exports DEVKITPRO/DEVKITARM via env.sh;
       # these ?= defaults are only fallbacks:
       DEVKITPRO ?= /opt/devkitpro
       DEVKITARM ?= $(DEVKITPRO)/devkitARM
       LIBCTRU   ?= $(DEVKITPRO)/libctru
       ifeq "$(DEVKITARM)" ""
       $(error CROSS_TO_3DS requires devkitPro; set DEVKITPRO/DEVKITARM)
       endif

       override TARGET_CC   = $(DEVKITARM)/bin/arm-none-eabi-gcc
       override TARGET_CXX  = $(DEVKITARM)/bin/arm-none-eabi-g++
       override TARGET_AR   = $(DEVKITARM)/bin/arm-none-eabi-ar
       override TARGET_LINK = $(DEVKITARM)/bin/arm-none-eabi-gcc

       3DS_ARCH = -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft
       3DS_CFLAGS = -c -O2 $(3DS_ARCH) -mword-relocations -std=gnu17 \
           -fomit-frame-pointer \
           -ffunction-sections -fdata-sections \
           -I../include -I../nhlua/lua/src -I$(LIBCTRU)/include \
           $(LUAINCL) $(WINCFLAGS) \
           -DSYSCF -DSYSCF_FILE=\"sysconf\" -DNONHUUID \
           -DDLB -DHACKDIR=\"$(HACKDIR)\" -DDEFAULT_WINDOW_SYS=\"shim\" \
           -DNOMAIL -DNO_SIGNAL -DNO_FILE_LINKS -DNOTTYGRAPHICS \
           -DSHIM_GRAPHICS -DLIBNH -DNOPANICTRACE \
           -DCROSSCOMPILE -DCROSSCOMPILE_TARGET -DCROSS_TO_3DS -D__3DS__ \
           -Wall -Wextra -Wno-missing-field-initializers -Wreturn-type \
           -Wunused -Wformat -Wswitch -Wshadow
       override TARGET_CFLAGS = $(3DS_CFLAGS)
       LUA_TARGET_CFLAGS = $(TARGET_CFLAGS)
       override TARGET_CXXFLAGS = $(TARGET_CFLAGS)
       override TARGET_LFLAGS = $(3DS_ARCH) -L$(LIBCTRU)/lib
       override TARGET_LIBS += -lctru -lm

       override SYSSRC = ../sys/libnh/libnhmain.c \
           ../sys/unix/unixunix.c \
           ../win/shim/winshim.c \
           ../sys/3ds/nh3ds.c ../sys/3ds/nh3dslib.c
       override SYSOBJ = $(TARGETPFX)libnhmain.o $(TARGETPFX)unixunix.o \
           $(TARGETPFX)winshim.o \
           $(TARGETPFX)nh3ds.o $(TARGETPFX)nh3dslib.o
       override REGEXOBJ = $(TARGETPFX)cppregex.o
       override CPLUSPLUS_NEEDED = 1
       override TARGET_LINK = $(DEVKITARM)/bin/arm-none-eabi-g++
       override WINLIB=
       override LUALIB=
       override LUALIBS=
       override LUATOPLIB=
       override WINOBJ=
       override UUIDOBJ=
       override DLLIB=
       override GAME=
       override GAMEBIN = $(TARGETPFX)NetHack3ds.3dsx
       3DS_DAT = bogusmon data engrave epitaph oracles quest.lua rumors
       3DSDEP = include/nhlua.h $(3DS_DAT) spec_levs check-dlb
       override ALLDEP = $(3DSDEP) 3ds
       override PREGAME += mkdir -p $(TARGETDIR) $(TARGETPFX)3ds-data ;
       override CLEANMORE += rm -rf $(TARGETDIR) ;

   Decisions (each one verified against source, replacing the older
   question marks):

   * -DNONHUUID: libnhmain.c's get_nhuuid() falls back to an all-zero uuid
     under NONHUUID; no libcrypto on the target. (WASM instead uses NHUUID
     via emscripten crypto.)
   * -DSYSCF_FILE=\"sysconf\": the relative name resolves through
     fqname(filep, SYSCONFPREFIX, 0) against HACKDIR (sdmc:/3ds/nethack) --
     exactly where the pkg installs it. No -DSECURE (no setuid semantics
     on 3DS; add back only if SYSCF handling complains).
   * -DNO_FILE_LINKS: src/files.c has an open/create fallback for
     getlock() for filesystems without link() (FAT on the SD card).
   * -DNOPANICTRACE: this tree is NH_STATUS_WIP (patchlevel.h line 33), so
     PANICTRACE would otherwise be compiled in (global.h line 444) and
     pull in exec()/gdb/backtrace machinery that cannot exist on 3DS.
     -DNOPANICTRACE undefs it the same way MSDOS does (config.h line 263).
   * -DNO_SIGNAL: also makes libnhmain.c's sethanguphandler() disappear
     entirely (`#ifndef NO_SIGNAL`), so no signal stubs are needed at all.
   * SYSSRC as implemented (P1): libnhmain.c + unixunix.c (IS needed --
     getlock/HLOCK) + winshim.c + the sys/3ds objects. unixres.c,
     ioctl.c and unixtty.c are NOT linked: unixtty.c needs termios which
     devkitARM newlib lacks, so error()/intron()/introff() are
     reimplemented in nh3dslib.c. (Note: the WASM section's longer list
     is not evidence of need -- wasm links with
     -s ERROR_ON_UNDEFINED_SYMBOLS=0.)
   * Regex resolved in P1: cppregex.cpp (libstdc++) from the start --
     devkitARM newlib's <regex.h> was not trusted. Consequences:
     CPLUSPLUS_NEEDED=1 and TARGET_LINK=arm-none-eabi-g++.
   * nh3dslib.c stub set (P1-verified): getlogin/getpwnam/getpwuid ->
     NULL, getuid -> 1, umask/creat/waitpid/getgid/setgid/setuid/execv
     no-ops, fcntl F_SETLK/F_SETLKW/F_UNLCK -> 0 (libsysbase rejects
     F_SETLK; without the stub lock_file() on perm fails and the game
     quits). chmod/chdir/mkdir exist in libsysbase.
   * -D__3DS__ spelled out explicitly -- verified NOT predefined by
     devkitARM 16.1.0's gcc (-dM check), so it is required.
   * -mfloat-abi=hard, NOT softfp: P0 verified that softfp selects the
     bare armv6k multilib where 3dsx_crt0.o and -lctru do not exist --
     modern devkitPro builds the 3DS hard-float (their 3ds_rules use
     -mfloat-abi=hard -mtp=soft). -mword-relocations completes the 32-bit
     ABI requirements.
   * -std=gnu17: devkitARM 16.1.0 is GCC 16, which defaults to C23; the
     mac68k sections already document that C23 breaks savefile.h's
     _Generic save dispatch. Same medicine, preemptively.
   * REGEXOBJ: cppregex.o (see above; posixregex was the first choice on
     paper but cppregex was taken immediately in P1).
   * GNU_LIBC caveat: resolved in P1 -- -DGNU_LIBC (inherited from
     multiw-2.501 ~line 205) works as-is on newlib; the MUSL=1 fallback
     was not needed.
   * mkdir(\"save\", 0777) at startup (in nh3ds.c): set_savefile_name()
     builds save/<uid><plname> under the playground but nothing creates
     the save/ subdirectory -- without this, #save silently writes
     nothing and auto-restore can never work. Found by P2 testing.

3. `sys/unix/hints/include/cross-post.501` -- link + data + package rules
   (new `ifdef CROSS_TO_3DS` section; model: WASM data rules + MIPS pkg):

       3DS_DATA_DIR = $(TARGETPFX)3ds-data
       3DS_TARGET = $(TARGETPFX)NetHack3ds.3dsx

       $(TARGETPFX)libnhmain.o : ../sys/libnh/libnhmain.c $(HACK_H)
       $(TARGETPFX)unixunix.o : ../sys/unix/unixunix.c $(HACK_H)
       $(TARGETPFX)nh3ds.o : ../sys/3ds/nh3ds.c $(HACK_H)
       $(TARGETPFX)nh3dslib.o : ../sys/3ds/nh3dslib.c $(HACK_H)

       $(TARGETPFX)NetHack3ds.elf: pregame $(TARGET_HACKLIB) \
               $(TARGETPFX)date.o $(HOSTOBJ) $(HOBJ) $(LUACROSSLIB) \
               $(3DS_DATA_DIR)
           -rm $@
           $(TARGET_LINK) -specs=3dsx.specs $(TARGET_LFLAGS) -o $@ \
               $(HOBJ) $(TARGETPFX)date.o $(TARGET_HACKLIB) \
               $(LUACROSSLIB) $(TARGETPFX)nh3ds.o $(TARGET_LIBS)

       $(3DS_TARGET): $(TARGETPFX)NetHack3ds.elf
           $(DEVKITPRO)/tools/bin/3dsxtool $< $@

       $(3DS_DATA_DIR): $(3DS_DATA_DIR)/nhdat
           touch $(3DS_DATA_DIR)/perm $(3DS_DATA_DIR)/record \
               $(3DS_DATA_DIR)/logfile $(3DS_DATA_DIR)/xlogfile
           cp ../sys/libnh/sysconf $(3DS_DATA_DIR)/sysconf

       $(3DS_DATA_DIR)/nhdat:
           ( cd ..; $(MAKE) INSTDIR='$(3DS_DATA_DIR)' $(3DSDEP) dofiles-dlb )

       .PHONY: 3dspkg
       3dspkg: $(3DS_TARGET) $(3DS_DATA_DIR)
           mkdir -p $(TARGETPFX)pkg/nethack
           cp $(3DS_TARGET) $(TARGETPFX)pkg/
           cp $(3DS_DATA_DIR)/nhdat $(3DS_DATA_DIR)/sysconf \
               $(TARGETPFX)pkg/nethack/
           touch $(TARGETPFX)pkg/nethack/record \
               $(TARGETPFX)pkg/nethack/logfile $(TARGETPFX)pkg/nethack/perm
           cp ../sys/3ds/defaults.nh $(TARGETPFX)pkg/nethack/defaults.nh
           cp ../dat/license ../dat/symbols $(TARGETPFX)pkg/nethack/
           cp ../sys/3ds/README.3ds $(TARGETPFX)pkg/
           cd $(TARGETPFX)pkg && zip -9r ../nh$(NHV)3ds.zip *
           @echo 3DS package zip file $(TARGETPFX)nh$(NHV)3ds.zip

   No SMDH/icon for P1 -- 3dsxtool works without --smdh; add an icon in
   P3/P5 (smdhtool wants a 48x48 png). $(NHV) is 501 (multiw-1.501), so
   the zip is nh5013ds.zip.

4. `sys/unix/Makefile.top` -- a `3ds:` rule next to the `wasm:` rule
       (line ~249): ( cd src ; $(MAKE) CROSS_TO_3DS=1 ../targets/3ds/NetHack3ds.3dsx )

5. `sys/unix/README.hints` -- document `make CROSS_TO_3DS=1` and the
   devkitPro prerequisite.

No changes to `hints/linux.501` / `hints/macOS.501` themselves: both already
pull in cross-pre1/pre2/post via `#-INCLUDE` (processed by setup.sh), and the
ifdef guards keep them inert until CROSS_TO_3DS=1 is passed.

sys/3ds/ components
-------------------
* `nh3ds.c` -- the front-end and 3DS "main". Responsibilities:
    - status: P2 MVP is implemented (see Implementation status below):
      full console rendering (top 50x30 map/messages/status, bottom 40x30
      tail/popups), hid input mapping, buffered menus, swkbd text entry,
      save/restore glue, and an unattended rig mode (3drig file) for
      emulator testing. The original P1 stub is in git history.
    - callback form: the non-emscripten shim takes a direct C function
      pointer, `void (*)(const char *name, void *ret_ptr, const char *fmt,
      ...)` (winshim.c lines 84-90) -- the string-named callback is the
      emscripten/JS path only.
    - startup: gfxInitDefault, two consoles (see layout), fsInit/sdmc, atexit
      cleanup; then `shim_graphics_set_callback(shim_cb); nhmain(1, argv);`
    - shim callback: one variadic function; parse `fmt` chars ('i' int,
      's' char*, 'p' void*, 'c'/'b'/'0'/'1'/'2' small ints, all promoted),
      dispatch on event name, write return through `ret_ptr`.
    - map state: 80x21 glyph grid filled from `shim_print_glyph` events
      (glyph_info carries glyph + color; char via `mapglyph()`), flushed to
      the top console on `display_nhwindow`; viewport follows the hero using
      `shim_cliparound(x,y)` (levels are up to ~79 wide; top console is
      50 columns with the default 8x8 font).
    - messages: putstr(WIN_MESSAGE) ring buffer; `getmsghistory` /
      `putmsghistory` for restore; 3 lines on top console, full history
      browsable on bottom screen.
    - status: `status_update` events are field-structured (fldidx + value +
      color) -> render a proper 2-row status bar, not a tty string.
      Labels are port-owned (nh3ds_st_initfmt[]) because the core's
      genl_status_enablefield never forwards fmts to the port (mirrors
      curses/wintty).
    - menus: buffer add_menu items per winid; select_menu renders the list
      on the bottom screen (d-pad navigate, A select/toggle, B/START done),
      returns selected MENU_ITEM_P array; paginate.
    - text input: libctru software keyboard (swkbd) for getlin, askname,
      yn_function (when resp is long), get_ext_cmd (typed command name).
      get_ext_cmd is reimplemented over our own menus -- upstream
      extcmd_via_menu() is TTY/CURSES-only.
    - display_file: read via dlb (extern dlb_fopen in the lib), paginated
      into a NHW_TEXT window.
* `nh3dslib.c` -- newlib gap-fillers the game expects but devkitARM lacks.
  P1-verified set: getlogin/getpwnam/getpwuid -> NULL, getuid -> 1,
  umask/creat/waitpid/getgid/setgid/setuid/execv no-ops, fcntl
  F_SETLK/F_SETLKW/F_UNLCK -> 0 (libsysbase rejects F_SETLK; without the
  stub lock_file() on perm fails and the game quits). chmod/chdir/mkdir
  exist in libsysbase. unixtty.c is NOT linked, so error()/intron()/
  introff() are reimplemented here. NO_SIGNAL/NO_FILE_LINKS/NOMAIL define
  the rest away (sethanguphandler() vanishes entirely under NO_SIGNAL).
* `sysconf` -- not a new file: the pkg rule copies `sys/libnh/sysconf`
  (SYSCF is enabled; its PANICTRACE_* entries are inert with
  -DNOPANICTRACE).
* `defaults.nh` -- sample config (OPTIONS for shim UI; windowtype is baked
  to "shim" anyway).
* `README.3ds` -- install/run instructions for players.

UI design (phase 1 text)
------------------------
Top screen, 50x30 (8x8 console font):
    rows  0- 2   messages (last 3)
    rows  3-23   map viewport, 21 rows (cursor shown; scrolls with hero)
    rows 24-25   status (2 rows, field-driven, colored)
    rows 26-29   key legend / prompt hints
Bottom screen, 40x30:
    default: message history tail + key legend
    menu active: menu list + prompt
    text prompt: (libctru swkbd overlays)

Proposed button mapping (tunable; NetHackDS-style, remappable later):

    D-pad / circle pad   hjkl move; two held = diagonals (yubn)
    L/R + direction      run (HJKL / uppercase)
    A                    '.' (rest / advance) -- also "yes/ok" in prompts
    B                    'i' inventory          [menus: cancel]
    X                    ',' pick up            [menus: toggle]
    Y                    's' search; explicit 'y' in yn prompts
    START                'S' save               [menus: confirm]
    SELECT               '?' help
    Prompts (yn)         A = default/first letter, Y = explicit 'y',
                         B = 'n'/escape, swkbd fallback when resp doesn't
                         fit buttons (Y-as-y is needed for default-'n'
                         prompts like "Really save?")
    Text entry           swkbd (name, wishes, #commands, engrave, ...)

Roadmap
-------
P0  DONE 2026-10-24. Toolchain = full `3ds-dev` group (22 pkgs: devkitARM
    16.1.0, libctru 2.7.0, 3dstools/3dslink/tex3ds/...) installed via
    local-root pacman under /mnt/halveGig/virtuoos/devkitpro/ -- see Host
    setup below. P0 hello-world .3dsx builds + converts (valid 3DSX
    header). Not committed.

P1  DONE 2026-10-24 (commits 8d72aad2a, 823d24fd4): CROSS_TO_3DS builds the
    full game (core + Lua + shim + front-end) and links NetHack3ds.3dsx;
    newlib gaps resolved in nh3dslib.c. Exit met: boots in Azahar through
    moveloop.

P2  DONE 2026-10-24 (commits 5294dab31, 69ca9b585, 4fabae251, 62c283943):
    console rendering, hid input, menus + swkbd, character creation,
    save/restore, death -> tombstone -> topten -> clean exit -- all
    verified in Azahar (see Implementation status below). The "full
    ascension-attemptable session" is met mechanically; a long real-input
    play session is the remaining bit (the 3drig rig autopilot plays
    itself meanwhile).

P3  UX polish: bottom-screen menus w/ touch, message history browsing, key
    remapping, options via defaults.nh, status hilites/colors, ~50-col
    viewport tuning (consider 800px wide mode or 6x8 font for 66-100 cols).

P4  DONE 2026-10-24: top screen fully citro2d with the fun one -- a
    stereoscopic depth map ("diorama"): the level renders as parallax
    layers driven by the 3D slider (walls/UI at the glass, entities
    behind, furniture raised, a dedicated under-the-hero layer, floors
    deeper, stairs-down deepest, levitating heroes lifted).  Glyphs
    come from the 8x8 console font via a GPU texture atlas; colors
    from the game's own glyph map.  Verified with a debug rig (forced
    slider + per-eye BMP dumps matched against the font).  Bottom
    screen unchanged (libctru console: menus, touch, swkbd, message
    tail).

P5  (optional) .cia packaging (makerom+bannertool), upstream pull request to
    NetHack/NetHack -- the ifdef-isolated cross-pre/post + sys/3ds additions
    are shaped for that (the devteam's Cross-compiling doc explicitly invites
    this).

Implementation status (2026-10-24, overnight session)
-----------------------------------------------------
Branch `3ds`, six commits at the time of writing (since pushed and
continued -- see `git log` for the current list):

    8d72aad2a  build integration + P1 stub front-end
    823d24fd4  Azahar boot fixes (select_menu -1, canned rests, fcntl)
    5294dab31  P2a console rendering (map/messages/status, colors)
    69ca9b585  P2b hid input (d-pad hjkl, L/R run, A/B/X/Y/START/SELECT)
    4fabae251  P2c menus + swkbd + character creation + get_ext_cmd
    62c283943  save/restore (mkdir("save") fix, Y=y in prompts, tail)

Verified in Azahar: boot -> character creation -> movement -> menus ->
save ("save/1Wizard" written, alock.* cleaned) -> restore ("Hello Wizard,
... welcome back", resumed turn) -> death (jackal) -> tombstone -> topten
-> clean exit. Sizes: NetHack3ds.3dsx 3,921,508 B; elf 6,136,632 B (with
symbols); nh5013ds.zip 2,368,931 B.

P4 status (2026-10-24, stereo map)
----------------------------------
* Top screen moved off the libctru console onto citro2d/citro3d: GPU
  init/cleanup, a 256x64 RGBA8 glyph atlas built from libctru's
  default_font_bin (the same 8x8 console font, Morton-swizzled for the
  PICA texture layout), palette tints per stored glyph color, and a
  layered stereo draw of the whole level buffer.  All top-screen
  console output ported to citro2d text (messages, 2-row status,
  legend footer, menus, ext-command list, exit screen); the libctru
  console remains only on the bottom screen.
* Depth planes (px disparity at slider = 1.0, positive = behind the
  glass), nh3ds_depth[]: WALL/UI 0, LEV 2, ENTITY 4, FURNITURE 6,
  UNDER 8, FLOOR 10, DOWN 13; per-eye shift = eye * depth * slider *
  0.5.  Painter order DOWN -> FLOOR -> UNDER -> FURNITURE -> ENTITY ->
  LEV -> WALL, alpha blended.
* Layer classification (nh3ds_glyph_layer): walls/doors/trees/bars/
  lavawall/closed drawbridges -> WALL; altar/grave/throne/sink/
  fountain/webs/stairs+ladders up -> FURNITURE; stairs+ladders down/
  pits/holes/trapdoors -> DOWN; room/corr/ice/air/cloud/pool/water/
  lava/S_ndoor/open drawbridges -> FLOOR; objects/monsters/swallow/
  explosion/warning -> ENTITY.  Hero = ENTITY, or LEV when levitating
  or flying (BL_MASK_LEV/BL_MASK_FLY).
* print_glyph's bkglyphinfo (previously ignored) now feeds a background
  terrain plane, enabled via iflags.use_background_glyph in
  init_nhwindows; the hero's cell keeps the last non-hero glyph in an
  "under" buffer rendered between ENTITY and FURNITURE so what the
  hero stands on stays visible underneath it.
* Debug rig, gated by a `3drig` file in the playground: forces the
  slider to 1.0, ring-dumps both eye framebuffers as BMPs every 64
  refreshes (32 slots, dump3d_NN_L/R.bmp), pins the hero to the LEV
  plane when the file starts with 'L', and runs a deliberately dumb
  autopilot (random momentum walk; opens doors when walled in,
  searches when doorless, descends when standing on '>') so unattended
  runs exercise the depth layers.  Walker instrumentation: [walk] and
  [demo key] lines in nh3ds.log.
* Verified in Azahar via the eye dumps + numpy font matching against
  the extracted font: walls at plane 0, entities +4px, floors +10px,
  stairs-up raised (FURNITURE), hero pinned to LEV via the flag, UI
  text identical in both eyes (plane 0); parallax direction correct.
  Colors match the game's glyph map -- notably the yellow '<' on
  Dlvl 1 is the branch exit stair (S_brupstair, CLR_YELLOW by
  definition), not a renderer bug.

Risks / open questions
----------------------
Resolved while writing the spec:

* NHUUID: use -DNONHUUID; libnhmain.c then uses an all-zero uuid, no
  libcrypto needed. (WASM's NHUUID path is emscripten-specific.)
* sysconf location: -DSYSCF_FILE=\"sysconf\" resolves via fqname against
  HACKDIR, where the pkg installs it. No -DSECURE.
* getlock()/save locking on FAT: OK -- src/files.c has a NO_FILE_LINKS
  path (uses open/create instead of link()), enabled by -DNO_FILE_LINKS.
* PANICTRACE: tree is NH_STATUS_WIP so it would compile in; -DNOPANICTRACE
  (the MSDOS knob, config.h line 263) removes it and the copied sysconf's
  PANICTRACE_* entries become inert.
* signals: -DNO_SIGNAL removes sethanguphandler() and friends outright.
* Toolchain specifics (P0-verified): -mfloat-abi=hard (+ -mtp=soft,
  -mword-relocations) mandatory; -D__3DS__ NOT predefined by devkitARM
  16.1.0; -specs=3dsx.specs found by name (lives in
  $DEVKITARM/arm-none-eabi/lib/); -L$(LIBCTRU)/lib must be explicit;
  gcc relocates cleanly to the custom prefix.

Resolved by P1/P2 (overnight build):

* newlib gaps: enumerated stub set in nh3dslib.c (see components).
* regex: cppregex.cpp + libstdc++; TARGET_LINK is g++.
* unixunix.c IS linked (getlock/HLOCK); unixres.c/ioctl.c/unixtty.c are
  not (error/intron/introff reimplemented in nh3dslib.c).
* -DGNU_LIBC on newlib: works as-is; MUSL=1 not needed.
* Memory: NetHack3ds.3dsx 3.74 MiB, elf 5.85 MiB (with symbols),
  nh5013ds.zip 2.26 MiB -- comfortable in 64 MB (old3DS).

Still open:

* Real hardware via 3dslink untested (Azahar only so far); bottom-screen
  redraw cost on device unknown.
* swkbd fallback for yn prompts with long response sets (buttons cover
  current cases).
* getmsghistory() still returns empty (restore works via putmsghistory
  replay).
* Touch input for menus, message-history browsing, key remapping,
  defaults.nh options (P3); .cia (P5).
* 8x8 console font = 50 map columns: viewport scroll solves it; the
  same font now also feeds the P4 GPU glyph atlas.
* No fork/exec/threads needed by the game (moveloop is single-threaded) --
  matches 3DS fine. delay_output -> svcSleep.

Host setup (Arch Linux) -- DONE: local install, no root
-------------------------------------------------------
devkitPro lives entirely under /mnt/halveGig/virtuoos/devkitpro/ (530 MB,
everything owned by user virtuoos):

    pacman.conf   local pacman config: RootDir=root, DBPath=db,
                  CacheDir=cache, GPGDir=gnupg (NOT /etc),
                  SigLevel = Required DatabaseRequired,
                  [dkp-libs]  https://pkg.devkitpro.org/packages
                  [dkp-linux] https://pkg.devkitpro.org/packages/linux/x86_64/
    env.sh        export DEVKITPRO=.../devkitpro/root/opt/devkitpro
                  export DEVKITARM=$DEVKITPRO/devkitARM
                  PATH += $DEVKITARM/bin:$DEVKITPRO/tools/bin
    root/         the toolchain (423M): devkitARM 16.1.0 (GCC 16), libctru
                  2.7.0, tools (3dsxtool, smdhtool, 3dslink, tex3ds, ...) in
                  root/opt/devkitpro/tools/bin
    p0-test/      P0 hello-world: builds, 3dsxtool -> valid .3dsx

P0-verified build facts (baked into the cross-pre2 spec above):
* -mfloat-abi=hard -mtp=soft -mword-relocations (hard-float is the
  multilib that carries 3dsx_crt0.o + newlib; softfp does not link).
* -D__3DS__ not predefined by gcc -- pass it explicitly.
* -specs=3dsx.specs found by name; -L$DEVKITPRO/libctru/lib explicit;
  -lctru -lm. gcc relocation to the custom prefix works.
* libctru ships no .pc file; use the $DEVKITPRO/libctru paths directly.

pacman maintenance (updates / extra packages): plain pacman refuses db
writes as non-root (hardcoded geteuid check), so prefix `unshare -r`
(user namespaces are allowed on this host):
    unshare -r pacman --config /mnt/halveGig/virtuoos/devkitpro/pacman.conf -Sy
    unshare -r pacman --config ... -S <pkg>
Read-only ops (-Q/-Si/-Sg) work without the prefix.

Testing (Azahar, built from source at /mnt/halveGig/virtuoos/azahar/):
    binary:  /mnt/halveGig/virtuoos/azahar/azahar/build/bin/Release/azahar
    SD root: ~/.local/share/azahar-emu/sdmc/
             (sdmc:/3ds/nethack = ~/.local/share/azahar-emu/sdmc/3ds/nethack)
    logs:    ~/.local/share/azahar-emu/log/
    NOTE upstream Azahar bug: any long CLI option containing c/x/o is
    hijacked by getopt -- use short forms only (-f fullscreen, -w
    windowed, -v version); positional file paths are safe.
    See also /mnt/halveGig/virtuoos/azahar/REPORT.md and run-azahar.sh.

Build:
    source /mnt/halveGig/virtuoos/devkitpro/env.sh
    cd sys/unix && sh setup.sh hints/linux.501 && cd ../..
    make CROSS_TO_3DS=1 all
    make CROSS_TO_3DS=1 3dspkg      # -> targets/3ds/pkg + nh5013ds.zip
    # P1 iteration loop: fix whatever the link complains about (regex,
    # newlib gaps, unixunix.c/unixres.c) until the .3dsx links with the
    # stub main.

Run (hardware):
    unzip pkg to sdmc:/3ds/nethack on the SD card (or 3dslink NetHack3ds.3dsx
    with the pkg contents in place), launch from the Homebrew Launcher.
    (Emulator testing: see Azahar above.)

Repo workflow
-------------
* Branch `3ds` off NetHack-5.0 (this file is its first commit).
* User forks NetHack/NetHack on GitHub, pushes `3ds` there, clones on the
  Arch box; implementation continues there (fast iteration + emulator).
* Keep every change ifdef-isolated / additive so the branch stays easy to
  rebase onto upstream NetHack-5.0.x as patches land.

References
----------
* `Porting`, `Cross-compiling` (top level) -- devteam guidance
* `sys/unix/hints/include/cross-pre1.501`, `cross-pre2.501`, `cross-post.501`
  -- existing cross targets to clone from (wasm is closest)
* `win/shim/winshim.c` -- shim event list + fmt strings
* `sys/libnh/README.md` -- nhmain/shim_graphics_set_callback API
* `doc/window.txt` -- windowport semantics behind each event
* https://github.com/fancypantalons/NetHack -- UX inspiration (DS port)
