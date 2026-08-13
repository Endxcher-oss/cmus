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

#ifndef CMUS_LYRICS_H
#define CMUS_LYRICS_H

#include <stdbool.h>
#include <stddef.h>

struct track_info;

struct lyric_line {
	/* timestamp in milliseconds, -1 if not synchronized */
	int msec;
	char *text;
};

struct lyrics {
	struct lyric_line *lines;
	int nr_lines;
	bool synced;
};

/* load lyrics for @ti: embedded tag first, then a sidecar file
 * returns 0 on success, -1 if no lyrics found
 */
int lyrics_load(const struct track_info *ti, struct lyrics *lyrics);
void lyrics_free(struct lyrics *lyrics);

/* current line to display for playback position @pos_ms (milliseconds) */
const char *lyrics_get_line(const struct lyrics *lyrics, int pos_ms);

#endif
