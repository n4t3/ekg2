/* $Id$ */

/*
 *  (C) Copyright 2003 Wojtek Kaniewski <wojtekka@irc.pl>
 *		  2004 Piotr Kupisiewicz (deli@rzepaknet.us>
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
#include "win32.h"

#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

#ifndef NO_POSIX_SYSTEM
#  include <dlfcn.h>
#else 
#  include <winbase.h>
#endif

#include "configfile.h"
#include "commands.h"
#include "debug.h"
#include "dynstuff.h"
#include "events.h"
#include "objects.h"
#include "plugins.h"
#include "userlist.h"
#include "stuff.h"
#include "vars.h"
#include "themes.h"
#include "xmalloc.h"
#include "dynstuff_inline.h"

#define __DECLARE_QUERIES_STUFF
#include "queries.h"

#if !defined(va_copy) && defined(__va_copy)
#define va_copy(DST,SRC) __va_copy(DST,SRC)
#endif

plugin_t *plugins = NULL;
static LIST_ADD_COMPARE(plugin_register_compare, plugin_t *) { return data2->prio - data1->prio; }

DYNSTUFF_LIST_DECLARE_SORTED_NF(plugins, plugin_t, plugin_register_compare,
	static __DYNSTUFF_LIST_ADD_SORTED,		/* plugins_add() */
	__DYNSTUFF_LIST_UNLINK)				/* plugins_unlink() */

list_t watches = NULL;

query_t* queries[QUERIES_BUCKETS];

query_def_t* registered_queries;
int registered_queries_count = 0;

LIST_FREE_ITEM(query_free_data, query_t *) {
	xfree(data->name);
}

DYNSTUFF_LIST_DECLARE(queries_list, query_t, query_free_data,
	static __DYNSTUFF_ADD,
	static __DYNSTUFF_REMOVE_SAFE,
	__DYNSTUFF_DESTROY)


#ifdef EKG2_WIN32_HELPERS
# define WIN32_REQUEST_HELPER
# include "win32_helper.h"
#endif

int ekg2_dlinit() {
#ifdef EKG2_WIN32_HELPERS
	INIT_HELPER_FUNC(&win32_helper);

	int i;
	for (i = 0; i < (sizeof(win32_helper) / sizeof(void *)); i++) {
		void **cur = & ((void **) &win32_helper)[i];
		if (!*cur) {
			*cur = (void *) &win32_stub_function;
			printf("Making evil thing on element: %d\n", i);
		}
	}
#endif

	return 0;
/*	return lt_dlinit() */
}

/**
 * ekg2_dlclose()
 *
 * Close handler to dynamic loaded library.<br>
 * Support POSIX dlclose() and FreeLibrary() [WINDOWS]
 *
 * @todo For support of more dynamic interfaces see sources of lt_dlclose() [libltdl]
 *
 * @param plugin - Handler to loaded library.
 *
 * @return	0 on success, else fail.
 */

/* it only support posix dlclose() but maybe in future... */
int ekg2_dlclose(void *plugin) {
#ifndef NO_POSIX_SYSTEM
	return dlclose(plugin);
#else
	return (FreeLibrary(plugin) == 0);	/* FreeLibrary() return 0 on fail. */
#endif
}

/**
 * ekg2_dlopen()
 *
 * Load dynamic library file from @a name<br>
 * Support POSIX dlopen() and LoadLibraryA() [WINDOWS]
 *
 * @todo For support of more dynamic interfaces see sources of lt_dlopen() [libltdl]
 *
 * @todo Think more about flags for dlopen() [was: RTLD_LAZY | RTLD_GLOBAL]
 *
 * @param name - Full path of library to load.
 *
 * @return Pointer to the loaded library, or NULL if fail.
 */

static void *ekg2_dlopen(const char *name) {
#ifdef NO_POSIX_SYSTEM
	void *tmp = LoadLibraryA(name);
#else
	/* RTLD_LAZY is bad flag, because code can SEGV on executing undefined symbols...
	 *	it's better to fail earlier than later with SIGSEGV
	 *
	 * RTLD_GLOBAL is bad flag also, because we have no need to export symbols to another plugns
	 *	we should do it by queries... Yeah, I know it was used for example in perl && irc plugin.
	 *	But we cannot do it. Because if we load irc before sim plugin. Than we'll have unresolved symbols
	 *	even if we load sim plugin later.
	 */
	/*
	 * RTLD_GLOBAL is required by perl and python plugins...
	 *	need investigation. [XXX]
	 */
	void *tmp = dlopen(name, RTLD_NOW | RTLD_GLOBAL);
#endif
	if (!tmp) {
		debug_warn("[plugin] could not be loaded: %s %s\n", name, dlerror());
	} else {
		debug_ok("[plugin] loaded: %s\n", name);
	}
	return tmp;
}

/**
 * ekg2_dlsym()
 *
 * Get symbol with @a name from loaded dynamic library.<br>
 * Support POSIX dlsym() and GetProcAddress() [WINDOWS]
 *
 * @todo For support of more dynamic interfaces see lt_dlsym() [libltdl]
 *
 * @param plugin	- Pointer to the loaded library.
 * @param name		- Name of symbol to lookup.
 *
 * @return Address of symbol or NULL if error occur.
 */

static void *ekg2_dlsym(void *plugin, char *name) {
#ifndef NO_POSIX_SYSTEM
	void *tmp;
	const char *error;

	dlerror();			/* Clear any existing error */
	tmp = dlsym(plugin, name);	/* Look for symbol */

	/* Be POSIX like, if dlerror() returns smth, even if dlsym() successful return pointer. Then report error.
	 * man 3 dlsym */
	if ((error = dlerror())) {
		debug_error("[plugin] plugin: %x symbol: %s error: %s\n", plugin, name, error);
		return NULL;
	}

	return tmp;
#else
	return GetProcAddress(plugin, name);
#endif
}

/*
 * plugin_load()
 *
 * �aduje wtyczk� o podanej nazwie.
 * 
 * 0/-1
 */
int plugin_load(const char *name, int prio, int quiet)
{
#ifdef SHARED_LIBS
	char lib[PATH_MAX];
	char *env_ekg_plugins_path = NULL;
	char *init = NULL;
#endif

	plugin_t *pl;
	void *plugin = NULL;
	int (*plugin_init)() = NULL;

	if (!name)
		return -1;

	if (plugin_find(name)) {
		printq("plugin_already_loaded", name); 
		return -1;
	}
#ifdef SHARED_LIBS
#ifndef NO_POSIX_SYSTEM
#ifdef SCONS
#	define DOTLIBS "" 
#else
#	define DOTLIBS ".libs/"
#endif
	if ((env_ekg_plugins_path = getenv("EKG_PLUGINS_PATH"))) {
		if (snprintf(lib, sizeof(lib), "%s/%s.so", env_ekg_plugins_path, name) < sizeof(lib))
			plugin = ekg2_dlopen(lib);
		if (!plugin && (snprintf(lib, sizeof(lib), "%s/%s/" DOTLIBS "%s.so", env_ekg_plugins_path, name, name) < sizeof(lib)))
				plugin = ekg2_dlopen(lib);
	}

#ifndef SKIP_RELATIVE_PLUGINS_DIR
	/* The following lets ekg2 load plugins when it is run directly from
	 * the source tree, without installation. This can be beneficial when
	 * developing the program, or for less knowlegeable users, who don't
	 * know how to or cannot for some other reason use installation prefix
	 * to install in their home directory. However this impses a security
	 * risk if the program installed in the system directory is run in
	 * untrusted $CWD or when $CWD/../plugins is untrusted.
	 *
	 * TODO(porridge,darkjames): This can be fixed by having a wrapper
	 * script in the source tree to run ekg/.libs/ekg2 with
	 * EKG_PLUGINS_PATH set appropriately.
	 */
	if (!plugin) {
		if (snprintf(lib, sizeof(lib), "plugins/%s/" DOTLIBS "%s.so", name, name) < sizeof(lib))
			plugin = ekg2_dlopen(lib);
	}

	if (!plugin) {
		if (snprintf(lib, sizeof(lib), "../plugins/%s/" DOTLIBS "%s.so", name, name) < sizeof(lib))
			plugin = ekg2_dlopen(lib);
	}
#endif

	if (!plugin) {
		if (snprintf(lib, sizeof(lib), "%s/%s.so", PLUGINDIR, name) < sizeof(lib))
			plugin = ekg2_dlopen(lib);
	}
#else	/* NO_POSIX_SYSTEM */
	if (!plugin) {
		if (snprintf(lib, sizeof(lib), "c:\\ekg2\\plugins\\%s.dll", name) < sizeof(lib))
			plugin = ekg2_dlopen(lib);
	}
#endif /* SHARED_LIBS */
	if (!plugin) {
		printq("plugin_doesnt_exist", name);
		return -1;
	}
#endif

#ifdef STATIC_LIBS
#ifndef SCONS
/* first let's try to load static plugin... */
	extern int jabber_plugin_init(int prio);
	extern int irc_plugin_init(int prio);
	extern int gtk_plugin_init(int prio);

	debug("searching for name: %s in STATICLIBS: %s\n", name, STATIC_LIBS);

	if (!xstrcmp(name, "jabber")) plugin_init = &jabber_plugin_init;
	if (!xstrcmp(name, "irc")) plugin_init = &irc_plugin_init;
	if (!xstrcmp(name, "gtk")) plugin_init = &gtk_plugin_init;
//	if (!xstrcmp(name, "miranda")) plugin_init = &miranda_plugin_init;
#else
	debug_function("plugin_load(), trying to find static plugin '%s'\n", name);
	void *plugin_load_static(const char *name); /* autogenerated by scons */
	plugin_init = plugin_load_static(name);
#endif
#endif

#ifdef SHARED_LIBS
	if (!plugin_init) {
# ifdef EKG2_WIN32_HELPERS
		void (*plugin_preinit)(void *);
		char *preinit = saprintf("win32_plugin_init");
		if (!(plugin_preinit = ekg2_dlsym(plugin, preinit))) {
			debug("NO_POSIX_SYSTEM, PLUGIN:%s NOT COMPILATED WITH EKG2_WIN32_SHARED_LIB?!\n", name);
			printq("plugin_incorrect", name);
			xfree(preinit);
			return -1;
		}
		xfree(preinit);
		plugin_preinit(&win32_helper);
# endif
/* than if we don't have static plugin... let's try to load it dynamicly */
		init = saprintf("%s_plugin_init", name);

		if (!(plugin_init = ekg2_dlsym(plugin, init))) {
			printq("plugin_incorrect", name);
			ekg2_dlclose(plugin);
			xfree(init);
			return -1;
		}
		xfree(init);
	}
#endif
	if (!plugin_init) {
		printq("plugin_doesnt_exist", name);
		return -1;
	}

	if (plugin_init(prio) == -1) {
		printq("plugin_not_initialized", name);
		ekg2_dlclose(plugin);
		return -1;
	}

	if ((pl = plugin_find(name))) {
		pl->dl = plugin;
	} else {
		debug_error("plugin_load() plugin_find(%s) not found.\n", name);
		/* It's FATAL */
	}

	query_emit(pl, "set-vars-default");

	printq("plugin_loaded", name);

	if (!in_autoexec) {
		const char *tmp;

		in_autoexec = 1;
		if ((tmp = prepare_pathf("config-%s", name)))
			config_read(tmp);
		if ((pl->pclass == PLUGIN_PROTOCOL) && (tmp = prepare_pathf("sessions-%s", name)))
			session_read(tmp);

		if (pl)
			query_emit(pl, "config-postinit");

		in_autoexec = 0;
		config_changed = 1;
	}
	return 0;
}

/**
 * plugin_find()
 *
 * Find plugin by name
 *
 * @param name - name of plugin_t
 *
 * @return plugin_t with given name, or NULL if not found.
 */

plugin_t *plugin_find(const char *name)
{
	plugin_t *p;

	for (p = plugins; p; p = p->next) {
		if (!xstrcmp(p->name, name))
			return p;
	}

	return NULL;
}

/**
 * plugin_find_uid()
 *
 * Find <i>PLUGIN_PROTOCOL</i> plugin which can handle @a uid
 * 
 * @todo used only by session_add() in session.c move it there?
 *
 * @sa valid_plugin_uid() - For function to check if given plugin can handle given uid
 *
 * @return If such plugin was founded return it, or NULL if not found.
 */

plugin_t *plugin_find_uid(const char *uid) {
	plugin_t *p;

	for (p = plugins; p; p = p->next) {
		if (p && p->pclass == PLUGIN_PROTOCOL && p->name && valid_plugin_uid(p, uid))
			return p;
	}

	return NULL;
}

/*
 * plugin_unload()
 *
 * usuwa z pami�ci dan� wtyczk�, lub je�li wtyczka jest wkompilowana na
 * sta�e, deaktywuje j�.
 *
 * 0/-1
 */
int plugin_unload(plugin_t *p)
{
	char *name; 
	list_t l;

	if (!p)
		return -1;

	if (config_expert_mode == 0 && p->pclass == PLUGIN_UI) {
		plugin_t *plug;

		int unloadable = 0;
		for (plug = plugins; plug; plug = plug->next) {
			if (plug->pclass == PLUGIN_UI && plug != p) 
				unloadable = 1;
		}
		if (!unloadable) {
			print("plugin_unload_ui", p->name);
			return -1;
		}
	}

	for (l = watches; l; l = l->next) {
		watch_t *w = l->data;

		if (w && w->plugin == p && (w->removed == 1 || w->removed == -1)) {
			print("generic_error", "XXX cannot remove this plugin when there some watches active");
			return -1;
		}
	}
	/* XXX, to samo dla timerow */

	name = xstrdup(p->name);

	if (p->destroy)
		p->destroy();

	if (p->dl)
		ekg2_dlclose(p->dl);

	print("plugin_unloaded", name);

	xfree(name);

	if (!in_autoexec)
		config_changed = 1;

	return 0;
}

/*
 * plugin_register()
 *
 * rejestruje dan� wtyczk�.
 *
 * 0/-1
 */
int plugin_register(plugin_t *p, int prio) {
	if (prio == -254) {
		switch (p->pclass) {
			case PLUGIN_UI:
				p->prio = 0;
				break;
			case PLUGIN_LOG:
				p->prio = 5;
				break;
			case PLUGIN_SCRIPTING:
				p->prio = 10;
				break;
			case PLUGIN_PROTOCOL:
				p->prio = 15;
				break;
			default:
				p->prio = 20;
				break;
		}
	} else {
		p->prio = prio;
	}

	plugins_add(p);

	return 0;
}

/*
 * plugin_unregister()
 *
 * od��cza wtyczk�.
 *
 * 0/-1
 */
int plugin_unregister(plugin_t *p)
{
	/* XXX eXtreme HACK warning
	 * (mp) na razie jest tak.  docelowo: wyladowywac pluginy tylko z
	 * glownego programu (queriesami?)
	 * to cos segfaultowalo (wczesniej czy pozniej), jesli bylo wywolane z
	 * ncurses.  niestety, problem pozostaje dla innych pluginow i takiego
	 * np. rc. sie zrobi nast razem */

	/* j/w If any plugin has backtrace here, and we try to remove it from memory.
	 * ekg2 do SEGV.
	 */

	struct timer *t;
	session_t *s;
	query_t **kk;
	variable_t *v;
	command_t *c;
	list_t l;

	if (!p)
		return -1;

/* XXX think about sequence of unloading....: currently: watches, timers, sessions, queries, variables, commands */

	for (l = watches; l; l = l->next) {
		watch_t *w = l->data;

		if (w && w->plugin == p)
			watch_free(w);
	}

	for (t = timers; t; t = t->next) {
		if (t->plugin == p)
			t = timers_removei(t);
	}

	for (s = sessions; s; ) {
		session_t *next = s->next;

		if (s->plugin == p)
			session_remove(s->uid);
		s = next;
	}

	for (kk = queries; kk < &queries[QUERIES_BUCKETS]; ++kk) {
		query_t *g;

		for (g = *kk; g; ) {
			query_t *next = g->next;
			if (g->plugin == p)
				queries_list_remove(kk, g);
			g = next;
		}
	}

	for (v = variables; v; v = v->next) {
		if (v->plugin == p) 
			v = variables_removei(v);
	}

	for (c = commands; c; c = c->next) {
		if (c->plugin == p)
			c = commands_removei(c);
	}

	plugins_unlink(p);

	return 0;
}

/**
 * plugin_var_find()
 *
 * it looks for given variable name in given plugin
 *
 * @param	pl - plugin
 * @param	name - variable name
 *
 * returns sequence number+1 of variable if found, else 0
 */

int plugin_var_find(plugin_t *pl, const char *name) {
	int i;

	if (!pl || !pl->params)
		return 0;

	for (i = 0; (pl->params[i].key /* && pl->params[i].id != -1 */); i++) {
		if (!xstrcasecmp(pl->params[i].key, name))
			return i+1;
	}
	return 0;
}

int plugin_var_add(plugin_t *pl, const char *name, int type, const char *value, int secret, plugin_notify_func_t *notify) { return -1; }


static LIST_FREE_ITEM(registered_query_free_data, query_def_t *) {
	xfree(data->name);
}

void registered_queries_free() {
	if (!registered_queries)
	    return;

	LIST_DESTROY2(registered_queries, registered_query_free_data);

	/* this has been already done in call above */
	registered_queries = NULL;
}

static int query_register_common(const char* name, query_def_t **res) {
	query_def_t *gd;
	int found = 0, name_hash = ekg_hash(name);

	for (gd = registered_queries; gd; gd = gd->next) {
	    if (name_hash == gd->name_hash && !xstrcmp(gd->name, name)) {
			found = 1;
			break;
		}
	}
	if (found) {
		debug_error("query_register() oh noez, seems like it's already registered: [%s]\n", name);
		debug_error("I'm not sure what I should do, so I'm simply bailing out...\n");
		return -1;

	} else {
		gd            = xmalloc(sizeof(query_def_t));
		gd->name      = xstrdup(name);
		gd->name_hash = name_hash;
		registered_queries_count++;

		LIST_ADD2(&registered_queries, gd);
	}

	*res = gd;

	return 0;
}

int query_register(const char *name, ...) {
	query_def_t *gd;
	int i, arg;
	va_list va;

	if (query_register_common(name, &gd)) {
	    return -1;
	}

	va_start(va, name);
	for (i = 0; i < QUERY_ARGS_MAX; i++) {
		arg = va_arg(va, int);
		gd->params[i] = arg;
		if (arg == QUERY_ARG_END)
			break;
	}
	va_end(va);
	return 0;
}

/*
 * alternative way for registering queries
 */
int query_register_const(const query_def_t *def) {
        query_def_t *gd;

	if (query_register_common(def->name, &gd)) {
	    return -1;
	}
	memcpy(gd->params, def->params, sizeof(def->params));

	return 0;
}


int query_free(query_t* g) {

    queries_list_remove(&queries[g->name_hash & (QUERIES_BUCKETS - 1)], g);

    return 0;
}

query_t *query_connect(plugin_t *plugin, const char *name, query_handler_func_t *handler, void *data) {
	int found = 0;
	query_def_t* gd;

	query_t *q = xmalloc(sizeof(query_t));

	q->name         = xstrdup(name);
	q->name_hash    = ekg_hash(name);
	q->plugin	= plugin;
	q->handler	= handler;
	q->data		= data;

	for (gd = registered_queries; gd; gd = gd->next) {
		if (q->name_hash == gd->name_hash && !xstrcmp(gd->name, name)) {
			found = 1;
			break;
		}
	}

	if (!found) {
		debug_error("query_connect() NOT FOUND[%d]: %s\n", registered_queries_count, __(name));

		gd            = xmalloc(sizeof(query_def_t));
		gd->name      = xstrdup(name);
		gd->name_hash = q->name_hash;
		registered_queries_count++;

		LIST_ADD2(&registered_queries, gd);
	}

	queries_list_add(&queries[q->name_hash & (QUERIES_BUCKETS - 1)], q);

	return q;
}

static int query_emit_inner(query_t *g, va_list ap) {
	static int nested = 0;
	int (*handler)(void *data, va_list ap) = g->handler;
	int result;
	va_list ap_plugin;

	if (nested >= 32) {
		return -1;
	}

	g->count++;
	/*
	 * pc and amd64: va_arg remove var from va_list when you use va_arg, 
	 * so we must keep orig va_list for next plugins
	 */
	nested++;;
	va_copy(ap_plugin, ap);
	result = handler(g->data, ap_plugin);
	va_end(ap_plugin);
	nested--;

	return result != -1 ? 0 : -1;
}

int query_emit(plugin_t *plugin, const char* name, ...) {
	int result = -2;
	va_list ap;
	query_t* g;
	int name_hash, bucket_id;

	name_hash = ekg_hash(name);
	bucket_id = name_hash & (QUERIES_BUCKETS - 1);

	va_start(ap, name);

	for (g = queries[bucket_id]; g; g = g->next) {
	    if (name_hash == g->name_hash && (!plugin || (plugin == g->plugin)) && !xstrcmp(name, g->name)) {

		result = query_emit_inner(g, ap);

		if (result == -1) {
		    break;
		}
	    }
	}

	va_end(ap);

	return result;
}

static LIST_ADD_COMPARE(query_compare, query_t *) {
	/*				any other suggestions: vvv ? */
	const int ap = (data1->plugin ? data1->plugin->prio : -666);
	const int bp = (data2->plugin ? data2->plugin->prio : -666);

	return (bp-ap);
}

/**
 * queries_reconnect()
 *
 * Reconnect (resort) all queries, e.g. after plugin prio change.
 */

void queries_reconnect() {
	size_t i;
	for (i = 0; i < QUERIES_BUCKETS; ++i) {
		LIST_RESORT2(&(queries[i]), query_compare);
	}
}

/*
 * watch_find()
 *
 * zwraca obiekt watch_t o podanych parametrach.
 */
watch_t *watch_find(plugin_t *plugin, int fd, watch_type_t type) {
	list_t l;
	
	for (l = watches; l; l = l->next) {
		watch_t *w = l->data;

			/* XXX: added simple plugin ignoring, make something nicer? */
		if (w && ((plugin == (void*) -1) || w->plugin == plugin) && w->fd == fd && (w->type & type) && !(w->removed > 0))
			return w;
	}

	return NULL;
}

static LIST_FREE_ITEM(watch_free_data, watch_t *) {
	data->removed = 2;	/* to avoid situation: when handler of watch, execute watch_free() on this watch... stupid */

	if (data->buf) {
		int (*handler)(int, int, const char *, void *) = data->handler;
		string_free(data->buf, 1);
		/* DO WE WANT TO SEND ALL  IN BUFOR TO FD ? IF IT'S WATCH_WRITE_LINE? or parse all data if it's WATCH_READ_LINE? mmh. XXX */
		if (handler)
			handler(1, data->fd, NULL, data->data);
	} else {
		int (*handler)(int, int, int, void *) = data->handler;
		if (handler)
			handler(1, data->fd, data->type, data->data);
	}
}

/*
 * watch_free()
 *
 * zwalnia pami�� po obiekcie watch_t.
 * zwraca wska�nik do nast�pnego obiektu do iterowania
 * albo NULL, jak nie mo�na skasowa�.
 */
void watch_free(watch_t *w) {
	if (!w)
		return;

	if (w->removed == 2)
		return;

	if (w->removed == -1 || w->removed == 1) { /* watch is running.. we cannot remove it */
		w->removed = 1;
		return;
	}

	if (w->type == WATCH_WRITE && w->buf && !w->handler && w->plugin) {	/* XXX */
		debug_error("[INTERNAL_DEBUG] WATCH_LINE_WRITE must be removed by plugin, manually (settype to WATCH_NONE and than call watch_free()\n");
		return;
	}

	watch_free_data(w);
	list_remove_safe(&watches, w, 1);

	ekg_watches_removed++;
	debug("watch_free() REMOVED WATCH, watches removed this loop: %d oldwatch: 0x%x\n", ekg_watches_removed, w);
}

/*
 * watch_handle_line()
 *
 * obs�uga deskryptor�w przegl�danych WATCH_READ_LINE.
 */
void watch_handle_line(watch_t *w)
{
	char buf[1024], *tmp;
	int ret, res = 0;
	int (*handler)(int, int, const char *, void *) = w->handler;

	if (!w || w->removed == -1)
		return;	/* watch is running in another thread / context */

	w->removed = -1;
#ifndef NO_POSIX_SYSTEM
	ret = read(w->fd, buf, sizeof(buf) - 1);
#else
	ret = recv(w->fd, buf, sizeof(buf) - 1, 0);
	if (ret == -1 && WSAGetLastError() == WSAENOTSOCK) {
		printf("recv() failed Error: %d, using ReadFile()", WSAGetLastError());
		res = ReadFile(w->fd, &buf, sizeof(buf)-1, &ret, NULL);
		printf(" res=%d ret=%d\n", res, ret);
	}
	res = 0;
#endif

	if (ret > 0) {
		buf[ret] = 0;
		string_append(w->buf, buf);
#ifdef NO_POSIX_SYSTEM
		printf("RECV: %s\n", buf);
#endif
	}

	if (ret == 0 || (ret == -1 && errno != EAGAIN))
		string_append_c(w->buf, '\n');

	while ((tmp = xstrchr(w->buf->str, '\n'))) {
		size_t strlen = tmp - w->buf->str;		/* get len of str from begining to \n char */
		char *line = xstrndup(w->buf->str, strlen);	/* strndup() str with len == strlen */

		/* we strndup() str with len == strlen, so we don't need to call xstrlen() */
		if (strlen > 1 && line[strlen - 1] == '\r')
			line[strlen - 1] = 0;

		if ((res = handler(0, w->fd, line, w->data)) == -1) {
			xfree(line);
			break;
		}

		string_remove(w->buf, strlen + 1);

		xfree(line);
	}

	/* je�li koniec strumienia, lub nie jest to ci�g�e przegl�danie,
	 * zwolnij pami�� i usu� z listy */
	if (res == -1 || ret == 0 || (ret == -1 && errno != EAGAIN) || w->removed == 1) {
		int fd = w->fd;
		w->removed = 0;

		watch_free(w);
		close(fd);
		return;
	} 
	w->removed = 0;
}

/* ripped from irc plugin */
int watch_handle_write(watch_t *w) {
	int (*handler)(int, int, const char *, void *) = w->handler;
	int res = -1;
	int len = (w && w->buf) ? w->buf->len : 0;

	if (!w || w->removed == -1) return -1;	/* watch is running in another thread / context */
	if (w->transfer_limit == -1) return 0;	/* transfer limit turned on, don't send anythink... XXX */
	debug_io("[watch_handle_write] fd: %d in queue: %d bytes.... ", w->fd, len);
	if (!len) return -1;

	w->removed = -1;

	if (handler) {
		res = handler(0, w->fd, w->buf->str, w->data);
	} else {
#ifdef NO_POSIX_SYSTEM
		res = send(w->fd, w->buf->str, len, 0 /* MSG_NOSIGNAL */);
#else
		res = write(w->fd, w->buf->str, len);
#endif
	}

	debug_io(" ... wrote:%d bytes (handler: 0x%x) ", res, handler);

	if (res == -1 &&
#ifdef NO_POSIX_SYSTEM
			(WSAGetLastError() != 666)
#else
			1
#endif
		) {
#ifdef NO_POSIX_SYSTEM
		debug("WSAError: %d\n", WSAGetLastError());
#else
		debug("Error: %s %d\n", strerror(errno), errno);
		w->removed = 0;
		watch_free(w);
#endif
		return -1;
	}
	
	if (res > len) {
		/* use debug_fatal() */
		/* debug_fatal() should do:
		 *	- print this info to all open windows with RED color
		 *	- change some variable 'ekg2_need_restart' to 1.
		 *	- @ ncurses if we have ekg2_need_restart set, and if colors turned on, change from blue to red..
		 *	- and do other happy stuff.
		 *
		 * XXX, implement and use it. It should be used as ASSERT()
		 */
		
		debug_error("watch_write(): handler returned bad value, 0x%x vs 0x%x\n", res, len);
		res = len;
	} else if (res < 0) {
		debug_error("watch_write(): handler returned negative value other than -1.. XXX\n");
		res = 0;
	}

	string_remove(w->buf, res);
	debug_io("left: %d bytes\n", w->buf->len);

	w->removed = 0;
	return res;
}

int watch_write_data(watch_t *w, const char *buf, int len) {		/* XXX, refactory: watch_write() */
	int was_empty;

	if (!w || !buf || len <= 0)
		return -1;

	was_empty = !w->buf->len;
	string_append_raw(w->buf, buf, len);

	if (was_empty) 
		return watch_handle_write(w); /* let's try to write somethink ? */
	return 0;
}

int watch_write(watch_t *w, const char *format, ...) {			/* XXX, refactory: watch_writef() */
	char		*text;
	int		textlen;
	va_list		ap;
	int		res;

	if (!w || !format)
		return -1;

	va_start(ap, format);
	text = vsaprintf(format, ap);
	va_end(ap);
	
	textlen = xstrlen(text); 

	debug_io("[watch]_send: %s\n", text ? textlen ? text: "[0LENGTH]":"[FAILED]");

	if (!text) 
		return -1;

	res = watch_write_data(w, text, textlen);

	xfree(text);
	return res;
}


/**
 * watch_handle()
 *
 * Handler for watches with type: <i>WATCH_READ</i> or <i>WATCH_WRITE</i><br>
 * Mark watch with w->removed = -1, to indicate that watch is in use. And it shouldn't be
 * executed again. [If watch can or even must be executed twice from ekg_loop() than you must
 * change w->removed by yourself.]<br>
 * 
 * If handler of watch return -1 or watch was removed inside function [by watch_remove() or watch_free()]. Than it'll be removed.<br>
 * ELSE Update w->started field to current time.
 *
 * @param w	- watch_t to handler
 *
 * @todo We only check for w->removed == -1, maybe instead change it to: w->removed != 0
 */

void watch_handle(watch_t *w) {
	int (*handler)(int, int, int, void *);
	int res;

	if (!w || w->removed == -1)	/* watch is running in another thread / context */
		return;

	w->removed = -1;
	handler = w->handler;
		
	res = handler(0, w->fd, w->type, w->data);

	if (res == -1 || w->removed == 1) {
		w->removed = 0;
		watch_free(w);
		return;
	}

	w->started = time(NULL);
	w->removed = 0;
}

/**
 * watch_add()
 *
 * Create new watch_t and add it on the beginning of watches list.
 *
 * @param plugin	- plugin
 * @param fd		- fd to watch data for.
 * @param type		- type of watch.
 * @param handler	- handler of watch.
 * @param data		- data which be passed to handler.
 *
 * @return Created watch_t. if @a type is either WATCH_READ_LINE or WATCH_WRITE_LINE than also allocate memory for buffer
 */

watch_t *watch_add(plugin_t *plugin, int fd, watch_type_t type, watcher_handler_func_t *handler, void *data) {
	watch_t *w	= xmalloc(sizeof(watch_t));
	w->plugin	= plugin;
	w->fd		= fd;
	w->type		= type;

	if (w->type == WATCH_READ_LINE) {
		w->type = WATCH_READ;
		w->buf = string_init(NULL);
	} else if (w->type == WATCH_WRITE_LINE) {
		w->type = WATCH_WRITE;
		w->buf = string_init(NULL);
	}
	
	w->started = time(NULL);
	w->handler = handler;
	w->data    = data;

	list_add_beginning(&watches, w);
	return w;
}

/**
 * watch_add_session()
 *
 * Create new session watch_t and add it on the beginning of watches list.
 *
 * @param session	- session
 * @param fd		- fd to watch data for
 * @param type		- type of watch.
 * @param handler	- handler of watch.
 *
 * @return	If @a session is NULL, or @a session->plugin is NULL, it return NULL.<br>
 *		else created watch_t
 */

watch_t *watch_add_session(session_t *session, int fd, watch_type_t type, watcher_session_handler_func_t *handler) {
	watch_t *w;
	if (!session || !session->plugin) {
		debug_error("watch_add_session() s: 0x%x s->plugin: 0x%x\n", session, session ? session->plugin : NULL);
		return NULL;
	}
	w = watch_add(session->plugin, fd, type, (watcher_handler_func_t *) handler, session);

	w->is_session = 1;
	return w;
}

int watch_remove(plugin_t *plugin, int fd, watch_type_t type)
{
	int res = -1;
	watch_t *w;
/* XXX, here can be deadlock feel warned. */
	while ((w = watch_find(plugin, fd, type))) {
		watch_free(w);
		res = 0;
	}

	return res;
}

/**
 * have_plugin_of_class()
 *
 * Check if we have loaded plugin from @a pclass
 *
 * @param pclass 
 *
 * @return	1 - If such plugin was founded<br>
 *		else 0
 */

int have_plugin_of_class(plugin_class_t pclass) {
	plugin_t *p;

	for (p = plugins; p; p = p->next) {
		if (p->pclass == pclass) return 1;
	}

	return 0;
}

PROPERTY_INT_SET(watch, timeout, time_t)

/*
 *  plugin_abi_version()
 *
 * @param plugin_abi_ver, plugin_name
 *
 * @return	1 - if core ABI version is the sama as plugin ABI version
 *		else 0
 */
int plugin_abi_version(int plugin_abi_ver, const char * plugin_name) {

	if (EKG_ABI_VER == plugin_abi_ver)
		return 1;

	debug_error("ABI versions mismatch.  %s_plugin ABI ver. %d,  core ABI ver. %d\n", plugin_name, plugin_abi_ver, EKG_ABI_VER);
	return 0;

}
/*
 * Local Variables:
 * mode: c
 * c-file-style: "k&r"
 * c-basic-offset: 8
 * indent-tabs-mode: t
 * End:
 * vim: noet
 */
