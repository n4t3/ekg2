/*
 *  (C) Copyright 2002-2003 Wojtek Kaniewski <wojtekka@irc.pl>
 *			    Wojtek Bojdo³ <wojboj@htcon.pl>
 *			    Pawe³ Maziarz <drg@infomex.pl>
 *		  2008-2010 Wies³aw Ochmiñski <wiechu@wiechu.com>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License Version 2 as
 *  published by the Free Software Foundation.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include "ekg2-config.h"

#include "ecurses.h"

#include <stdlib.h>
#include <string.h>

#include <ekg/windows.h>
#include <ekg/xmalloc.h>

#include <ekg/stuff.h>

#include "nc-stuff.h"

static inline int xucwidth(const gunichar c) {
	if (G_UNLIKELY(g_unichar_iswide(c)))
		return 2;
	else if (G_UNLIKELY(g_unichar_iszerowidth(c)))
		return 0;
	else
		return 1;
}

static inline int xucswidth(const gunichar *s) {
	int ret = 0;

	for (; *s; s++)
		ret += xucwidth(*s);

	return ret;
}

static inline int xucslen(const gunichar *s) {
	int ret = 0;

	for (; *s; s++)
		ret++;

	return ret;
}

/*
 * ncurses_backlog_split()
 *
 * dzieli linie tekstu w buforze na linie ekranowe.
 *
 *  - w - okno do podzielenia
 *  - full - czy robimy pe³ne uaktualnienie?
 *  - removed - ile linii ekranowych z góry usuniêto?
 *
 * zwraca rozmiar w liniach ekranowych ostatnio dodanej linii.
 */
int ncurses_backlog_split(window_t *w, int full, int removed)
{
	int i, res = 0, bottom = 0;
	char *timestamp_format = NULL;
	ncurses_window_t *n;

	if (!w || !(n = w->priv_data))
		return 0;

	/* przy pe³nym przebudowaniu ilo¶ci linii nie musz± siê koniecznie
	 * zgadzaæ, wiêc nie bêdziemy w stanie pó¼niej stwierdziæ czy jeste¶my
	 * na koñcu na podstawie ilo¶ci linii mieszcz±cych siê na ekranie. */
	if (full && n->start == n->lines_count - w->height)
		bottom = 1;
	
	/* mamy usun±æ co¶ z góry, bo wywalono liniê z backloga. */
	if (removed) {
		for (i = 0; i < removed && i < n->lines_count; i++) {
			g_free(n->lines[i].ts);
			g_free(n->lines[i].ts_attr);
		}
		g_memmove(&n->lines[0], &n->lines[removed], sizeof(struct screen_line) * (n->lines_count - removed));
		n->lines_count -= removed;
	}

	/* je¶li robimy pe³ne przebudowanie backloga, czy¶cimy wszystko */
	if (full) {
		for (i = 0; i < n->lines_count; i++) {
			g_free(n->lines[i].ts);
			g_free(n->lines[i].ts_attr);
		}
		n->lines_count = 0;
		g_free(n->lines);
		n->lines = NULL;
	}

	if (config_timestamp && config_timestamp_show && config_timestamp[0])
		timestamp_format = format_string(config_timestamp);

	/* je¶li upgrade... je¶li pe³ne przebudowanie... */
	for (i = (!full) ? 0 : (n->backlog_size - 1); i >= 0; i--) {
		struct screen_line *l;
		gunichar *str; 
		guint16 *attr;
		int j, margin_left, wrapping = 0;

		time_t ts;			/* current ts */
		time_t lastts = 0;		/* last cached ts */
		char lasttsbuf[100];		/* last cached strftime() result */
		int prompt_width;

		str = n->backlog[i]->str + n->backlog[i]->prompt_len;
		attr = n->backlog[i]->attr + n->backlog[i]->prompt_len;
		ts = n->backlog[i]->ts;
		margin_left = (!w->floating) ? n->backlog[i]->margin_left : -1;

		prompt_width = xucswidth(n->backlog[i]->str);
		
		for (;;) {
			int word, width;
			int ts_width = 0;

			if (!i)
				res++;

			n->lines_count++;
			n->lines = g_realloc(n->lines, n->lines_count * sizeof(struct screen_line));
			l = &n->lines[n->lines_count - 1];

			l->str = str;
			l->attr = attr;
			l->len = xucslen(str);
			l->ts = NULL;
			l->ts_attr = NULL;
			l->backlog = i;
			l->margin_left = (!wrapping || margin_left == -1) ? margin_left : 0;

			l->prompt_len = n->backlog[i]->prompt_len;
			if (!n->backlog[i]->prompt_empty) {
				l->prompt_str = n->backlog[i]->str;
				l->prompt_attr = n->backlog[i]->attr;
			} else {
				l->prompt_str = NULL;
				l->prompt_attr = NULL;
			}

			if ((!w->floating || (w->id == WINDOW_LASTLOG_ID && ts)) && timestamp_format) {
				fstring_t *s = NULL;

				if (!ts || lastts != ts) {	/* generate new */
					struct tm *tm = localtime(&ts);

					strftime(lasttsbuf, sizeof(lasttsbuf)-1, timestamp_format, tm);
					lastts = ts;
				}

				s = fstring_new(lasttsbuf);

				l->ts = s->str;
				ts_width = xucswidth(l->ts);
				ts_width++;			/* for separator between timestamp and text */
				l->ts_attr = s->attr;

				xfree(s);
			}

			width = w->width - ts_width - prompt_width - n->margin_left - n->margin_right; 

			if ((w->frames & WF_LEFT))
				width -= 1;
			if ((w->frames & WF_RIGHT))
				width -= 1;
			
			{
				int str_width = 0;

				for (j = 0, word = 0; j < l->len;) {
					gunichar ch = str[j];
					int ch_width;

					if (g_unichar_isspace(ch))
						word = j + 1;

					if (str_width >= width) {
						l->len = (!w->nowrap && word) ? word : 		/* XXX, (str_width > width) ? word-1 : word? */
							(str_width > width && j) ? j /* - 1 */ : j;

						/* avoid dead loop -- always move forward */
						/* XXX, a co z bledami przy rysowaniu? moze lepiej str++; attr++; albo break? */
						if (!l->len)
							l->len = 1;

						if (g_unichar_isspace(str[l->len])) {
							l->len--;
							str++;
							attr++;
						}
						break;
					}

					ch_width = xucwidth(str[j]);
					str_width += ch_width;
					j++;
				}
				if (w->nowrap)
					break;
			}

			str += l->len;
			attr += l->len;

			if (! *str)
				break;

			wrapping = 1;
		}
	}
	xfree(timestamp_format);

	if (bottom) {
		n->start = n->lines_count - w->height;
		if (n->start < 0)
			n->start = 0;
	}

	if (full) {
		if (window_current && window_current->id == w->id) 
			ncurses_redraw(w);
		else
			n->redraw = 1;
	}

	return res;
}

/*
 *
 */
int ncurses_backlog_add_real(window_t *w, fstring_t *str) {
	int i, removed = 0;
	ncurses_window_t *n = w->priv_data;
	
	if (!w)
		return 0;

	if (n->backlog_size == config_backlog_size) {
		fstring_t *line = n->backlog[n->backlog_size - 1];
		int i;

		for (i = 0; i < n->lines_count; i++) {
			if (n->lines[i].backlog == n->backlog_size - 1)
				removed++;
		}

		fstring_free(line);

		n->backlog_size--;
	} else 
		n->backlog = xrealloc(n->backlog, (n->backlog_size + 1) * sizeof(fstring_t *));

	memmove(&n->backlog[1], &n->backlog[0], n->backlog_size * sizeof(fstring_t *));
	n->backlog[0] = str;

	n->backlog_size++;

	for (i = 0; i < n->lines_count; i++)
		n->lines[i].backlog++;

	return ncurses_backlog_split(w, 0, removed);
}

/*
 * ncurses_backlog_add()
 *
 * dodaje do bufora okna. zak³adamy dodawanie linii ju¿ podzielonych.
 * je¶li doda siê do backloga liniê zawieraj±c± '\n', bêdzie ¼le.
 *
 *  - w - wska¼nik na okno ekg
 *  - str - linijka do dodania
 *
 * zwraca rozmiar dodanej linii w liniach ekranowych.
 */
int ncurses_backlog_add(window_t *w, fstring_t *str) {
	return ncurses_backlog_add_real(w, str);
}


/*
 * changed_backlog_size()
 *
 * wywo³ywane po zmianie warto¶ci zmiennej ,,backlog_size''.
 */
void changed_backlog_size(const char *var)
{
	window_t *w;

	if (config_backlog_size < ncurses_screen_height)
		config_backlog_size = ncurses_screen_height;

	for (w = windows; w; w = w->next) {
		ncurses_window_t *n = w->priv_data;
		int i;
				
		if (n->backlog_size <= config_backlog_size)
			continue;
				
		for (i = config_backlog_size; i < n->backlog_size; i++)
			fstring_free(n->backlog[i]);

		n->backlog_size = config_backlog_size;
		n->backlog = xrealloc(n->backlog, n->backlog_size * sizeof(fstring_t *));

		ncurses_backlog_split(w, 1, 0);
	}
}

