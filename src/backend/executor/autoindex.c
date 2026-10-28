/*
 * autoindex.c
 *	  Called for every sequential scan.  Finds "column = constant" filters,
 *	  estimates how much an index would have saved (real cost model), and
 *	  reports that to the shared tracker.
 */
#include "postgres.h"

#include <math.h>

#include "access/htup_details.h"
#include "access/parallel.h"
#include "catalog/catalog.h"
#include "catalog/pg_class.h"
#include "catalog/pg_index.h"
#include "executor/autoindex.h"
#include "executor/executor.h"
#include "miscadmin.h"
#include "nodes/primnodes.h"
#include "optimizer/optimizer.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/syscache.h"

/* Is this clause "column = constant"?  If yes, return the column number. */
static bool
IsEqualityOnColumn(Node *clause, Index scanrelid, AttrNumber *attno)
{
	OpExpr	   *op;
	Node	   *left;
	Node	   *right;
	Var		   *var;
	char	   *opname;

	if (!IsA(clause, OpExpr))
		return false;
	op = (OpExpr *) clause;
	if (list_length(op->args) != 2)
		return false;

	/* operator must be "=" */
	opname = get_opname(op->opno);
	if (opname == NULL || strcmp(opname, "=") != 0)
		return false;

	left = (Node *) linitial(op->args);
	right = (Node *) lsecond(op->args);

	/* varchar columns are wrapped in a cast node; look through it */
	if (IsA(left, RelabelType))
		left = (Node *) ((RelabelType *) left)->arg;
	if (IsA(right, RelabelType))
		right = (Node *) ((RelabelType *) right)->arg;

	if (IsA(left, Var) && IsA(right, Const))
		var = (Var *) left;
	else if (IsA(right, Var) && IsA(left, Const))
		var = (Var *) right;
	else
		return false;

	if (var->varno != scanrelid || var->varattno <= 0)
		return false;

	*attno = var->varattno;
	return true;
}

/* Does a valid, non-partial index already start with this column? */
static bool
HasLeadingIndex(Relation rel, AttrNumber attno)
{
	List	   *indexes = RelationGetIndexList(rel);
	ListCell   *lc;
	bool		found = false;

	foreach(lc, indexes)
	{
		HeapTuple	tup = SearchSysCache1(INDEXRELID, ObjectIdGetDatum(lfirst_oid(lc)));
		Form_pg_index idx;

		if (!HeapTupleIsValid(tup))
			continue;
		idx = (Form_pg_index) GETSTRUCT(tup);
		if (idx->indisvalid && idx->indkey.values[0] == attno &&
			heap_attisnull(tup, Anum_pg_index_indpred, NULL))
			found = true;
		ReleaseSysCache(tup);
		if (found)
			break;
	}
	list_free(indexes);
	return found;
}

/*
 * Cost of fetching nrows rows through a btree:
 *   walk down the tree (height pages), then one random page per row
 *   (never more than the table has), plus per-tuple CPU.
 */
static double
IndexLookupCost(double ntuples, double npages, double nrows)
{
	double		height = ceil(log(Max(ntuples, 2.0)) / log(AUTOINDEX_BTREE_FANOUT));

	return random_page_cost * (height + Min(nrows, npages)) +
		(cpu_index_tuple_cost + cpu_tuple_cost) * nrows;
}

/*
 * Cost of building the index: read the table once, sort the keys
 * (about n*log2(n) comparisons), write the leaf pages.
 */
static double
IndexBuildCost(double ntuples, double npages)
{
	double		n = Max(ntuples, 2.0);

	return seq_page_cost * npages +
		2.0 * cpu_operator_cost * n * log2(n) +
		seq_page_cost * n / AUTOINDEX_BTREE_FANOUT;
}

/* Called once from ExecInitSeqScan. */
void
AutoIndexObserveSeqScan(SeqScan *node, Relation rel,
						EState *estate, int eflags)
{
	ListCell   *lc;
	double		ntuples;
	double		npages;
	double		savings;
	double		build_cost;

	if (!AutoIndexShmemReady())
		return;

	/* skip parallel workers and plain EXPLAIN so scans are not double counted */
	if (IsParallelWorker() || (eflags & EXEC_FLAG_EXPLAIN_ONLY))
		return;

	/* only normal user tables */
	if (rel->rd_rel->relkind != RELKIND_RELATION || IsSystemRelation(rel))
		return;

	/* no statistics yet (table never analyzed) */
	ntuples = rel->rd_rel->reltuples;
	npages = rel->rd_rel->relpages;
	if (ntuples <= 0 || npages <= 0)
		return;

	/*
	 * Saving = (what this seq scan cost) - (what an index scan returning the
	 * same number of rows would cost).  For gender = 'M' (half the table)
	 * the index scan is MORE expensive, so the saving is negative and the
	 * scan is ignored.  This replaces a hand-picked selectivity cutoff.
	 */
	savings = node->scan.plan.total_cost -
		IndexLookupCost(ntuples, npages, node->scan.plan.plan_rows);
	if (savings <= 0)
		return;
	build_cost = IndexBuildCost(ntuples, npages);

	foreach(lc, node->scan.plan.qual)
	{
		AttrNumber	attno;

		if (!IsEqualityOnColumn((Node *) lfirst(lc), node->scan.scanrelid, &attno))
			continue;
		if (HasLeadingIndex(rel, attno))
			continue;

		AutoIndexTrackerRecord(MyDatabaseId, RelationGetRelid(rel), attno,
							   savings, build_cost);
	}
}