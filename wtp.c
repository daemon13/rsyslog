/* wtp.c
 *
 * This file implements the worker thread pool (wtp) class.
 * 
 * File begun on 2008-01-20 by RGerhards
 *
 * There is some in-depth documentation available in doc/dev_queue.html
 * (and in the web doc set on http://www.rsyslog.com/doc). Be sure to read it
 * if you are getting aquainted to the object.
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
#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <signal.h>
#include <pthread.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

#include "rsyslog.h"
#include "syslogd.h"
#include "stringbuf.h"
#include "srUtils.h"
#include "wtp.h"
#include "wti.h"
#include "obj.h"

/* static data */
DEFobjStaticHelpers

/* forward-definitions */

/* methods */

/* get the header for debug messages
 * The caller must NOT free or otherwise modify the returned string!
 */
static inline uchar *
wtpGetDbgHdr(wtp_t *pThis)
{
	ISOBJ_TYPE_assert(pThis, wtp);

	if(pThis->pszDbgHdr == NULL)
		return (uchar*) "wtp"; /* should not normally happen */
	else
		return pThis->pszDbgHdr;
}



/* Not implemented dummy function for constructor */
static rsRetVal NotImplementedDummy() { return RS_RET_OK; }
/* Standard-Constructor for the wtp object
 */
BEGINobjConstruct(wtp) /* be sure to specify the object type also in END macro! */
	pthread_mutex_init(&pThis->mut, NULL);
	pthread_cond_init(&pThis->condThrdTrm, NULL);
	/* set all function pointers to "not implemented" dummy so that we can safely call them */
	pThis->pfChkStopWrkr = NotImplementedDummy;
	pThis->pfIsIdle = NotImplementedDummy;
	pThis->pfDoWork = NotImplementedDummy;
	pThis->pfOnIdle = NotImplementedDummy;
	pThis->pfOnWorkerCancel = NotImplementedDummy;
	pThis->pfOnWorkerStartup = NotImplementedDummy;
	pThis->pfOnWorkerShutdown = NotImplementedDummy;
ENDobjConstruct(wtp)


/* Construction finalizer
 * rgerhards, 2008-01-17
 */
rsRetVal
wtpConstructFinalize(wtp_t *pThis)
{
	DEFiRet;
	int i;
	uchar pszBuf[64];
	size_t lenBuf;
	wti_t *pWti;

	ISOBJ_TYPE_assert(pThis, wtp);

	dbgprintf("%s: finalizing construction of worker thread pool\n", wtpGetDbgHdr(pThis));
	/* alloc and construct workers - this can only be done in finalizer as we previously do
	 * not know the max number of workers
	 */
	if((pThis->pWrkr = malloc(sizeof(wti_t*) * pThis->iNumWorkerThreads)) == NULL)
		ABORT_FINALIZE(RS_RET_OUT_OF_MEMORY);

	for(i = 0 ; i < pThis->iNumWorkerThreads ; ++i) {
RUNLOG_VAR("%d", i);
RUNLOG_VAR("%p", pThis->pWrkr[i]);
		CHKiRet(wtiConstruct(&pThis->pWrkr[i]));
		pWti = pThis->pWrkr[i];
		lenBuf = snprintf((char*)pszBuf, sizeof(pszBuf), "%s/w%d", wtpGetDbgHdr(pThis), i);
		CHKiRet(wtiSetDbgHdr(pWti, pszBuf, lenBuf));
		CHKiRet(wtiSetpWtp(pWti, pThis));
		CHKiRet(wtiConstructFinalize(pWti));
	}
		

finalize_it:
	RETiRet;
}


/* Destructor */
rsRetVal
wtpDestruct(wtp_t **ppThis)
{
	DEFiRet;
	wtp_t *pThis;
	int iCancelStateSave;
	int i;

dbgPrintAllDebugInfo();
RUNLOG;
	assert(ppThis != NULL);
	pThis = *ppThis;
	ISOBJ_TYPE_assert(pThis, wtp);

	/* we can not be canceled, that would have a myriad of side-effects */
	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &iCancelStateSave);

	/* destruct workers */
	for(i = 0 ; i < pThis->iNumWorkerThreads ; ++i)
		wtiDestruct(&pThis->pWrkr[i]);

	free(pThis->pWrkr);
	pThis->pWrkr = NULL;

	/* actual destruction */
	pthread_cond_destroy(&pThis->condThrdTrm);
	pthread_mutex_destroy(&pThis->mut);

	if(pThis->pszDbgHdr != NULL)
		free(pThis->pszDbgHdr);

	/* and finally delete the queue objet itself */
	free(pThis);
	*ppThis = NULL;

	/* back to normal */
	pthread_setcancelstate(iCancelStateSave, NULL);

	RETiRet;
}


/* wake up at least one worker thread.
 * rgerhards, 2008-01-20
 */
rsRetVal
wtpWakeupWrkr(wtp_t *pThis)
{
	DEFiRet;

	// TODO; mutex?
	ISOBJ_TYPE_assert(pThis, wtp);
dbgprintf("wtpWakeupWrkr 1, cond %p\n", pThis->pcondBusy);
	pthread_cond_signal(pThis->pcondBusy);
dbgprintf("wtpWakeupWrkr 2\n");
	RETiRet;
}
/* wake up all worker threads.
 * rgerhards, 2008-01-16
 */
rsRetVal
wtpWakeupAllWrkr(wtp_t *pThis)
{
	DEFiRet;

	ISOBJ_TYPE_assert(pThis, wtp);
	// TODO; mutex?
	pthread_cond_broadcast(pThis->pcondBusy);
	RETiRet;
}


/* check if we had any worker thread changes and, if so, act
 * on them. At a minimum, terminated threads are harvested (joined).
 * This function MUST NEVER block on the queue mutex!
 */
rsRetVal
wtpProcessThrdChanges(wtp_t *pThis)
{
	DEFiRet;
	int i;

	ISOBJ_TYPE_assert(pThis, wtp);

	RUNLOG;
	if(pThis->bThrdStateChanged == 0)
		FINALIZE;
	RUNLOG;

	/* go through all threads */
	for(i = 0 ; i < pThis->iNumWorkerThreads ; ++i) {
		wtiProcessThrdChanges(pThis->pWrkr[i], LOCK_MUTEX);
	}

finalize_it:
	RUNLOG;
	RETiRet;
}


/* Sent a specific state for the worker thread pool.
 * rgerhards, 2008-01-21
 */
rsRetVal
wtpSetState(wtp_t *pThis, wtpState_t iNewState)
{
	DEFiRet;

	ISOBJ_TYPE_assert(pThis, wtp);
	pThis->wtpState = iNewState;
	// TODO: must wakeup workers?

	RETiRet;
}


/* check if the worker shall shutdown (1 = yes, 0 = no)
 * TODO: check if we can use atomic operations to enhance performance
 * Note: there may be two mutexes locked, the bLockUsrMutex is the one in our "user"
 * (e.g. the queue clas)
 * rgerhards, 2008-01-21
 */
rsRetVal
wtpChkStopWrkr(wtp_t *pThis, int bLockMutex, int bLockUsrMutex)
{
	DEFiRet;
	DEFVARS_mutexProtection;

	BEGIN_MTX_PROTECTED_OPERATIONS(&pThis->mut, bLockMutex);
	if(   (pThis->wtpState == wtpState_SHUTDOWN_IMMEDIATE)
	   || ((pThis->wtpState == wtpState_SHUTDOWN) && pThis->pfIsIdle(pThis->pUsr, bLockUsrMutex)))
		iRet = RS_RET_TERMINATE_NOW;
	END_MTX_PROTECTED_OPERATIONS(&pThis->mut);

	/* try customer handler if one was set and we do not yet have a definite result */
	if(iRet == RS_RET_OK && pThis->pfChkStopWrkr != NULL) {
		iRet = pThis->pfChkStopWrkr(pThis->pUsr, bLockUsrMutex);
	}

	RETiRet;
}


/* Send a shutdown command to all workers and see if they terminate.
 * A timeout may be specified.
 * rgerhards, 2008-01-14
 */
rsRetVal
wtpShutdownAll(wtp_t *pThis, wtpState_t tShutdownCmd, struct timespec *ptTimeout)
{
	DEFiRet;
	int bTimedOut;
	int iCancelStateSave;

dbgPrintAllDebugInfo();
RUNLOG_VAR("%p", pThis);
RUNLOG_VAR("%d", tShutdownCmd);
	ISOBJ_TYPE_assert(pThis, wtp);

	wtpSetState(pThis, tShutdownCmd);
	wtpWakeupAllWrkr(pThis);
		
	/* and wait for their termination */
dbgprintf("%s: waiting for mutex %p\n", wtpGetDbgHdr(pThis), &pThis->mut);
	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &iCancelStateSave);
	d_pthread_mutex_lock(&pThis->mut);
	pthread_cleanup_push(mutexCancelCleanup, &pThis->mut);
	pthread_setcancelstate(iCancelStateSave, NULL);
	bTimedOut = 0;
	while(pThis->iCurNumWrkThrd > 0 && !bTimedOut) {
		dbgprintf("%s: waiting %ldms on worker thread termination, %d still running\n",
			   wtpGetDbgHdr(pThis), timeoutVal(ptTimeout), pThis->iCurNumWrkThrd);

		if(d_pthread_cond_timedwait(&pThis->condThrdTrm, &pThis->mut, ptTimeout) != 0) {
			dbgprintf("%s: timeout waiting on worker thread termination\n", wtpGetDbgHdr(pThis));
			bTimedOut = 1;	/* we exit the loop on timeout */
		}
	}
	pthread_cleanup_pop(1);

	if(bTimedOut)
		iRet = RS_RET_TIMED_OUT;
	
	/* see if we need to harvest (join) any terminated threads (even in timeout case,
	 * some may have terminated...
	 */
	wtpProcessThrdChanges(pThis);

dbgprintf("wtpShutdownAll exit");
	RETiRet;
}


/* indicate that a thread has terminated and awake anyone waiting on it
 * rgerhards, 2008-01-23
 */
rsRetVal wtpSignalWrkrTermination(wtp_t *pThis)
{
	DEFiRet;
	//TODO: mutex or not mutex, that's the question ;)DEFVARS_mutexProtection;

	ISOBJ_TYPE_assert(pThis, wtp);

	//BEGIN_MTX_PROTECTED_OPERATIONS(&pThis->mut, LOCK_MUTEX);
dbgprintf("signaling thread termination, cond %p\n", &pThis->condThrdTrm);
	pthread_cond_signal(&pThis->condThrdTrm); /* activate anyone waiting on thread shutdown */
	//END_MTX_PROTECTED_OPERATIONS(&pThis->mut);
	RETiRet;
}


/* Unconditionally cancel all running worker threads.
 * rgerhards, 2008-01-14
 */
rsRetVal
wtpCancelAll(wtp_t *pThis)
{
	DEFiRet;
	int i;
	// TODO: mutex?? // TODO: cancellation in wti!

	ISOBJ_TYPE_assert(pThis, wtp);

	/* process any pending thread requests so that we know who actually is still running */
	wtpProcessThrdChanges(pThis);

RUNLOG_VAR("%d", pThis->iNumWorkerThreads);;
	/* first tell the workers our request */
	for(i = 0 ; i < pThis->iNumWorkerThreads ; ++i) {
		// TODO: mutex lock!
RUNLOG_VAR("%p", pThis->pWrkr[i]);
		if(pThis->pWrkr[i]->tCurrCmd >= eWRKTHRD_TERMINATING) {
RUNLOG;
			dbgprintf("%s: canceling worker thread %d\n", wtpGetDbgHdr(pThis), i);
			pthread_cancel(pThis->pWrkr[i]->thrdID);
		}
	}
RUNLOG;

	RETiRet;
}



/* Set the Inactivity Guard
 * rgerhards, 2008-01-21
 */
rsRetVal
wtpSetInactivityGuard(wtp_t *pThis, int bNewState, int bLockMutex)
{
	DEFiRet;
	DEFVARS_mutexProtection;

RUNLOG;
	BEGIN_MTX_PROTECTED_OPERATIONS(&pThis->mut, bLockMutex);
RUNLOG;
	pThis->bInactivityGuard = bNewState;
RUNLOG;
	END_MTX_PROTECTED_OPERATIONS(&pThis->mut);
RUNLOG;

	RETiRet;
}


/* cancellation cleanup handler for executing worker
 * decrements the worker counter
 * rgerhards, 2008-01-20
 */
void
wtpWrkrExecCancelCleanup(void *arg)
{
	wtp_t *pThis = (wtp_t*) arg;

	ISOBJ_TYPE_assert(pThis, wtp);
	pThis->iCurNumWrkThrd--;
	wtpSignalWrkrTermination(pThis);

	dbgprintf("%s: thread CANCELED with %d workers running.\n", wtpGetDbgHdr(pThis), pThis->iCurNumWrkThrd);

}


/* wtp worker shell. This is started and calls into the actual
 * wti worker.
 * rgerhards, 2008-01-21
 */
static void *
wtpWorker(void *arg) /* the arg is actually a wti object, even though we are in wtp! */
{
	DEFiRet;
	DEFVARS_mutexProtection;
	wti_t *pWti = (wti_t*) arg;
	wtp_t *pThis;
	sigset_t sigSet;

	ISOBJ_TYPE_assert(pWti, wti);
	pThis = pWti->pWtp;
	ISOBJ_TYPE_assert(pThis, wtp);

	sigfillset(&sigSet);
	pthread_sigmask(SIG_BLOCK, &sigSet, NULL);

	BEGIN_MTX_PROTECTED_OPERATIONS(&pThis->mut, LOCK_MUTEX);

	/* do some late initialization */

	pthread_cleanup_push(wtpWrkrExecCancelCleanup, pThis);

	// TODO: review code below - if still needed (setState yes!)? 
	/* finally change to RUNNING state. We need to check if we actually should still run,
	 * because someone may have requested us to shut down even before we got a chance to do
	 * our init. That would be a bad race... -- rgerhards, 2008-01-16
	 */
	//if(qWrkrGetState(pWrkrInst) == eWRKTHRD_RUN_INIT)
		wtiSetState(pWti, eWRKTHRD_RUNNING, 0, MUTEX_ALREADY_LOCKED); /* we are running now! */

	do {
		END_MTX_PROTECTED_OPERATIONS(&pThis->mut);

		iRet = wtiWorker(pWti); /* just to make sure: this is NOT protected by the mutex! */

		BEGIN_MTX_PROTECTED_OPERATIONS(&pThis->mut, LOCK_MUTEX);
	} while(pThis->iCurNumWrkThrd == 1 && pThis->bInactivityGuard == 1);
	/* inactivity guard prevents shutdown of all workers while one should be running due to race
	 * condition. It can lead to one more worker running than desired, but that is acceptable. After
	 * all, that worker will shutdown itself due to inactivity timeout. If, however, none were running
	 * when one was required, processing could come to a halt. -- rgerhards, 2008-01-21
	 */

	pthread_cleanup_pop(0);
	pThis->iCurNumWrkThrd--;
	wtpSignalWrkrTermination(pThis);

	dbgprintf("%s: Worker thread %lx, terminated, num workers now %d\n",
		  wtpGetDbgHdr(pThis), (unsigned long) pWti, pThis->iCurNumWrkThrd);

	END_MTX_PROTECTED_OPERATIONS(&pThis->mut);

	ENDfunc
	pthread_exit(0);
}


/* start a new worker */
static rsRetVal
wtpStartWrkr(wtp_t *pThis, int bLockMutex)
{
	DEFiRet;
	DEFVARS_mutexProtection;
	wti_t *pWti;
	int i;
	int iState;

	ISOBJ_TYPE_assert(pThis, wtp);

	wtpProcessThrdChanges(pThis);

	BEGIN_MTX_PROTECTED_OPERATIONS(&pThis->mut, bLockMutex);

	pThis->iCurNumWrkThrd++;

	/* find free spot in thread table. If we find at least one worker that is in initialization,
	 * we do NOT start a new one. Let's give the other one a chance, first.
	 */
	for(i = 0 ; i < pThis->iNumWorkerThreads ; ++i) {
		if(wtiGetState(pThis->pWrkr[i], LOCK_MUTEX) == eWRKTHRD_STOPPED) {
			break;
		}
	}

dbgprintf("%s: after thrd search: i %d, max %d\n", wtpGetDbgHdr(pThis), i, pThis->iNumWorkerThreads);
	if(i == pThis->iNumWorkerThreads)
		ABORT_FINALIZE(RS_RET_NO_MORE_THREADS);

	pWti = pThis->pWrkr[i];
	wtiSetState(pWti, eWRKTHRD_RUN_CREATED, 0, LOCK_MUTEX);
	iState = pthread_create(&(pWti->thrdID), NULL, wtpWorker, (void*) pWti);
	dbgprintf("%s: started with state %d, num workers now %d\n",
		  wtpGetDbgHdr(pThis), iState, pThis->iCurNumWrkThrd);

	/* we try to give the starting worker a little boost. It won't help much as we still
 	 * hold the queue's mutex, but at least it has a chance to start on a single-CPU system.
 	 */
	pthread_yield();

	/* indicate we just started a worker and would like to see it running */
	wtpSetInactivityGuard(pThis, 1, MUTEX_ALREADY_LOCKED);

finalize_it:
	END_MTX_PROTECTED_OPERATIONS(&pThis->mut);
	RETiRet;
}


/* set the number of worker threads that should be running. If less than currently running,
 * a new worker may be started. Please note that there is no guarantee the number of workers
 * said will be running after we exit this function. It is just a hint. If the number is
 * higher than one, and no worker is started, the "busy" condition is signaled to awake a worker.
 * So the caller can assume that there is at least one worker re-checking if there is "work to do"
 * after this function call.
 * rgerhards, 2008-01-21
 */
rsRetVal
wtpAdviseMaxWorkers(wtp_t *pThis, int nMaxWrkr)
{
	DEFiRet;
	DEFVARS_mutexProtection;
	int nMissing; /* number workers missing to run */
	int i;

	if(pThis == NULL) dbgPrintAllDebugInfo();
	ISOBJ_TYPE_assert(pThis, wtp);

dbgprintf("%s: wtpAdviseMaxWorker with %d called, currNum %d, max %d\n", wtpGetDbgHdr(pThis), nMaxWrkr, pThis->iCurNumWrkThrd, pThis->iNumWorkerThreads);
	if(nMaxWrkr == 0)
		FINALIZE;

	BEGIN_MTX_PROTECTED_OPERATIONS(&pThis->mut, LOCK_MUTEX);

	nMissing = nMaxWrkr - pThis->iCurNumWrkThrd;
	if(nMissing > pThis->iNumWorkerThreads)
		nMissing = pThis->iNumWorkerThreads;
	else if(nMissing < 0)
		nMissing = 0;

	if(nMissing > 0) {
		dbgprintf("%s: high activity - starting %d additional worker thread(s).\n", wtpGetDbgHdr(pThis), nMissing);
		/* start the rqtd nbr of workers */
		for(i = 0 ; i < nMissing ; ++i) {
			CHKiRet(wtpStartWrkr(pThis, MUTEX_ALREADY_LOCKED));
		}
	} else {
dbgprintf("wtpAdviseMaxWorkers signals busy\n");
		wtpWakeupWrkr(pThis);
	}

	
finalize_it:
	END_MTX_PROTECTED_OPERATIONS(&pThis->mut);
	RETiRet;
}


/* some simple object access methods */
DEFpropSetMeth(wtp, toWrkShutdown, long);
DEFpropSetMeth(wtp, wtpState, wtpState_t);
DEFpropSetMeth(wtp, iNumWorkerThreads, int);
DEFpropSetMeth(wtp, pUsr, void*);
DEFpropSetMethPTR(wtp, pmutUsr, pthread_mutex_t);
DEFpropSetMethPTR(wtp, pcondBusy, pthread_cond_t);
DEFpropSetMethFP(wtp, pfChkStopWrkr, rsRetVal(*pVal)(void*, int));
DEFpropSetMethFP(wtp, pfIsIdle, rsRetVal(*pVal)(void*, int));
DEFpropSetMethFP(wtp, pfDoWork, rsRetVal(*pVal)(void*, void*, int));
DEFpropSetMethFP(wtp, pfOnIdle, rsRetVal(*pVal)(void*, int));
DEFpropSetMethFP(wtp, pfOnWorkerCancel, rsRetVal(*pVal)(void*, void*));
DEFpropSetMethFP(wtp, pfOnWorkerStartup, rsRetVal(*pVal)(void*));
DEFpropSetMethFP(wtp, pfOnWorkerShutdown, rsRetVal(*pVal)(void*));


/* set the debug header message
 * The passed-in string is duplicated. So if the caller does not need
 * it any longer, it must free it. Must be called only before object is finalized.
 * rgerhards, 2008-01-09
 */
rsRetVal
wtpSetDbgHdr(wtp_t *pThis, uchar *pszMsg, size_t lenMsg)
{
	DEFiRet;

dbgprintf("objID: %d\n", pThis->pObjInfo->objID);
	ISOBJ_TYPE_assert(pThis, wtp);
	assert(pszMsg != NULL);
	
	if(lenMsg < 1)
		ABORT_FINALIZE(RS_RET_PARAM_ERROR);

	if(pThis->pszDbgHdr != NULL) {
		free(pThis->pszDbgHdr);
		pThis->pszDbgHdr = NULL;
	}

	if((pThis->pszDbgHdr = malloc(sizeof(uchar) * lenMsg + 1)) == NULL)
		ABORT_FINALIZE(RS_RET_OUT_OF_MEMORY);

	memcpy(pThis->pszDbgHdr, pszMsg, lenMsg + 1); /* always think about the \0! */

finalize_it:
	RETiRet;
}

/* Initialize the stream class. Must be called as the very first method
 * before anything else is called inside this class.
 * rgerhards, 2008-01-09
 */
BEGINObjClassInit(wtp, 1)
ENDObjClassInit(wtp)

/*
 * vi:set ai:
 */