/* $Id$ */

/*
 *  (C) Copyright 2002-2003 Wojtek Kaniewski <wojtekka@irc.pl>
 *			    Wojtek Bojdo� <wojboj@htcon.pl>
 *			    Pawe� Maziarz <drg@infomex.pl>
 *		  2008-2010 Wies�aw Ochmi�ski <wiechu@wiechu.com>
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

#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#include <ekg/xmalloc.h>

#include <ekg/stuff.h>

#include "bindings.h"
#include "backlog.h"
#include "contacts.h"
#include "input.h"
#include "notify.h"
#include "nc-stuff.h"
#include "spell.h"
#include "statusbar.h"

WINDOW *ncurses_input	= NULL;		/* okno wpisywania tekstu */
WINDOW *ncurses_contacts= NULL;

CHAR_T *ncurses_history[HISTORY_MAX];	/* zapami�tane linie */
int ncurses_history_index = 0;		/* offset w historii */

int ncurses_debug = 0;			/* debugowanie */

int ncurses_screen_height;
int ncurses_screen_width;

static struct termios old_tio;

int winch_pipe[2];
int have_winch_pipe = 0;


	/* this one is meant to check whether we need to send some chatstate to disconnecting session,
	 * so jabber plugin doesn't need to care about this anymore */
QUERY(ncurses_session_disconnect_handler) {
	const char	*session	= *va_arg(ap, const char **);
	const session_t	*s		= session_find(session);
	window_t	*w;

	for (w = windows; w; w = w->next) {
		if (w->session != s)
			continue;

		ncurses_window_gone(w);
	}

	return 0;
}


/*
 * color_pair()
 *
 * zwraca numer COLOR_PAIR odpowiadaj�cej danej parze atrybut�w: kolorze
 * tekstu i kolorze t�a.
 */
int color_pair(int fg, int bg) {
	if (!config_display_color) {
		if (bg != COLOR_BLACK)
			return A_REVERSE;
		else
			return A_NORMAL;
	}

	if (fg == COLOR_BLACK && bg == COLOR_BLACK) {
		fg = 7;
	} else if (fg == COLOR_WHITE && bg == COLOR_BLACK) {
		fg = 0;
	}

	return COLOR_PAIR(fg + 8 * bg);
}

/*
 * ncurses_commit()
 *
 * zatwierdza wszystkie zmiany w buforach ncurses i wy�wietla je na ekranie.
 */
void ncurses_commit(void)
{
	ncurses_refresh();

	if (ncurses_header)
		wnoutrefresh(ncurses_header);

	wnoutrefresh(ncurses_status);

	wnoutrefresh(input);

	doupdate();
}

/*
 * ncurses_resize()
 *
 * dostosowuje rozmiar okien do rozmiaru ekranu, przesuwaj�c odpowiednio
 * wy�wietlan� zawarto��.
 */
void ncurses_resize(void)
{
	int left, right, top, bottom, width, height;
	window_t *w;

	left = 0;
	right = stdscr->_maxx + 1;
	top = config_header_size;
	bottom = stdscr->_maxy + 1 - ncurses_input_size - config_statusbar_size;
	width = right - left;
	height = bottom - top;

	if (width < 1)
		width = 1;
	if (height < 1)
		height = 1;

	for (w = windows; w; w = w->next) {
		ncurses_window_t *n = w->priv_data;
		int old_width = w->width;

		if (!n)
			continue;

		if (!w->edge)
			continue;

		w->hide = 0;

		if ((w->edge & WF_LEFT)) {
			if (w->width * 2 > width)
				w->hide = 1;
			else {
				w->left = left;
				w->top = top;
				w->height = height;
				width -= w->width;
				left += w->width;
			}
		}

		if ((w->edge & WF_RIGHT)) {
			if (w->width * 2 > width)
				w->hide = 1;
			else {
				w->left = right - w->width;
				w->top = top;
				w->height = height;
				width -= w->width;
				right -= w->width;
			}
		}

		if ((w->edge & WF_TOP)) {
			if (w->height * 2 > height)
				w->hide = 1;
			else {
				w->left = left;
				w->top = top;
				w->width = width;
				height -= w->height;
				top += w->height;
			}
		}

		if ((w->edge & WF_BOTTOM)) {
			if (w->height * 2 > height)
				w->hide = 1;
			else {
				w->left = left;
				w->top = bottom - w->height;
				w->width = width;
				height -= w->height;
				bottom -= w->height;
			}
		}

		wresize(n->window, w->height, w->width);
		mvwin(n->window, w->top, w->left);

		n->redraw = 1;

		/* if width changed, we should recalculate screen_lines, like for normal windows. */
		/* XXX, only for !w->nowrap windows? */
		if (old_width != w->width && w->floating /* XXX ? */)
			ncurses_backlog_split(w, 1, 0);
	}

	if (left < 0)			left = 0;
	if (left > stdscr->_maxx)	left = stdscr->_maxx;

	if (top < 0)			top = 0;
	if (top > stdscr->_maxy)	top = stdscr->_maxy;

	for (w = windows; w; w = w->next) {
		ncurses_window_t *n = w->priv_data;
		int delta;

		if (!n || w->floating)
			continue;

		delta = height - w->height;

		if (n->lines_count - n->start == w->height) {
			n->start -= delta;

			if (delta < 0) {
				if (n->start > n->lines_count)
					n->start = n->lines_count;
			} else {
				if (n->start < 0)
					n->start = 0;
			}
		}

		if (n->overflow > height)
			n->overflow = height;

		w->height = height;

		if (w->height < 1)
			w->height = 1;

		if (w->width != width && !w->doodle) {
			w->width = width;
			ncurses_backlog_split(w, 1, 0);
		}

		w->width = width;

		wresize(n->window, w->height, w->width);

		w->top = top;
		w->left = left;

		mvwin(n->window, w->top, w->left);

		if (n->overflow) {
			n->start = n->lines_count - w->height + n->overflow;
			if (n->start < 0)
				n->start = 0;
		}

		ncurses_update_real_prompt(n);
		n->redraw = 1;
	}

	ncurses_screen_width = width;
	ncurses_screen_height = height;
}

/*
 * fstring_attr2ncurses_attr()
 *
 * Convert internal ekg2 fstring attr value, to value which ncurses understand.
 *
 */

static inline int fstring_attr2ncurses_attr(short chattr) {
	int attr = A_NORMAL;

	if ((chattr & FSTR_BOLD))
		attr |= A_BOLD;

	if ((chattr & FSTR_BLINK))
		attr |= A_BLINK;

	if (!(chattr & FSTR_NORMAL))
		attr |= color_pair(chattr & FSTR_FOREMASK, config_display_transparent ? COLOR_BLACK: (chattr>>3)&7);

	if ((chattr & FSTR_UNDERLINE))
		attr |= A_UNDERLINE;

	if ((chattr & FSTR_REVERSE))
		attr |= A_REVERSE;

	if ((chattr & FSTR_ALTCHARSET))
		attr |= A_ALTCHARSET;

	return attr;
}

/*
 * ncurses_fixchar()
 *
 * When we recv control character (ASCII code below 32), we can add 64 to it, and REVERSE attr.
 * When we recv ISO control character [and we're using console under iso charset] (ASCII code between 128..159), we can REVERSE attr, and return '?'
 */

inline CHAR_T ncurses_fixchar(CHAR_T ch, int *attr) {
	if (ch < 32) {
		*attr |= A_REVERSE;
		return ch + 64;
	}

	if (ch > 127 && ch < 160 &&
#if USE_UNICODE
		0 &&
#endif
		config_use_iso)
	{
		*attr |= A_REVERSE;
		return '?';
	}

	return ch;
}

/*
 * cmd_mark()
 *
 * add marker (red line) to window
 *
 */
COMMAND(cmd_mark) {
	window_t *w;
	ncurses_window_t *n;

	if (match_arg(params[0], 'a', ("all"), 2)) {
		for (w = windows; w; w = w->next) {
			if (!w->floating && (w->act <= EKG_WINACT_MSG)) {
				n = w->priv_data;
				n->last_red_line = time(0);
				n->redraw = 1;
			}
		}
		return 0;
	} else if (params[0] && (atoi(params[0]) || xstrcmp(params[1], ("0")))) {
		extern int window_last_id;
		int id = atoi(params[0]);
		w = window_exist(id<0 ? window_last_id : id);
	} else
		w = window_current;

	if (w && !w->floating && (w->act <= EKG_WINACT_MSG)) {
		n = w->priv_data;
		n->last_red_line = time(0);
		n->redraw = 1;
	}
	return 0;
}

/*
 * draw_thin_red_line()
 *
 */
static void draw_thin_red_line(window_t *w, int y)
{
	ncurses_window_t *n = w->priv_data;
	int attr = color_pair(COLOR_RED, COLOR_BLACK) | A_BOLD | A_ALTCHARSET;
	unsigned char ch = (unsigned char) ncurses_fixchar((CHAR_T) ACS_HLINE, &attr);

	wattrset(n->window, attr);
	mvwhline(n->window, y, 0, ch, w->width);
}

/*
 * ncurses_redraw()
 *
 * przerysowuje zawarto�� okienka.
 *
 *  - w - okno
 */
void ncurses_redraw(window_t *w)
{
	int x, y, left, top, height, width, fix_trl;
	ncurses_window_t *n = w->priv_data;
	int dtrl = 0;	/* dtrl -- draw thin red line
			 *	0 - not on this page or line already drawn
			 *	1 - mayby on this page, we'll see later
			 */

	if (!n)
		return;

	left = n->margin_left;
	top = n->margin_top;
	height = w->height - n->margin_top - n->margin_bottom;
	width = w->width - n->margin_left - n->margin_right;

	if (w->doodle) {
		n->redraw = 0;
		return;
	}

	if (n->handle_redraw) {
		/* handler mo�e sam narysowa� wszystko, wtedy zwraca -1.
		 * mo�e te� tylko uaktualni� zawarto�� okna, wtedy zwraca
		 * 0 i rysowaniem zajmuje si� ta funkcja. */
		if (n->handle_redraw(w) == -1)
			return;
	}

	werase(n->window);

	if (w->floating) {
		const char *vertical_line_char	= format_find("contacts_vertical_line_char");
		const char *horizontal_line_char= format_find("contacts_horizontal_line_char");
		char vline_ch = vertical_line_char[0];
		char hline_ch = horizontal_line_char[0];
		int attr = color_pair(COLOR_BLUE, COLOR_BLACK);
		int x0 = n->margin_left, y0 = n->margin_top;
		int x1 = w->width - 1 - n->margin_right;
		int y1 = w->height - 1 - n->margin_bottom;

		if (!vline_ch || !hline_ch) {
			vline_ch = ACS_VLINE;
			hline_ch = ACS_HLINE;
			attr |= A_ALTCHARSET;
		}
		wattrset(n->window, attr);

		if ((w->frames & WF_LEFT)) {
			left++;
			mvwvline(n->window, y0, x0, vline_ch, y1-y0+1);
		}

		if ((w->frames & WF_RIGHT)) {
			mvwvline(n->window, y0, x1, vline_ch, y1-y0+1);
		}

		if ((w->frames & WF_TOP)) {
			top++;
			height--;
			mvwhline(n->window, y0, x0, vline_ch, x1-x0+1);
			if (w->frames & WF_LEFT)  mvwaddch(n->window, y0, x0, ACS_ULCORNER);
			if (w->frames & WF_RIGHT) mvwaddch(n->window, y0, x1, ACS_URCORNER);
		}

		if ((w->frames & WF_BOTTOM)) {
			height--;
			mvwhline(n->window, y1, x0, vline_ch, x1-x0+1);
			if (w->frames & WF_LEFT)  mvwaddch(n->window, y1, x0, ACS_LLCORNER);
			if (w->frames & WF_RIGHT) mvwaddch(n->window, y1, x1, ACS_LRCORNER);
		}

	}

	if (n->start < 0)
		n->start = 0;

	if (config_text_bottomalign && (!w->floating || config_text_bottomalign == 2)
			&& n->start == 0 && n->lines_count < height)
	{
		const int tmp = height - n->lines_count;

		if (tmp > top)
			top = tmp;
	}

	fix_trl=0;
	for (y = 0; y < height && n->start + y < n->lines_count; y++) {
		struct screen_line *l = &n->lines[n->start + y];

		int cur_y = (top + y + fix_trl);

		int fixup = 0;

		if (( y == 0 ) && n->last_red_line && (n->backlog[l->backlog]->ts < n->last_red_line))
			dtrl = 1;	/* First line timestamp is less then mark. Mayby marker is on this page? */

		if (dtrl && (n->backlog[l->backlog]->ts >= n->last_red_line)) {
			draw_thin_red_line(w, cur_y);
			if ((n->lines_count-n->start == height - (top - n->margin_top)) ) {
				/* we have stolen line for marker, so we scroll up */
				wmove(n->window, n->margin_top, 0);
				winsdelln(n->window,-1);
			} else {
				fix_trl = 1;
				cur_y++;
			}
			dtrl = 0;
		}

		wattrset(n->window, A_NORMAL);
		wmove(n->window, cur_y, left);

		if (l->ts) {
			for (x = 0; l->ts[x]; x++) {
				int attr = fstring_attr2ncurses_attr(l->ts_attr[x]);
				unsigned char ch = (unsigned char) ncurses_fixchar((CHAR_T) (unsigned char) l->ts[x], &attr);

				wattrset(n->window, attr);
				waddch(n->window, ch);
			}
		/* render separator */
			wattrset(n->window, A_NORMAL);
			waddch(n->window, ' ');
		}

		if (l->prompt_str) {
			for (x = 0; x < l->prompt_len; x++) {
				int attr = fstring_attr2ncurses_attr(l->prompt_attr[x]);
				CHAR_T ch = ncurses_fixchar(l->prompt_str[x], &attr);

				wattrset(n->window, attr);

				/* XXX, width vs len? */
				if (!fixup && (l->margin_left != -1 && x >= l->margin_left)) {
					int x, y;

					getyx(n->window, y, x);
					x = x - l->margin_left + config_margin_size;
					wmove(n->window, y, x);

					fixup = 1;
				}
				waddch(n->window, ch);
			}
		}

		for (x = 0; x < l->len; x++) {
			int attr = fstring_attr2ncurses_attr(l->attr[x]);
			CHAR_T ch = ncurses_fixchar(l->str[x], &attr);

			wattrset(n->window, attr);

			/* XXX, width vs len? */
			if (!fixup && (l->margin_left != -1 && (x + l->prompt_len) >= l->margin_left)) {
				int x, y;

				getyx(n->window, y, x);
				x = x - l->margin_left + config_margin_size;
				wmove(n->window, y, x);

				fixup = 1;
			}
			waddch(n->window, ch);
		}
	}

	n->redraw = 0;

	if (dtrl && (n->start + y >= n->lines_count)) {
		/* marker still not drawn and last line from backlog. */
		if (y >= height - (top - n->margin_top)) {
			wmove(n->window, n->margin_top, 0);
			winsdelln(n->window,-1);
			y--;
		}
		draw_thin_red_line(w, top+y);
	}

	if (w == window_current)
		ncurses_redraw_input(0);
}

/*
 * ncurses_clear()
 *
 * czy�ci zawarto�� okna.
 */
void ncurses_clear(window_t *w, int full)
{
	ncurses_window_t *n = w->priv_data;
	w->more = 0;

	if (!full) {
		n->start = n->lines_count;
		n->redraw = 1;
		n->overflow = w->height;
		return;
	}

	if (n->backlog) {
		int i;

		for (i = 0; i < n->backlog_size; i++)
			fstring_free(n->backlog[i]);

		xfree(n->backlog);

		n->backlog = NULL;
		n->backlog_size = 0;
	}

	if (n->lines) {
		int i;

		for (i = 0; i < n->lines_count; i++) {
			xfree(n->lines[i].ts);
			xfree(n->lines[i].ts_attr);
		}

		xfree(n->lines);

		n->lines = NULL;
		n->lines_count = 0;
	}

	n->start = 0;
	n->redraw = 1;
}

/*
 * ncurses_refresh()
 *
 * wnoutrefresh()uje aktualnie wy�wietlane okienko.
 */
void ncurses_refresh(void)
{
	window_t *w;

	if (window_current && window_current->priv_data /* !window_current->floating */) {
		ncurses_window_t *n = window_current->priv_data;

		if (n->redraw)
			ncurses_redraw(window_current);

		if (!window_current->hide)
			wnoutrefresh(n->window);
	}

	for (w = windows; w; w = w->next) {
		ncurses_window_t *n = w->priv_data;

		if (!w->floating || w->hide)
			continue;

		if (n->handle_redraw) {
			if (n->redraw)
				ncurses_redraw(w);
		} else {
			if (w->last_update != time(NULL)) {
				w->last_update = time(NULL);

				ncurses_clear(w, 1);

				ncurses_redraw(w);
			}

		}
		touchwin(n->window);
		wnoutrefresh(n->window);
	}

	mvwin(ncurses_status, stdscr->_maxy + 1 - ncurses_input_size - config_statusbar_size, 0);
	wresize(input, ncurses_input_size, input->_maxx + 1);
	mvwin(input, stdscr->_maxy - ncurses_input_size + 1, 0);
}

/*
 * ncurses_window_kill()
 *
 * usuwa podane okno.
 */
int ncurses_window_kill(window_t *w)
{
	ncurses_window_t *n = w->priv_data;

	if (!n)
		return -1;

	ncurses_clear(w, 1);

	xfree(n->prompt);
	xfree(n->prompt_real);
	delwin(n->window);
	xfree(n);
	w->priv_data = NULL;

	if (w->floating)
		ncurses_resize();

	ncurses_window_gone(w);

	return 0;
}

#ifdef SIGWINCH
static void sigwinch_handler()
{
	signal(SIGWINCH, sigwinch_handler);
	if (have_winch_pipe) {
		char c = ' ';
		write(winch_pipe[1], &c, 1);
	}
}
#endif

/*
 * ncurses_init()
 *
 * inicjalizuje ca�� zabaw� z ncurses.
 */
void ncurses_init(void)
{
	int background;

	ncurses_screen_width = getenv("COLUMNS") ? atoi(getenv("COLUMNS")) : 80;
	ncurses_screen_height = getenv("LINES") ? atoi(getenv("LINES")) : 24;

	initscr();
	cbreak();
	noecho();
	nonl();
#ifdef HAVE_NCURSES_ULC
	if (!config_use_iso
#if USE_UNICODE
			&& 0
#endif
			)
		use_legacy_coding(2);
#endif

	if (config_display_transparent) {
		background = COLOR_DEFAULT;
		use_default_colors();
	} else {
		background = COLOR_BLACK;
		assume_default_colors(COLOR_WHITE, COLOR_BLACK);
	}

	ncurses_screen_width = stdscr->_maxx + 1;
	ncurses_screen_height = stdscr->_maxy + 1;

	ncurses_status = newwin(1, stdscr->_maxx + 1, stdscr->_maxy - 1, 0);
	input = newwin(1, stdscr->_maxx + 1, stdscr->_maxy, 0);
	keypad(input, TRUE);
	nodelay(input, TRUE);

	start_color();

	init_pair(7, COLOR_BLACK, background);	/* ma�e obej�cie domy�lnego koloru */
	init_pair(1, COLOR_RED, background);
	init_pair(2, COLOR_GREEN, background);
	init_pair(3, COLOR_YELLOW, background);
	init_pair(4, COLOR_BLUE, background);
	init_pair(5, COLOR_MAGENTA, background);
	init_pair(6, COLOR_CYAN, background);

#define __init_bg(x, y) \
	init_pair(x, COLOR_BLACK, y); \
	init_pair(x + 1, COLOR_RED, y); \
	init_pair(x + 2, COLOR_GREEN, y); \
	init_pair(x + 3, COLOR_YELLOW, y); \
	init_pair(x + 4, COLOR_BLUE, y); \
	init_pair(x + 5, COLOR_MAGENTA, y); \
	init_pair(x + 6, COLOR_CYAN, y); \
	init_pair(x + 7, COLOR_WHITE, y);

	__init_bg(8, COLOR_RED);
	__init_bg(16, COLOR_GREEN);
	__init_bg(24, COLOR_YELLOW);
	__init_bg(32, COLOR_BLUE);
	__init_bg(40, COLOR_MAGENTA);
	__init_bg(48, COLOR_CYAN);
	__init_bg(56, COLOR_WHITE);

#undef __init_bg

	ncurses_contacts_changed("contacts");
	ncurses_commit();

	/* deaktywujemy klawisze INTR, QUIT, SUSP i DSUSP */
	if (!tcgetattr(0, &old_tio)) {
		struct termios tio;

		memcpy(&tio, &old_tio, sizeof(tio));
		tio.c_cc[VINTR] = _POSIX_VDISABLE;
		tio.c_cc[VQUIT] = _POSIX_VDISABLE;
#ifdef VDSUSP
		tio.c_cc[VDSUSP] = _POSIX_VDISABLE;
#endif
#ifdef VSUSP
		tio.c_cc[VSUSP] = _POSIX_VDISABLE;
#endif

		tcsetattr(0, TCSADRAIN, &tio);
	}

#ifdef SIGWINCH
	signal(SIGWINCH, sigwinch_handler);
#endif

	memset(ncurses_history, 0, sizeof(ncurses_history));

	ncurses_binding_init();

#ifdef WITH_ASPELL
	if (config_aspell)
		ncurses_spellcheck_init();
#endif

	ncurses_line = xmalloc(LINE_MAXLEN * sizeof(CHAR_T));

	ncurses_history[0] = ncurses_line;
}

/*
 * ncurses_deinit()
 *
 * zamyka, robi porz�dki.
 */
void ncurses_deinit(void)
{
	static int done = 0;
	window_t *w;
	int i;

	signal(SIGINT, SIG_DFL);
#ifdef SIGWINCH
	signal(SIGWINCH, SIG_DFL);
#endif
	if (have_winch_pipe) {
		close(winch_pipe[0]);
		close(winch_pipe[1]);
	}

	for (w = windows; w; w = w->next)
		ncurses_window_kill(w);

	tcsetattr(0, TCSADRAIN, &old_tio);

	keypad(input, FALSE);

	werase(input);
	wnoutrefresh(input);
	doupdate();

	delwin(input);
	delwin(ncurses_status);
	if (ncurses_header)
		delwin(ncurses_header);
	endwin();

	for (i = 0; i < HISTORY_MAX; i++)
		if (ncurses_history[i] != ncurses_line) {
			xfree(ncurses_history[i]);
			ncurses_history[i] = NULL;
		}

	if (ncurses_lines) {
		for (i = 0; ncurses_lines[i]; i++) {
			if (ncurses_lines[i] != ncurses_line)
				xfree(ncurses_lines[i]);
			ncurses_lines[i] = NULL;
		}

		xfree(ncurses_lines);
		ncurses_lines = NULL;
	}

#ifdef WITH_ASPELL
	delete_aspell_speller(spell_checker);
#endif

	xfree(ncurses_line);
	xfree(ncurses_yanked);

	done = 1;
}

/*
 * ncurses_window_new()
 *
 * tworzy nowe okno ncurses do istniej�cego okna ekg.
 */
int ncurses_window_new(window_t *w)
{
	ncurses_window_t *n;

	if (w->priv_data)
		return 0;

	w->priv_data = n = xmalloc(sizeof(ncurses_window_t));

	if (w->id == WINDOW_CONTACTS_ID) {
		ncurses_contacts_set(w);

	} else if (w->id == WINDOW_LASTLOG_ID) {
		ncurses_lastlog_new(w);

	} else if (w->target || w->alias) {
		const char *f = format_find("ncurses_prompt_query");

		n->prompt = format_string(f, w->alias ? w->alias : w->target);
		n->prompt_len = xstrlen(n->prompt);

		ncurses_update_real_prompt(n);
	} else {
		const char *f = format_find("ncurses_prompt_none");

		if (format_ok(f)) {
			n->prompt = format_string(f);
			n->prompt_len = xstrlen(n->prompt);

			ncurses_update_real_prompt(n);
		}
	}

	n->window = newwin(w->height, w->width, w->top, w->left);

	if (config_mark_on_window_change && !w->floating)
		command_exec_format(NULL, NULL, 0, "/mark %d", w->id);

	ncurses_resize();

	return 0;
}

/*
 * Local Variables:
 * mode: c
 * c-file-style: "k&r"
 * c-basic-offset: 8
 * indent-tabs-mode: t
 * End:
 */
