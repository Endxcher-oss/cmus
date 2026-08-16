/*
 * Copyright 2008-2013 Various Authors
 * Copyright 2004-2005 Timo Hirvonen
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

#include "misc.h"
#include "prog.h"
#include "xmalloc.h"
#include "xstrjoin.h"
#include "ui_curses.h"
#include "file.h"
#include "config/libdir.h"
#include "config/datadir.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <ctype.h>
#include <stdint.h>
#include <inttypes.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <stdarg.h>
#include <pwd.h>

const char *cmus_config_dir = NULL;
const char *cmus_playlist_dir = NULL;
const char *cmus_socket_path = NULL;
const char *cmus_data_dir = NULL;
const char *cmus_lib_dir = NULL;
const char *home_dir = NULL;
const char *cmus_albumart_dir = NULL;

char **get_words(const char *text)
{
	char **words;
	int i, j, count;

	while (*text == ' ')
		text++;

	count = 0;
	i = 0;
	while (text[i]) {
		count++;
		while (text[i] && text[i] != ' ')
			i++;
		while (text[i] == ' ')
			i++;
	}
	words = xnew(char *, count + 1);

	i = 0;
	j = 0;
	while (text[i]) {
		int start = i;

		while (text[i] && text[i] != ' ')
			i++;
		words[j++] = xstrndup(text + start, i - start);
		while (text[i] == ' ')
			i++;
	}
	words[j] = NULL;
	return words;
}

int strptrcmp(const void *a, const void *b)
{
	const char *as = *(char **)a;
	const char *bs = *(char **)b;

	return strcmp(as, bs);
}

int strptrcoll(const void *a, const void *b)
{
	const char *as = *(char **)a;
	const char *bs = *(char **)b;

	return strcoll(as, bs);
}

const char *escape(const char *str)
{
	static char *buf = NULL;
	static size_t alloc = 0;
	size_t len = strlen(str);
	size_t need = len * 2 + 1;
	int s, d;

	if (need > alloc) {
		alloc = (need + 16) & ~(16 - 1);
		buf = xrealloc(buf, alloc);
	}

	d = 0;
	for (s = 0; str[s]; s++) {
		if (str[s] == '\\') {
			buf[d++] = '\\';
			buf[d++] = '\\';
			continue;
		}
		if (str[s] == '\n') {
			buf[d++] = '\\';
			buf[d++] = 'n';
			continue;
		}
		buf[d++] = str[s];
	}
	buf[d] = 0;
	return buf;
}

const char *unescape(const char *str)
{
	static char *buf = NULL;
	static size_t alloc = 0;
	size_t need = strlen(str) + 1;
	int do_escape = 0;
	int s, d;

	if (need > alloc) {
		alloc = (need + 16) & ~(16 - 1);
		buf = xrealloc(buf, alloc);
	}

	d = 0;
	for (s = 0; str[s]; s++) {
		if (!do_escape && str[s] == '\\')
			do_escape = 1;
		else {
			buf[d++] = (do_escape && str[s] == 'n') ? '\n' : str[s];
			do_escape = 0;
		}
	}
	buf[d] = 0;
	return buf;
}

static int dir_exists(const char *dirname)
{
	DIR *dir;

	dir = opendir(dirname);
	if (dir == NULL) {
		if (errno == ENOENT)
			return 0;
		return -1;
	}
	closedir(dir);
	return 1;
}

static void make_dir(const char *dirname)
{
	int rc;

	rc = dir_exists(dirname);
	if (rc == 1)
		return;
	if (rc == -1)
		die_errno("error: opening `%s'", dirname);
	rc = mkdir(dirname, 0700);
	if (rc == -1)
		die_errno("error: creating directory `%s'", dirname);
}

static int make_dir_if_missing(const char *dirname)
{
	int rc = dir_exists(dirname);

	if (rc == 1)
		return 0;
	if (rc == -1)
		return -1;
	if (mkdir(dirname, 0700) == -1 && errno != EEXIST)
		return -1;
	return 0;
}

static int make_dirs_for_path(const char *path)
{
	char *tmp;
	char *p;
	int rc;

	if (!path || !*path)
		return -1;
	tmp = xstrdup(path);
	p = tmp;
	if (*p == '/')
		p++;

	while (*p) {
		if (*p == '/') {
			*p = '\0';
			rc = make_dir_if_missing(tmp);
			if (rc) {
				free(tmp);
				return -1;
			}
			*p = '/';
		}
		p++;
	}
	rc = make_dir_if_missing(tmp);
	free(tmp);
	return rc;
}

static char *get_non_empty_env(const char *name)
{
	const char *val;

	val = getenv(name);
	if (val == NULL || val[0] == 0)
		return NULL;
	return xstrdup(val);
}

const char *get_filename(const char *path)
{
	const char *file = strrchr(path, '/');
	if (!file)
		file = path;
	else
		file++;
	if (!*file)
		return NULL;
	return file;
}

static void move_old_playlist(void)
{
	char *default_playlist = xstrjoin(cmus_playlist_dir, "/Default");
	char *old_playlist = xstrjoin(cmus_config_dir, "/playlist.pl");
	int rc = rename(old_playlist, default_playlist);
	if (rc && errno != ENOENT)
		die_errno("error: unable to move %s to playlist directory",
				old_playlist);
	free(default_playlist);
	free(old_playlist);
}

int misc_init(void)
{
	char *xdg_runtime_dir = get_non_empty_env("XDG_RUNTIME_DIR");

	home_dir = get_non_empty_env("HOME");
	if (home_dir == NULL)
		die("error: environment variable HOME not set\n");

	cmus_config_dir = get_non_empty_env("CMUS_HOME");
	if (cmus_config_dir == NULL) {
		char *cmus_home = xstrjoin(home_dir, "/.cmus");
		int cmus_home_exists = dir_exists(cmus_home);

		if (cmus_home_exists == 1) {
			cmus_config_dir = xstrdup(cmus_home);
		} else if (cmus_home_exists == -1) {
			die_errno("error: opening `%s'", cmus_home);
		} else {
			char *xdg_config_home = get_non_empty_env("XDG_CONFIG_HOME");
			if (xdg_config_home == NULL) {
				xdg_config_home = xstrjoin(home_dir, "/.config");
			}

			make_dir(xdg_config_home);
			cmus_config_dir = xstrjoin(xdg_config_home, "/cmus");

			free(xdg_config_home);
		}

		free(cmus_home);
	}
	make_dir(cmus_config_dir);

	cmus_playlist_dir = get_non_empty_env("CMUS_PLAYLIST_DIR");
	if (!cmus_playlist_dir)
		cmus_playlist_dir = xstrjoin(cmus_config_dir, "/playlists");

	int playlist_dir_is_new = dir_exists(cmus_playlist_dir) == 0;
	make_dir(cmus_playlist_dir);
	if (playlist_dir_is_new)
		move_old_playlist();

	cmus_socket_path = get_non_empty_env("CMUS_SOCKET");
	if (cmus_socket_path == NULL) {
		if (xdg_runtime_dir == NULL) {
			cmus_socket_path = xstrjoin(cmus_config_dir, "/socket");
		} else {
			cmus_socket_path = xstrjoin(xdg_runtime_dir, "/cmus-socket");
		}
	}

	cmus_lib_dir = getenv("CMUS_LIB_DIR");
	if (!cmus_lib_dir)
		cmus_lib_dir = LIBDIR "/cmus";

	cmus_data_dir = getenv("CMUS_DATA_DIR");
	if (!cmus_data_dir)
		cmus_data_dir = DATADIR "/cmus";

	cmus_albumart_dir = "/tmp/cmus/albumart";

	/* Create eagerly when possible, but don't fail startup if the
	 * cache directory is read-only; saving art retries this later. */
	(void)make_dirs_for_path(cmus_albumart_dir);

	free(xdg_runtime_dir);
	return 0;
}

int replaygain_decode(unsigned int field, int *gain)
{
	unsigned int name_code, originator_code, sign_bit, val;

	name_code = (field >> 13) & 0x7;
	if (!name_code || name_code > 2)
		return 0;
	originator_code = (field >> 10) & 0x7;
	if (!originator_code)
		return 0;
	sign_bit = (field >> 9) & 0x1;
	val = field & 0x1ff;
	if (sign_bit && !val)
		return 0;
	*gain = (sign_bit ? -1 : 1) * val;
	return name_code;
}

static char *get_home_dir(const char *username)
{
	struct passwd *passwd;

	if (username == NULL)
		return xstrdup(home_dir);
	passwd = getpwnam(username);
	if (passwd == NULL)
		return NULL;
	/* don't free passwd */
	return xstrdup(passwd->pw_dir);
}

char *expand_filename(const char *name)
{
	if (name[0] == '~') {
		char *slash;

		slash = strchr(name, '/');
		if (slash) {
			char *username, *home;

			if (slash - name - 1 > 0) {
				/* ~user/... */
				username = xstrndup(name + 1, slash - name - 1);
			} else {
				/* ~/... */
				username = NULL;
			}
			home = get_home_dir(username);
			free(username);
			if (home) {
				char *expanded;

				expanded = xstrjoin(home, slash);
				free(home);
				return expanded;
			} else {
				return xstrdup(name);
			}
		} else {
			if (name[1] == 0) {
				return xstrdup(home_dir);
			} else {
				char *home;

				home = get_home_dir(name + 1);
				if (home)
					return home;
				return xstrdup(name);
			}
		}
	} else {
		return xstrdup(name);
	}
}

void shuffle_array(void *array, size_t n, size_t size)
{
	char tmp[size];
	char *arr = array;
	for (ssize_t i = 0; i < (ssize_t)n - 1; ++i) {
		size_t rnd = (size_t) rand();
		size_t j = i + rnd / (RAND_MAX / (n - i) + 1);
		memcpy(tmp, arr + j * size, size);
		memcpy(arr + j * size, arr + i * size, size);
		memcpy(arr + i * size, tmp, size);
	}
}

size_t uri_encode(const char *src, size_t len, char *dst)
{
	static const char hex[] = "0123456789ABCDEF";
	size_t i, j = 0;

	for (i = 0; i < len; i++) {
		unsigned char c = (unsigned char)src[i];
		if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
				(c >= '0' && c <= '9') || c == '-' || c == '_' ||
				c == '.' || c == '~' || c == '/') {
			dst[j++] = c;
		} else {
			dst[j++] = '%';
			dst[j++] = hex[c >> 4];
			dst[j++] = hex[c & 0xf];
		}
	}
	dst[j] = '\0';
	return j;
}

static uint64_t fnv1a64(const char *str)
{
	uint64_t hash = UINT64_C(1469598103934665603);

	while (*str) {
		hash ^= (unsigned char)*str++;
		hash *= UINT64_C(1099511628211);
	}
	return hash;
}

static char *albumart_path_for_track(const char *filename)
{
	const char *base = get_filename(filename);
	char *stem = xstrdup(base && *base ? base : "cover");
	char *dot = strrchr(stem, '.');
	char *p;
	char hashbuf[17];

	if (dot)
		*dot = '\0';

	for (p = stem; *p; p++) {
		unsigned char c = (unsigned char)*p;
		/* Keep normal printable characters, including UTF-8, so cache
		 * names stay readable. Only hide control characters and '/'. */
		if (c < 0x20 || c == 0x7f || c == '/')
			*p = '_';
	}
	if (!*stem) {
		free(stem);
		stem = xstrdup("cover");
	}

	snprintf(hashbuf, sizeof(hashbuf), "%016" PRIx64, fnv1a64(filename));
	char *path = xstrjoin(cmus_albumart_dir, "/", stem, "-", hashbuf);
	free(stem);
	return path;
}

static int albumart_ensure_dir(void)
{
	return make_dirs_for_path(cmus_albumart_dir);
}

char *albumart_save_data(const char *filename, const void *data, size_t len)
{
	char *path;
	int fd;

	if (!cmus_albumart_dir || !filename || !data || !len)
		return NULL;
	if (albumart_ensure_dir() == -1)
		return NULL;

	path = albumart_path_for_track(filename);
	fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR);
	if (fd < 0) {
		free(path);
		return NULL;
	}
	if (write_all(fd, data, len) == -1) {
		close(fd);
		free(path);
		return NULL;
	}
	close(fd);
	return path;
}

char *albumart_cached_path(const char *filename)
{
	char *path;
	struct stat st;

	if (!cmus_albumart_dir || !filename)
		return NULL;
	path = albumart_path_for_track(filename);
	if (stat(path, &st) == 0 && S_ISREG(st.st_mode))
		return path;
	free(path);
	return NULL;
}

static int b64_decode(const char *in, char **out, int *out_len)
{
	static const int inv[] = {
		62, -1, -1, -1, 63, 52, 53, 54, 55, 56, 57, 58,
		59, 60, 61, -1, -1, -1, -1, -1, -1, -1, 0, 1, 2, 3, 4, 5,
		6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20,
		21, 22, 23, 24, 25, -1, -1, -1, -1, -1, -1, 26, 27, 28,
		29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42,
		43, 44, 45, 46, 47, 48, 49, 50, 51
	};
	size_t len, i, j;
	int v;

	if (!in || !out || !out_len)
		return 0;

	len = strlen(in);
	if (len == 0 || len % 4 != 0)
		return 0;

	for (i = 0; i < len; i++) {
		char c = in[i];
		if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
				(c >= 'a' && c <= 'z') || c == '+' || c == '/' || c == '='))
			return 0;
	}

	*out_len = len / 4 * 3;
	if (len > 0 && in[len - 1] == '=')
		(*out_len)--;
	if (len > 1 && in[len - 2] == '=')
		(*out_len)--;
	if (*out_len <= 0)
		return 0;
	*out = xnew(char, *out_len);

	for (i = 0, j = 0; i < len; i += 4, j += 3) {
		v = inv[(unsigned char)in[i] - 43];
		v = (v << 6) | inv[(unsigned char)in[i + 1] - 43];
		v = in[i + 2] == '=' ? v << 6 : (v << 6) | inv[(unsigned char)in[i + 2] - 43];
		v = in[i + 3] == '=' ? v << 6 : (v << 6) | inv[(unsigned char)in[i + 3] - 43];

		(*out)[j] = (v >> 16) & 0xff;
		if (in[i + 2] != '=')
			(*out)[j + 1] = (v >> 8) & 0xff;
		if (in[i + 3] != '=')
			(*out)[j + 2] = v & 0xff;
	}
	return 1;
}

static uint32_t read_be32(const unsigned char *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
		((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

char *albumart_save_base64(const char *filename, const char *encoded)
{
	unsigned char *decoded;
	int decoded_len;
	unsigned int pos;
	uint32_t mime_len, desc_len, pic_len;
	char *path;

	if (!b64_decode(encoded, (char **)&decoded, &decoded_len))
		return NULL;

	/* METADATA_BLOCK_PICTURE: type(4) mime(4+len) desc(4+len) dims(16) len(4) data */
	pos = 4;
	if (decoded_len < 4) {
		free(decoded);
		return NULL;
	}
	mime_len = read_be32(decoded + pos);
	pos += 4;
	if (mime_len > (unsigned int)decoded_len - pos) {
		free(decoded);
		return NULL;
	}
	pos += mime_len;
	if (pos + 4 > (unsigned int)decoded_len) {
		free(decoded);
		return NULL;
	}
	desc_len = read_be32(decoded + pos);
	pos += 4;
	if (desc_len > (unsigned int)decoded_len - pos) {
		free(decoded);
		return NULL;
	}
	pos += desc_len;
	if (pos + 16 + 4 > (unsigned int)decoded_len) {
		free(decoded);
		return NULL;
	}
	pos += 16;
	pic_len = read_be32(decoded + pos);
	pos += 4;
	if (pic_len > (unsigned int)decoded_len - pos) {
		free(decoded);
		return NULL;
	}

	path = albumart_save_data(filename, decoded + pos, pic_len);
	free(decoded);
	return path;
}
