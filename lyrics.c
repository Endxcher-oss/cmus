/*
 * Copyright 2008-2013 Various Authors
 * Copyright 2026 Various Authors
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

#include "lyrics.h"
#include "cache.h"
#include "keyval.h"
#include "path.h"
#include "track_info.h"
#include "utils.h"
#include "xmalloc.h"
#include "xstrjoin.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>

static void lyrics_add_line(struct lyrics *lyrics, int msec, const char *text)
{
	struct lyric_line *l;

	lyrics->lines = xrenew(struct lyric_line, lyrics->lines, lyrics->nr_lines + 1);
	l = &lyrics->lines[lyrics->nr_lines];
	l->msec = msec;
	l->text = xstrdup(text);
	lyrics->nr_lines++;
}

static int parse_timestamp(const char *s, int len)
{
	const char *p = s;
	const char *end = s + len;
	int m = 0, sec = 0, ms = 0, digits;

	while (p < end && *p >= '0' && *p <= '9') {
		m = m * 10 + (*p - '0');
		p++;
	}
	if (p >= end || *p != ':')
		return -1;
	p++;
	if (p >= end || *p < '0' || *p > '9')
		return -1;
	while (p < end && *p >= '0' && *p <= '9') {
		sec = sec * 10 + (*p - '0');
		p++;
	}
	if (p < end && *p == '.') {
		p++;
		digits = 0;
		while (p < end && *p >= '0' && *p <= '9' && digits < 3) {
			ms = ms * 10 + (*p - '0');
			digits++;
			p++;
		}
		while (digits < 3) {
			ms *= 10;
			digits++;
		}
		while (p < end && *p >= '0' && *p <= '9')
			p++;
	}
	if (p != end)
		return -1;
	return (m * 60 + sec) * 1000 + ms;
}

static bool line_has_timestamp(const char *line)
{
	const char *p = line;

	while (*p == '[') {
		const char *end = strchr(p + 1, ']');

		if (!end)
			break;
		if (parse_timestamp(p + 1, end - (p + 1)) >= 0)
			return true;
		p = end + 1;
	}
	return false;
}

static int parse_offset(const char *s, int len)
{
	bool neg = false;
	int val = 0;

	if (len > 0 && (*s == '+' || *s == '-')) {
		neg = (*s == '-');
		s++;
		len--;
	}
	while (len > 0 && *s >= '0' && *s <= '9') {
		val = val * 10 + (*s - '0');
		s++;
		len--;
	}
	return neg ? -val : val;
}

static void parse_line(char *line, struct lyrics *lyrics, int *offset, bool synced)
{
	int msecs[32];
	int nr_msecs = 0;
	const char *p = line;
	char *text;
	size_t tlen;
	int i;

	while (*p == '[') {
		const char *end = strchr(p + 1, ']');
		int len;

		if (!end)
			break;
		len = end - (p + 1);
		int ms = parse_timestamp(p + 1, len);
		if (ms >= 0) {
			if (nr_msecs < N_ELEMENTS(msecs))
				msecs[nr_msecs++] = ms;
		} else if (len > 7 && strncasecmp(p + 1, "offset:", 7) == 0) {
			*offset = parse_offset(p + 1 + 7, len - 7);
		}
		p = end + 1;
	}

	text = (char *)p;
	while (*text == ' ' || *text == '\t')
		text++;
	if (*text == 0)
		return;

	tlen = strlen(text);
	while (tlen > 0 && (text[tlen - 1] == ' ' || text[tlen - 1] == '\t' ||
			text[tlen - 1] == '\r'))
		text[--tlen] = 0;
	if (tlen == 0)
		return;

	if (nr_msecs > 0) {
		for (i = 0; i < nr_msecs; i++)
			lyrics_add_line(lyrics, msecs[i] + *offset, text);
	} else if (!synced) {
		lyrics_add_line(lyrics, -1, text);
	}
}

static char **split_lines(char *content, int *nr_out)
{
	int alloc = 0, nr = 0;
	char **lines = NULL;
	char *p = content;

	while (*p) {
		char *nl = strchr(p, '\n');

		if (nr >= alloc) {
			alloc = alloc ? alloc * 2 : 16;
			lines = xrenew(char *, lines, alloc);
		}
		lines[nr++] = p;
		if (!nl)
			break;
		*nl = 0;
		p = nl + 1;
	}
	*nr_out = nr;
	return lines;
}

static int line_cmp(const void *a, const void *b)
{
	const struct lyric_line *la = a;
	const struct lyric_line *lb = b;

	return la->msec - lb->msec;
}

static int lrc_parse(char *content, struct lyrics *lyrics)
{
	char **lines;
	int i, nr;
	bool synced = false;
	int offset = 0;

	lines = split_lines(content, &nr);
	for (i = 0; i < nr; i++) {
		if (line_has_timestamp(lines[i])) {
			synced = true;
			break;
		}
	}
	lyrics->synced = synced;

	for (i = 0; i < nr; i++)
		parse_line(lines[i], lyrics, &offset, synced);

	free(lines);

	if (synced && lyrics->nr_lines > 0) {
		qsort(lyrics->lines, lyrics->nr_lines, sizeof(struct lyric_line), line_cmp);

		/* merge lines sharing the same timestamp, joining text with a space */
		int out = 0;

		for (int m = 0; m < lyrics->nr_lines; ) {
			int n = m + 1;

			while (n < lyrics->nr_lines && lyrics->lines[n].msec == lyrics->lines[m].msec)
				n++;

			if (n - m > 1) {
				size_t total = strlen(lyrics->lines[m].text);
				size_t pos;
				char *merged;
				int k;

				for (k = m + 1; k < n; k++)
					total += 1 + strlen(lyrics->lines[k].text);

				merged = xnew(char, total + 1);
				pos = strlen(lyrics->lines[m].text);
				memcpy(merged, lyrics->lines[m].text, pos);
				for (k = m + 1; k < n; k++) {
					size_t slen = strlen(lyrics->lines[k].text);

					merged[pos++] = ' ';
					memcpy(merged + pos, lyrics->lines[k].text, slen);
					pos += slen;
				}
				merged[pos] = 0;

				free(lyrics->lines[m].text);
				lyrics->lines[m].text = merged;

				for (k = m + 1; k < n; k++)
					free(lyrics->lines[k].text);
			}

			if (out != m)
				lyrics->lines[out] = lyrics->lines[m];
			out++;
			m = n;
		}
		lyrics->nr_lines = out;
	}

	return lyrics->nr_lines > 0 ? 0 : -1;
}

static int read_text_file(const char *filename, char **content)
{
	struct stat st;
	int fd;
	ssize_t rd;

	fd = open(filename, O_RDONLY);
	if (fd == -1)
		return -1;
	if (fstat(fd, &st) == -1 || st.st_size < 0 || st.st_size > 1024 * 1024) {
		close(fd);
		return -1;
	}

	*content = xnew(char, st.st_size + 1);
	rd = read(fd, *content, st.st_size);
	close(fd);
	if (rd < 0) {
		free(*content);
		return -1;
	}
	(*content)[rd] = 0;
	return 0;
}

static int load_sidecar(const char *filename, struct lyrics *lyrics)
{
	char *dir = path_dirname(filename);
	const char *bname = path_basename(filename);
	const char *ext = get_extension(bname);
	char *base = xstrndup(bname, ext ? (size_t)(ext - bname - 1) : strlen(bname));
	const char *exts[] = { "lrc", "txt" };
	int i;
	int rc = -1;

	for (i = 0; i < N_ELEMENTS(exts); i++) {
		char *path = xstrjoin(dir, "/", base, ".", exts[i]);
		char *content;

		if (read_text_file(path, &content) == 0) {
			if (lrc_parse(content, lyrics) == 0)
				rc = 0;
			free(content);
		}
		free(path);
		if (rc == 0)
			break;
	}

	free(base);
	free(dir);
	return rc;
}

static int parse_embedded(const char *embedded, struct lyrics *lyrics)
{
	char *copy;
	int rc;

	if (!embedded || !*embedded)
		return -1;

	copy = xstrdup(embedded);
	rc = lrc_parse(copy, lyrics);
	free(copy);
	return rc;
}

int lyrics_load(const struct track_info *ti, struct lyrics *lyrics)
{
	memset(lyrics, 0, sizeof(*lyrics));

	if (ti && ti->comments && parse_embedded(keyvals_get_val(ti->comments, "lyrics"), lyrics) == 0)
		return 0;

	if (ti && ti->filename && load_sidecar(ti->filename, lyrics) == 0)
		return 0;

	if (ti && ti->filename && !is_url(ti->filename)) {
		struct track_info *fresh = cache_get_ti(ti->filename, 1);

		if (fresh) {
			int rc = parse_embedded(keyvals_get_val(fresh->comments, "lyrics"), lyrics);

			track_info_unref(fresh);
			if (rc == 0)
				return 0;
		}
	}

	return -1;
}

void lyrics_free(struct lyrics *lyrics)
{
	int i;

	for (i = 0; i < lyrics->nr_lines; i++)
		free(lyrics->lines[i].text);
	free(lyrics->lines);
	lyrics->lines = NULL;
	lyrics->nr_lines = 0;
	lyrics->synced = false;
}

const char *lyrics_get_line(const struct lyrics *lyrics, int pos_ms)
{
	int lo, hi, found = 0;

	if (lyrics->nr_lines <= 0)
		return NULL;
	if (!lyrics->synced)
		return lyrics->lines[0].text;

	if (pos_ms < lyrics->lines[0].msec)
		return lyrics->lines[0].text;

	lo = 0;
	hi = lyrics->nr_lines - 1;
	while (lo <= hi) {
		int mid = (lo + hi) / 2;

		if (lyrics->lines[mid].msec <= pos_ms) {
			found = mid;
			lo = mid + 1;
		} else {
			hi = mid - 1;
		}
	}
	return lyrics->lines[found].text;
}
