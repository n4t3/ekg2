#ifndef __EKG_RECODE_H
#define __EKG_RECODE_H

#include "dynstuff.h"

#ifdef __cplusplus
extern "C" {
#endif

void *ekg_convert_string_init(const char *from, const char *to, void **rev);
void ekg_convert_string_destroy(void *ptr);
char *ekg_convert_string_p(const char *ps, void *ptr);
char *ekg_convert_string(const char *ps, const char *from, const char *to);
string_t ekg_convert_string_t_p(string_t s, void *ptr);
string_t ekg_convert_string_t(string_t s, const char *from, const char *to);

void changed_console_charset(const char *name);
int ekg_converters_display(int quiet);

#define recode_xfree(org, ret) do { if (org != ret) xfree((char *) ret); } while(0); 

#define ekg_iso2_to_locale ekg_iso2_to_utf8
#define ekg_locale_to_iso2_use ekg_utf8_to_iso2_const
#define ekg_locale_to_cp_use ekg_utf8_to_cp_const

#define ekg_cp_to_locale ekg_cp_to_utf8
#define ekg_cp_to_locale_dup ekg_cp_to_utf8_dup
#define ekg_locale_to_cp_dup ekg_utf8_to_cp_dup
#define ekg_locale_to_cp ekg_utf8_to_cp

const char *ekg_utf8_to_iso2_const(const char *buf);
char *ekg_iso2_to_utf8(char *buf);

char *ekg_cp_to_utf8(char *buf);
char *ekg_cp_to_utf8_dup(const char *buf);
char *ekg_utf8_to_cp(char *buf);
char *ekg_utf8_to_cp_dup(const char *buf);
const char *ekg_utf8_to_cp_const(const char *buf);

#ifdef __cplusplus
}
#endif

#endif
