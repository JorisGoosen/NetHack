# NetHack for Nintendo 3DS

Native port of NetHack 5.0 to the Nintendo 3DS using devkitARM + libctru,
cross-compiled from a Linux/macOS host through NetHack 5.0's built-in
cross-compile framework (`CROSS_TO_3DS`).

Playable on hardware and in the Azahar emulator: console messages and
menus, touch verb buttons and a letter keyboard, save/restore, and a
stereoscopic 3D map on the top screen.  The game core is unmodified --
the port drives it through the built-in "shim" windowport.  See
**README.3ds** for install and play instructions.

Interim option: fancypantalons' NetHackDS (3.6.7, DS-mode homebrew) runs on
a 3DS via TWiLightMenu++, but it is 3.6.7, not 5.0.

## Building from source

Install devkitPro with the `3ds-dev` group (devkitARM, libctru, 3dstools)
and put `DEVKITPRO` and `DEVKITARM` in the environment (e.g.
`source /opt/devkitpro/env.sh`).  Then, from the top of the tree:

    sys/unix/setup.sh sys/unix/hints/linux.501    # or macOS.501
    make CROSS_TO_3DS=1 all
    make CROSS_TO_3DS=1 3dspkg

The last step produces targets/3ds/nh5013ds.zip: the .3dsx plus the data
files to install into sdmc:/3ds/nethack on the SD card (see README.3ds).
`make CROSS_TO_3DS=1 3dscia` additionally wraps the build as an
installable Home-menu title, targets/3ds/NetHack3ds.cia; that target
needs makerom (github.com/3DSGuy/Project_CTR) on PATH or in
$DEVKITPRO/tools/bin, and sys/3ds/cia.rsf is its ROM spec.  The build
flags and toolchain notes live in
sys/unix/hints/include/cross-pre2.501 and cross-post.501.

## Not yet implemented

- The permanent-inventory pane (perm_invent): the core option is
  accepted, but the bottom screen keeps the message tail and inventory
  is always a menu away ('i', or B).
