#include "xlog_parser.h"

#include <ctype.h>

#include <box/lua/xlog_parser_v12.h>

extern "C" {
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
} /* extern "C" */

#include "lua/utils.h"

static const char *xloglib_name = "xlog";


static void
lbox_xlog_skip_header(struct lua_State *L, FILE *f, const char *filename)
{
	char buf[256];
	for (;;) {
		if (fgets(buf, sizeof(buf), f) == NULL) {
			luaL_error(L, "%s: failed to read log file header",
				   filename);
		}
		/** Empty line indicates the end of file header. */
		if (strcmp(buf, "\n") == 0)
			break;
		/* Skip header */
	}
}

static int
lbox_xlog_parser_open(struct lua_State *L)
{
	int args_n = lua_gettop(L);
	if (args_n != 1 || !lua_isstring(L, 1))
		luaL_error(L, "Usage: parser.open(log_filename)");

	const char *filename = luaL_checkstring(L, 1);

	FILE *f = fopen(filename, "r");
	if (f == NULL)
		luaL_error(L, "%s: failed to open file", filename);

	char filetype[32], version[32];
	if (fgets(filetype, sizeof(filetype), f) == NULL ||
	    fgets(version, sizeof(version), f) == NULL) {
		luaL_error(L, "%s: failed to read log file header", filename);
	}

	if (strcmp("0.12\n", version) == 0) {
		lbox_xlog_skip_header(L, f, filename);
		lbox_pushxlog_v12(L, f);
	} else {
		luaL_error(L, "%s: unsupported file format version '%.*s'",
			   filename, strlen(version - 1), version);
	}

	return 1;
}

static const struct luaL_reg lbox_xlog_parser_lib [] = {
	{"open",	lbox_xlog_parser_open},
	{NULL, NULL}
};

/** Initialize box.xlog.parser package. */
void
box_lua_xlog_parser_init(struct lua_State *L)
{
	box_lua_xlog_parser_v12_init(L);
	luaL_register_module(L, xloglib_name, lbox_xlog_parser_lib);

	lua_newtable(L);
	lua_setmetatable(L, -2);
	lua_pop(L, 1);
}
