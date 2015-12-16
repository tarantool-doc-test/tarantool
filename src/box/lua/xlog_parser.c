/*
 *
 * Copyright 2010-2015, Tarantool AUTHORS, please see AUTHORS file.
 *
 * Redistribution and use in source and binary forms, with or
 * without modification, are permitted provided that the following
 * conditions are met:
 *
 * 1. Redistributions of source code must retain the above
 *    copyright notice, this list of conditions and the
 *    following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above
 *    copyright notice, this list of conditions and the following
 *    disclaimer in the documentation and/or other materials
 *    provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY <COPYRIGHT HOLDER> ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
 * <COPYRIGHT HOLDER> OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
 * THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */
#include "xlog_parser.h"
#include "msgpuck/msgpuck.h"

#include <box/xlog.h>
#include <box/xrow.h>
#include <ctype.h>
#include <box/iproto_constants.h>

extern "C" {
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
} /* extern "C" */

#include "lua/utils.h"

static int
parse_mp(struct lua_State *L, const char **beg, const char *end)
{
	uint32_t size, i;
	switch(mp_typeof(**beg)) {
	case MP_UINT:
		lua_pushinteger(L, mp_decode_uint(beg));
		break;
	case MP_STR:
		const char *buf;
		buf = mp_decode_str(beg, &size);
		if (*beg > end) {
			i = size - (*beg - end);
			say_warn("warning: decoded %u symbols from MP_STRING, "
				 "%u expected", i, size);
			*beg = end;
			size = i;
		}
		lua_pushlstring(L, buf, size);
		break;
	case MP_ARRAY:
		size = mp_decode_array(beg);
		lua_newtable(L);
		for(i = 0; i < size && *beg < end; i++) {
			parse_mp(L, beg, end);
			lua_rawseti(L, -2, i + 1);
		}
		if (i != size)
			say_warn("warning: decoded %u values from MP_ARRAY, "
				 "%u expected", i, size);
		break;
	case MP_MAP:
		size = mp_decode_map(beg);
		lua_newtable(L);
		for (i = 0; i < size && *beg < end; i++) {
			parse_mp(L, beg, end);
			parse_mp(L, beg, end);
			lua_settable(L, -3);
		}
		if (i != size)
			say_warn("warning: decoded %u values from MP_MAP, "
				 "%u expected", i, size);
		break;
	case MP_BOOL:
		lua_pushboolean(L, mp_decode_bool(beg));
		break;
	case MP_FLOAT:
		lua_pushnumber(L, mp_decode_float(beg));
		break;
	case MP_DOUBLE:
		lua_pushnumber(L, mp_decode_double(beg));
		break;
	case MP_INT:
		lua_pushinteger(L, mp_decode_int(beg));
		break;
	default:
		{
			char buf[32];
			sprintf(buf, "UNKNOWN MP_TYPE:%u", mp_typeof(**beg));
			lua_pushstring(L, buf);
			return -1;
		}
		break;
	}
	return 0;
}

static int
parse_body(struct lua_State *L, const char *ptr, size_t len)
{
	const char **beg = &ptr;
	const char *end = ptr + len;
	if (mp_typeof(**beg) == MP_MAP) {
		uint32_t size = mp_decode_map(beg);
		uint32_t i;
		for (i = 0; i < size && *beg < end; i++) {
			if (mp_typeof(**beg) == MP_UINT) {
				char buf[32];
				uint32_t v = mp_decode_uint(beg);
				if (v < IPROTO_KEY_MAX && iproto_key_strs[v] &&
						iproto_key_strs[v][0])
					sprintf(buf, "%s", iproto_key_strs[v]);
				else
					sprintf(buf, "unknown_key#%u", v);
				lua_pushstring(L, buf);
			}
			parse_mp(L, beg, end);
			lua_settable(L, -3);
		}
		if (i != size)
			say_warn("warning: decoded %u values from MP_MAP, "
				 "%u expected", i, size);
	}
	return 0;
}

static int
lbox_xlog_parser_close(struct lua_State *L)
{
	int args_n = lua_gettop(L);
	xlog *plog = (xlog *)lua_touserdata(L, 1);
	if (args_n != 1)
		luaL_error(L, "Usage: parser.iterate(xlog_pointer)");
	xlog_close(plog);
	return 0;
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

	xlog *l = (struct xlog *) calloc(1, sizeof(*l));

	if (l == NULL)
		tnt_raise(OutOfMemory, sizeof(*l), "malloc", "struct xlog");

	l->f = f;
	l->filename[0] = 0;
	l->mode = LOG_READ;
	l->dir = NULL;
	l->is_inprogress = false;
	l->eof_read = false;
	vclock_create(&l->vclock);


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
	lua_pushlightuserdata(L, l);
	return 1;
}

static int
next_row(struct lua_State *L, struct xlog_cursor *cur,
	 struct xrow_header *row) {
	row->crc_not_check = 1;
	if (xlog_cursor_next(cur, row) != 0)
		return -1;
	lua_newtable(L);
	lua_pushstring(L, "HEADER");

	lua_newtable(L);
	lua_pushstring(L, "type");
	if (row->type < IPROTO_TYPE_STAT_MAX && iproto_type_strs[row->type]) {
		lua_pushstring(L, iproto_type_strs[row->type]);
	} else {
		char buf[32];
		sprintf(buf, "UNKNOWN#%u", row->type);
		lua_pushstring(L, buf);
	}
	lua_settable(L, -3);
	lua_pushstring(L, "lsn");
	lua_pushinteger(L, row->lsn);
	lua_settable(L, -3);
	lua_pushstring(L, "server_id");
	lua_pushinteger(L, row->server_id);
	lua_settable(L, -3);
	lua_pushstring(L, "timestamp");
	lua_pushnumber(L, row->tm);
	lua_settable(L, -3);

	lua_settable(L, -3); /* HEADER */

	for (int i = 0; i < row->bodycnt; i++) {
		if (i == 0) {
			lua_pushstring(L, "BODY");
		} else {
			char buf[8];
			sprintf(buf, "BODY%d", i + 1);
			lua_pushstring(L, buf);
		}

		lua_newtable(L);
		parse_body(L, (char *)row->body[i].iov_base,
			   row->body[i].iov_len);
		lua_settable(L, -3);  /* BODY */
	}
	return 0;
}

static int
lbox_xlog_parser_next(struct lua_State *L)
{
	int args_n = lua_gettop(L);
	xlog *plog = (xlog *)lua_touserdata(L, 1);
	if (args_n != 1)
		luaL_error(L, "Usage: parser.next(xlog_pointer)");
	xlog_cursor *cur = new xlog_cursor;
	xlog_cursor_open(cur, plog);
	struct xrow_header row;
	if (next_row(L, cur, &row) == 0) {
		delete cur;
		return 1;
	}
	delete cur;
	return 0;
}

static int
iter(struct lua_State *L)
{
	xlog_cursor *cur = (xlog_cursor *)lua_touserdata(L, 1);
	int i = luaL_checkinteger(L, 2);
	struct xrow_header row;

	lua_pushinteger(L, i + 1);
	if (next_row(L, cur, &row) == 0) {
		return 2;
	}
	else {
		xlog_cursor_close(cur);
		delete cur;
		return 0;
	}

}

static int
lbox_xlog_parser_iterate(struct lua_State *L)
{

	int args_n = lua_gettop(L);
	xlog *plog = (xlog *)lua_touserdata(L, 1);
	if (args_n != 1)
		luaL_error(L, "Usage: parser.iterate(xlog_pointer)");
	xlog_cursor *cur = new xlog_cursor;
	xlog_cursor_open(cur, plog);
	lua_pushcclosure(L, &iter, 1);
	lua_pushlightuserdata(L, cur);
	lua_pushinteger(L, 0);
	return 3;
}

static const struct luaL_reg xlog_parser_lib [] = {
	{"open", lbox_xlog_parser_open},
	{"close", lbox_xlog_parser_close},
	{"iterate", lbox_xlog_parser_iterate},
	{"next", lbox_xlog_parser_next},
	{NULL, NULL}
};

/** Initialize box.xlog.parser package. */
void
box_lua_xlog_parser_init(struct lua_State *L)
{

	luaL_register_module(L, "xlog.parser", xlog_parser_lib);

	lua_newtable(L);
	lua_setmetatable(L, -2);
	lua_pop(L, 1);
}

