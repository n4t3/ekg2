/*
 *  (C) Copyright 2009-2010 Jakub Zawadzki <darkjames@darkjames.ath.cx>
 *			Wies�aw Ochmi�ski <wiechu@wiechu.com>
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


/* NOTES/THINK/BUGS:
 * 	- do we need any #define? 
 * 	- if we stop using ekg_convert_string_init() in plugins this file could be smaller.
 * 	- don't use gg_*() funcs, always use iconv? lite iconv in compat/ ?
 * 	- create:
 * 		static struct ekg_converter same_enc;
 * 		
 * 		we should know if iconv_open() failed, or we have good console_charset..
 * 		give info to user, if this first happen.
 *
 * 	- we should also reinit encodings, if user changed console_charset.
 * 	- implement ekg_any_to_core(), ekg_locale_to_any()
 *
 * 	- Check if this code works OK.
 */

#include "ekg2-config.h"

#include <errno.h>
#include <string.h>

#include "commands.h"
#include "recode.h"
#include "stuff.h"
#include "windows.h"
#include "xmalloc.h"

struct ekg_encoding_pair {
	gchar *from;
	gchar *to;
};

/**
 * ekg_convert_string_init()
 *
 * Initialize string conversion thing for two given charsets.
 *
 * @param from		- input encoding (will be duped; if NULL, console_charset will be assumed).
 * @param to		- output encoding (will be duped; if NULL, console_charset will be assumed).
 * @param rev		- pointer to assign reverse conversion into; if NULL, no reverse converter will be initialized.
 * 
 * @return	Pointer that should be passed to other ekg_convert_string_*(), even if it's NULL.
 *
 * @sa ekg_convert_string_destroy()	- deinits charset conversion.
 * @sa ekg_convert_string_p()		- main charset conversion function.
 */
void *ekg_convert_string_init(const char *from, const char *to, void **rev) {
	struct ekg_encoding_pair *enc;

	if (rev) {
		enc = g_new(struct ekg_encoding_pair, 1);
		enc->from = g_strdup(to);
		enc->to = g_strdup(from);
		*rev = enc;
	}

	enc = g_new(struct ekg_encoding_pair, 1);
	enc->from = g_strdup(from);
	enc->to = g_strdup(to);
	return enc;
}

/**
 * ekg_convert_string_destroy()
 *
 * Frees internal data associated with given pointer, and uninitalizes iconv, if it's not needed anymore.
 *
 * @note If 'rev' param was used with ekg_convert_string_init(), this functions must be called two times
 *	- with returned value, and with rev-associated one.
 *
 * @param ptr		- pointer returned by ekg_convert_string_init().
 *
 * @sa ekg_convert_string_init()	- init charset conversion.
 * @sa ekg_convert_string_p()		- main charset conversion function.
 */

void ekg_convert_string_destroy(void *ptr) {
	struct ekg_encoding_pair *e = ptr;
	g_free(e->from);
	g_free(e->to);
	g_free(ptr);
}

/**
 * ekg_convert_string_p()
 *
 * Converts string to specified encoding, using pointer returned by ekg_convert_string_init().
 * Invalid characters in input will be replaced with question marks.
 *
 * @param ps		- string to be converted (won't be freed).
 * @param ptr		- pointer returned by ekg_convert_string_init().
 *
 * @return	Pointer to allocated result or NULL, if some failure has occured or no conversion
 *			is needed (i.e. resulting string would be same as input).
 *
 * @sa ekg_convert_string_init()	- init charset conversion.
 */

char *ekg_convert_string_p(const char *ps, void *ptr) {
	struct ekg_encoding_pair *e = ptr;
	return ekg_convert_string(ps, e->from, e->to);
}

/**
 * ekg_convert_string()
 *
 * Converts string to specified encoding, replacing invalid chars with question marks.
 *
 * @note Deprecated, in favour of ekg_convert_string_p(). Should be used only on single
 *	conversions, where charset pair won't be used again.
 *
 * @param ps		- string to be converted (it won't be freed).
 * @param from		- input encoding (if NULL, console_charset will be assumed).
 * @param to		- output encoding (if NULL, console_charset will be assumed).
 *
 * @return	Pointer to allocated result on success, NULL on failure
 *			or when both encodings are equal.
 *
 * @sa ekg_convert_string_p()	- more optimized version.
 */
char *ekg_convert_string(const char *ps, const char *from, const char *to) {
	char *res;
	gsize written;

	if (!from)
		from = "utf-8";
	if (!to)
		to = "utf-8";

	res = g_convert_with_fallback(ps, -1, to, from, NULL, NULL, &written, NULL);

	return res ? res : g_strdup(ps);
}

string_t ekg_convert_string_t_p(string_t s, void *ptr) {
	struct ekg_encoding_pair *e = ptr;
	return ekg_convert_string_t(s, e->from, e->to);
}

string_t ekg_convert_string_t(string_t s, const char *from, const char *to) {
	char *res;
	string_t ret;
	gsize written;

	if (!from)
		from = "utf-8";
	if (!to)
		to = "utf-8";

	res = g_convert_with_fallback(s->str, s->len, to, from, NULL, NULL, &written, NULL);
	ret = string_init(NULL);

	if (!res)
		string_append_raw(ret, g_memdup(s->str, s->len), s->len);
	else
		string_append_raw(ret, res, written);

	return ret;
}

void changed_console_charset(const char *name) {
	if (1) {
		/* XXX: replace with some recoding test? */
	} else if (!in_autoexec && xstrcasecmp(console_charset, config_console_charset)) 
		print("console_charset_bad", console_charset, config_console_charset);
}

void ekg_recode_inc_ref(const gchar *enc) {
}

void ekg_recode_dec_ref(const gchar *enc) {
}

char *ekg_recode_from_core(const gchar *enc, char *buf) {
	gchar *res = ekg_recode_from_core_use(enc, buf);
	if (res != buf)
		g_free(buf);
	return res;
}

char *ekg_recode_to_core(const gchar *enc, char *buf) {
	gchar *res = ekg_recode_to_core_use(enc, buf);
	if (res != buf)
		g_free(buf);
	return res;
}

char *ekg_recode_from_core_dup(const gchar *enc, const char *buf) {
	gchar *res = ekg_recode_from_core_use(enc, buf);
	return res == buf ? g_strdup(res) : res;
}

char *ekg_recode_to_core_dup(const gchar *enc, const char *buf) {
	gchar *res = ekg_recode_to_core_use(enc, buf);
	return res == buf ? g_strdup(res) : res;
}

const char *ekg_recode_from_core_use(const gchar *enc, const char *buf) {
	gsize written;
	gchar *res;

	if (!buf)
		return NULL;

	res = g_convert_with_fallback(buf, -1, enc, "utf-8",
			NULL, NULL, &written, NULL);
	return res ? res : g_strdup(buf);
}

const char *ekg_recode_to_core_use(const gchar *enc, const char *buf) {
	gsize written;
	gchar *res;

	if (!buf)
		return NULL;

	res = g_convert_with_fallback(buf, -1, "utf-8", enc,
			NULL, NULL, &written, NULL);
	return res ? res : buf;
}
