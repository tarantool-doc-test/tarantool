/*
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
#include "sophia_index.h"
#include "sophia_engine.h"
#include "cfg.h"
#include "xrow.h"
#include "tuple.h"
#include "scoped_guard.h"
#include "txn.h"
#include "index.h"
#include "recovery.h"
#include "relay.h"
#include "space.h"
#include "schema.h"
#include "port.h"
#include "request.h"
#include "iproto_constants.h"
#include "small/rlist.h"
#include <errinj.h>
#include <sophia.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <errno.h>

void sophia_error(void *env)
{
	char *error = (char *)sp_getstring(env, "sophia.error", NULL);
	char msg[512];
	snprintf(msg, sizeof(msg), "%s", error);
	tnt_raise(ClientError, ER_SOPHIA, msg);
}

int sophia_info(const char *name, sophia_info_f cb, void *arg)
{
	SophiaEngine *e = (SophiaEngine *)engine_find("sophia");
	void *cursor = sp_getobject(e->env, NULL);
	void *o = NULL;
	if (name) {
		while ((o = sp_get(cursor, o))) {
			char *key = (char *)sp_getstring(o, "key", 0);
			if (name && strcmp(key, name) != 0)
				continue;
			char *value = (char *)sp_getstring(o, "value", 0);
			cb(key, value, arg);
			return 1;
		}
		sp_destroy(cursor);
		return 0;
	}
	while ((o = sp_get(cursor, o))) {
		char *key = (char *)sp_getstring(o, "key", 0);
		char *value = (char *)sp_getstring(o, "value", 0);
		cb(key, value, arg);
	}
	sp_destroy(cursor);
	return 0;
}

struct SophiaSpace: public Handler {
	SophiaSpace(Engine*);
	virtual struct tuple *
	executeReplace(struct txn*, struct space *space,
	               struct request *request);
	virtual struct tuple *
	executeDelete(struct txn*, struct space *space,
	              struct request *request);
	virtual struct tuple *
	executeUpdate(struct txn*, struct space *space,
	              struct request *request);
	virtual void
	executeUpsert(struct txn*, struct space *space,
	              struct request *request);
};

struct tuple *
SophiaSpace::executeReplace(struct txn *txn, struct space *space,
                            struct request *request)
{
	(void) txn;

	SophiaIndex *index = (SophiaIndex *)index_find(space, 0);

	space_validate_tuple_raw(space, request->tuple);
	tuple_field_count_validate(space->format, request->tuple);

	int size = request->tuple_end - request->tuple;
	const char *key =
		tuple_field_raw(request->tuple, size,
		                index->key_def->parts[0].fieldno);
	primary_key_validate(index->key_def, key, index->key_def->part_count);

	/* Switch from INSERT to REPLACE during recovery.
	 *
	 * Database might hold newer key version than currenly
	 * recovered log record.
	 */
	enum dup_replace_mode mode = DUP_REPLACE_OR_INSERT;
	if (request->type == IPROTO_INSERT) {
		SophiaEngine *engine = (SophiaEngine *)space->handler->engine;
		if (engine->recovery_complete)
			mode = DUP_INSERT;
	}
	index->replace_or_insert(request->tuple, request->tuple_end, mode);
	return NULL;
}

struct tuple *
SophiaSpace::executeDelete(struct txn *txn, struct space *space,
                           struct request *request)
{
	(void) txn;

	SophiaIndex *index = (SophiaIndex *)index_find(space, request->index_id);
	const char *key = request->key;
	uint32_t part_count = mp_decode_array(&key);
	primary_key_validate(index->key_def, key, part_count);
	index->remove(key);
	return NULL;
}

struct tuple *
SophiaSpace::executeUpdate(struct txn *txn, struct space *space,
                           struct request *request)
{
	(void) txn;

	/* Try to find the tuple by unique key */
	SophiaIndex *index = (SophiaIndex *)index_find(space, request->index_id);
	const char *key = request->key;
	uint32_t part_count = mp_decode_array(&key);
	primary_key_validate(index->key_def, key, part_count);
	struct tuple *old_tuple = index->findByKey(key, part_count);

	if (old_tuple == NULL)
		return NULL;
	/* Sophia always yields a zero-ref tuple, GC it here. */
	TupleRef old_ref(old_tuple);

	/* Do tuple update */
	struct tuple *new_tuple =
		tuple_update(space->format,
		             region_alloc_xc_cb,
		             &fiber()->gc,
		             old_tuple, request->tuple,
		             request->tuple_end,
		             request->index_base);
	TupleRef ref(new_tuple);

	space_validate_tuple(space, new_tuple);
	space_check_update(space, old_tuple, new_tuple);

	index->replace_or_insert(new_tuple->data,
	                         new_tuple->data + new_tuple->bsize,
	                         DUP_REPLACE);
	return NULL;
}

void
SophiaSpace::executeUpsert(struct txn *txn, struct space *space,
                           struct request *request)
{
	(void) txn;
	SophiaIndex *index = (SophiaIndex *)index_find(space, request->index_id);

	/* Check field count in tuple */
	space_validate_tuple_raw(space, request->tuple);
	tuple_field_count_validate(space->format, request->tuple);

	/* Extract key from tuple */
	uint32_t key_len = request->tuple_end - request->tuple;
	char *key = (char *) region_alloc_xc(&fiber()->gc, key_len);
	key_len = key_parts_create_from_tuple(index->key_def, request->tuple,
					      key, key_len);

	/* validate upsert key */
	uint32_t part_count = index->key_def->part_count;
	primary_key_validate(index->key_def, key, part_count);

	index->upsert(key,
	              request->ops,
	              request->ops_end,
	              request->tuple,
	              request->tuple_end,
	              request->index_base);
}

SophiaSpace::SophiaSpace(Engine *e)
	:Handler(e)
{
}

SophiaEngine::SophiaEngine()
	:Engine("sophia")
	 ,m_prev_commit_lsn(-1)
	 ,m_prev_checkpoint_lsn(-1)
	 ,m_checkpoint_lsn(-1)
	 ,recovery_complete(0)
{
	flags = 0;
	env = NULL;
}

SophiaEngine::~SophiaEngine()
{
	if (env)
		sp_destroy(env);
}

static inline int
sophia_poll(SophiaEngine *e)
{
	void *req = sp_poll(e->env);
	if (req == NULL)
		return 0;
	struct fiber *fiber =
		(struct fiber *)sp_getstring(req, "arg", NULL);
	assert(fiber != NULL);
	fiber_set_key(fiber, FIBER_KEY_MSG, req);
	fiber_call(fiber);
	return 1;
}

static inline int
sophia_queue(SophiaEngine *e)
{
	return sp_getint(e->env, "performance.reqs");
}

static inline void
sophia_on_event(void *arg)
{
	SophiaEngine *engine = (SophiaEngine *)arg;
	ev_async_send(engine->cord->loop, &engine->watcher);
}

static void
sophia_idle_cb(ev_loop *loop, struct ev_idle *w, int /* events */)
{
	SophiaEngine *engine = (SophiaEngine *)w->data;
	sophia_poll(engine);
	if (sophia_queue(engine) == 0)
		ev_idle_stop(loop, w);
}

static void
sophia_async_schedule(ev_loop *loop, struct ev_async *w, int /* events */)
{
	SophiaEngine *engine = (SophiaEngine *)w->data;
	sophia_poll(engine);
	if (sophia_queue(engine))
		ev_idle_start(loop, &engine->idle);
}

void
SophiaEngine::init()
{
	cord = cord();
	ev_idle_init(&idle, sophia_idle_cb);
	ev_async_init(&watcher, sophia_async_schedule);
	ev_async_start(cord->loop, &watcher);
	watcher.data = this;
	idle.data = this;
	env = sp_env();
	if (env == NULL)
		panic("failed to create sophia environment");
	sp_setint(env, "sophia.path_create", 0);
	sp_setstring(env, "sophia.path", cfg_gets("sophia_dir"), 0);
	sp_setstring(env, "scheduler.on_event", (const void *)sophia_on_event, 0);
	sp_setstring(env, "scheduler.on_event_arg", (const void *)this, 0);
	sp_setint(env, "scheduler.threads", cfg_geti("sophia.threads"));
	sp_setint(env, "memory.limit", cfg_geti64("sophia.memory_limit"));
	sp_setint(env, "compaction.node_size", cfg_geti("sophia.node_size"));
	sp_setint(env, "compaction.page_size", cfg_geti("sophia.page_size"));
	sp_setint(env, "compaction.0.async", 1);
	sp_setint(env, "log.enable", 0);
	sp_setint(env, "log.two_phase_recover", 1);
	sp_setint(env, "log.commit_lsn", 1);
	int rc = sp_open(env);
	if (rc == -1)
		sophia_error(env);
	say_info("sophia: started two-phase recovery\n");
}

void
SophiaEngine::endRecovery()
{
	if (recovery_complete)
		return;
	/* complete two-phase recovery */
	int rc = sp_open(env);
	if (rc == -1)
		sophia_error(env);
	say_info("sophia: completed two-phase recovery\n");
	recovery_complete = 1;
}

Handler *
SophiaEngine::open()
{
	return new SophiaSpace(this);
}

static inline void
sophia_send_row(struct relay *relay, uint32_t space_id, char *tuple,
                uint32_t tuple_size, int64_t lsn)
{
	struct recovery *r = relay->r;
	struct request_replace_body body;
	body.m_body = 0x82; /* map of two elements. */
	body.k_space_id = IPROTO_SPACE_ID;
	body.m_space_id = 0xce; /* uint32 */
	body.v_space_id = mp_bswap_u32(space_id);
	body.k_tuple = IPROTO_TUPLE;
	struct xrow_header row;
	row.type = IPROTO_INSERT;
	row.server_id = 0;
	/* TODO: lsn */
	(void)lsn;
	(void)r;
	row.lsn = lsn;
	//row.lsn = vclock_inc(&r->vclock, row.server_id);
	row.bodycnt = 2;
	row.body[0].iov_base = &body;
	row.body[0].iov_len = sizeof(body);
	row.body[1].iov_base = tuple;
	row.body[1].iov_len = tuple_size;
	relay_send(relay, &row);
}

namespace { /* anonymous namespace to disable export of structures */
struct relay_space_info {
	void *cursor;
	void *obj;
	struct key_def *key_def;
	struct relay_space_info *next;
};

struct relay_info {
	struct relay *relay;
	struct relay_space_info *first;
	struct relay_space_info *last;
};
} /* namespace { */

static void
join_collect_space_f(struct space *sp, void *udata)
{
	if (!space_is_sophia(sp))
		return;
	Index *ind = space_index(sp, 0);
	if (!ind)
		return; /* primary key was not created - nothing to send */
	SophiaIndex *sind = (SophiaIndex *)ind;
	void *env = sind->env;
	void *db = sind->db;

	struct relay_info *relay_info = (struct relay_info *)udata;
	struct relay_space_info *new_info = (struct relay_space_info *)
		region_alloc0_xc(&fiber()->gc, sizeof(struct relay_space_info));
	if (relay_info->last)
		relay_info->last->next = new_info;
	else
		relay_info->first = new_info;
	relay_info->last = new_info;

	new_info->key_def = key_def_dup(ind->key_def);
	new_info->cursor = sp_cursor(env);
	if (new_info->cursor == NULL)
		sophia_error(env);
	/* tell cursor not to hold a transaction, which in result enables
	 * compaction process for a duplicates */
	sp_setint(new_info->cursor, "read_commited", 1);
	new_info->obj = sp_document(db);
}

static void
join_clean(struct relay_info *relay_info)
{
	for (struct relay_space_info *space_info = relay_info->first;
	     space_info; space_info = space_info->next) {
		if (space_info->obj)
			sp_destroy(space_info->obj);
		if (space_info->cursor)
			sp_destroy(space_info->cursor);
		if (space_info->key_def)
			free(space_info->key_def);
	}
}

static void
join_send_space(struct relay_info *relay_info,
		struct relay_space_info *space_info)
{
	while ((space_info->obj = sp_get(space_info->cursor, space_info->obj)))
	{
		int64_t lsn = sp_getint(space_info->obj, "lsn");

		uint32_t tuple_size;
		char *tuple = (char *)
			sophia_tuple_new(space_info->obj, space_info->key_def,
					 NULL, &tuple_size);
		try {
			sophia_send_row(relay_info->relay,
					space_info->key_def->space_id,
					tuple, tuple_size, lsn);
		} catch (...) {
			free(tuple);
			throw;
		}
		free(tuple);
	}
	sp_destroy(space_info->cursor);
	space_info->cursor = 0;
}

static void
join_send_spaces(struct relay_info *relay_info)
{
	for (struct relay_space_info *space_info = relay_info->first;
	     space_info; space_info = space_info->next) {
		join_send_space(relay_info, space_info);
	}
}


/**
 * Relay all data currently stored in Sophia engine
 * to the replica.
 */
int64_t
SophiaEngine::join(struct relay *relay)
{
	struct relay_info relay_info = {relay, 0, 0};
	auto guard = make_scoped_guard([&]{ join_clean(&relay_info); });
	space_foreach(join_collect_space_f, &relay_info);
	join_send_spaces(&relay_info);
	printf("SophiaEngine::join: checkpoint %d\n",
	       (int)sp_getint(env, "metric.lsn"));
	return (int64_t)sp_getint(env, "metric.lsn");
}

Index*
SophiaEngine::createIndex(struct key_def *key_def)
{
	switch (key_def->type) {
	case TREE:
		return new SophiaIndex(key_def);
	default:
		assert(false);
		return NULL;
	}
}

void
SophiaEngine::dropIndex(Index *index)
{
	SophiaIndex *i = (SophiaIndex *)index;
	/* schedule asynchronous drop */
	int rc = sp_drop(i->db);
	if (rc == -1)
		sophia_error(env);
	/* unref db object */
	rc = sp_destroy(i->db);
	if (rc == -1)
		sophia_error(env);
	/* automatically start asynchronous database drop
	 * when a last transaction completes */
	rc = sp_destroy(i->db);
	if (rc == -1)
		sophia_error(env);
	i->db  = NULL;
	i->env = NULL;
}

void
SophiaEngine::keydefCheck(struct space *space, struct key_def *key_def)
{
	switch (key_def->type) {
	case TREE: {
		if (! key_def->opts.is_unique) {
			tnt_raise(ClientError, ER_MODIFY_INDEX,
				  key_def->name,
				  space_name(space),
				  "Sophia TREE index must be unique");
		}
		if (key_def->iid != 0) {
			tnt_raise(ClientError, ER_MODIFY_INDEX,
				  key_def->name,
				  space_name(space),
				  "Sophia TREE secondary indexes are not supported");
		}
		const int keypart_limit = 8;
		if (key_def->part_count > keypart_limit) {
			tnt_raise(ClientError, ER_MODIFY_INDEX,
			          key_def->name,
			          space_name(space),
			          "Sophia TREE index too many key-parts (8 max)");
		}
		int i = 0;
		while (i < key_def->part_count) {
			struct key_part *part = &key_def->parts[i];
			if (part->type != NUM && part->type != STRING) {
				tnt_raise(ClientError, ER_MODIFY_INDEX,
				          key_def->name,
				          space_name(space),
				          "Sophia TREE index field type must be STR or NUM");
			}
			if (part->fieldno != i) {
				tnt_raise(ClientError, ER_MODIFY_INDEX,
				          key_def->name,
				          space_name(space),
				          "Sophia TREE key-parts must follow first and cannot be sparse");
			}
			i++;
		}
		break;
	}
	default:
		tnt_raise(ClientError, ER_INDEX_TYPE,
			  key_def->name,
			  space_name(space));
		break;
	}
}

void
SophiaEngine::begin(struct txn *txn)
{
	assert(txn->engine_tx == NULL);
	txn->engine_tx = sp_begin(env);
	if (txn->engine_tx == NULL)
		sophia_error(env);
}

void
SophiaEngine::prepare(struct txn *txn)
{
	/* A half committed transaction is no longer
	 * being part of concurrent index, but still can be
	 * commited or rolled back.
	 *
	 * This mode disables conflict resolution for 'prepared'
	 * transactions and solves the issue with concurrent
	 * write-write conflicts during wal write/yield.
	 *
	 * It is important to maintain correct serial
	 * commit order by wal_writer.
	 */
	sp_setint(txn->engine_tx, "half_commit", 1);

	int rc = sp_commit(txn->engine_tx);
	switch (rc) {
	case 1: /* rollback */
		txn->engine_tx = NULL;
	case 2: /* lock */
		tnt_raise(ClientError, ER_TRANSACTION_CONFLICT);
		break;
	case -1:
		sophia_error(env);
		break;
	}
}

void
SophiaEngine::commit(struct txn *txn, int64_t signature)
{
	if (txn->engine_tx == NULL)
		return;

	if (txn->n_rows > 0) {
		/* commit transaction using transaction commit signature */
		assert(signature >= 0);

		if (m_prev_commit_lsn == signature) {
			panic("sophia commit panic: m_prev_commit_lsn == signature = %"
			      PRIu64, signature);
		}
		/* Set tx id in Sophia only if tx has WRITE requests */
		sp_setint(txn->engine_tx, "lsn", signature);
		m_prev_commit_lsn = signature;
	}

	int rc = sp_commit(txn->engine_tx);
	if (rc == -1) {
		panic("sophia commit failed: txn->signature = %"
		      PRIu64, signature);
	}
	txn->engine_tx = NULL;
}

void
SophiaEngine::rollbackStatement(struct txn_stmt* /* stmt */)
{
	say_info("SophiaEngine::rollbackStatement()");
}

void
SophiaEngine::rollback(struct txn *txn)
{
	if (txn->engine_tx) {
		sp_destroy(txn->engine_tx);
		txn->engine_tx = NULL;
	}
}

void
SophiaEngine::beginJoin()
{
}

void
SophiaEngine::endJoin()
{
	endRecovery();
}

void
SophiaEngine::recoverToCheckpoint(int64_t lsn)
{
	/* do nothing except saving the latest snapshot lsn */
	m_prev_checkpoint_lsn = lsn;
}

int
SophiaEngine::beginCheckpoint(int64_t lsn)
{
	assert(m_checkpoint_lsn == -1);
	int rc;
	if (lsn != m_prev_checkpoint_lsn) {
		/* do not initiate checkpoint during bootstrap,
		 * thread pool is not up yet */
		if (m_prev_checkpoint_lsn == -1)
			goto done;
		/* begin asynchronous checkpoint */
		rc = sp_setint(env, "scheduler.checkpoint", 0);
		if (rc == -1)
			sophia_error(env);
done:
		m_checkpoint_lsn = lsn;
		return 0;
	}
	errno = EEXIST;
	return -1;
}

int
SophiaEngine::waitCheckpoint()
{
	assert(m_checkpoint_lsn != -1);
	/* bootstrap case */
	if (m_prev_checkpoint_lsn == -1)
		return 0;
	for (;;) {
		int64_t is_active = sp_getint(env, "scheduler.checkpoint_active");
		if (! is_active)
			break;
		fiber_yield_timeout(.020);
	}
	return 0;
}

void
SophiaEngine::commitCheckpoint()
{
	m_prev_checkpoint_lsn = m_checkpoint_lsn;
	m_checkpoint_lsn = -1;
}

void
SophiaEngine::abortCheckpoint()
{
	if (m_checkpoint_lsn >= 0)
		m_checkpoint_lsn = -1;
}
