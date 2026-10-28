/* autoindex.h - automatic index creation (v3: real costs, cumulative, no decay) */
#ifndef AUTOINDEX_H
#define AUTOINDEX_H

#include "access/attnum.h"
#include "fmgr.h"
#include "nodes/execnodes.h"
#include "utils/relcache.h"

#define AUTOINDEX_NAME_PREFIX	"autoidx_"	/* our indexes: autoidx_<relid>_<attno> */
#define AUTOINDEX_DATABASE		"postgres"	/* worker connects here */
#define AUTOINDEX_BTREE_FANOUT	256.0
#define AUTOINDEX_NAPTIME_MS	5000		/* worker wakes up this often */

/* shared_mem.c */
extern Size AutoIndexShmemSize(void);
extern void AutoIndexShmemInit(void);
extern bool AutoIndexShmemReady(void);
extern void AutoIndexTrackerRecord(Oid dbid, Oid relid, AttrNumber attno,
								   double savings, double build_cost);
extern bool AutoIndexTrackerClaim(Oid dbid, Oid *relid, AttrNumber *attno);

/* autoindex.c */
extern void AutoIndexObserveSeqScan(SeqScan *node, Relation rel,
									EState *estate, int eflags);

/* index_worker.c */
extern PGDLLEXPORT void AutoIndexWorkerMain(Datum main_arg);
extern void AutoIndexWorkerRegister(void);

#endif