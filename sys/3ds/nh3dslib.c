/* NetHack 5.0 nh3dslib.c    $NHDT-Date: $  $NHDT-Branch: 3ds $:$NHDT-Revision: $ */
/* Copyright (c) the NetHack 3DS port contributors, 2026.                */
/* NetHack may be freely redistributed.  See license for details.        */

/* newlib gap-fillers for the devkitARM (3DS) build.
 *
 * devkitARM's newlib (armv6k/fpu) lacks chunks of POSIX that the game
 * core references; this file supplies the missing pieces.  Everything
 * here is either a safe no-op or a small reimplementation on top of
 * calls that libsysbase/libctru do provide (open/close).
 *
 *   - getlogin/getpwnam/getpwuid -> NULL: whoami() falls through to
 *     askname() (the front-end supplies a name via shim_askname) and
 *     get_unix_pw()/check_user_string() fail cleanly.
 *   - getuid -> fixed value: only used for lock/save file names.
 *   - umask -> 0: no permission semantics on FAT.
 *   - creat -> open(O_WRONLY|O_CREAT|O_TRUNC): used by getlock().
 *   - error(): the port's fatal-error routine (unix normally gets it
 *     from sys/share/unixtty.c, which needs termios and can't be
 *     linked here); mirrors the unixtty.c behavior using the shim
 *     windowport.
 *   - intron/introff: no-ops (there are no ^C interrupts to toggle;
 *     unixtty.c defines them for the tty port).
 *   - waitpid/getgid/setgid/setuid/execv: referenced only by code
 *     paths behind runtime checks that never fire on the 3DS (the
 *     msghandler fork in pline.c); return failure or no-op so the
 *     link succeeds.
 *
 * If the link demands more symbols, add them here (NO_SIGNAL, NOSHELL,
 * NOSUSPEND, NOMAIL and NO_FILE_LINKS already compile the rest away).
 */

#include "hack.h"

#include <fcntl.h>
#include <pwd.h>
#include <stdarg.h>
#include <stdio.h>
#include <sys/types.h>
#include <unistd.h>

/* ----------------------------------------------------------------------*/
/* user database                                                          */
/* ----------------------------------------------------------------------*/

char *
getlogin(void)
{
    return (char *) 0;
}

struct passwd *
getpwnam(const char *nam)
{
    (void) nam;
    return (struct passwd *) 0;
}

struct passwd *
getpwuid(uid_t uid)
{
    (void) uid;
    return (struct passwd *) 0;
}

uid_t
getuid(void)
{
    return (uid_t) 1; /* single-user handheld */
}

/* ----------------------------------------------------------------------*/
/* file modes / fds                                                       */
/* ----------------------------------------------------------------------*/

mode_t
umask(mode_t cmask)
{
    (void) cmask;
    return (mode_t) 0;
}

int
creat(const char *path, mode_t mode)
{
    return open(path, O_WRONLY | O_CREAT | O_TRUNC, mode);
}

/* No advisory file locking on the 3DS (libsysbase's fcntl() rejects
 * F_SETLK, which made lock_file() on the "perm" lock give up after 10
 * retries).  The 3DS runs a single game process, so pretending every
 * lock attempt succeeds preserves the upstream USE_FCNTL code path
 * (unixconf.h defines USE_FCNTL whenever POSIX_TYPES is set).  Other
 * fcntl commands never occur in this binary. */
int
fcntl(int fd, int cmd, ...)
{
    (void) fd;
    switch (cmd) {
    case F_SETLK:
    case F_SETLKW:
    case F_UNLCK:
        return 0;
    default:
        return -1;
    }
}

/* ----------------------------------------------------------------------*/
/* process bits that cannot exist on the 3DS                              */
/* ----------------------------------------------------------------------*/

pid_t
waitpid(pid_t pid, int *status, int options)
{
    (void) pid, (void) options;
    if (status)
        *status = 0;
    return (pid_t) -1;
}

gid_t
getgid(void)
{
    return (gid_t) 0;
}

int
setgid(gid_t gid)
{
    (void) gid;
    return 0;
}

int
setuid(uid_t uid)
{
    (void) uid;
    return 0;
}

int
execv(const char *path, char *const argv[])
{
    (void) path, (void) argv;
    return -1;
}

/* ----------------------------------------------------------------------*/
/* fatal error + interrupt toggles (normally from sys/share/unixtty.c)    */
/* ----------------------------------------------------------------------*/

/* fatal error */
void
error(const char *s, ...)
{
    va_list the_args;
    char buf[BUFSZ];

    va_start(the_args, s);
    (void) vsnprintf(buf, sizeof buf, s, the_args);
    va_end(the_args);
    if (iflags.window_inited)
        exit_nhwindows((char *) 0);
    printf("%s\n", buf);
    exit(EXIT_FAILURE);
}

void
intron(void) /* enable kbd interrupts if enabled when game started */
{
    /* no signals on the 3DS */
}

void
introff(void) /* disable kbd interrupts if required */
{
    /* no signals on the 3DS */
}

/*nh3dslib.c*/
