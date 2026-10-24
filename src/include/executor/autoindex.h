/*-------------------------------------------------------------------------
 * autoindex.h
 *	  Automatic index creation: sequential-scan observation.
 *-------------------------------------------------------------------------
 */
#ifndef AUTOINDEX_H
#define AUTOINDEX_H

#include "nodes/execnodes.h"
#include "utils/relcache.h"

extern void AutoIndexObserveSeqScan(SeqScan *node, Relation rel,
									EState *estate, int eflags);

#endif							