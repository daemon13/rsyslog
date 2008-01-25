/* Definition of the worker thread instance (wti) class.
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

#ifndef WTI_H_INCLUDED
#define WTI_H_INCLUDED

#include <pthread.h>
#include "wtp.h"
#include "obj.h"

/* the worker thread instance class */
typedef struct wti_s {
	BEGINobjInstance;
	pthread_t thrdID;  /* thread ID */
	qWrkCmd_t tCurrCmd; /* current command to be carried out by worker */
	obj_t *pUsrp;		/* pointer to an object meaningful for current user pointer (e.g. queue pUsr data elemt) */
	wtp_t *pWtp; /* my worker thread pool (important if only the work thread instance is passed! */
	pthread_cond_t condInitDone; /* signaled when the thread startup is done (once per thread existance) */
	pthread_mutex_t mut;
	uchar *pszDbgHdr;	/* header string for debug messages */
} wti_t;

/* some symbolic constants for easier reference */


/* prototypes */
rsRetVal wtiConstruct(wti_t **ppThis);
rsRetVal wtiConstructFinalize(wti_t *pThis);
rsRetVal wtiDestruct(wti_t **ppThis);
rsRetVal wtiWorker(wti_t *pThis);
rsRetVal wtiProcessThrdChanges(wti_t *pThis, int bLockMutex);
rsRetVal wtiSetDbgHdr(wti_t *pThis, uchar *pszMsg, size_t lenMsg);
rsRetVal wtiSetState(wti_t *pThis, qWrkCmd_t tCmd, int bActiveOnly, int bLockMutex);
rsRetVal wtiJoinThrd(wti_t *pThis);
qWrkCmd_t wtiGetState(wti_t *pThis, int bLockMutex);
PROTOTYPEObjClassInit(wti);
PROTOTYPEpropSetMeth(wti, pszDbgHdr, uchar*);
PROTOTYPEpropSetMeth(wti, pWtp, wtp_t*);

#endif /* #ifndef WTI_H_INCLUDED */