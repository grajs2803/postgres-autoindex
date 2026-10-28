/*
 * index_worker.c
 *	  Background worker: wakes up every few seconds and builds any index
 *	  the tracker has marked as worthwhile.
 */
#include "postgres.h"

#include "access/xact.h"
#include "executor/autoindex.h"
#include "executor/spi.h"
#include "lib/stringinfo.h"
#include "libpq/pqsignal.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "postmaster/bgworker.h"
#include "postmaster/interrupt.h"
#include "storage/latch.h"
#include "tcop/tcopprot.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/snapmgr.h"
#include "utils/wait_event.h"

/* Run CREATE INDEX for one (table, column). */
static void
BuildIndex(Oid relid, AttrNumber attno)
{
	StringInfoData sql;
	char	   *relname;
	char	   *attname;

	StartTransactionCommand();
	PushActiveSnapshot(GetTransactionSnapshot());

	relname = get_rel_name(relid);
	attname = get_attname(relid, attno, true);

	/* table or column may have been dropped meanwhile */
	if (relname != NULL && attname != NULL)
	{
		initStringInfo(&sql);
		appendStringInfo(&sql, "CREATE INDEX IF NOT EXISTS " AUTOINDEX_NAME_PREFIX "%u_%d ON %s (%s)",
						 relid, (int) attno,
						 quote_qualified_identifier(get_namespace_name(get_rel_namespace(relid)),
													relname),
						 quote_identifier(attname));

		SPI_connect();
		SPI_execute(sql.data, false, 0);
		SPI_finish();

		ereport(LOG, (errmsg("auto_index: created index: %s", sql.data)));
	}

	PopActiveSnapshot();
	CommitTransactionCommand();
}

void
AutoIndexWorkerMain(Datum main_arg)
{
	pqsignal(SIGTERM, die);
	BackgroundWorkerUnblockSignals();
	BackgroundWorkerInitializeConnection(AUTOINDEX_DATABASE, NULL, 0);

	for (;;)
	{
		Oid			relid;
		AttrNumber	attno;

		CHECK_FOR_INTERRUPTS();

		while (AutoIndexTrackerClaim(MyDatabaseId, &relid, &attno))
			BuildIndex(relid, attno);

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

	memset(&bgw, 0, sizeof(bgw));
	bgw.bgw_flags = BGWORKER_SHMEM_ACCESS | BGWORKER_BACKEND_DATABASE_CONNECTION;
	bgw.bgw_start_time = BgWorkerStart_RecoveryFinished;
	bgw.bgw_restart_time = 10;
	snprintf(bgw.bgw_library_name, BGW_MAXLEN, "postgres");
	snprintf(bgw.bgw_function_name, BGW_MAXLEN, "AutoIndexWorkerMain");
	snprintf(bgw.bgw_name, BGW_MAXLEN, "auto index worker");
	snprintf(bgw.bgw_type, BGW_MAXLEN, "auto index worker");
	RegisterBackgroundWorker(&bgw);
}