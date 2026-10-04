/*
 * Copyright 2025 Various Authors
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#include "inhibit.h"

#ifdef CONFIG_INHIBIT

#if defined(CONFIG_SDBUS_BASU) || defined(CONFIG_MPRIS_BASU)
#include <basu/sd-bus.h>
#else
#include <systemd/sd-bus.h>
#endif

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

#include "debug.h"
#include "options.h"
#include "ui_curses.h"

/*
 * Keep the machine from going to sleep while cmus is playing.
 *
 * Suspend is inhibited, but the *screen* is not: the display is still allowed
 * to blank and to lock, exactly as the power settings say.
 *
 * Two locks are used, both best effort:
 *
 *  - logind (system bus): Inhibit() returns a file descriptor which is only
 *    valid while it is kept open.  Releasing the lock is just a matter of
 *    closing it, so a crashed cmus can never leave a stale inhibitor behind.
 *    Only "sleep" is requested.  "idle" is deliberately *not* requested: it
 *    would keep the session from ever counting as idle, which is what
 *    blanking and locking are driven by.
 *
 *    The "what" string is interpreted by the desktop environment as well.
 *    KDE's PowerDevil (daemon/powerdevilpolicyagent.cpp, policiesInLogindWhat)
 *    maps "sleep" to the InterruptSession policy - precisely the policy its
 *    SuspendSession action requires, i.e. the gate in front of "suspend after
 *    N minutes of inactivity" - while "idle" maps to ChangeScreenSettings
 *    (dimming and blanking).  Asking for "sleep" alone therefore stops Plasma
 *    from suspending on idle without touching the screen behaviour.
 *
 *  - org.freedesktop.ScreenSaver (session bus): the classic interface
 *    implemented by most desktop environments; Inhibit() returns a cookie
 *    which is passed back to UnInhibit().  This one commonly covers blanking
 *    and locking too (KWin, for instance, feeds nothing but this interface
 *    into its idle handling), so it is only used as a fallback when logind is
 *    not around, and never on top of a working logind.
 *
 * Failure is not fatal: playback continues and a single warning is shown.
 */

/* the calls below are local and normally complete in microseconds, this only
 * guards against a hung or unreachable bus daemon blocking the main loop */
#define INHIBIT_CALL_TIMEOUT_USEC 2000000

/* arguments shown by "systemd-inhibit --list" and friends */
#define INHIBIT_APP_NAME "cmus"
#define INHIBIT_REASON "Playing audio"
/* "sleep" blocks suspend; "idle" would stop blanking/locking as well, see above */
#define INHIBIT_LOGIND_WHAT "sleep"

static sd_bus *system_bus;
static sd_bus *session_bus;
static int logind_fd = -1;
static uint32_t screensaver_cookie;
static const char *screensaver_path;
static int inhibited;
static int inhibit_warned;

static void logind_release(void)
{
	if (logind_fd != -1) {
		close(logind_fd);
		logind_fd = -1;
	}
}

static int logind_acquire(void)
{
	sd_bus_error err = SD_BUS_ERROR_NULL;
	sd_bus_message *reply = NULL;
	int fd, rc;

	if (!system_bus) {
		rc = sd_bus_open_system(&system_bus);
		if (rc < 0)
			return rc;
		sd_bus_set_method_call_timeout(system_bus,
				INHIBIT_CALL_TIMEOUT_USEC);
	}

	rc = sd_bus_call_method(system_bus, "org.freedesktop.login1",
			"/org/freedesktop/login1", "org.freedesktop.login1.Manager",
			"Inhibit", &err, &reply, "ssss",
			INHIBIT_LOGIND_WHAT, INHIBIT_APP_NAME, INHIBIT_REASON,
			"block");
	if (rc < 0) {
		d_print("logind Inhibit() failed: %s\n",
				err.message ? err.message : strerror(-rc));
		sd_bus_error_free(&err);
		return rc;
	}
	sd_bus_error_free(&err);

	rc = sd_bus_message_read(reply, "h", &fd);
	if (rc <= 0) {
		sd_bus_message_unref(reply);
		return rc < 0 ? rc : -EIO;
	}

	/* the fd is owned by the message, keep a copy of our own */
	logind_fd = dup(fd);
	if (logind_fd != -1)
		fcntl(logind_fd, F_SETFD, FD_CLOEXEC);
	rc = logind_fd == -1 ? -errno : 0;
	sd_bus_message_unref(reply);
	if (rc < 0)
		logind_fd = -1;
	return rc;
}

static int screensaver_inhibit(const char *path, uint32_t *cookie)
{
	sd_bus_error err = SD_BUS_ERROR_NULL;
	sd_bus_message *reply = NULL;
	int rc;

	rc = sd_bus_call_method(session_bus, "org.freedesktop.ScreenSaver",
			path, "org.freedesktop.ScreenSaver", "Inhibit", &err,
			&reply, "ss", INHIBIT_APP_NAME, INHIBIT_REASON);
	if (rc >= 0 && reply)
		rc = sd_bus_message_read(reply, "u", cookie);
	sd_bus_error_free(&err);
	if (reply)
		sd_bus_message_unref(reply);
	return rc;
}

static int screensaver_acquire(void)
{
	static const char * const paths[] = {
		"/org/freedesktop/ScreenSaver",
		/* older (ksmserver) implementations use this path */
		"/ScreenSaver",
	};
	uint32_t cookie = 0;
	unsigned int i;
	int rc = -ENOTSUP;

	if (!session_bus) {
		rc = sd_bus_open_user(&session_bus);
		if (rc < 0)
			return rc;
		sd_bus_set_method_call_timeout(session_bus,
				INHIBIT_CALL_TIMEOUT_USEC);
	}

	for (i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
		rc = screensaver_inhibit(paths[i], &cookie);
		if (rc > 0) {
			screensaver_cookie = cookie;
			screensaver_path = paths[i];
			return 0;
		}
	}
	d_print("org.freedesktop.ScreenSaver.Inhibit() failed\n");
	return rc < 0 ? rc : -EIO;
}

static void screensaver_release(void)
{
	sd_bus_error err = SD_BUS_ERROR_NULL;
	sd_bus_message *reply = NULL;

	if (!session_bus || !screensaver_path)
		return;

	sd_bus_call_method(session_bus, "org.freedesktop.ScreenSaver",
			screensaver_path, "org.freedesktop.ScreenSaver",
			"UnInhibit", &err, &reply, "u", screensaver_cookie);
	sd_bus_error_free(&err);
	if (reply)
		sd_bus_message_unref(reply);
	screensaver_path = NULL;
	screensaver_cookie = 0;
}

static void inhibit_acquire(void)
{
	int acquired = 0;

	if (inhibited)
		return;

	if (logind_acquire() == 0) {
		acquired = 1;
	} else if (screensaver_acquire() == 0) {
		/* fallback only: this one inhibits blanking and locking as well,
		 * which is not what we want when logind can be used instead */
		acquired = 1;
	}

	if (!acquired) {
		if (!inhibit_warned) {
			error_msg("could not inhibit sleep "
					"(no usable D-Bus service found)");
			inhibit_warned = 1;
		}
		return;
	}

	inhibited = 1;
	d_print("sleep inhibition acquired\n");
}

static void inhibit_release(void)
{
	if (!inhibited)
		return;

	logind_release();
	screensaver_release();
	inhibited = 0;
	d_print("sleep inhibition released\n");
}

void inhibit_update(enum player_status status)
{
	if (!sleep_inhibit) {
		inhibit_release();
		return;
	}
	if (status == PLAYER_STATUS_PLAYING)
		inhibit_acquire();
	else
		inhibit_release();
}

void inhibit_free(void)
{
	inhibit_release();

	if (system_bus) {
		sd_bus_unref(system_bus);
		system_bus = NULL;
	}
	if (session_bus) {
		sd_bus_unref(session_bus);
		session_bus = NULL;
	}
}

#endif
