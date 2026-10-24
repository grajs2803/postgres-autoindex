#include "postgres.h"

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
	Const	   *constant;

	if (clause == NULL || !IsA(clause, OpExpr))
		return false;

	op = (OpExpr *) clause;
	if (list_length(op->args) != 2)
		return false;

	if (!AutoIndexIsBtreeEquality(op->opno))
		return false;

	left = AutoIndexStrip((Node *) linitial(op->args));
	right = AutoIndexStrip((Node *) lsecond(op->args));

	if (IsA(left, Var) && IsA(right, Const))
	{
		var = (Var *) left;
		constant = (Const *) right;
	}
	else if (IsA(left, Const) && IsA(right, Var))
	{
		var = (Var *) right;
		constant = (Const *) left;
	}
	else
		return false;

	if (var->varattno <= 0 ||
		var->varno != scanrelid ||
		var->varlevelsup != 0 ||
		constant->constisnull)
		return false;

	*attno = var->varattno;
	return true;
}

/*
 * Called once from ExecInitSeqScan.  
 */
void
AutoIndexObserveSeqScan(SeqScan *node, Relation rel,
						EState *estate, int eflags)
{
	ListCell   *lc;

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

		if (AutoIndexIsSimpleEquality(clause, node->scan.scanrelid, &attno))
			elog(LOG, "auto_index: equality scan relid=%u rel=%s attno=%d cost=%.2f rows=%.0f",
				 RelationGetRelid(rel),
				 RelationGetRelationName(rel),
				 attno,
				 node->scan.plan.total_cost,
				 node->scan.plan.plan_rows);
		else
			elog(DEBUG1, "auto_index: clause rejected relid=%u clause=%s",
				 RelationGetRelid(rel),
				 nodeToString(clause));
	}
}