/*
 * shared_mem.c
 *	  Shared table with one entry per (database, table, column).
 *	  Adds up how much an index would have saved; when the total beats the
 *	  cost of building it, the entry is marked pending for the worker.
 */
#include "postgres.h"

#include "executor/autoindex.h"
#include "storage/lwlock.h"
#include "storage/shmem.h"
#include "utils/hsearch.h"

#define MAX_ENTRIES 1024

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
	double		total_savings;	/* sum of savings over all scans so far */
	bool		pending;		/* threshold crossed, worker should build */
	bool		done;			/* worker already took it */
} AutoIndexEntry;

typedef struct AutoIndexShared
{
	LWLock		lock;
} AutoIndexShared;

static AutoIndexShared *shared = NULL;
static HTAB *hash = NULL;

Size
AutoIndexShmemSize(void)
{
	return add_size(MAXALIGN(sizeof(AutoIndexShared)),
					hash_estimate_size(MAX_ENTRIES, sizeof(AutoIndexEntry)));
}

void
AutoIndexShmemInit(void)
{
	HASHCTL		info;
	bool		found;

	shared = ShmemInitStruct("AutoIndex State", sizeof(AutoIndexShared), &found);
	if (!found)
		LWLockInitialize(&shared->lock, LWLockNewTrancheId());

	memset(&info, 0, sizeof(info));
	info.keysize = sizeof(AutoIndexKey);
	info.entrysize = sizeof(AutoIndexEntry);
	hash = ShmemInitHash("AutoIndex Hash", MAX_ENTRIES, MAX_ENTRIES,
						 &info, HASH_ELEM | HASH_BLOBS);
}

bool
AutoIndexShmemReady(void)
{
	return hash != NULL;
}

/* Add one scan's saving; mark pending when total savings > build cost. */
void
AutoIndexTrackerRecord(Oid dbid, Oid relid, AttrNumber attno,
					   double savings, double build_cost)
{
	AutoIndexKey key;
	AutoIndexEntry *e;
	bool		found;

	memset(&key, 0, sizeof(key));	/* zero padding: key is compared bytewise */
	key.dbid = dbid;
	key.relid = relid;
	key.attno = attno;

	LWLockAcquire(&shared->lock, LW_EXCLUSIVE);
	e = (AutoIndexEntry *) hash_search(hash, &key, HASH_ENTER_NULL, &found);
	if (e != NULL)
	{
		if (!found)
		{
			e->nscans = 0;
			e->total_savings = 0;
			e->pending = false;
			e->done = false;
		}
		e->nscans++;
		e->total_savings += savings;

		if (!e->done && !e->pending && e->total_savings > build_cost)
			e->pending = true;
	}
	LWLockRelease(&shared->lock);
}

/* Worker: take one pending entry of this database. */
bool
AutoIndexTrackerClaim(Oid dbid, Oid *relid, AttrNumber *attno)
{
	HASH_SEQ_STATUS status;
	AutoIndexEntry *e;
	bool		claimed = false;

	LWLockAcquire(&shared->lock, LW_EXCLUSIVE);
	hash_seq_init(&status, hash);
	while ((e = (AutoIndexEntry *) hash_seq_search(&status)) != NULL)
	{
		if (e->pending && e->key.dbid == dbid)
		{
			e->pending = false;
			e->done = true;
			*relid = e->key.relid;
			*attno = e->key.attno;
			claimed = true;
			hash_seq_term(&status);
			break;
		}
	}
	LWLockRelease(&shared->lock);
	return claimed;
}