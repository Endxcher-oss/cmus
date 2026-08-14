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

#include "info.h"
#include "cache.h"
#include "editable.h"
#include "lib.h"
#include "play_queue.h"
#include "player.h"
#include "pl.h"
#include "track_info.h"
#include "keyval.h"
#include "uchar.h"
#include "xmalloc.h"
#include "gbuf.h"
#include "utils.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdarg.h>
#include <sys/stat.h>

struct window *info_win;
struct searchable *info_searchable;
LIST_HEAD(info_head);

static inline void info_entry_to_iter(struct info_entry *e, struct iter *iter)
{
	iter->data0 = &info_head;
	iter->data1 = e;
	iter->data2 = NULL;
}

static GENERIC_ITER_PREV(info_get_prev, struct info_entry, node)
static GENERIC_ITER_NEXT(info_get_next, struct info_entry, node)

static int info_search_get_current(void *data, struct iter *iter, enum search_direction dir)
{
	return window_get_sel(info_win, iter);
}

static int info_search_matches(void *data, struct iter *iter, const char *text)
{
	struct info_entry *e = iter_to_info_entry(iter);

	return u_strcasestr(e->text, text) != NULL;
}

static const struct searchable_ops info_search_ops = {
	.get_prev = info_get_prev,
	.get_next = info_get_next,
	.get_current = info_search_get_current,
	.matches = info_search_matches
};

static int info_width = 80;

static void info_add_row(const char *text)
{
	struct info_entry *e = xnew(struct info_entry, 1);

	e->text = xstrdup(text);
	list_add_tail(&e->node, &info_head);
}

static void info_add_wrapped(const char *text)
{
	struct gbuf cur = { gbuf_empty_buffer, 0, 0 };
	int i = 0;
	int w = 0;

	while (text[i]) {
		uchar u;
		int cw;

		if (text[i] == '\r') {
			i++;
			continue;
		}
		if (text[i] == '\n') {
			info_add_row(cur.buffer);
			gbuf_clear(&cur);
			w = 0;
			i++;
			continue;
		}
		u = u_get_char(text, &i);
		cw = u_char_width(u);
		if (w > 0 && w + cw > info_width) {
			info_add_row(cur.buffer);
			gbuf_clear(&cur);
			w = 0;
		}
		gbuf_add_uchar(&cur, u);
		w += cw;
	}
	if (cur.len > 0)
		info_add_row(cur.buffer);
	gbuf_free(&cur);
}

static void info_add(const char *fmt, ...)
{
	struct gbuf buf = { gbuf_empty_buffer, 0, 0 };
	va_list ap;

	va_start(ap, fmt);
	gbuf_vaddf(&buf, fmt, ap);
	va_end(ap);

	info_add_wrapped(buf.buffer);
	gbuf_free(&buf);
}

static int info_is_core_key(const char *key)
{
	return !strcasecmp(key, "title") ||
		!strcasecmp(key, "artist") ||
		!strcasecmp(key, "album") ||
		!strcasecmp(key, "albumartist") ||
		!strcasecmp(key, "genre") ||
		!strcasecmp(key, "date") ||
		!strcasecmp(key, "tracknumber") ||
		!strcasecmp(key, "discnumber") ||
		!strcasecmp(key, "comment");
}

void info_refresh_if_changed(void)
{
	struct track_info *ti = player_info.ti;
	struct track_info *fresh;
	struct stat st;

	if (!ti || !ti->filename || is_url(ti->filename))
		return;
	if (stat(ti->filename, &st) || st.st_mtime == ti->mtime)
		return;

	cache_lock();
	fresh = cache_get_ti(ti->filename, 1);
	cache_unlock();
	if (!fresh)
		return;

	/* keep every view of this track consistent, mirroring
	 * job_handle_update_cache_result() but for a single file */
	if (lib_remove(ti))
		lib_add_track(fresh, NULL);
	pl_update_track(ti, fresh);
	editable_update_track(&pq_editable, ti, fresh);

	if (player_info.ti == ti) {
		track_info_ref(fresh);
		player_file_changed(fresh);
	}
	track_info_unref(fresh);
}

void info_update(int width)
{
	struct info_entry *e;
	struct list_head *item, *tmp;
	struct track_info *ti;

	info_width = width - 1;

	info_refresh_if_changed();
	ti = player_info.ti;

	window_set_empty(info_win);

	list_for_each_safe(item, tmp, &info_head) {
		e = container_of(item, struct info_entry, node);
		list_del(&e->node);
		free(e->text);
		free(e);
	}

	if (!ti) {
		info_add("No track playing");
	} else {
		info_add("Title: %s", ti->title ? ti->title : "(none)");
		info_add("Artist: %s", ti->artist ? ti->artist : "(none)");
		info_add("Album: %s", ti->album ? ti->album : "(none)");
		info_add("Album Artist: %s", ti->albumartist ? ti->albumartist : "(none)");
		info_add("Genre: %s", ti->genre ? ti->genre : "(none)");
		if (ti->date > 0)
			info_add("Date: %d", ti->date);
		if (ti->tracknumber > 0)
			info_add("Track: %d", ti->tracknumber);
		if (ti->discnumber > 0)
			info_add("Disc: %d", ti->discnumber);
		info_add("Duration: %d:%02d", ti->duration / 60, ti->duration % 60);
		if (ti->codec_profile)
			info_add("Codec Profile: %s", ti->codec_profile);

		int sample_rate = ti->sample_rate;
		if (sample_rate <= 0 && ti->filename && !is_url(ti->filename)) {
			struct track_info *fresh = cache_get_ti(ti->filename, 1);

			if (fresh) {
				if (fresh->sample_rate > 0)
					sample_rate = fresh->sample_rate;
				track_info_unref(fresh);
			}
		}
		struct gbuf codec_buf = { gbuf_empty_buffer, 0, 0 };
		int n = 0;
		if (ti->codec)
			gbuf_addf(&codec_buf, "%s%s", n++ ? ", " : "", ti->codec);
		if (sample_rate > 0)
			gbuf_addf(&codec_buf, "%s%d Hz", n++ ? ", " : "", sample_rate);
		if (ti->bitrate > 0)
			gbuf_addf(&codec_buf, "%s%ld kbps", n++ ? ", " : "",
					(long) (ti->bitrate / 1000. + 0.5));
		if (n > 0)
			info_add("Codec: %s", codec_buf.buffer);
		gbuf_free(&codec_buf);

		info_add("Filename: %s", ti->filename);

		if (ti->comments) {
			int i;

			for (i = 0; ti->comments[i].key; i++) {
				if (info_is_core_key(ti->comments[i].key))
					continue;
				if (!strcasecmp(ti->comments[i].key, "lyrics")) {
					info_add("Lyrics:");
					info_add("%s", ti->comments[i].val);
				} else {
					info_add("%s: %s", ti->comments[i].key, ti->comments[i].val);
				}
			}
		}
	}

	window_set_contents(info_win, &info_head);
	window_changed(info_win);
}

void info_init(void)
{
	struct iter iter;

	info_win = window_new(info_get_prev, info_get_next);
	window_set_empty(info_win);

	iter.data0 = &info_head;
	iter.data1 = NULL;
	iter.data2 = NULL;
	info_searchable = searchable_new(NULL, &iter, &info_search_ops);
}

void info_exit(void)
{
	struct info_entry *e;
	struct list_head *item, *tmp;

	list_for_each_safe(item, tmp, &info_head) {
		e = container_of(item, struct info_entry, node);
		list_del(&e->node);
		free(e->text);
		free(e);
	}

	searchable_free(info_searchable);
	window_free(info_win);
}
