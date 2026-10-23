NetHack 5.0 for Nintendo 3DS -- Port Plan
=========================================

Status:   planning / scaffolding
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

Build system integration (proposed)
-----------------------------------
Follow the four existing cross targets (msdos, amiga, wasm, mips). Three
files carry the per-target rules; all additions are `ifdef`-isolated so they
fold back upstream cleanly (per `Porting`: always add, never delete).

1. `sys/unix/hints/include/cross-pre1.501` -- target declaration block:

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

2. `sys/unix/hints/include/cross-pre2.501` -- toolchain + flags block, modeled
   on the WASM section (lines ~302-424):

       override TARGET_CC  = $(DEVKITARM)/bin/arm-none-eabi-gcc
       override TARGET_CXX = $(DEVKITARM)/bin/arm-none-eabi-g++
       override TARGET_AR  = $(DEVKITARM)/bin/arm-none-eabi-ar
       override TARGET_LINK= $(DEVKITARM)/bin/arm-none-eabi-gcc

   3DS_CFLAGS (base, from devkitPro 3ds_rules):
       -march=armv6k -mtune=mpcore -mfloat-abi=softfp -mfpu=vfp -O2
   plus the WASM-section define set, adapted:
       -DSYSCF -DSYSCF_FILE=\"sysconf\" -DNHUUID? -> -DNONHUUID (no crypto src)
       -DSECURE? -> drop (no setuid semantics on 3DS)
       -DDLB -DHACKDIR=\"$(HACKDIR)\" -DDEFAULT_WINDOW_SYS=\"shim\"
       -DNOMAIL -DNO_SIGNAL -DNO_FILE_LINKS -DNOTTYGRAPHICS
       -DSHIM_GRAPHICS -DLIBNH
       -DCROSSCOMPILE -DCROSSCOMPILE_TARGET -DCROSS_TO_3DS -D__3DS__
       $(LUAINCL) -I../include -I../nhlua/lua/src

   SYSSRC (start from the WASM set, trimmed to what actually links):
       ../sys/libnh/libnhmain.c      (game entry: nhmain(argc, argv))
       ../win/shim/winshim.c         (shim windowport)
       ../sys/3ds/nh3dslib.c         (newlib gap-fillers, see below)
       possibly ../sys/unix/unixunix.c / unixres.c if referenced; drop
       ../sys/share/ioctl.c + unixtty.c if devkitARM newlib lacks termios/ioctl
       REGEXOBJ = posixregex.o (sys/share/posixregex.c; devkitARM newlib
       ships <regex.h>; fallback: cppregex.cpp via libstdc++, or bundle pcre)

3. `sys/unix/hints/include/cross-post.501` -- link + data + package rules:

       $(TARGETPFX)NetHack3ds.elf: pregame $(TARGET_HACKLIB) $(TARGETPFX)date.o \
               $(HOBJ) $(LUACROSSLIB) $(TARGETPFX)nh3ds.o
           $(TARGET_LINK) -specs=3dsx.specs $(TARGET_LFLAGS) -o $@ \
               $(HOBJ) $(TARGETPFX)date.o $(TARGET_HACKLIB) \
               $(TARGETPFX)nh3ds.o $(TARGET_LIBS) -lctru -lm
       $(TARGETPFX)NetHack3ds.3dsx: $(TARGETPFX)NetHack3ds.elf
           $(DEVKITARM)/bin/3dsxtool $< $@ --smdh=$(TARGETPFX)NetHack3ds.smdh ...
       $(TARGETPFX)3ds-data/: nhdat + touch record/logfile/perm + cp sysconf
       3dspkg: assemble targets/3ds/pkg (3dsx, nhdat, sysconf, defaults.nh,
       license, symbols) + zip -> targets/3ds/nh5013ds.zip

4. `sys/unix/Makefile.top` -- a `3ds:` rule next to the `wasm:` rule
       (line ~249): ( cd src ; $(MAKE) CROSS_TO_3DS=1 ../targets/3ds/NetHack3ds.3dsx )

5. `sys/unix/README.hints` -- document `make CROSS_TO_3DS=1`.

No changes to `hints/linux.501` / `hints/macOS.501` themselves: both already
pull in cross-pre1/pre2/post via `#-INCLUDE` (processed by setup.sh), and the
ifdef guards keep them inert until CROSS_TO_3DS=1 is passed.

sys/3ds/ components
-------------------
* `nh3ds.c` -- the front-end and 3DS "main". Responsibilities:
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
    - menus: buffer add_menu items per winid; select_menu renders the list
      on the bottom screen (d-pad navigate, A select/toggle, B/START done),
      returns selected MENU_ITEM_P array; paginate.
    - text input: libctru software keyboard (swkbd) for getlin, askname,
      yn_function (when resp is long), get_ext_cmd (typed command name).
    - display_file: read via dlb (extern dlb_fopen in the lib), paginated
      into a NHW_TEXT window.
* `nh3dslib.c` -- newlib gap-fillers the game expects but devkitARM lacks:
  getlogin/getpwnam/getpwuid -> NULL (whoami() then falls back to askname),
  anything else link errors reveal. NO_SIGNAL/NO_FILE_LINKS remove the rest.
* `sysconf` -- copy of `sys/libnh/sysconf` (SYSCF is enabled).
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
    Y                    's' search
    START                'S' save               [menus: confirm]
    SELECT               '?' help
    Prompts (yn)         A = default/first letter, B = 'n'/escape,
                         swkbd fallback when resp doesn't fit buttons
    Text entry           swkbd (name, wishes, #commands, engrave, ...)

Roadmap
-------
P0  Host toolchain ready: devkitpro-pacman + `dkp-pacman -S 3ds-dev 3ds-tools`;
    hello-world .3dsx runs on target (real device via Homebrew Launcher or
    emulator) -- sanity only, not committed.

P1  `make CROSS_TO_3DS=1` cross-compiles the game (core + Lua + shim) into an
    ARM static archive; resolve newlib gaps. Exit: archive links with a stub
    main.

P2  Front-end MVP playable: boots to character creation (swkbd), map renders,
    movement + basic commands work, save/restore works, death -> record file.
    Exit: a full ascension-attemptable session in Azahar (Citra fork) and on
    real hardware via `3dslink`.

P3  UX polish: bottom-screen menus w/ touch, message history browsing, key
    remapping, options via defaults.nh, status hilites/colors, ~50-col
    viewport tuning (consider 800px wide mode or 6x8 font for 66-100 cols).

P4  Graphics: citro2d tile renderer (win/share tile data), then the fun one --
    stereoscopic depth: render map layers (floor / items / monsters / walls)
    at different parallax planes for a diorama effect using the 3D slider.

P5  (optional) .cia packaging (makerom+bannertool), upstream pull request to
    NetHack/NetHack -- the ifdef-isolated cross-pre/post + sys/3ds additions
    are shaped for that (the devteam's Cross-compiling doc explicitly invites
    this).

Risks / open questions
----------------------
* devkitARM newlib gaps (getpwuid, signals, maybe termios/ioctl): stubbed in
  nh3dslib.c; NO_SIGNAL/NO_FILE_LINKS/NOMAIL define away most of it.
* getlock()/save locking on FAT: OK -- src/files.c has a NO_FILE_LINKS path
  (uses open/create instead of link()).
* regex: posixregex vs devkitARM newlib's regex.h; verify early in P1.
  Fallbacks: cppregex.cpp (libstdc++) or bundled pcre.
* Lua + core in 64 MB (old3DS): comfortable; nhdat ~1-2 MB on SD.
* 8x8 console font = 50 map columns: viewport scroll solves it; revisit font
  in P3/P4.
* No fork/exec/threads needed by the game (moveloop is single-threaded) --
  matches 3DS fine. delay_output -> svcSleep.
* Emulator vs hardware drift: test on both; Azahar for iteration, 3dslink
  for on-device deploys.

Host setup (Arch Linux)
-----------------------
    # devkitPro pacman (official package/script from devkitpro.org), then:
    sudo dkp-pacman -S 3ds-dev 3ds-tools
    export DEVKITPRO=/opt/devkitpro
    export DEVKITARM=/opt/devkitpro/devkitARM

Build:
    cd sys/unix && sh setup.sh hints/linux.501 && cd ../..
    make CROSS_TO_3DS=1 all
    make CROSS_TO_3DS=1 3dspkg      # -> targets/3ds/pkg + nh5013ds.zip

Run:
    unzip pkg to 3ds:/3ds/nethack on the SD card (or 3dslink NetHack3ds.3dsx
    with the pkg contents in place), launch from the Homebrew Launcher.

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
