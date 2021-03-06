/*-------------------------------------------------------------------------
 *
 * pglogical_apply.c
 * 		pglogical apply logic
 *
 * Copyright (c) 2015, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		  pglogical.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "miscadmin.h"
#include "libpq-fe.h"
#include "pgstat.h"

#include "access/htup_details.h"
#include "access/xact.h"

#include "catalog/namespace.h"

#include "commands/dbcommands.h"
#include "commands/sequence.h"
#include "commands/tablecmds.h"
#include "commands/trigger.h"

#include "executor/executor.h"

#include "libpq/pqformat.h"

#include "mb/pg_wchar.h"

#include "nodes/makefuncs.h"
#include "nodes/parsenodes.h"

#include "optimizer/planner.h"

#include "replication/origin.h"

#include "rewrite/rewriteHandler.h"

#include "storage/ipc.h"
#include "storage/lmgr.h"
#include "storage/proc.h"

#include "tcop/pquery.h"
#include "tcop/utility.h"

#include "utils/builtins.h"
#include "utils/int8.h"
#include "utils/jsonb.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/snapmgr.h"

#include "pglogical_conflict.h"
#include "pglogical_node.h"
#include "pglogical_proto.h"
#include "pglogical_queue.h"
#include "pglogical_relcache.h"
#include "pglogical_repset.h"
#include "pglogical_rpc.h"
#include "pglogical_sync.h"
#include "pglogical_worker.h"
#include "pglogical.h"


void pglogical_apply_main(Datum main_arg);

static bool			in_remote_transaction = false;
static XLogRecPtr	remote_origin_lsn = InvalidXLogRecPtr;
static RepOriginId	remote_origin_id = InvalidRepOriginId;

static Oid			QueueRelid = InvalidOid;

static List		   *SyncingTables = NIL;

PGLogicalApplyWorker	   *MyApplyWorker = NULL;
PGLogicalSubscription	   *MySubscription = NULL;

static PGconn	   *applyconn = NULL;

typedef struct PGLFlushPosition
{
	dlist_node node;
	XLogRecPtr local_end;
	XLogRecPtr remote_end;
} PGLFlushPosition;

dlist_head lsn_mapping = DLIST_STATIC_INIT(lsn_mapping);

typedef struct ApplyExecState {
	EState			   *estate;
	EPQState		   epqstate;
	ResultRelInfo	   *resultRelInfo;
	TupleTableSlot	   *slot;
} ApplyExecState;

static void handle_queued_message(HeapTuple msgtup, bool tx_just_started);
static void handle_startup_param(const char *key, const char *value);
static bool parse_bool_param(const char *key, const char *value);
static void process_syncing_tables(XLogRecPtr end_lsn);
static void start_sync_worker(RangeVar *rv);

/*
 * Check if given relation is in process of being synchronized.
 *
 * TODO: performance
 */
static bool
check_syncing_relation(const char *nspname, const char *relname)
{
	return list_length(SyncingTables) &&
		list_member(SyncingTables, makeRangeVar((char *)nspname, (char *)relname, -1));
}

static bool
ensure_transaction(void)
{
	if (IsTransactionState())
	{
		if (CurrentMemoryContext != MessageContext)
			MemoryContextSwitchTo(MessageContext);
		return false;
	}

	StartTransactionCommand();
	MemoryContextSwitchTo(MessageContext);
	return true;
}

static void
handle_begin(StringInfo s)
{
	XLogRecPtr		commit_lsn;
	TimestampTz		commit_time;
	TransactionId	remote_xid;

	pglogical_read_begin(s, &commit_lsn, &commit_time, &remote_xid);

	replorigin_session_origin_timestamp = commit_time;
	replorigin_session_origin_lsn = commit_lsn;
	remote_origin_id = InvalidRepOriginId;

	in_remote_transaction = true;

	pgstat_report_activity(STATE_RUNNING, NULL);
}

/*
 * Handle COMMIT message.
 */
static void
handle_commit(StringInfo s)
{
	XLogRecPtr		commit_lsn;
	XLogRecPtr		end_lsn;
	TimestampTz		commit_time;

	pglogical_read_commit(s, &commit_lsn, &end_lsn, &commit_time);

	Assert(commit_lsn == replorigin_session_origin_lsn);
	Assert(commit_time == replorigin_session_origin_timestamp);

	if (IsTransactionState())
	{
		PGLFlushPosition *flushpos;

		CommitTransactionCommand();
		MemoryContextSwitchTo(TopMemoryContext);

		/* Track commit lsn  */
		flushpos = (PGLFlushPosition *) palloc(sizeof(PGLFlushPosition));
		flushpos->local_end = XactLastCommitEnd;
		flushpos->remote_end = end_lsn;

		dlist_push_tail(&lsn_mapping, &flushpos->node);
		MemoryContextSwitchTo(MessageContext);
	}

	/*
	 * If the xact isn't from the immediate upstream, advance the slot of the
	 * node it originally came from so we start replay of that node's change
	 * data at the right place.
	 *
	 * This is only necessary when we're streaming data from one peer (A) that
	 * in turn receives from other peers (B, C), and we plan to later switch to
	 * replaying directly from B and/or C, no longer receiving forwarded xacts
	 * from A. When we do the switchover we need to know the right place at
	 * which to start replay from B and C. We don't actually do that yet, but
	 * we'll want to be able to do cascaded initialisation in future, so it's
	 * worth keeping track.
	 *
	 * A failure can occur here (see #79) if there's a cascading
	 * replication configuration like:
	 *
	 * X--> Y -> Z
	 * |         ^
	 * |         |
	 * \---------/
	 *
	 * where the direct and indirect connections from X to Z use different
	 * replication sets so as not to conflict, and where Y and Z are on the
	 * same PostgreSQL instance. In this case our attempt to advance the
	 * replication identifier here will ERROR because it's already in use
	 * for the direct connection from X to Z. So don't do that.
	 */
	if (remote_origin_id != InvalidRepOriginId &&
		remote_origin_id != replorigin_session_origin)
	{
		elog(DEBUG3, "advancing origin oid %u for forwarded row to %X/%X",
			remote_origin_id,
			(uint32)(XactLastCommitEnd>>32), (uint32)XactLastCommitEnd);

		replorigin_advance(remote_origin_id, remote_origin_lsn,
						   XactLastCommitEnd, false, false /* XXX ? */);
	}

	in_remote_transaction = false;

	/*
	 * Stop replay if we're doing limited replay and we've replayed up to the
	 * last record we're supposed to process.
	 */
	if (MyApplyWorker->replay_stop_lsn != InvalidXLogRecPtr
			&& MyApplyWorker->replay_stop_lsn <= end_lsn)
	{
		ereport(LOG,
				(errmsg("pglogical %s finished processing; replayed to %X/%X of required %X/%X",
				 MyPGLogicalWorker->worker_type == PGLOGICAL_WORKER_SYNC ? "sync" : "apply",
				 (uint32)(end_lsn>>32), (uint32)end_lsn,
				 (uint32)(MyApplyWorker->replay_stop_lsn >>32),
				 (uint32)MyApplyWorker->replay_stop_lsn)));

		/*
		 * Flush all writes so the latest position can be reported back to the
		 * sender.
		 */
		XLogFlush(GetXLogWriteRecPtr());

		/*
		 * Disconnect.
		 *
		 * This needs to happen before the pglogical_sync_worker_finish()
		 * call otherwise slot drop will fail.
		 */
		PQfinish(applyconn);

		/*
		 * If this is sync worker, finish it.
		 */
		if (MyPGLogicalWorker->worker_type == PGLOGICAL_WORKER_SYNC)
			pglogical_sync_worker_finish();

		/* Stop gracefully */
		proc_exit(0);
	}

	process_syncing_tables(end_lsn);

	pgstat_report_activity(STATE_IDLE, NULL);
}

/*
 * Handle ORIGIN message.
 */
static void
handle_origin(StringInfo s)
{
	char		   *origin;

	/*
	 * ORIGIN message can only come inside remote transaction and before
	 * any actual writes.
	 */
	if (!in_remote_transaction || IsTransactionState())
		elog(ERROR, "ORIGIN message sent out of order");

	/* We have to start transaction here so that we can work with origins. */
	ensure_transaction();

	origin = pglogical_read_origin(s, &remote_origin_lsn);
	remote_origin_id = replorigin_by_name(origin, true);
}

/*
 * Handle RELATION message.
 *
 * Note we don't do validation against local schema here. The validation is
 * posponed until first change for given relation comes.
 */
static void
handle_relation(StringInfo s)
{
	(void) pglogical_read_rel(s);
}


static EState *
create_estate_for_relation(PGLogicalRelation *rel)
{
	EState	   *estate;
	ResultRelInfo *resultRelInfo;
	RangeTblEntry *rte;

	estate = CreateExecutorState();

	rte = makeNode(RangeTblEntry);
	rte->rtekind = RTE_RELATION;
	rte->relid = RelationGetRelid(rel->rel);
	rte->relkind = rel->rel->rd_rel->relkind;
	estate->es_range_table = list_make1(rte);

	resultRelInfo = makeNode(ResultRelInfo);
/*	InitResultRelInfo(resultRelInfo,
					  rel,
					  1,
					  0);*/
	resultRelInfo->ri_RangeTableIndex = 1;
	resultRelInfo->ri_RelationDesc = rel->rel;
	resultRelInfo->ri_TrigInstrument = NULL;

	estate->es_result_relations = resultRelInfo;
	estate->es_num_result_relations = 1;
	estate->es_result_relation_info = resultRelInfo;

	if (rel->hasTriggers)
		resultRelInfo->ri_TrigDesc = CopyTriggerDesc(rel->rel->trigdesc);

	if (resultRelInfo->ri_TrigDesc)
	{
		int			n = resultRelInfo->ri_TrigDesc->numtriggers;

		resultRelInfo->ri_TrigFunctions = (FmgrInfo *)
			palloc0(n * sizeof(FmgrInfo));
		resultRelInfo->ri_TrigWhenExprs = (List **)
			palloc0(n * sizeof(List *));

		/* Triggers might need a slot */
		estate->es_trig_tuple_slot = ExecInitExtraTupleSlot(estate);
	}
	else
	{
		resultRelInfo->ri_TrigFunctions = NULL;
		resultRelInfo->ri_TrigWhenExprs = NULL;
	}

	return estate;
}

static List *
UserTableUpdateOpenIndexes(EState *estate, TupleTableSlot *slot)
{
	List	   *recheckIndexes = NIL;

	if (estate->es_result_relation_info->ri_NumIndices > 0)
	{
		recheckIndexes = ExecInsertIndexTuples(slot,
											   &slot->tts_tuple->t_self,
											   estate
#if PG_VERSION_NUM >= 90500
											   , false, NULL, NIL
#endif
											   );

		/* FIXME: recheck the indexes */
		if (recheckIndexes != NIL)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("pglogical doesn't support index rechecks")));

		list_free(recheckIndexes);
	}

	return recheckIndexes;
}

static bool
physatt_in_attmap(PGLogicalRelation *rel, int attid)
{
	AttrNumber	i;

	for (i = 0; i < rel->natts; i++)
		if (rel->attmap[i] == attid)
			return true;

	return false;
}

/*
 * Executes default values for columns for which we didn't get any data.
 *
 * TODO: this needs caching, it's not exactly fast.
 */
static void
fill_missing_defaults(PGLogicalRelation *rel, EState *estate,
					  PGLogicalTupleData *tuple)
{
	TupleDesc	desc = RelationGetDescr(rel->rel);
	AttrNumber	num_phys_attrs = desc->natts;
	int			i;
	AttrNumber	attnum,
				num_defaults = 0;
	int		   *defmap;
	ExprState **defexprs;
	ExprContext *econtext;

	econtext = GetPerTupleExprContext(estate);

	/* We got all the data via replication, no need to evaluate anything. */
	if (num_phys_attrs == rel->natts)
		return;

	defmap = (int *) palloc(num_phys_attrs * sizeof(int));
	defexprs = (ExprState **) palloc(num_phys_attrs * sizeof(ExprState *));

	for (attnum = 0; attnum < num_phys_attrs; attnum++)
	{
		Expr	   *defexpr;

		if (desc->attrs[attnum]->attisdropped)
			continue;

		if (physatt_in_attmap(rel, attnum))
			continue;

		defexpr = (Expr *) build_column_default(rel->rel, attnum + 1);

		if (defexpr != NULL)
		{
			/* Run the expression through planner */
			defexpr = expression_planner(defexpr);

			/* Initialize executable expression in copycontext */
			defexprs[num_defaults] = ExecInitExpr(defexpr, NULL);
			defmap[num_defaults] = attnum;
			num_defaults++;
		}

	}

	for (i = 0; i < num_defaults; i++)
		tuple->values[defmap[i]] = ExecEvalExpr(defexprs[i],
												econtext,
												&tuple->nulls[defmap[i]],
												NULL);
}

static ApplyExecState *
init_apply_exec_state(PGLogicalRelation *rel)
{
	ApplyExecState	   *aestate = palloc0(sizeof(ApplyExecState));

	/* Initialize the executor state. */
	aestate->estate = create_estate_for_relation(rel);
	aestate->resultRelInfo = aestate->estate->es_result_relation_info;

	aestate->slot = ExecInitExtraTupleSlot(aestate->estate);
	ExecSetSlotDescriptor(aestate->slot, RelationGetDescr(rel->rel));

	if (aestate->resultRelInfo->ri_TrigDesc)
		EvalPlanQualInit(&aestate->epqstate, aestate->estate, NULL, NIL, -1);

	/* Prepare to catch AFTER triggers. */
	AfterTriggerBeginQuery();

	return aestate;
}


static void
finish_apply_exec_state(ApplyExecState *aestate)
{
	/* Handle queued AFTER triggers. */
	AfterTriggerEndQuery(aestate->estate);

	/* Terminate EPQ execution if active. */
	EvalPlanQualEnd(&aestate->epqstate);

	/* Cleanup tuple table. */
	ExecResetTupleTable(aestate->estate->es_tupleTable, true);

	/* Free the memory. */
	FreeExecutorState(aestate->estate);
	pfree(aestate);
}

static void
handle_insert(StringInfo s)
{
	PGLogicalTupleData	newtup;
	PGLogicalRelation  *rel;
	ApplyExecState	   *aestate;
	Oid					conflicts;
	TupleTableSlot	   *localslot;
	HeapTuple			remotetuple;
	HeapTuple			applytuple;
	PGLogicalConflictResolution resolution;
	bool				started_tx = ensure_transaction();
	List			   *recheckIndexes = NIL;
	MemoryContext		oldctx;

	rel = pglogical_read_insert(s, RowExclusiveLock, &newtup);

	/* If in list of relations which are being synchronized, skip. */
	if (check_syncing_relation(rel->nspname, rel->relname))
	{
		pglogical_relation_close(rel, NoLock);
		return;
	}

	/* Initialize the executor state. */
	aestate = init_apply_exec_state(rel);
	localslot = ExecInitExtraTupleSlot(aestate->estate);
	ExecSetSlotDescriptor(localslot, RelationGetDescr(rel->rel));

	/* Get snapshot */
	PushActiveSnapshot(GetTransactionSnapshot());

	ExecOpenIndices(aestate->resultRelInfo
#if PG_VERSION_NUM >= 90500
					, false
#endif
					);

	/* Check for existing tuple with same key */
	conflicts = pglogical_tuple_find_conflict(aestate->estate,
											  &newtup,
											  localslot);

	/* Process and store remote tuple in the slot */
	oldctx = MemoryContextSwitchTo(GetPerTupleMemoryContext(aestate->estate));
	fill_missing_defaults(rel, aestate->estate, &newtup);
	remotetuple = heap_form_tuple(RelationGetDescr(rel->rel),
								  newtup.values, newtup.nulls);
	MemoryContextSwitchTo(oldctx);
	ExecStoreTuple(remotetuple, aestate->slot, InvalidBuffer, true);

	if (aestate->resultRelInfo->ri_TrigDesc &&
		aestate->resultRelInfo->ri_TrigDesc->trig_insert_before_row)
	{
		aestate->slot = ExecBRInsertTriggers(aestate->estate,
											 aestate->resultRelInfo,
											 aestate->slot);

		if (aestate->slot == NULL)		/* "do nothing" */
		{
			PopActiveSnapshot();
			finish_apply_exec_state(aestate);
			pglogical_relation_close(rel, NoLock);
			return;
		}

	}

	/* trigger might have changed tuple */
	remotetuple = ExecMaterializeSlot(aestate->slot);

	if (OidIsValid(conflicts))
	{
		/* Tuple already exists, try resolving conflict. */
		bool apply = try_resolve_conflict(rel->rel, localslot->tts_tuple,
										  remotetuple, &applytuple,
										  &resolution);

		pglogical_report_conflict(CONFLICT_INSERT_INSERT, rel->rel,
								  localslot->tts_tuple, remotetuple,
								  applytuple, resolution);

		if (apply)
		{
			if (applytuple != remotetuple)
				ExecStoreTuple(applytuple, aestate->slot, InvalidBuffer, false);

			if (aestate->resultRelInfo->ri_TrigDesc &&
				aestate->resultRelInfo->ri_TrigDesc->trig_update_before_row)
			{
				aestate->slot = ExecBRUpdateTriggers(aestate->estate,
													 &aestate->epqstate,
													 aestate->resultRelInfo,
													 &localslot->tts_tuple->t_self,
													 NULL,
													 aestate->slot);

				if (aestate->slot == NULL)		/* "do nothing" */
				{
					PopActiveSnapshot();
					finish_apply_exec_state(aestate);
					pglogical_relation_close(rel, NoLock);
				}

			}

			/* trigger might have changed tuple */
			remotetuple = ExecMaterializeSlot(aestate->slot);

			/* Check the constraints of the tuple */
			if (rel->rel->rd_att->constr)
				ExecConstraints(aestate->resultRelInfo, aestate->slot,
								aestate->estate);

			simple_heap_update(rel->rel, &localslot->tts_tuple->t_self,
							   aestate->slot->tts_tuple);

			if (!HeapTupleIsHeapOnly(aestate->slot->tts_tuple))
				recheckIndexes = UserTableUpdateOpenIndexes(aestate->estate,
															aestate->slot);

			/* AFTER ROW UPDATE Triggers */
			ExecARUpdateTriggers(aestate->estate, aestate->resultRelInfo,
								 &localslot->tts_tuple->t_self,
								 NULL, applytuple, recheckIndexes);
		}
	}
	else
	{
		/* Check the constraints of the tuple */
		if (rel->rel->rd_att->constr)
			ExecConstraints(aestate->resultRelInfo, aestate->slot,
							aestate->estate);

		simple_heap_insert(rel->rel, aestate->slot->tts_tuple);
		UserTableUpdateOpenIndexes(aestate->estate, aestate->slot);

		/* AFTER ROW INSERT Triggers */
		ExecARInsertTriggers(aestate->estate, aestate->resultRelInfo,
							 remotetuple, recheckIndexes);
	}

	ExecCloseIndices(aestate->resultRelInfo);

	PopActiveSnapshot();

	/* if INSERT was into our queue, process the message. */
	if (RelationGetRelid(rel->rel) == QueueRelid)
	{
		HeapTuple		ht;
		LockRelId		lockid = rel->rel->rd_lockInfo.lockRelId;
		TransactionId	oldxid = GetTopTransactionId();
		Relation		qrel;

		/*
		 * Release transaction bound resources for CONCURRENTLY support.
		 */
		MemoryContextSwitchTo(MessageContext);
		ht = heap_copytuple(aestate->slot->tts_tuple);

		finish_apply_exec_state(aestate);

		LockRelationIdForSession(&lockid, RowExclusiveLock);
		pglogical_relation_close(rel, NoLock);

		handle_queued_message(ht, started_tx);

		qrel = heap_open(QueueRelid, RowExclusiveLock);

		UnlockRelationIdForSession(&lockid, RowExclusiveLock);

		heap_close(qrel, NoLock);

		if (oldxid != GetTopTransactionId())
			CommitTransactionCommand();
	}
	else
	{
		/* Otherwise do normal cleanup. */
		finish_apply_exec_state(aestate);
		pglogical_relation_close(rel, NoLock);
	}

	CommandCounterIncrement();
}


static void
handle_update(StringInfo s)
{
	PGLogicalTupleData	oldtup;
	PGLogicalTupleData	newtup;
	PGLogicalTupleData *searchtup;
	PGLogicalRelation  *rel;
	ApplyExecState	   *aestate;
	bool				found;
	bool				hasoldtup;
	TupleTableSlot	   *localslot;
	HeapTuple			remotetuple;
	List			   *recheckIndexes = NIL;
	MemoryContext		oldctx;

	ensure_transaction();

	rel = pglogical_read_update(s, RowExclusiveLock, &hasoldtup, &oldtup,
								&newtup);

	/* If in list of relations which are being synchronized, skip. */
	if (check_syncing_relation(rel->nspname, rel->relname))
	{
		pglogical_relation_close(rel, NoLock);
		return;
	}

	/* Initialize the executor state. */
	aestate = init_apply_exec_state(rel);
	localslot = ExecInitExtraTupleSlot(aestate->estate);
	ExecSetSlotDescriptor(localslot, RelationGetDescr(rel->rel));

	PushActiveSnapshot(GetTransactionSnapshot());

	/* Search for existing tuple with same key */
	searchtup = hasoldtup ? &oldtup : &newtup;
	found = pglogical_tuple_find_replidx(aestate->estate, searchtup, localslot);

	/*
	 * Tuple found.
	 *
	 * Note this will fail if there are other conflicting unique indexes.
	 */
	if (found)
	{
		TransactionId	xmin;
		TimestampTz		local_ts;
		RepOriginId		local_origin;
		bool			apply;
		HeapTuple		applytuple;

		/* Process and store remote tuple in the slot */
		oldctx = MemoryContextSwitchTo(GetPerTupleMemoryContext(aestate->estate));
		fill_missing_defaults(rel, aestate->estate, &newtup);
		remotetuple = heap_modify_tuple(localslot->tts_tuple,
										RelationGetDescr(rel->rel),
										newtup.values,
										newtup.nulls,
										newtup.changed);
		MemoryContextSwitchTo(oldctx);
		ExecStoreTuple(remotetuple, aestate->slot, InvalidBuffer, true);


		if (aestate->resultRelInfo->ri_TrigDesc &&
			aestate->resultRelInfo->ri_TrigDesc->trig_update_before_row)
		{
			aestate->slot = ExecBRUpdateTriggers(aestate->estate,
												 &aestate->epqstate,
												 aestate->resultRelInfo,
												 &localslot->tts_tuple->t_self,
												 NULL, aestate->slot);

			if (aestate->slot == NULL)		/* "do nothing" */
			{
				PopActiveSnapshot();
				finish_apply_exec_state(aestate);
				pglogical_relation_close(rel, NoLock);
				return;
			}
		}

		/* trigger might have changed tuple */
		remotetuple = ExecMaterializeSlot(aestate->slot);

		get_tuple_origin(localslot->tts_tuple, &xmin, &local_origin,
						 &local_ts);

		/*
		 * If the local tuple was previously updated by different transaction
		 * on different server, consider this to be conflict and resolve it.
		 */
		if (xmin != GetTopTransactionId() &&
			local_origin != replorigin_session_origin)
		{
			PGLogicalConflictResolution resolution;

			apply = try_resolve_conflict(rel->rel, localslot->tts_tuple,
										 remotetuple, &applytuple,
										 &resolution);

			pglogical_report_conflict(CONFLICT_UPDATE_UPDATE, rel->rel,
									  localslot->tts_tuple, remotetuple,
									  applytuple, resolution);

			if (applytuple != remotetuple)
				ExecStoreTuple(applytuple, aestate->slot, InvalidBuffer, false);
		}
		else
		{
			apply = true;
			applytuple = remotetuple;
		}

		if (apply)
		{
			/* Check the constraints of the tuple */
			if (rel->rel->rd_att->constr)
				ExecConstraints(aestate->resultRelInfo, aestate->slot,
								aestate->estate);

			simple_heap_update(rel->rel, &localslot->tts_tuple->t_self,
							   aestate->slot->tts_tuple);

			/* Only update indexes if it's not HOT update. */
			if (!HeapTupleIsHeapOnly(aestate->slot->tts_tuple))
			{
				ExecOpenIndices(aestate->resultRelInfo
#if PG_VERSION_NUM >= 90500
								, false
#endif
							   );
				recheckIndexes = UserTableUpdateOpenIndexes(aestate->estate,
															aestate->slot);
				ExecCloseIndices(aestate->resultRelInfo);
			}

			/* AFTER ROW UPDATE Triggers */
			ExecARUpdateTriggers(aestate->estate, aestate->resultRelInfo,
								 &localslot->tts_tuple->t_self,
								 NULL, applytuple, recheckIndexes);
		}
	}
	else
	{
		/*
		 * The tuple to be updated could not be found.
		 *
		 * We can't do INSERT here because we might not have whole tuple.
		 */
		remotetuple = heap_form_tuple(RelationGetDescr(rel->rel),
									  newtup.values,
									  newtup.nulls);
		pglogical_report_conflict(CONFLICT_UPDATE_DELETE, rel->rel, NULL,
								  remotetuple, NULL, PGLogicalResolution_Skip);
	}

	/* Cleanup. */
	PopActiveSnapshot();
	finish_apply_exec_state(aestate);
	pglogical_relation_close(rel, NoLock);

	CommandCounterIncrement();
}

static void
handle_delete(StringInfo s)
{
	PGLogicalTupleData	oldtup;
	PGLogicalRelation  *rel;
	ApplyExecState	   *aestate;
	TupleTableSlot	   *localslot;

	ensure_transaction();

	rel = pglogical_read_delete(s, RowExclusiveLock, &oldtup);

	/* If in list of relations which are being synchronized, skip. */
	if (check_syncing_relation(rel->nspname, rel->relname))
	{
		pglogical_relation_close(rel, NoLock);
		return;
	}

	/* Initialize the executor state. */
	aestate = init_apply_exec_state(rel);
	localslot = ExecInitExtraTupleSlot(aestate->estate);
	ExecSetSlotDescriptor(localslot, RelationGetDescr(rel->rel));

	PushActiveSnapshot(GetTransactionSnapshot());

	if (pglogical_tuple_find_replidx(aestate->estate, &oldtup, localslot))
	{
		if (aestate->resultRelInfo->ri_TrigDesc &&
			aestate->resultRelInfo->ri_TrigDesc->trig_update_before_row)
		{
			bool dodelete = ExecBRDeleteTriggers(aestate->estate,
												 &aestate->epqstate,
												 aestate->resultRelInfo,
												 &localslot->tts_tuple->t_self,
												 NULL);

			if (!dodelete)		/* "do nothing" */
			{
				PopActiveSnapshot();
				finish_apply_exec_state(aestate);
				pglogical_relation_close(rel, NoLock);
				return;
			}
		}

		/* Tuple found, delete it. */
		simple_heap_delete(rel->rel, &localslot->tts_tuple->t_self);

		/* AFTER ROW DELETE Triggers */
		ExecARDeleteTriggers(aestate->estate, aestate->resultRelInfo,
							 &localslot->tts_tuple->t_self, NULL);
	}
	else
	{
		/* The tuple to be deleted could not be found. */
		HeapTuple remotetuple = heap_form_tuple(RelationGetDescr(rel->rel),
												oldtup.values, oldtup.nulls);
		pglogical_report_conflict(CONFLICT_DELETE_DELETE, rel->rel, NULL,
								  remotetuple, NULL, PGLogicalResolution_Skip);
	}

	/* Cleanup. */
	PopActiveSnapshot();
	finish_apply_exec_state(aestate);
	pglogical_relation_close(rel, NoLock);

	CommandCounterIncrement();
}

inline static bool
getmsgisend(StringInfo msg)
{
	return msg->cursor == msg->len;
}

static void
handle_startup(StringInfo s)
{
	uint8 msgver = pq_getmsgbyte(s);
	if (msgver != 1)
		elog(ERROR, "Expected startup message version 1, but got %u", msgver);

	/*
	 * The startup message consists of null-terminated strings as key/value
	 * pairs. The first entry is always the format identifier.
	 */
	do {
		const char *k, *v;

		k = pq_getmsgstring(s);
		if (strlen(k) == 0)
			ereport(ERROR,
					(errcode(ERRCODE_PROTOCOL_VIOLATION),
					 errmsg("invalid startup message: key has zero length")));

		if (getmsgisend(s))
			ereport(ERROR,
					(errcode(ERRCODE_PROTOCOL_VIOLATION),
					 errmsg("invalid startup message: key '%s' has no following value", k)));

		/* It's OK to have a zero length value */
		v = pq_getmsgstring(s);

		handle_startup_param(k, v);
	} while (!getmsgisend(s));
}

static bool
parse_bool_param(const char *key, const char *value)
{
	bool result;

	if (!parse_bool(value, &result))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("couldn't parse value '%s' for key '%s' as boolean",
						value, key)));

	return result;
}

static void
handle_startup_param(const char *key, const char *value)
{
	elog(DEBUG2, "apply got pglogical startup msg param  %s=%s", key, value);

	if (strcmp(key, "pg_version") == 0)
		elog(DEBUG1, "upstream Pg version is %s", value);

	if (strcmp(key, "encoding") == 0)
	{
		int encoding = pg_char_to_encoding(value);

		if (encoding != GetDatabaseEncoding())
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("expected encoding=%s from upstream but got %s",
						 GetDatabaseEncodingName(), value)));
	}

	if (strcmp(key, "forward_changeset_origins") == 0)
	{
		bool fwd = parse_bool_param(key, value);
		/* FIXME: Store this somewhere */
		elog(DEBUG1, "changeset origin forwarding enabled: %s", fwd ? "t" : "f");
	}

	if (strcmp(key, "hooks.startup_hook_enabled") == 0)
	{
		if (!parse_bool_param(key, value))
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("pglogical requested a startup hook, but it was not activated"),
					 errdetail("hooks.startup_hook_enabled='f' returned by upstream")));
	}

	if (strcmp(key, "hooks.row_filter_enabled") == 0)
	{
		if (!parse_bool_param(key, value))
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("pglogical requested a row filter hook, but it was not activated"),
					 errdetail("hooks.startup_hook_enabled='f' returned by upstream")));
	}

	if (strcmp(key, "hooks.transaction_filter_enabled") == 0)
	{
		if (!parse_bool_param(key, value))
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("pglogical requested a transaction filter hook, but it was not activated"),
					 errdetail("hooks.startup_hook_enabled='f' returned by upstream")));
	}

	/*
	 * We just ignore a bunch of parameters here because we specify what we
	 * require when we send our params to the upstream. It's required to ERROR
	 * if it can't match what we asked for. It may send the startup message
	 * first, but it'll be followed by an ERROR if it does. There's no need
	 * to check params we can't do anything about mismatches of, like protocol
	 * versions and type sizes.
	 */
}

static RangeVar *
parse_relation_message(Jsonb *message)
{
	JsonbIterator  *it;
	JsonbValue		v;
	int				r;
	int				level = 0;
	char		   *key = NULL;
	char		  **parse_res = NULL;
	char		   *nspname = NULL;
	char		   *relname = NULL;

	/* Parse and validate the json message. */
	if (!JB_ROOT_IS_OBJECT(message))
		elog(ERROR, "malformed message in queued message tuple: root is not object");

	it = JsonbIteratorInit(&message->root);
	while ((r = JsonbIteratorNext(&it, &v, false)) != WJB_DONE)
	{
		if (level == 0 && r != WJB_BEGIN_OBJECT)
			elog(ERROR, "root element needs to be an object");
		else if (level == 0 && r == WJB_BEGIN_OBJECT)
		{
			level++;
		}
		else if (level == 1 && r == WJB_KEY)
		{
			if (strncmp(v.val.string.val, "schema_name", v.val.string.len) == 0)
				parse_res = &nspname;
			else if (strncmp(v.val.string.val, "table_name", v.val.string.len) == 0)
				parse_res = &relname;
			else
				elog(ERROR, "unexpected key: %s",
					 pnstrdup(v.val.string.val, v.val.string.len));

			key = v.val.string.val;
		}
		else if (level == 1 && r == WJB_VALUE)
		{
			if (!key)
				elog(ERROR, "in wrong state when parsing key");

			if (v.type != jbvString)
				elog(ERROR, "unexpected type for key '%s': %u", key, v.type);

			*parse_res = pnstrdup(v.val.string.val, v.val.string.len);
		}
		else if (level == 1 && r != WJB_END_OBJECT)
		{
			elog(ERROR, "unexpected content: %u at level %d", r, level);
		}
		else if (r == WJB_END_OBJECT)
		{
			level--;
			parse_res = NULL;
			key = NULL;
		}
		else
			elog(ERROR, "unexpected content: %u at level %d", r, level);

	}

	/* Check if we got both schema and table names. */
	if (!nspname)
		elog(ERROR, "missing schema_name in relation message");

	if (!relname)
		elog(ERROR, "missing table_name in relation message");

	return makeRangeVar(nspname, relname, -1);
}

/*
 * Handle TRUNCATE message comming via queue table.
 */
static void
handle_truncate(QueuedMessage *queued_message)
{
	RangeVar	   *rv;

	/*
	 * If table doesn't exist locally, it can't be subscribed.
	 *
	 * TODO: should we error here?
	 */
	rv = parse_relation_message(queued_message->message);

	/* If in list of relations which are being synchronized, skip. */
	if (check_syncing_relation(rv->schemaname, rv->relname))
		return;

	truncate_table(rv->schemaname, rv->relname);
}

/*
 * Handle TABLESYNC message comming via queue table.
 */
static void
handle_table_sync(QueuedMessage *queued_message)
{
	RangeVar	   *rv;
	MemoryContext			oldcontext;
	PGLogicalSyncStatus		*oldsync;
	PGLogicalSyncStatus		newsync;

	rv = parse_relation_message(queued_message->message);

	oldsync = get_table_sync_status(MyApplyWorker->subid, rv->schemaname,
									rv->relname, true);

	if (oldsync)
	{
		elog(INFO,
			 "table sync came from queue for table %s.%s which already being synchronized, skipping",
			 rv->schemaname, rv->relname);

		return;
	}

	/*
	 * Create synchronization status for the subscription.
	 * Currently we only support data sync for tables.
	 */
	newsync.kind = SYNC_KIND_DATA;
	newsync.subid = MyApplyWorker->subid;
	newsync.nspname = rv->schemaname;
	newsync.relname = rv->relname;
	newsync.status = SYNC_STATUS_INIT;
	create_local_sync_status(&newsync);

	/* Keep the lists persistent. */
	oldcontext = MemoryContextSwitchTo(TopMemoryContext);
	SyncingTables = lappend(SyncingTables,
							makeRangeVar(pstrdup(rv->schemaname),
										 pstrdup(rv->relname), -1));
	MemoryContextSwitchTo(oldcontext);
}

/*
 * Handle SEQUENCE message comming via queue table.
 */
static void
handle_sequence(QueuedMessage *queued_message)
{
	Jsonb		   *message = queued_message->message;
	JsonbIterator  *it;
	JsonbValue		v;
	int				r;
	int				level = 0;
	char		   *key = NULL;
	char		  **parse_res = NULL;
	char		   *nspname = NULL;
	char		   *relname = NULL;
	char		   *last_value_raw = NULL;
	int64			last_value;
	Oid				nspoid;
	Oid				reloid;

	/* Parse and validate the json message. */
	if (!JB_ROOT_IS_OBJECT(message))
		elog(ERROR, "malformed message in queued message tuple: root is not object");

	it = JsonbIteratorInit(&message->root);
	while ((r = JsonbIteratorNext(&it, &v, false)) != WJB_DONE)
	{
		if (level == 0 && r != WJB_BEGIN_OBJECT)
			elog(ERROR, "root element needs to be an object");
		else if (level == 0 && r == WJB_BEGIN_OBJECT)
		{
			level++;
		}
		else if (level == 1 && r == WJB_KEY)
		{
			if (strncmp(v.val.string.val, "schema_name", v.val.string.len) == 0)
				parse_res = &nspname;
			else if (strncmp(v.val.string.val, "sequence_name", v.val.string.len) == 0)
				parse_res = &relname;
			else if (strncmp(v.val.string.val, "last_value", v.val.string.len) == 0)
				parse_res = &last_value_raw;
			else
				elog(ERROR, "unexpected key: %s",
					 pnstrdup(v.val.string.val, v.val.string.len));

			key = v.val.string.val;
		}
		else if (level == 1 && r == WJB_VALUE)
		{
			if (!key)
				elog(ERROR, "in wrong state when parsing key");

			if (v.type != jbvString)
				elog(ERROR, "unexpected type for key '%s': %u", key, v.type);

			*parse_res = pnstrdup(v.val.string.val, v.val.string.len);
		}
		else if (level == 1 && r != WJB_END_OBJECT)
		{
			elog(ERROR, "unexpected content: %u at level %d", r, level);
		}
		else if (r == WJB_END_OBJECT)
		{
			level--;
			parse_res = NULL;
			key = NULL;
		}
		else
			elog(ERROR, "unexpected content: %u at level %d", r, level);

	}

	/* Check if we got both schema and table names. */
	if (!nspname)
		elog(ERROR, "missing schema_name in sequence message");

	if (!relname)
		elog(ERROR, "missing table_name in sequence message");

	if (!last_value_raw)
		elog(ERROR, "missing last_value in sequence message");

	nspoid = get_namespace_oid(nspname, false);
	reloid = get_relname_relid(relname, nspoid);
	scanint8(last_value_raw, false, &last_value);

	DirectFunctionCall2(setval_oid, ObjectIdGetDatum(reloid),
						Int64GetDatum(last_value));
}
/*
 * Handle SQL message comming via queue table.
 */
static void
handle_sql(QueuedMessage *queued_message, bool tx_just_started)
{
	JsonbIterator *it;
	JsonbValue	v;
	int			r;
	char	   *sql;

	/* Validate the json and extract the SQL string from it. */
	if (!JB_ROOT_IS_SCALAR(queued_message->message))
		elog(ERROR, "malformed message in queued message tuple: root is not scalar");

	it = JsonbIteratorInit(&queued_message->message->root);
	r = JsonbIteratorNext(&it, &v, false);
	if (r != WJB_BEGIN_ARRAY)
		elog(ERROR, "malformed message in queued message tuple, item type %d expected %d", r, WJB_BEGIN_ARRAY);

	r = JsonbIteratorNext(&it, &v, false);
	if (r != WJB_ELEM)
		elog(ERROR, "malformed message in queued message tuple, item type %d expected %d", r, WJB_ELEM);

	if (v.type != jbvString)
		elog(ERROR, "malformed message in queued message tuple, expected value type %d got %d", jbvString, v.type);

	sql = pnstrdup(v.val.string.val, v.val.string.len);

	r = JsonbIteratorNext(&it, &v, false);
	if (r != WJB_END_ARRAY)
		elog(ERROR, "malformed message in queued message tuple, item type %d expected %d", r, WJB_END_ARRAY);

	r = JsonbIteratorNext(&it, &v, false);
	if (r != WJB_DONE)
		elog(ERROR, "malformed message in queued message tuple, item type %d expected %d", r, WJB_DONE);

	/* Run the extracted SQL. */
	pglogical_execute_sql_command(sql, queued_message->role, tx_just_started);
}

/*
 * Handles messages comming from the queue.
 */
static void
handle_queued_message(HeapTuple msgtup, bool tx_just_started)
{
	QueuedMessage  *queued_message = queued_message_from_tuple(msgtup);

	switch (queued_message->message_type)
	{
		case QUEUE_COMMAND_TYPE_SQL:
			handle_sql(queued_message, tx_just_started);
			break;
		case QUEUE_COMMAND_TYPE_TRUNCATE:
			handle_truncate(queued_message);
			break;
		case QUEUE_COMMAND_TYPE_TABLESYNC:
			handle_table_sync(queued_message);
			break;
		case QUEUE_COMMAND_TYPE_SEQUENCE:
			handle_sequence(queued_message);
			break;
		default:
			elog(ERROR, "unknown message type '%c'",
				 queued_message->message_type);
	}
}

static void
replication_handler(StringInfo s)
{
	char action = pq_getmsgbyte(s);

	switch (action)
	{
		/* BEGIN */
		case 'B':
			handle_begin(s);
			break;
		/* COMMIT */
		case 'C':
			handle_commit(s);
			break;
		/* ORIGIN */
		case 'O':
			handle_origin(s);
			break;
		/* RELATION */
		case 'R':
			handle_relation(s);
			break;
		/* INSERT */
		case 'I':
			handle_insert(s);
			break;
		/* UPDATE */
		case 'U':
			handle_update(s);
			break;
		/* DELETE */
		case 'D':
			handle_delete(s);
			break;
		/* STARTUP MESSAGE */
		case 'S':
			handle_startup(s);
			break;
		default:
			elog(ERROR, "unknown action of type %c", action);
	}
}

/*
 * Figure out which write/flush positions to report to the walsender process.
 *
 * We can't simply report back the last LSN the walsender sent us because the
 * local transaction might not yet be flushed to disk locally. Instead we
 * build a list that associates local with remote LSNs for every commit. When
 * reporting back the flush position to the sender we iterate that list and
 * check which entries on it are already locally flushed. Those we can report
 * as having been flushed.
 *
 * Returns true if there's no outstanding transactions that need to be
 * flushed.
 */
static bool
get_flush_position(XLogRecPtr *write, XLogRecPtr *flush)
{
	dlist_mutable_iter iter;
	XLogRecPtr	local_flush = GetFlushRecPtr();

	*write = InvalidXLogRecPtr;
	*flush = InvalidXLogRecPtr;

	dlist_foreach_modify(iter, &lsn_mapping)
	{
		PGLFlushPosition *pos =
			dlist_container(PGLFlushPosition, node, iter.cur);

		*write = pos->remote_end;

		if (pos->local_end <= local_flush)
		{
			*flush = pos->remote_end;
			dlist_delete(iter.cur);
			pfree(pos);
		}
		else
		{
			/*
			 * Don't want to uselessly iterate over the rest of the list which
			 * could potentially be long. Instead get the last element and
			 * grab the write position from there.
			 */
			pos = dlist_tail_element(PGLFlushPosition, node,
									 &lsn_mapping);
			*write = pos->remote_end;
			return false;
		}
	}

	return dlist_is_empty(&lsn_mapping);
}

/*
 * Send a Standby Status Update message to server.
 *
 * 'recvpos' is the latest LSN we've received data to, force is set if we need
 * to send a response to avoid timeouts.
 */
static bool
send_feedback(PGconn *conn, XLogRecPtr recvpos, int64 now, bool force)
{
	static StringInfo	reply_message = NULL;

	static XLogRecPtr last_recvpos = InvalidXLogRecPtr;
	static XLogRecPtr last_writepos = InvalidXLogRecPtr;
	static XLogRecPtr last_flushpos = InvalidXLogRecPtr;

	XLogRecPtr writepos;
	XLogRecPtr flushpos;

	/* It's legal to not pass a recvpos */
	if (recvpos < last_recvpos)
		recvpos = last_recvpos;

	if (get_flush_position(&writepos, &flushpos))
	{
		/*
		 * No outstanding transactions to flush, we can report the latest
		 * received position. This is important for synchronous replication.
		 */
		flushpos = writepos = recvpos;
	}

	if (writepos < last_writepos)
		writepos = last_writepos;

	if (flushpos < last_flushpos)
		flushpos = last_flushpos;

	/* if we've already reported everything we're good */
	if (!force &&
		writepos == last_writepos &&
		flushpos == last_flushpos)
		return true;

	if (!reply_message)
	{
		MemoryContext	oldcontext = MemoryContextSwitchTo(TopMemoryContext);
		reply_message = makeStringInfo();
		MemoryContextSwitchTo(oldcontext);
	}
	else
		resetStringInfo(reply_message);

	pq_sendbyte(reply_message, 'r');
	pq_sendint64(reply_message, recvpos);		/* write */
	pq_sendint64(reply_message, flushpos);		/* flush */
	pq_sendint64(reply_message, writepos);		/* apply */
	pq_sendint64(reply_message, now);			/* sendTime */
	pq_sendbyte(reply_message, false);			/* replyRequested */

	elog(DEBUG2, "sending feedback (force %d) to recv %X/%X, write %X/%X, flush %X/%X",
		 force,
		 (uint32) (recvpos >> 32), (uint32) recvpos,
		 (uint32) (writepos >> 32), (uint32) writepos,
		 (uint32) (flushpos >> 32), (uint32) flushpos
		);

	if (PQputCopyData(conn, reply_message->data, reply_message->len) <= 0 ||
		PQflush(conn))
	{
		ereport(ERROR,
				(errcode(ERRCODE_CONNECTION_FAILURE),
				 errmsg("could not send feedback packet: %s",
						PQerrorMessage(conn))));
		return false;
	}

	if (recvpos > last_recvpos)
		last_recvpos = recvpos;
	if (writepos > last_writepos)
		last_writepos = writepos;
	if (flushpos > last_flushpos)
		last_flushpos = flushpos;

	return true;
}

/*
 * Apply main loop.
 */
void
apply_work(PGconn *streamConn)
{
	int			fd;
	char	   *copybuf = NULL;
	XLogRecPtr	last_received = InvalidXLogRecPtr;

	applyconn = streamConn;
	fd = PQsocket(applyconn);

	/* Init the MessageContext which we use for easier cleanup. */
	MessageContext = AllocSetContextCreate(TopMemoryContext,
										   "MessageContext",
										   ALLOCSET_DEFAULT_MINSIZE,
										   ALLOCSET_DEFAULT_INITSIZE,
										   ALLOCSET_DEFAULT_MAXSIZE);

	/* mark as idle, before starting to loop */
	pgstat_report_activity(STATE_IDLE, NULL);

	while (!got_SIGTERM)
	{
		int			rc;
		int			r;

		/*
		 * Background workers mustn't call usleep() or any direct equivalent:
		 * instead, they may wait on their process latch, which sleeps as
		 * necessary, but is awakened if postmaster dies.  That way the
		 * background process goes away immediately in an emergency.
		 */
		rc = WaitLatchOrSocket(&MyProc->procLatch,
							   WL_SOCKET_READABLE | WL_LATCH_SET |
							   WL_TIMEOUT | WL_POSTMASTER_DEATH,
							   fd, 1000L);

		ResetLatch(&MyProc->procLatch);

		MemoryContextSwitchTo(MessageContext);

		/* emergency bailout if postmaster has died */
		if (rc & WL_POSTMASTER_DEATH)
			proc_exit(1);

		if (PQstatus(applyconn) == CONNECTION_BAD)
		{
			elog(ERROR, "connection to other side has died");
		}

		if (rc & WL_SOCKET_READABLE)
			PQconsumeInput(applyconn);

		for (;;)
		{
			if (got_SIGTERM)
				break;

			if (copybuf != NULL)
			{
				PQfreemem(copybuf);
				copybuf = NULL;
			}

			r = PQgetCopyData(applyconn, &copybuf, 1);

			if (r == -1)
			{
				elog(ERROR, "data stream ended");
			}
			else if (r == -2)
			{
				elog(ERROR, "could not read COPY data: %s",
					 PQerrorMessage(applyconn));
			}
			else if (r < 0)
				elog(ERROR, "invalid COPY status %d", r);
			else if (r == 0)
			{
				/* need to wait for new data */
				break;
			}
			else
			{
				int c;
				StringInfoData s;

				MemoryContextSwitchTo(MessageContext);

				initStringInfo(&s);
				s.data = copybuf;
				s.len = r;
				s.maxlen = -1;

				c = pq_getmsgbyte(&s);

				if (c == 'w')
				{
					XLogRecPtr	start_lsn;
					XLogRecPtr	end_lsn;

					start_lsn = pq_getmsgint64(&s);
					end_lsn = pq_getmsgint64(&s);
					pq_getmsgint64(&s); /* sendTime */

					if (last_received < start_lsn)
						last_received = start_lsn;

					if (last_received < end_lsn)
						last_received = end_lsn;

					replication_handler(&s);
				}
				else if (c == 'k')
				{
					XLogRecPtr endpos;
					bool reply_requested;

					endpos = pq_getmsgint64(&s);
					/* timestamp = */ pq_getmsgint64(&s);
					reply_requested = pq_getmsgbyte(&s);

					send_feedback(applyconn, endpos,
								  GetCurrentTimestamp(),
								  reply_requested);
				}
				/* other message types are purposefully ignored */
			}
		}

		/* confirm all writes at once */
		send_feedback(applyconn, last_received, GetCurrentTimestamp(), false);

		if (!in_remote_transaction)
			process_syncing_tables(last_received);

		/* Cleanup the memory. */
		MemoryContextResetAndDeleteChildren(MessageContext);
	}
}

/*
 * Add context to the errors produced by pglogical_execute_sql_command().
 */
static void
execute_sql_command_error_cb(void *arg)
{
	errcontext("during execution of queued SQL statement: %s", (char *) arg);
}

/*
 * Execute an SQL command. This can be multiple multiple queries.
 */
void
pglogical_execute_sql_command(char *cmdstr, char *role, bool isTopLevel)
{
	List	   *commands;
	ListCell   *command_i;
	MemoryContext oldcontext;
	ErrorContextCallback errcallback;

	oldcontext = MemoryContextSwitchTo(MessageContext);

	errcallback.callback = execute_sql_command_error_cb;
	errcallback.arg = cmdstr;
	errcallback.previous = error_context_stack;
	error_context_stack = &errcallback;

	commands = pg_parse_query(cmdstr);

	MemoryContextSwitchTo(oldcontext);

	/*
	 * Do a limited amount of safety checking against CONCURRENTLY commands
	 * executed in situations where they aren't allowed. The sender side should
	 * provide protection, but better be safe than sorry.
	 */
	isTopLevel = isTopLevel && (list_length(commands) == 1);

	foreach(command_i, commands)
	{
		List	   *plantree_list;
		List	   *querytree_list;
		Node	   *command = (Node *) lfirst(command_i);
		const char *commandTag;
		Portal		portal;
		DestReceiver *receiver;

		/* temporarily push snapshot for parse analysis/planning */
		PushActiveSnapshot(GetTransactionSnapshot());

		oldcontext = MemoryContextSwitchTo(MessageContext);

		/*
		 * Set the current role to the user that executed the command on the
		 * origin server.  NB: there is no need to reset this afterwards, as
		 * the value will be gone with our transaction.
		 */
		SetConfigOption("role", role, PGC_INTERNAL, PGC_S_OVERRIDE);

		commandTag = CreateCommandTag(command);

		querytree_list = pg_analyze_and_rewrite(
			command, cmdstr, NULL, 0);

		plantree_list = pg_plan_queries(
			querytree_list, 0, NULL);

		PopActiveSnapshot();

		portal = CreatePortal("pglogical", true, true);
		PortalDefineQuery(portal, NULL,
						  cmdstr, commandTag,
						  plantree_list, NULL);
		PortalStart(portal, NULL, 0, InvalidSnapshot);

		receiver = CreateDestReceiver(DestNone);

		(void) PortalRun(portal, FETCH_ALL,
						 isTopLevel,
						 receiver, receiver,
						 NULL);
		(*receiver->rDestroy) (receiver);

		PortalDrop(portal, false);

		CommandCounterIncrement();

		MemoryContextSwitchTo(oldcontext);
	}

	/* protect against stack resets during CONCURRENTLY processing */
	if (error_context_stack == &errcallback)
		error_context_stack = errcallback.previous;
}

/*
 * Load list of tables currently pending sync.
 *
 * Must be inside transaction.
 */
static void
reread_unsynced_tables(Oid subid)
{
	MemoryContext	saved_ctx;
	List		   *unsynced_tables;
	ListCell	   *lc;

	/* Cleanup first. */
	if (list_length(SyncingTables) > 0)
	{
		ListCell	   *next;

		for (lc = list_head(SyncingTables); lc; lc = next)
		{
			RangeVar	   *rv = (RangeVar *) lfirst(lc);

			next = lnext(lc);

			pfree(rv->schemaname);
			pfree(rv->relname);
			pfree(rv);
			pfree(lc);
		}

		pfree(SyncingTables);
		SyncingTables = NIL;

	}

	/* Read new state. */
	unsynced_tables = get_unsynced_tables(subid);
	saved_ctx = MemoryContextSwitchTo(TopMemoryContext);
	foreach (lc, unsynced_tables)
	{
		RangeVar	   *rv = lfirst(lc);
		SyncingTables = lappend(SyncingTables,
								makeRangeVar(pstrdup(rv->schemaname),
											 pstrdup(rv->relname), -1));
	}

	MemoryContextSwitchTo(saved_ctx);
}

static void
process_syncing_tables(XLogRecPtr end_lsn)
{
	/* First check if we need to update the cached information. */
	if (MyApplyWorker->sync_pending)
	{
		StartTransactionCommand();
		MyApplyWorker->sync_pending = false;
		reread_unsynced_tables(MyApplyWorker->subid);
		CommitTransactionCommand();
	}

	/* Process currently pending sync tables. */
	if (list_length(SyncingTables) > 0)
	{
		ListCell	   *lc,
					   *prev,
					   *next;

		prev = NULL;
		for (lc = list_head(SyncingTables); lc; lc = next)
		{
			RangeVar	   *rv = (RangeVar *) lfirst(lc);
			PGLogicalSyncStatus	   *sync;
			char					status;

			/* We might delete the cell so advance it now. */
			next = lnext(lc);

			StartTransactionCommand();
			sync = get_table_sync_status(MyApplyWorker->subid, rv->schemaname,
										 rv->relname, true);

			/*
			 * TODO: what to do here? We don't really want to die,
			 * but this can mean many things, for now we just assume table is
			 * not relevant for us anymore and leave fixing to the user.
			 */
			if (!sync)
				status = SYNC_STATUS_READY;
			else
				status = sync->status;
			CommitTransactionCommand();

			if (status == SYNC_STATUS_SYNCWAIT)
			{
				PGLogicalWorker *worker;

				LWLockAcquire(PGLogicalCtx->lock, LW_EXCLUSIVE);
				worker = pglogical_sync_find(MyDatabaseId,
											 MyApplyWorker->subid,
											 rv->schemaname, rv->relname);
				if (pglogical_worker_running(worker) &&
					end_lsn >= worker->worker.apply.replay_stop_lsn)
				{
					worker->worker.apply.replay_stop_lsn = end_lsn;

					StartTransactionCommand();
					set_table_sync_status(MyApplyWorker->subid, rv->schemaname,
										  rv->relname, SYNC_STATUS_CATCHUP);
					CommitTransactionCommand();

					if (pglogical_worker_running(worker))
						SetLatch(&worker->proc->procLatch);
					LWLockRelease(PGLogicalCtx->lock);

					if (wait_for_sync_status_change(MyApplyWorker->subid,
													rv->schemaname,
													rv->relname,
													SYNC_STATUS_READY))
						status = SYNC_STATUS_READY;
				}
				else
					LWLockRelease(PGLogicalCtx->lock);
			}

			/* Ready? Remove it from local cache. */
			if (status == SYNC_STATUS_READY)
			{
				SyncingTables = list_delete_cell(SyncingTables, lc, prev);
				pfree(rv->schemaname);
				pfree(rv->relname);
				pfree(rv);
			}
			else
				prev = lc;
		}
	}

	/*
	 * If there are still pending tables for syncrhonization, launch the sync
	 * worker.
	 */
	if (list_length(SyncingTables) > 0)
	{
		List		   *workers;
		ListCell	   *lc;
		int				nworkers = 0;

		LWLockAcquire(PGLogicalCtx->lock, LW_EXCLUSIVE);
		workers = pglogical_sync_find_all(MyDatabaseId, MyApplyWorker->subid);
		foreach (lc, workers)
		{
			PGLogicalWorker	   *worker = (PGLogicalWorker *) lfirst(lc);

			if (pglogical_worker_running(worker))
				nworkers++;
		}
		LWLockRelease(PGLogicalCtx->lock);

		if (nworkers < 1)
		{
			RangeVar	   *rv = linitial(SyncingTables);

			start_sync_worker(rv);
		}
	}
}

static void
start_sync_worker(RangeVar *rv)
{
	PGLogicalWorker			worker;

	/* Start the sync worker. */
	memset(&worker, 0, sizeof(PGLogicalWorker));
	worker.worker_type = PGLOGICAL_WORKER_SYNC;
	worker.dboid = MyPGLogicalWorker->dboid;
	worker.worker.apply.subid = MyApplyWorker->subid;
	worker.worker.apply.sync_pending = false; /* Makes no sense for sync worker. */

	/* Tell the worker to stop at current position. */
	worker.worker.sync.apply.replay_stop_lsn = replorigin_session_origin_lsn;
	namestrcpy(&worker.worker.sync.nspname, rv->schemaname);
	namestrcpy(&worker.worker.sync.relname, rv->relname);

	(void) pglogical_worker_register(&worker);
}

void
pglogical_apply_main(Datum main_arg)
{
	int				slot = DatumGetInt32(main_arg);
	PGconn		   *streamConn;
	RepOriginId		originid;
	XLogRecPtr		origin_startpos;
	MemoryContext	saved_ctx;
	char		   *repsets;
	char		   *origins;

	/* Setup shmem. */
	pglogical_worker_attach(slot, PGLOGICAL_WORKER_APPLY);
	Assert(MyPGLogicalWorker->worker_type == PGLOGICAL_WORKER_APPLY);
	MyApplyWorker = &MyPGLogicalWorker->worker.apply;

	/* Establish signal handlers. */
	pqsignal(SIGTERM, handle_sigterm);
	BackgroundWorkerUnblockSignals();

	/* Attach to dsm segment. */
	Assert(CurrentResourceOwner == NULL);
	CurrentResourceOwner = ResourceOwnerCreate(NULL, "pglogical apply");

	/* Connect to our database. */
	BackgroundWorkerInitializeConnectionByOid(MyPGLogicalWorker->dboid, InvalidOid);

	/* Setup synchronous commit according to the user's wishes */
	SetConfigOption("synchronous_commit",
					pglogical_synchronous_commit ? "local" : "off",
					PGC_BACKEND, PGC_S_OVERRIDE);	/* other context? */

	/* Run as replica session replication role. */
	SetConfigOption("session_replication_role", "replica",
					PGC_SUSET, PGC_S_OVERRIDE);	/* other context? */

	/*
	 * Disable function body checks during replay. That's necessary because a)
	 * the creator of the function might have had it disabled b) the function
	 * might be search_path dependant and we don't fix the contents of
	 * functions.
	 */
	SetConfigOption("check_function_bodies", "off",
					PGC_INTERNAL, PGC_S_OVERRIDE);

	/* Load the subscription. */
	StartTransactionCommand();
	saved_ctx = MemoryContextSwitchTo(TopMemoryContext);
	MySubscription = get_subscription(MyApplyWorker->subid);
	MemoryContextSwitchTo(saved_ctx);
	CommitTransactionCommand();

	elog(LOG, "starting apply for subscription %s", MySubscription->name);

	/* If the subscription isn't initialized yet, initialize it. */
	pglogical_sync_subscription(MySubscription);

	elog(DEBUG1, "connecting to provider %s, dsn %s",
		 MySubscription->origin->name, MySubscription->origin_if->dsn);

	/*
	 * Cache the queue relation id.
	 * TODO: invalidation
	 */
	StartTransactionCommand();
	QueueRelid = get_queue_table_oid();

	originid = replorigin_by_name(MySubscription->slot_name, false);
	elog(DEBUG2, "setting up replication origin %s (oid %u)",
		MySubscription->slot_name, originid);
	replorigin_session_setup(originid);
	replorigin_session_origin = originid;
	origin_startpos = replorigin_session_get_progress(false);

	/* Start the replication. */
	streamConn = pglogical_connect_replica(MySubscription->origin_if->dsn,
										   MySubscription->name, NULL);

	repsets = stringlist_to_identifierstr(MySubscription->replication_sets);
	origins = stringlist_to_identifierstr(MySubscription->forward_origins);

	/*
	 * IDENTIFY_SYSTEM sets up some internal state on walsender so call it even
	 * if we don't (yet) want to use any of the results.
     */
	pglogical_identify_system(streamConn, NULL, NULL, NULL, NULL);

	pglogical_start_replication(streamConn, MySubscription->slot_name,
								origin_startpos, origins, repsets, NULL);
	pfree(repsets);

	CommitTransactionCommand();

	apply_work(streamConn);

	PQfinish(streamConn);

	/* We should only get here if we received sigTERM */
	proc_exit(0);
}
