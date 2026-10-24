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

/* GUCs */
bool		auto_index_enabled = true;
int			auto_index_min_scans = 10;

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
	}
	entry->nscans++;
	if (savings > 0)
		entry->total_savings += savings;
	entry->build_cost = build_cost;

	if (!entry->recommended &&
		entry->nscans >= auto_index_min_scans &&
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