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

#ifndef CMUS_INFO_H
#define CMUS_INFO_H

#include "list.h"
#include "window.h"
#include "search.h"

struct info_entry {
	struct list_head node;

	char *text;
};

static inline struct info_entry *iter_to_info_entry(struct iter *iter)
{
	return iter->data1;
}

extern struct window *info_win;
extern struct searchable *info_searchable;
extern struct list_head info_head;

void info_init(void);
void info_exit(void);

/* rebuild the info list from the currently playing track
 *
 * @width  wrap width in columns for long values
 */
void info_update(int width);

#endif
