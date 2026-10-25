# NetHack for Nintendo 3DS (work in progress)

Native port of NetHack 5.0.1 to the Nintendo 3DS using devkitARM + libctru,
cross-compiled from a Linux/macOS host through NetHack 5.0's built-in
cross-compile framework (`CROSS_TO_3DS`).

Playable on hardware and in the Azahar emulator: console messages and
menus, touch verb buttons and a letter keyboard, save/restore, and a
stereoscopic 3D map on the top screen.  The game core is unmodified --
the port drives it through the built-in "shim" windowport.  See
**PLAN.md** for the design, status, and roadmap, and **README.3ds** for
install and play instructions.

Interim option: fancypantalons' NetHackDS (3.6.7, DS-mode homebrew) runs on
a 3DS via TWiLightMenu++, but it is 3.6.7, not 5.0.
