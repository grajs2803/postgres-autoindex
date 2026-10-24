#include "postgres.h"
#include <math.h>
#include "access/parallel.h"
#include "access/stratnum.h"
#include "catalog/catalog.h"
#include "catalog/pg_am_d.h"
#include "catalog/pg_amop.h"
#include "catalog/pg_class.h"
#include "executor/autoindex.h"
#include "executor/executor.h"
#include "nodes/nodeFuncs.h"
#include "nodes/primnodes.h"
#include "utils/catcache.h"
#include "utils/rel.h"
#include "utils/syscache.h"
#include "access/htup_details.h"
#include "catalog/pg_index.h"
#include "miscadmin.h"
#include "optimizer/optimizer.h"
#include "storage/lwlock.h"
#include "storage/shmem.h"
#include "utils/builtins.h"
#include "utils/hsearch.h"
#include "utils/lsyscache.h"
#include "utils/relcache.h"
#include "access/relation.h"
#include "access/xact.h"
#include "pgstat.h"
#include "postmaster/bgworker.h"
#include "postmaster/interrupt.h"
#include "storage/latch.h"
#include "tcop/tcopprot.h"
#include "utils/guc.h"
#include "utils/snapmgr.h"
#include "utils/wait_event.h"
#include "tcop/dest.h"
#include "tcop/utility.h"
#include "utils/memutils.h"
#include "access/itup.h"
#include "access/nbtree.h"
#include "executor/spi.h"
#include "storage/bufpage.h"




/* GUCs */
bool		auto_index_enabled = true;
int			auto_index_min_scans = 10;
char	   *auto_index_database = NULL;
int			auto_index_max_per_table = 3;
int			auto_index_max_size_mb = 1024;

/* Every index we create is named autoidx_<tableoid>_<attno>. */
#define AUTOINDEX_NAME_PREFIX	"autoidx_"

/* After this many failed builds of the same candidate, give up for good. */
#define AUTOINDEX_MAX_FAILURES	3

/* How often the worker wakes up to look for work. */
#define AUTOINDEX_NAPTIME_MS	10000

#define AUTOINDEX_MAX_ENTRIES	1024
#define AUTOINDEX_BTREE_FANOUT	256.0

/* Hash compares keys with memcmp, so padding must be zeroed: always memset. */
typedef struct AutoIndexKey
{
	Oid			dbid;
	Oid			relid;
	AttrNumber	attno;
} AutoIndexKey;

typedef struct AutoIndexEntry
{
	AutoIndexKey key;			/* must be first */
	int64		nscans;
	double		total_savings;
	double		build_cost;
	bool		recommended;
	bool		attempted;		/* worker has tried to build it (never retry) */
	int			failures;		/* failed builds so far (drives backoff) */
} AutoIndexEntry;

typedef struct AutoIndexSharedState
{
	LWLock		lock;
} AutoIndexSharedState;

static AutoIndexSharedState *AutoIndexState = NULL;
static HTAB *AutoIndexHash = NULL;

Size
AutoIndexShmemSize(void)
{
	Size		size = MAXALIGN(sizeof(AutoIndexSharedState));

	return add_size(size, hash_estimate_size(AUTOINDEX_MAX_ENTRIES,
											 sizeof(AutoIndexEntry)));
}

void
AutoIndexShmemInit(void)
{
	HASHCTL		info;
	bool		found;

	AutoIndexState = ShmemInitStruct("AutoIndex State",
									 sizeof(AutoIndexSharedState), &found);
	if (!found)
		LWLockInitialize(&AutoIndexState->lock, LWLockNewTrancheId());

	info.keysize = sizeof(AutoIndexKey);
	info.entrysize = sizeof(AutoIndexEntry);
	AutoIndexHash = ShmemInitHash("AutoIndex Hash",
								  AUTOINDEX_MAX_ENTRIES, AUTOINDEX_MAX_ENTRIES,
								  &info, HASH_ELEM | HASH_BLOBS | HASH_FIXED_SIZE);
}

/*
 * Is "opno" the equality operator of some btree operator family
 */
static bool
AutoIndexIsBtreeEquality(Oid opno)
{
	CatCList   *catlist;
	bool		found = false;
	int			i;

	catlist = SearchSysCacheList1(AMOPOPID, ObjectIdGetDatum(opno));

	for (i = 0; i < catlist->n_members; i++)
	{
		CatCTup    *ct = catlist->members[i];
		Form_pg_amop aform = (Form_pg_amop) GETSTRUCT(&ct->tuple);

		if (aform->amopmethod == BTREE_AM_OID &&
			aform->amopstrategy == BTEqualStrategyNumber)
		{
			found = true;
			break;
		}
	}

	ReleaseSysCacheList(catlist);
	return found;
}

/*
 * varchar = 'abc' is planned as
 * texteq(RelabelType(Var), Const), so without this we would miss it.
 */
static Node *
AutoIndexStrip(Node *n)
{
	while (n != NULL && IsA(n, RelabelType))
		n = (Node *) ((RelabelType *) n)->arg;
	return n;
}
static bool
AutoIndexIsComparisonValue(Node *n)
{
	if (IsA(n, Const))
		return !((Const *) n)->constisnull;
	if (IsA(n, Param))
		return ((Param *) n)->paramkind == PARAM_EXTERN;
	return false;
}
/*
 * Does "clause" have the form  column = non-null-constant  (either order)
 * on the relation with range-table index "scanrelid"
 */
static bool
AutoIndexIsSimpleEquality(Node *clause, Index scanrelid, AttrNumber *attno)
{
	OpExpr	   *op;
	Node	   *left;
	Node	   *right;
	Var		   *var;

	if (clause == NULL || !IsA(clause, OpExpr))
		return false;

	op = (OpExpr *) clause;
	if (list_length(op->args) != 2)
		return false;

	if (!AutoIndexIsBtreeEquality(op->opno))
		return false;

	left = AutoIndexStrip((Node *) linitial(op->args));
	right = AutoIndexStrip((Node *) lsecond(op->args));

	if (IsA(left, Var) && AutoIndexIsComparisonValue(right))
		var = (Var *) left;
	else if (IsA(right, Var) && AutoIndexIsComparisonValue(left))
		var = (Var *) right;
	else
		return false;

	if (var->varattno <= 0 || var->varno != scanrelid || var->varlevelsup != 0)
		return false;

	*attno = var->varattno;
	return true;
}
/* Already a valid, non-partial index leading with attno? Then don't nag. */
static bool
AutoIndexHasLeadingIndex(Relation rel, AttrNumber attno)
{
	List	   *indexes = RelationGetIndexList(rel);
	ListCell   *lc;
	bool		found = false;

	foreach(lc, indexes)
	{
		HeapTuple	tup = SearchSysCache1(INDEXRELID,
										  ObjectIdGetDatum(lfirst_oid(lc)));
		Form_pg_index idx;

		if (!HeapTupleIsValid(tup))
			continue;
		idx = (Form_pg_index) GETSTRUCT(tup);
		if (idx->indisvalid && idx->indnkeyatts > 0 &&
			idx->indkey.values[0] == attno &&
			heap_attisnull(tup, Anum_pg_index_indpred, NULL))
			found = true;
		ReleaseSysCache(tup);
		if (found)
			break;
	}
	list_free(indexes);
	return found;
}

static double
AutoIndexLookupCost(double ntuples, double npages, double nrows)
{
	double		height = ceil(log(Max(ntuples, 2.0)) / log(AUTOINDEX_BTREE_FANOUT));

	return random_page_cost * (height + Min(nrows, npages)) +
		(cpu_index_tuple_cost + cpu_tuple_cost) * nrows;
}

static double
AutoIndexBuildCost(double ntuples, double npages)
{
	double		n = Max(ntuples, 2.0);

	return seq_page_cost * npages +
		2.0 * cpu_operator_cost * n * log2(n) +
		seq_page_cost * n / AUTOINDEX_BTREE_FANOUT;
}

static void
AutoIndexRecord(Relation rel, AttrNumber attno, double seq_cost, double nrows)
{
	AutoIndexKey key;
	AutoIndexEntry *entry;
	bool		found;
	double		ntuples = rel->rd_rel->reltuples;
	double		npages = rel->rd_rel->relpages;
	double		savings, build_cost, total_savings = 0;
	int64		nscans = 0;
	bool		recommend_now = false;

	if (ntuples <= 0 || npages <= 0)	/* never analyzed: no stats yet */
		return;

	savings = seq_cost - AutoIndexLookupCost(ntuples, npages, nrows);
	build_cost = AutoIndexBuildCost(ntuples, npages);

	memset(&key, 0, sizeof(key));
	key.dbid = MyDatabaseId;
	key.relid = RelationGetRelid(rel);
	key.attno = attno;

	LWLockAcquire(&AutoIndexState->lock, LW_EXCLUSIVE);
	entry = (AutoIndexEntry *) hash_search(AutoIndexHash, &key,
										   HASH_ENTER_NULL, &found);
	if (entry == NULL)			/* table full: drop, don't error */
	{
		LWLockRelease(&AutoIndexState->lock);
		return;
	}
	if (!found)
	{
		entry->nscans = 0;
		entry->total_savings = 0;
		entry->recommended = false;
		entry->attempted = false;
		entry->failures = 0;
	}
	entry->nscans++;
	if (savings > 0)
		entry->total_savings += savings;
	entry->build_cost = build_cost;

	if (!entry->recommended &&
		entry->nscans >= ((int64) auto_index_min_scans << entry->failures) &&
		entry->total_savings > entry->build_cost)
	{
		entry->recommended = true;
		recommend_now = true;
		nscans = entry->nscans;
		total_savings = entry->total_savings;
	}
	LWLockRelease(&AutoIndexState->lock);

	if (recommend_now)			/* catalog lookups + logging outside the lock */
	{
		char	   *nspname = get_namespace_name(RelationGetNamespace(rel));
		char	   *attname = get_attname(RelationGetRelid(rel), attno, false);

		ereport(LOG,
				(errmsg("auto_index: recommend CREATE INDEX ON %s (%s)",
						quote_qualified_identifier(nspname,
												   RelationGetRelationName(rel)),
						quote_identifier(attname)),
				 errdetail("%lld scans, estimated savings %.0f > build cost %.0f.",
						   (long long) nscans, total_savings, build_cost)));
	}
}
/*
 * Called once from ExecInitSeqScan.  
 */
void
AutoIndexObserveSeqScan(SeqScan *node, Relation rel,
						EState *estate, int eflags)
{
	ListCell   *lc;
	if (!auto_index_enabled || AutoIndexHash == NULL)
		return;
	/* Not a real, user-visible execution of this scan? */
	if (IsParallelWorker() ||
		(eflags & EXEC_FLAG_EXPLAIN_ONLY) ||
		estate->es_epq_active != NULL)
		return;

	/* Only ordinary, permanent, non-catalog tables are index candidates. */
	if (rel->rd_rel->relkind != RELKIND_RELATION ||
		rel->rd_rel->relpersistence != RELPERSISTENCE_PERMANENT ||
		IsSystemRelation(rel))
		return;

	foreach(lc, node->scan.plan.qual)
	{
		Node	   *clause = (Node *) lfirst(lc);
		AttrNumber	attno;

		if (!AutoIndexIsSimpleEquality(clause, node->scan.scanrelid, &attno))
			continue;
		if (AutoIndexHasLeadingIndex(rel, attno))
			continue;

		AutoIndexRecord(rel, attno,
						node->scan.plan.total_cost,
						node->scan.plan.plan_rows);
	}
}

/* How many indexes on this table did *we* create? */
static int
AutoIndexCountOurs(Relation rel)
{
	List	   *indexes = RelationGetIndexList(rel);
	ListCell   *lc;
	int			n = 0;

	foreach(lc, indexes)
	{
		char	   *name = get_rel_name(lfirst_oid(lc));

		if (name != NULL &&
			strncmp(name, AUTOINDEX_NAME_PREFIX, strlen(AUTOINDEX_NAME_PREFIX)) == 0)
			n++;
	}
	list_free(indexes);
	return n;
}


/*
 * Estimate the size of a single-column btree before building it.
 *
 *	 per-entry bytes = MAXALIGN(IndexTupleData + key width) + ItemIdData
 *	 usable per page = (BLCKSZ - page header - btree special) * fillfactor
 *	 leaf pages      = ceil(N * entry / usable)
 *
 * Inner pages add only ~1/fanout (<1%) on top, so we ignore them.
 */
static double
AutoIndexEstimateSizeMB(Relation rel, AttrNumber attno)
{
	Oid			relid = RelationGetRelid(rel);
	double		ntuples = Max(rel->rd_rel->reltuples, 0);
	int32		width = get_attavgwidth(relid, attno);	/* from ANALYZE */
	double		entry_bytes;
	double		usable;
	double		pages;

	if (width <= 0)				/* no stats: fall back to the type's typical width */
	{
		Oid			typid;
		int32		typmod;
		Oid			collid;

		get_atttypetypmodcoll(relid, attno, &typid, &typmod, &collid);
		width = get_typavgwidth(typid, typmod);
	}

	entry_bytes = MAXALIGN(sizeof(IndexTupleData) + width) + sizeof(ItemIdData);
	usable = (BLCKSZ - SizeOfPageHeaderData - MAXALIGN(sizeof(BTPageOpaqueData))) *
		(BTREE_DEFAULT_FILLFACTOR / 100.0);
	pages = ceil(ntuples * entry_bytes / usable);

	return pages * BLCKSZ / (1024.0 * 1024.0);
}

/* A build failed: count it, and unless we've given up, make it re-earn evidence. */
static void
AutoIndexNoteFailure(Oid relid, AttrNumber attno)
{
	AutoIndexKey key;
	AutoIndexEntry *entry;

	memset(&key, 0, sizeof(key));
	key.dbid = MyDatabaseId;
	key.relid = relid;
	key.attno = attno;

	LWLockAcquire(&AutoIndexState->lock, LW_EXCLUSIVE);
	entry = (AutoIndexEntry *) hash_search(AutoIndexHash, &key, HASH_FIND, NULL);
	if (entry != NULL)
	{
		entry->failures++;
		if (entry->failures < AUTOINDEX_MAX_FAILURES)
		{
			entry->nscans = 0;
			entry->total_savings = 0;
			entry->recommended = false;
			entry->attempted = false;
		}
		/* else: leave attempted = true -> permanently retired */
	}
	LWLockRelease(&AutoIndexState->lock);
}


/*
 * Run one SQL statement exactly as if a client had sent it, so commands
 * that must be top-level (CREATE INDEX CONCURRENTLY) are allowed.
 *
 * CIC commits our transaction internally, which frees every transaction-
 * scoped memory context.  So the parse/plan trees must live in a context
 * that survives commits -- the same reason exec_simple_query() parses in
 * MessageContext instead of the transaction's context.
 */
static void
AutoIndexRunTopLevel(const char *sql)
{
	MemoryContext stmtcxt;
	List	   *parsetrees;
	List	   *querytrees;
	List	   *plantrees;
	PlannedStmt *pstmt;

	stmtcxt = AllocSetContextCreate(TopMemoryContext, "AutoIndex statement",
									ALLOCSET_DEFAULT_SIZES);
	MemoryContextSwitchTo(stmtcxt);

	parsetrees = pg_parse_query(sql);
	Assert(list_length(parsetrees) == 1);
	querytrees = pg_analyze_and_rewrite_fixedparams(linitial_node(RawStmt, parsetrees),
													sql, NULL, 0, NULL);
	plantrees = pg_plan_queries(querytrees, sql, CURSOR_OPT_PARALLEL_OK, NULL);
	pstmt = linitial_node(PlannedStmt, plantrees);

	/* Same snapshot dance as PortalRunUtility(); CIC pops it itself. */
	PushActiveSnapshot(GetTransactionSnapshot());
	ProcessUtility(pstmt, sql, false, PROCESS_UTILITY_TOPLEVEL,
				   NULL, NULL, None_Receiver, NULL);
	if (ActiveSnapshotSet())
		PopActiveSnapshot();

	/*
	 * Don't switch back to the context we started in: it belonged to a
	 * transaction CIC already committed.  Use the current transaction's.
	 */
	MemoryContextSwitchTo(TopTransactionContext);
	MemoryContextDelete(stmtcxt);
}





/* ---------------------------------------------------------------------
 * Background worker: turn recommendations into real indexes.
 * ---------------------------------------------------------------------
 */

/* Build one index.  Runs in its own transaction. */
static void
AutoIndexBuild(Oid relid, AttrNumber attno)
{
	Relation	rel;
	StringInfoData sql;
	bool		build = false;

	SetCurrentStatementStartTimestamp();
	StartTransactionCommand();

	/* Validation needs a snapshot; release it before CIC (CIC wants a clean stack). */
	PushActiveSnapshot(GetTransactionSnapshot());
	rel = try_relation_open(relid, AccessShareLock);
	if (rel != NULL)
	{
		char	   *attname = get_attname(relid, attno, true);

		if (attname != NULL && !AutoIndexHasLeadingIndex(rel, attno))
		{
			int			ours = AutoIndexCountOurs(rel);
			double		est_mb = AutoIndexEstimateSizeMB(rel, attno);

			if (ours >= auto_index_max_per_table)
				ereport(LOG,
						(errmsg("auto_index: skipping %s (%s): table already has %d auto indexes",
								RelationGetRelationName(rel), attname, ours),
						 errhint("Raise auto_index_max_per_table to allow more.")));
			else if (est_mb > auto_index_max_size_mb)
				ereport(LOG,
						(errmsg("auto_index: skipping %s (%s): estimated size %.0f MB exceeds auto_index_max_size_mb = %d",
								RelationGetRelationName(rel), attname, est_mb,
								auto_index_max_size_mb)));
			else
			{
				MemoryContext old = MemoryContextSwitchTo(TopMemoryContext);

				initStringInfo(&sql);
				MemoryContextSwitchTo(old);
				appendStringInfo(&sql,
								 "CREATE INDEX CONCURRENTLY IF NOT EXISTS " AUTOINDEX_NAME_PREFIX "%u_%d ON %s (%s)",
								 relid, (int) attno,
								 quote_qualified_identifier(get_namespace_name(RelationGetNamespace(rel)),
															RelationGetRelationName(rel)),
								 quote_identifier(attname));
				build = true;
				ereport(LOG, (errmsg("auto_index: building, estimated size %.1f MB", est_mb)));
			}
		}
		relation_close(rel, AccessShareLock);
	}
	PopActiveSnapshot();

	if (build)
	{
		pgstat_report_activity(STATE_RUNNING, sql.data);
		AutoIndexRunTopLevel(sql.data);
		ereport(LOG, (errmsg("auto_index: created index: %s", sql.data)));
	}

	CommitTransactionCommand();
	pgstat_report_activity(STATE_IDLE, NULL);
}
/*
 * Drop INVALID indexes we created (leftovers of failed CIC builds).
 * Only names with our prefix are touched -- never a human's index.
 */
static void
AutoIndexCleanupInvalid(void)
{
	MemoryContext keep;
	MemoryContext old;
	List	   *qualnames = NIL;
	List	   *relnames = NIL;
	ListCell   *lc1;
	ListCell   *lc2;

	/* Survives the commits that DROP INDEX CONCURRENTLY does internally. */
	keep = AllocSetContextCreate(TopMemoryContext, "AutoIndex cleanup",
								 ALLOCSET_SMALL_SIZES);

	/* 1. Find them (read-only catalog query via SPI). */
	SetCurrentStatementStartTimestamp();
	StartTransactionCommand();
	PushActiveSnapshot(GetTransactionSnapshot());
	SPI_connect();
	if (SPI_execute("SELECT quote_ident(n.nspname) || '.' || quote_ident(c.relname), c.relname "
					"FROM pg_index i "
					"JOIN pg_class c ON c.oid = i.indexrelid "
					"JOIN pg_namespace n ON n.oid = c.relnamespace "
					"WHERE NOT i.indisvalid AND c.relname LIKE 'autoidx\\_%'",
					true, 0) == SPI_OK_SELECT)
	{
		for (uint64 i = 0; i < SPI_processed; i++)
		{
			HeapTuple	tup = SPI_tuptable->vals[i];
			TupleDesc	desc = SPI_tuptable->tupdesc;

			old = MemoryContextSwitchTo(keep);
			qualnames = lappend(qualnames, pstrdup(SPI_getvalue(tup, desc, 1)));
			relnames = lappend(relnames, pstrdup(SPI_getvalue(tup, desc, 2)));
			MemoryContextSwitchTo(old);
		}
	}
	SPI_finish();
	PopActiveSnapshot();
	CommitTransactionCommand();

	/* 2. Drop each one in its own transaction, then do the bookkeeping. */
	forboth(lc1, qualnames, lc2, relnames)
	{
		char	   *qualname = (char *) lfirst(lc1);
		char	   *relname = (char *) lfirst(lc2);
		char	   *sql;
		unsigned int relid;
		int			attno;

		old = MemoryContextSwitchTo(keep);
		sql = psprintf("DROP INDEX CONCURRENTLY IF EXISTS %s", qualname);
		MemoryContextSwitchTo(old);

		SetCurrentStatementStartTimestamp();
		StartTransactionCommand();
		pgstat_report_activity(STATE_RUNNING, sql);
		AutoIndexRunTopLevel(sql);
		CommitTransactionCommand();
		pgstat_report_activity(STATE_IDLE, NULL);

		ereport(LOG, (errmsg("auto_index: dropped invalid index %s", qualname)));

		/* The deterministic name tells us exactly which candidate failed. */
		if (sscanf(relname, AUTOINDEX_NAME_PREFIX "%u_%d", &relid, &attno) == 2)
			AutoIndexNoteFailure((Oid) relid, (AttrNumber) attno);
	}

	MemoryContextDelete(keep);
}
/* Build every pending recommendation for the database we're connected to. */
static void
AutoIndexCreatePending(void)
{
	for (;;)
	{
		HASH_SEQ_STATUS status;
		AutoIndexEntry *entry;
		Oid			relid = InvalidOid;
		AttrNumber	attno = 0;

		/* Claim one entry under the lock... */
		LWLockAcquire(&AutoIndexState->lock, LW_EXCLUSIVE);
		hash_seq_init(&status, AutoIndexHash);
		while ((entry = (AutoIndexEntry *) hash_seq_search(&status)) != NULL)
		{
			if (entry->recommended && !entry->attempted &&
				entry->key.dbid == MyDatabaseId)
			{
				entry->attempted = true;
				relid = entry->key.relid;
				attno = entry->key.attno;
				hash_seq_term(&status);		/* required when leaving a scan early */
				break;
			}
		}
		LWLockRelease(&AutoIndexState->lock);

		/* ...and do the slow work without it. */
		if (!OidIsValid(relid))
			return;
		AutoIndexBuild(relid, attno);
	}
}

void
AutoIndexWorkerMain(Datum main_arg)
{
	pqsignal(SIGTERM, die);
	pqsignal(SIGHUP, SignalHandlerForConfigReload);
	BackgroundWorkerUnblockSignals();

	BackgroundWorkerInitializeConnection(auto_index_database, NULL, 0);
	
	/* Session-wide: replaces the old SET LOCAL done through SPI. */
	SetConfigOption("lock_timeout", "5s", PGC_SUSET, PGC_S_OVERRIDE);
	for (;;)
	{
		CHECK_FOR_INTERRUPTS();

		if (ConfigReloadPending)
		{
			ConfigReloadPending = false;
			ProcessConfigFile(PGC_SIGHUP);
		}

		if (auto_index_enabled)
		{
			AutoIndexCleanupInvalid();
			AutoIndexCreatePending();
		}

		(void) WaitLatch(MyLatch,
						 WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
						 AUTOINDEX_NAPTIME_MS, PG_WAIT_EXTENSION);
		ResetLatch(MyLatch);
	}
}

/* Called once by the postmaster at startup. */
void
AutoIndexWorkerRegister(void)
{
	BackgroundWorker bgw;

	if (IsBinaryUpgrade)
		return;

	memset(&bgw, 0, sizeof(bgw));
	bgw.bgw_flags = BGWORKER_SHMEM_ACCESS | BGWORKER_BACKEND_DATABASE_CONNECTION;
	bgw.bgw_start_time = BgWorkerStart_RecoveryFinished;
	bgw.bgw_restart_time = 10;	/* seconds; worker is restarted after an ERROR */
	snprintf(bgw.bgw_library_name, sizeof(bgw.bgw_library_name), "postgres");
	snprintf(bgw.bgw_function_name, sizeof(bgw.bgw_function_name), "AutoIndexWorkerMain");
	snprintf(bgw.bgw_name, sizeof(bgw.bgw_name), "auto index worker");
	snprintf(bgw.bgw_type, sizeof(bgw.bgw_type), "auto index worker");
	RegisterBackgroundWorker(&bgw);
}