/* debug.h
 *
 * Definitions for the debug and run-time analysis support module.
 * Contains a lot of macros.
 *
 * Copyright 2008 Rainer Gerhards and Adiscon GmbH.
 *
 * This file is part of rsyslog.
 *
 * Rsyslog is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Rsyslog is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Rsyslog.  If not, see <http://www.gnu.org/licenses/>.
 *
 * A copy of the GPL can be found in the file "COPYING" in this distribution.
 */
#ifndef DEBUG_H_INCLUDED
#define DEBUG_H_INCLUDED

#include <pthread.h>

/* external static data elements (some time to be replaced) */
extern int Debug;		/* debug flag  - read-only after startup */
extern int debugging_on;	 /* read-only, except on sig USR1 */

/* data types */

/* the function database. It is used as a static var inside each function. That provides
 * us the fast access to it that we need to make the instrumentation work. It's address
 * also serves as a unique function identifier and can be used inside other structures
 * to refer to the function (e.g. for pretty-printing names).
 * rgerhards, 2008-01-24
 */
typedef struct dbgFuncDBmutInfoEntry_s {
	pthread_mutex_t *pmut;
	int lockLn; /* line where it was locked (inside our func): -1 means mutex is not locked */
	pthread_t thrd; /* thrd where the mutex was locked */
	unsigned long lInvocation; /* invocation (unique during program run!) of this function that locked the mutex */
} dbgFuncDBmutInfoEntry_t;
typedef struct dbgFuncDB_s {
	unsigned magic;
	unsigned long nTimesCalled;
	const char *func;
	const char *file;
	int line;
	dbgFuncDBmutInfoEntry_t mutInfo[5];
	/* remember to update the initializer if you add anything or change the order! */
} dbgFuncDB_t;
#define dbgFUNCDB_MAGIC 0xA1B2C3D4
#define dbgFuncDB_t_INITIALIZER \
	{ \
	.magic = dbgFUNCDB_MAGIC,\
	.nTimesCalled = 0,\
	.func = __func__, \
	.file = __FILE__, \
	.line = __LINE__ \
	}


/* prototypes */
rsRetVal dbgClassInit(void);
rsRetVal dbgClassExit(void);
void sigsegvHdlr(int signum);
void dbgprintf(char *fmt, ...) __attribute__((format(printf,1, 2)));
int dbgMutexLock(pthread_mutex_t *pmut, dbgFuncDB_t *pFuncD, int lnB);
int dbgMutexUnlock(pthread_mutex_t *pmut, dbgFuncDB_t *pFuncD, int lnB);
int dbgCondWait(pthread_cond_t *cond, pthread_mutex_t *pmut, dbgFuncDB_t *pFuncD, int lnB);
int dbgCondTimedWait(pthread_cond_t *cond, pthread_mutex_t *pmut, const struct timespec *abstime, dbgFuncDB_t *pFuncD, int lnB);
int dbgEntrFunc(dbgFuncDB_t *pFuncDB);
void dbgExitFunc(dbgFuncDB_t *pFuncDB, int iStackPtrRestore);
void dbgSetThrdName(uchar *pszName);
void dbgPrintAllDebugInfo(void);

/* macros */
#if 1 /* DEV debug: set to 1 to get a rough call trace -- rgerhards, 2008-01-13 */
#	define BEGINfunc static dbgFuncDB_t dbgFuncDB=dbgFuncDB_t_INITIALIZER; int dbgCALLStaCK_POP_POINT = dbgEntrFunc(&dbgFuncDB);
#	define ENDfunc dbgExitFunc(&dbgFuncDB, dbgCALLStaCK_POP_POINT);
#else
#	define BEGINfunc
#	define ENDfunc
#endif
#if 1 /* DEV debug: set to 1 to enable -- rgerhards, 2008-01-13 */
#	define RUNLOG dbgprintf("%s:%d: %s: log point\n", __FILE__, __LINE__, __func__)
#	define RUNLOG_VAR(fmt, x) dbgprintf("%s:%d: %s: var '%s'[%s]: " fmt "\n", __FILE__, __LINE__, __func__, #x, fmt, x)
#else
#	define RUNLOG
#	define RUNLOG_VAR(x)
#endif

/* mutex operations */
#define MUTOP_LOCKWAIT		1
#define MUTOP_LOCK		2
#define MUTOP_UNLOCK		3


/* debug aides */
#if 1
#define d_pthread_mutex_lock(x)     dbgMutexLock(x, &dbgFuncDB, __LINE__)
#define d_pthread_mutex_unlock(x)   dbgMutexUnlock(x, &dbgFuncDB, __LINE__)
#define d_pthread_cond_wait(cond, mut)   dbgCondWait(cond, mut, &dbgFuncDB, __LINE__)
#define d_pthread_cond_timedwait(cond, mut, to)   dbgCondTimedWait(cond, mut, to, &dbgFuncDB, __LINE__)
#else
#define d_pthread_mutex_lock(x)     pthread_mutex_lock(x)
#define d_pthread_mutex_unlock(x)   pthread_mutex_unlock(x)
#define d_pthread_cond_wait(cond, mut)   pthread_cond_wait(cond, mut)
#define d_pthread_cond_timedwait(cond, mut, to)   pthread_cond_timedwait(cond, mut, to)
#endif
#endif /* #ifndef DEBUG_H_INCLUDED */