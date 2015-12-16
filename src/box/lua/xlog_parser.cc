#include "xlog_parser.h"

#include <ctype.h>

#include "msgpuck/msgpuck.h"

#include <box/xlog.h>
#include <box/xrow.h>
#include <box/iproto_constants.h>
#include <box/tuple.h>
#include <box/lua/tuple.h>
#include <lua/msgpack.h>

extern "C" {
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
} /* extern "C" */

#include "lua/utils.h"

/********************************** helpers **********************************/
static const char *xlog_parser_name = "xlog.parser";
static const char *xlog_parseri_name = "xlog.parser.iter";
static const char *xloglib_name = "xlog";

static void
lbox_pushxlog(struct lua_State *L, xlog *log)
{
	xlog **plog = (xlog **)lua_newuserdata(L, sizeof(xlog *));
	*plog = log;
	luaL_getmetatable(L, xlog_parser_name);
	lua_setmetatable(L, -2);
}

static void
lbox_pushxlogi(struct lua_State *L, xlog_cursor *cur)
{
	xlog_cursor **pcur = NULL;
	pcur = (xlog_cursor **)lua_newuserdata(L, sizeof(xlog_cursor *));
	*pcur = cur;
	luaL_getmetatable(L, xlog_parseri_name);
	lua_setmetatable(L, -2);
}

static xlog **
lua_checkxlog(struct lua_State *L, int narg, const char *src)
{
	int top = lua_gettop(L);
	if (narg > top || top + narg < 0)
		luaL_error(L, "usage: %s", src);
	void *log = luaL_checkudata(L, narg, xlog_parser_name);
	if (log == NULL)
		luaL_error(L, "usage: %s", src);
	return (xlog **)log;
}

static xlog_cursor **
lua_checkxlogi(struct lua_State *L, int narg, const char *src)
{
	int top = lua_gettop(L);
	if (narg > top || top + narg < 0)
		luaL_error(L, "usage: %s", src);
	void *cur = luaL_checkudata(L, narg, xlog_parseri_name);
	if (cur == NULL)
		luaL_error(L, "usage: %s", src);
	return (xlog_cursor **)cur;
}

static int
luamp_decode_verify(struct lua_State *L, struct luaL_serializer *cfg,
		    const char **beg, const char *end)
{
	const char *tmp = *beg;
	if (mp_check(&tmp, end))
		return -1;
	luamp_decode(L, cfg, beg);
	return 1;
}

/********************************** internal **********************************/

static int
parse_body_kv(struct lua_State *L, const char **beg, const char *end)
{
	if (mp_typeof(**beg) != MP_UINT) {
		/* That means we have broken package */
		return -1;
	}
	char buf[32];
	uint32_t v = mp_decode_uint(beg);
	if (v < IPROTO_KEY_MAX && iproto_key_strs[v] &&
	    iproto_key_strs[v][0]) {
		sprintf(buf, "%s", iproto_key_strs[v]);
	} else {
		sprintf(buf, "unknown_key#%u", v);
	}
	lua_pushstring(L, buf);
	switch (v) {
	case IPROTO_KEY:
	case IPROTO_TUPLE:
	case IPROTO_OPS:
		if (mp_typeof(**beg) == MP_ARRAY) {
			const char *tuple_beg = *beg;
			mp_next(beg);
			struct tuple *tuple = NULL;
			box_tuple_format_t *fmt = box_tuple_format_default();
			tuple = box_tuple_new(fmt, tuple_beg, *beg);
			if (!tuple) {
				lbox_error(L);
				return -1;
			}
			lbox_pushtuple(L, tuple);
			break;
		}
	default:
		if (luamp_decode_verify(L, luaL_msgpack_default, beg, end) == -1)
			lua_pushstring(L, "error");
	}
	lua_settable(L, -3);
	return 0;
}

static int
parse_body(struct lua_State *L, const char *ptr, size_t len)
{
	const char **beg = &ptr;
	const char *end = ptr + len;
	if (mp_typeof(**beg) != MP_MAP) {
		return -1;
	}
	uint32_t size = mp_decode_map(beg);
	uint32_t i;
	for (i = 0; i < size && *beg < end; i++) {
		if (parse_body_kv(L, beg, end) == -1) {
			/* TODO ERROR */
			break;
		}
	}
	if (i != size)
		say_warn("warning: decoded %u values from"
			 " MP_MAP, %u expected", i, size);
	return 0;
}

static int
next_row(struct lua_State *L, xlog_cursor *cur) {
	struct xrow_header row;
	row.crc_not_check = 1;
	if (xlog_cursor_next(cur, &row) != 0)
		return -1;
	lua_newtable(L);
	lua_pushstring(L, "HEADER");

	lua_newtable(L);
	lua_pushstring(L, "type");
	if (row.type < IPROTO_TYPE_STAT_MAX && iproto_type_strs[row.type]) {
		lua_pushstring(L, iproto_type_strs[row.type]);
	} else {
		char buf[32];
		sprintf(buf, "UNKNOWN#%u", row.type);
		lua_pushstring(L, buf);
	}
	lua_settable(L, -3);
	lua_pushstring(L, "lsn");
	lua_pushinteger(L, row.lsn);
	lua_settable(L, -3);
	lua_pushstring(L, "server_id");
	lua_pushinteger(L, row.server_id);
	lua_settable(L, -3);
	lua_pushstring(L, "timestamp");
	lua_pushnumber(L, row.tm);
	lua_settable(L, -3);

	lua_settable(L, -3); /* HEADER */

	for (int i = 0; i < row.bodycnt; i++) {
		if (i == 0) {
			lua_pushstring(L, "BODY");
		} else {
			char buf[8];
			sprintf(buf, "BODY%d", i + 1);
			lua_pushstring(L, buf);
		}

		lua_newtable(L);
		parse_body(L, (char *)row.body[i].iov_base,
			   row.body[i].iov_len);
		lua_settable(L, -3);  /* BODY */
	}
	return 0;
}

static int
iter(struct lua_State *L)
{
	xlog_cursor **cur = lua_checkxlogi(L, 1, "bad pairs argument");
	int i = luaL_checkinteger(L, 2);

	lua_pushinteger(L, i + 1);
	if (next_row(L, *cur) == 0)
		return 2;
	return 0;
}

/******************************** XLOG_PARSER ********************************/

static int
lbox_xlog_parser_gc(struct lua_State *L)
{
	say_info("collect garbage xlog_parser");
	xlog **log = lua_checkxlog(L, 1, "__gc");
	xlog_close(*log);
	return 0;
}

static int
lbox_xlog_parser_iterate(struct lua_State *L)
{
	int args_n = lua_gettop(L);
	xlog **log = lua_checkxlog(L, 1, "__pairs");
	if (args_n != 1)
		luaL_error(L, "Usage: parser:__pairs()");
	xlog_cursor *cur = (xlog_cursor *)calloc(1, sizeof(xlog_cursor));
	xlog_cursor_open(cur, *log);
	lua_pushcclosure(L, &iter, 1);
	lbox_pushxlogi(L, cur);
	lua_pushinteger(L, 0);
	return 3;
}

static int
lbox_xlog_parser_close(struct lua_State *L)
{
	int args_n = lua_gettop(L);
	if (args_n != 1)
		luaL_error(L, "Usage: parser:close()");
	xlog **log = lua_checkxlog(L, 1, "parser:close()");
	xlog_close(*log);
	return 0;
}

/******************************* XLOG_PARSERI ********************************/

static int
lbox_xlog_parseri_gc(struct lua_State *L)
{
	say_info("collect garbage xlog_parseri");
	xlog_cursor **cur = lua_checkxlogi(L, 1, "parser:__gc()");
	xlog_cursor_close(*cur);
	free(*cur);
	return 0;
}

/********************************** MODULE ***********************************/

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

	char filetype[32], version[32], buf[256];
	if (fgets(filetype, sizeof(filetype), f) == NULL ||
	    fgets(version, sizeof(version), f) == NULL) {
		luaL_error(L, "%s: failed to read log file header", filename);
	}

	if (strcmp("0.12\n", version) != 0) {
		luaL_error(L, "%s: unsupported file format version", filename);
	}
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

	xlog *log = (xlog *)calloc(1, sizeof(xlog));

	if (log == NULL)
		tnt_raise(OutOfMemory, sizeof(xlog), "malloc", "struct xlog");

	log->f = f;
	log->filename[0] = 0;
	log->mode = LOG_READ;
	log->dir = NULL;
	log->is_inprogress = false;
	log->eof_read = false;
	vclock_create(&log->vclock);

	lbox_pushxlog(L, log);
	return 1;
}

static const struct luaL_reg lbox_xlog_parser_meta [] = {
	{"__gc",	lbox_xlog_parser_gc},
	{"__ipairs",	lbox_xlog_parser_iterate},
	{"__pairs",	lbox_xlog_parser_iterate},
	{"pairs",	lbox_xlog_parser_iterate},
	{"close",	lbox_xlog_parser_close},
	{NULL, NULL}
};

static const struct luaL_reg lbox_xlog_parseri_meta [] = {
	{"__gc",	lbox_xlog_parseri_gc},
	{NULL, NULL}
};

static const struct luaL_reg lbox_xlog_parser_lib [] = {
	{"open",	lbox_xlog_parser_open},
	{NULL, NULL}
};

/** Initialize box.xlog.parser package. */
void
box_lua_xlog_parser_init(struct lua_State *L)
{
	luaL_register_type(L, xlog_parser_name, lbox_xlog_parser_meta);
	luaL_register_type(L, xlog_parseri_name, lbox_xlog_parseri_meta);
	luaL_register_module(L, xloglib_name, lbox_xlog_parser_lib);

	lua_newtable(L);
	lua_setmetatable(L, -2);
	lua_pop(L, 1);
}
