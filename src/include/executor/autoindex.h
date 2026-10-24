/*-------------------------------------------------------------------------
 * autoindex.h
 *	  Automatic index creation: sequential-scan observation.
 *-------------------------------------------------------------------------
 */
#ifndef AUTOINDEX_H
#define AUTOINDEX_H

#include "nodes/execnodes.h"
#include "utils/relcache.h"
#include "nodes/plannodes.h"

/* GUCs */
extern PGDLLIMPORT bool auto_index_enabled;
extern PGDLLIMPORT int auto_index_min_scans;

extern Size AutoIndexShmemSize(void);
extern void AutoIndexShmemInit(void);

extern void AutoIndexObserveSeqScan(SeqScan *node, Relation rel,
									EState *estate, int eflags);
#endif							