/*
** $Id: message.c 11176 2008-12-03 02:30:02Z houhuihua $
**
** message.c: The messaging module.
**
** Copyright (C) 2003 ~ 2007 Feynman Software.
** Copyright (C) 2000 ~ 2002 Wei Yongming.
**
** All right reserved by Feynman Software.
**
** Current maintainer: Wei Yongming.
**
** Create date: 2000/11/05
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"
#include "minigui.h"
#include "gdi.h"
#include "window.h"

#include "blockheap.h"
#include "cliprect.h"
#include "gal.h"
#include "internals.h"
#include "ctrlclass.h"
#include "timer.h"


/******************************************************************************/

#ifndef _LITE_VERSION

  #define SET_PADD(value) pMsg->pAdd = value
  #define SYNMSGNAME pMsg->pAdd?"Sync":"Normal"

#else

  #define SET_PADD(value)
  #define SYNMSGNAME "Normal"

#endif

#define ERR_MSG_CANCELED    ERR_QUEUE_FULL

/****************************** Message Allocation ****************************/
static BLOCKHEAP QMSGHeap;

/* QMSG allocation */
BOOL InitFreeQMSGList (void)
{
    InitBlockDataHeap (&QMSGHeap, sizeof (QMSG), SIZE_QMSG_HEAP);
    return TRUE;
}

void DestroyFreeQMSGList (void)
{
    DestroyBlockDataHeap (&QMSGHeap);
}

inline static PQMSG QMSGAlloc (void)
{
    return (PQMSG) BlockDataAlloc (&QMSGHeap);
}

inline static void FreeQMSG (PQMSG pqmsg)
{
    BlockDataFree (&QMSGHeap, pqmsg);
}

/****************************** Message Queue Management ************************/

BOOL InitMsgQueue (PMSGQUEUE pMsgQueue, int iBufferLen)
{
    memset (pMsgQueue, 0, sizeof(MSGQUEUE));

    pMsgQueue->dwState = QS_EMPTY;

#ifndef _LITE_VERSION
    pthread_mutex_init (&pMsgQueue->lock, NULL);
    sem_init (&pMsgQueue->wait, 0, 0);
    sem_init (&pMsgQueue->sync_msg, 0, 0);
#endif

    if (iBufferLen <= 0)
        iBufferLen = DEF_MSGQUEUE_LEN;

    pMsgQueue->msg = malloc (sizeof (MSG) * iBufferLen);

    if (!pMsgQueue->msg) {
#ifndef _LITE_VERSION
        pthread_mutex_destroy (&pMsgQueue->lock);
        sem_destroy (&pMsgQueue->wait);
        sem_destroy (&pMsgQueue->sync_msg);
        return FALSE;
#endif
    }

    pMsgQueue->len = iBufferLen;

    pMsgQueue->FirstTimerSlot = 0;
    pMsgQueue->TimerMask = 0;

    return TRUE;
}

void DestroyMsgQueue (PMSGQUEUE pMsgQueue)
{
    PQMSG head;
    PQMSG next;

    head = next = pMsgQueue->pFirstNotifyMsg;
    while (head) {
        next = head->next;

        FreeQMSG (head);
        head = next;
    }

#ifndef _LITE_VERSION
    pthread_mutex_destroy (&pMsgQueue->lock);
    sem_destroy (&pMsgQueue->wait);
    sem_destroy (&pMsgQueue->sync_msg);
#endif

    if (pMsgQueue->msg)
        free (pMsgQueue->msg);
    pMsgQueue->msg = NULL;
}

/*
 * hWnd may belong to a different thread, 
 * so this function should be thread-safe 
 */
PMSGQUEUE GetMsgQueue (HWND hWnd)
{
    PMAINWIN pWin;

    pWin = getMainWindowPtr(hWnd);

    if (pWin)
        return pWin->pMessages;
    return NULL;
}

/* post a message to a message queue */
BOOL QueueMessage (PMSGQUEUE msg_que, PMSG msg)
{
    LOCK_MSGQ(msg_que);

    /* check for the duplicated messages */
    if (msg->message == MSG_MOUSEMOVE 
            || msg->message == MSG_NCMOUSEMOVE
            || msg->message == MSG_DT_MOUSEMOVE) {
        int readpos = msg_que->readpos;
        PMSG a_msg, last_msg = NULL;

        while (readpos != msg_que->writepos) {
            a_msg = msg_que->msg + readpos;

            if (a_msg->message == msg->message
                        && a_msg->wParam == msg->wParam
                        && a_msg->hwnd == msg->hwnd) {
                last_msg = a_msg;
            }

            readpos ++;
            readpos %= msg_que->len;
        }

        if (last_msg) {
            last_msg->lParam = msg->lParam;
            last_msg->time = msg->time;
            goto ret;
        }
    }
    else if (msg->message == MSG_TIMEOUT 
                || msg->message == MSG_IDLE
                || msg->message == MSG_CARETBLINK) {
        int readpos = msg_que->readpos;
        PMSG a_msg;

        while (readpos != msg_que->writepos) {
            a_msg = msg_que->msg + readpos;

            if (a_msg->message == msg->message 
                    && a_msg->hwnd == msg->hwnd) {
                a_msg->wParam = msg->wParam;
                a_msg->lParam = msg->lParam;
                a_msg->time = msg->time;
                goto ret;
            }

            readpos ++;
            readpos %= msg_que->len;
        }
    }

    if ((msg_que->writepos + 1) % msg_que->len == msg_que->readpos) {
        UNLOCK_MSGQ(msg_que);
        return FALSE;
    }

    /* Write the data and advance write pointer */
    msg_que->msg [msg_que->writepos] = *msg;

    msg_que->writepos++;
    if (msg_que->writepos >= msg_que->len) msg_que->writepos = 0;

ret:
    msg_que->dwState |= QS_POSTMSG;

    UNLOCK_MSGQ (msg_que);

#ifndef _LITE_VERSION
    if (!BE_THIS_THREAD (msg->hwnd))
        POST_MSGQ (msg_que);
#endif

    return TRUE;
}

/******************************************************************************/

static inline WNDPROC GetWndProc (HWND hWnd)
{
     return ((PMAINWIN)hWnd)->MainWindowProc;
}

static HWND msgCheckInvalidRegion (PMAINWIN pWin)
{
    PCONTROL pCtrl = (PCONTROL)pWin;
    HWND hwnd;

    if (pCtrl->InvRgn.rgn.head)
        return (HWND)pCtrl;

    pCtrl = pCtrl->children;
    while (pCtrl) {

        if ((hwnd = msgCheckInvalidRegion ((PMAINWIN) pCtrl)))
            return hwnd;

        pCtrl = pCtrl->next;
    }

    return 0;
}

#if 0
ndef _LITE_VERSION
static PMAINWIN msgGetHostingRoot (PMAINWIN pHosted)
{
    PMAINWIN pHosting;
 
    pHosting = pHosted->pHosting;
    if (pHosting)
        return msgGetHostingRoot (pHosting);

    return pHosted;
}
#endif

static HWND msgCheckHostedTree (PMAINWIN pHosting)
{
    HWND hNeedPaint;
    PMAINWIN pHosted;

    if ( (hNeedPaint = msgCheckInvalidRegion (pHosting)) )
        return hNeedPaint;

    pHosted = pHosting->pFirstHosted;
    while (pHosted) {
        if ( (hNeedPaint = msgCheckHostedTree (pHosted)) )
            return hNeedPaint;

        pHosted = pHosted->pNextHosted;
    }

    return 0;
}

BOOL GUIAPI HavePendingMessageEx (HWND hWnd, BOOL bNoDeskTimer)
{
    PMSGQUEUE pMsgQueue;

    if (hWnd == 0) {
#ifndef _LITE_VERSION
        if (!(pMsgQueue = GetMsgQueueThisThread ()))
            return FALSE;
#else
        pMsgQueue = __mg_dsk_msg_queue;
#endif
    }
    else {
        if (!(pMsgQueue = GetMsgQueue(hWnd)))
            return FALSE;
    }

    LOCK_MSGQ (pMsgQueue);

#ifndef _LITE_VERSION
    if (pMsgQueue->dwState & QS_SYNCMSG) {
        if (pMsgQueue->pFirstSyncMsg) goto retok;
    }
#endif

    if (pMsgQueue->dwState & QS_NOTIFYMSG) {
        if (pMsgQueue->pFirstNotifyMsg) goto retok;
    }

    if (pMsgQueue->dwState & QS_POSTMSG) {
        if (pMsgQueue->readpos != pMsgQueue->writepos) goto retok;
    }

    if (pMsgQueue->dwState & (QS_QUIT | QS_PAINT)) {
        goto retok;
    }

    if (pMsgQueue->TimerMask)
        goto retok;
#ifdef _LITE_VERSION
    /* 
     * FIXME
     * We do not need to check QS_DESKTIMER, because it is for the 
     * desktop window, and user don't care it.
     */
    if (!bNoDeskTimer && (pMsgQueue->dwState & QS_DESKTIMER)) {
        goto retok;
    }
#endif

    UNLOCK_MSGQ (pMsgQueue);
#ifdef _LITE_VERSION
    return pMsgQueue->OnIdle (NULL);
#else
    return FALSE;
#endif

retok:
    UNLOCK_MSGQ (pMsgQueue);
    return TRUE;
}

BOOL GUIAPI HavePendingMessage (HWND hWnd)
{
    return HavePendingMessageEx (hWnd, FALSE);
}

int GUIAPI BroadcastMessage (int iMsg, WPARAM wParam, LPARAM lParam)
{
    MSG msg;
    
    msg.message = iMsg;
    msg.wParam = wParam;
    msg.lParam = lParam;

    return SendMessage (HWND_DESKTOP, MSG_BROADCASTMSG, 0, (LPARAM)(&msg));
}

#ifdef _MSG_STRING
#include "msgstr.h"

const char* GUIAPI Message2Str (int message)
{
    if (message >= 0x0000 && message <= 0x006F)
        return msgstr1 [message];
    else if (message >= 0x00A0 && message <= 0x010F)
        return msgstr2 [message - 0x00A0];
    else if (message >= 0x0120 && message <= 0x017F)
        return msgstr3 [message - 0x0120];
    else if (message >= 0xF000)
        return "Control Messages";
    else
        return "MSG_USER";
}

void GUIAPI PrintMessage (FILE* fp, HWND hWnd, 
                int iMsg, WPARAM wParam, LPARAM lParam)
{
    fprintf (fp, "Message %s: hWnd: %#x, wP: %x, lP: %lx.\n",
             Message2Str (iMsg), hWnd, wParam, lParam);
}

#endif

static inline void CheckCapturedMouseMessage (PMSG pMsg)
{
    if (__mg_capture_wnd == pMsg->hwnd 
            && pMsg->message >= MSG_FIRSTMOUSEMSG 
            && pMsg->message <= MSG_LASTMOUSEMSG 
            && !(pMsg->wParam & KS_CAPTURED)) {
        int x, y;
        x = LOSWORD (pMsg->lParam);
        y = HISWORD (pMsg->lParam);

        ClientToScreen (pMsg->hwnd, &x, &y);

        pMsg->lParam = MAKELONG (x, y);
        pMsg->wParam |= KS_CAPTURED;
    }
}

#define IS_MSG_WANTED(message) \
        ( (iMsgFilterMin <= 0 && iMsgFilterMax <= 0) || \
          (iMsgFilterMin > 0 && iMsgFilterMax >= iMsgFilterMin && \
           message >= iMsgFilterMin && message <= iMsgFilterMax) )

BOOL PeekMessageEx (PMSG pMsg, HWND hWnd, int iMsgFilterMin, int iMsgFilterMax, 
                          BOOL bWait, UINT uRemoveMsg)
{
    PMSGQUEUE pMsgQueue;
    PQMSG phead;

    if (!pMsg || (hWnd != HWND_DESKTOP && !MG_IS_MAIN_WINDOW(hWnd)))
        return FALSE;

#ifndef _LITE_VERSION
    if (!(pMsgQueue = GetMsgQueueThisThread ()))
            return FALSE;
#else
    pMsgQueue = __mg_dsk_msg_queue;
#endif

    memset (pMsg, 0, sizeof(MSG));

checkagain:

    LOCK_MSGQ (pMsgQueue);

    if (pMsgQueue->dwState & QS_QUIT) {
        pMsg->hwnd = hWnd;
        pMsg->message = MSG_QUIT;
        pMsg->wParam = 0;
        pMsg->lParam = 0;
        SET_PADD (NULL);

        if (uRemoveMsg == PM_REMOVE) {
            pMsgQueue->loop_depth --;
            if (pMsgQueue->loop_depth == 0)
                pMsgQueue->dwState &= ~QS_QUIT;
        }
 
        UNLOCK_MSGQ (pMsgQueue);
        return FALSE;
    }

    /* Dealing with sync messages before notify messages is better ? */
#ifndef _LITE_VERSION
    if (pMsgQueue->dwState & QS_SYNCMSG) {
        if (pMsgQueue->pFirstSyncMsg) {
            *pMsg = pMsgQueue->pFirstSyncMsg->Msg;
            SET_PADD (pMsgQueue->pFirstSyncMsg);
            if (IS_MSG_WANTED(pMsg->message)) {
              if (uRemoveMsg == PM_REMOVE) {
                  pMsgQueue->pFirstSyncMsg = pMsgQueue->pFirstSyncMsg->pNext;
              }

              UNLOCK_MSGQ (pMsgQueue);
              return TRUE;
            }
        }
        else
            pMsgQueue->dwState &= ~QS_SYNCMSG;
    }
#endif

    if (pMsgQueue->dwState & QS_NOTIFYMSG) {
        if (pMsgQueue->pFirstNotifyMsg) {
            phead = pMsgQueue->pFirstNotifyMsg;
            *pMsg = phead->Msg;
            SET_PADD (NULL);

            if (IS_MSG_WANTED(pMsg->message)) {
              if (uRemoveMsg == PM_REMOVE) {
                  pMsgQueue->pFirstNotifyMsg = phead->next;
                  FreeQMSG (phead);
              }

              UNLOCK_MSGQ (pMsgQueue);
              return TRUE;
            }
        }
        else
            pMsgQueue->dwState &= ~QS_NOTIFYMSG;
    }

    if (pMsgQueue->dwState & QS_POSTMSG) {
        if (pMsgQueue->readpos != pMsgQueue->writepos) {
            *pMsg = pMsgQueue->msg[pMsgQueue->readpos];
            SET_PADD (NULL);
            if (IS_MSG_WANTED(pMsg->message)) {
                CheckCapturedMouseMessage (pMsg);
                if (uRemoveMsg == PM_REMOVE) {
                    pMsgQueue->readpos++;
                    if (pMsgQueue->readpos >= pMsgQueue->len)
                        pMsgQueue->readpos = 0;
                }

                UNLOCK_MSGQ (pMsgQueue);
                return TRUE;
            }
        }
        else
            pMsgQueue->dwState &= ~QS_POSTMSG;
    }

    /*
     * check invalidate region of the windows
     */

    if (pMsgQueue->dwState & QS_PAINT && IS_MSG_WANTED(MSG_PAINT)) {
        PMAINWIN pHostingRoot;
        HWND hNeedPaint;
        PMAINWIN pWin;
 
#ifndef _LITE_VERSION
        /* REMIND this */
        if (hWnd == HWND_DESKTOP) {
            pMsg->hwnd = hWnd;
            pMsg->message = MSG_PAINT;
            pMsg->wParam = 0;
            pMsg->lParam = 0;
            SET_PADD (NULL);

            if (uRemoveMsg == PM_REMOVE) {
                pMsgQueue->dwState &= ~QS_PAINT;
            }
            UNLOCK_MSGQ (pMsgQueue);
            return TRUE;
        }
#endif

        pMsg->message = MSG_PAINT;
        pMsg->wParam = 0;
        pMsg->lParam = 0;
        SET_PADD (NULL);

#ifdef _LITE_VERSION
        pHostingRoot = __mg_dsk_win;
#else
        pHostingRoot = pMsgQueue->pRootMainWin;
#endif

        if ( (hNeedPaint = msgCheckHostedTree (pHostingRoot)) ) {
            pMsg->hwnd = hNeedPaint;
            pWin = (PMAINWIN) hNeedPaint;
            pMsg->lParam = (LPARAM)(&pWin->InvRgn.rgn);
            UNLOCK_MSGQ (pMsgQueue);
            return TRUE;
        }
 
        /* no paint message */
        pMsgQueue->dwState &= ~QS_PAINT;
    }

    /*
     * handle timer here
     */
#ifdef _LITE_VERSION
    if (pMsgQueue->dwState & QS_DESKTIMER) {
        pMsg->hwnd = HWND_DESKTOP;
        pMsg->message = MSG_TIMER;
        pMsg->wParam = 0;
        pMsg->lParam = 0;

        if (uRemoveMsg == PM_REMOVE) {
            pMsgQueue->dwState &= ~QS_DESKTIMER;
        }
        return TRUE;
    }
#endif

    if (pMsgQueue->TimerMask && IS_MSG_WANTED(MSG_TIMER)) {
        int slot;
        TIMER* timer;

#ifndef _LITE_VERSION
        if (hWnd == HWND_DESKTOP) {
            pMsg->hwnd = hWnd;
            pMsg->message = MSG_TIMER;
            pMsg->wParam = 0;
            pMsg->lParam = 0;
            SET_PADD (NULL);

            if (uRemoveMsg == PM_REMOVE) {
                pMsgQueue->TimerMask = 0;
            }
            UNLOCK_MSGQ (pMsgQueue);
            return TRUE;
        }
#endif

        /* get the first expired timer slot */
        slot = pMsgQueue->FirstTimerSlot;
        do {
            if (pMsgQueue->TimerMask & (0x01 << slot))
                break;

            slot ++;
            slot %= DEF_NR_TIMERS;
            if (slot == pMsgQueue->FirstTimerSlot) {
                slot = -1;
                break;
            }
        } while (TRUE);

        pMsgQueue->FirstTimerSlot ++;
        pMsgQueue->FirstTimerSlot %= DEF_NR_TIMERS;

        if ((timer = __mg_get_timer (slot))) {

            unsigned int tick_count = timer->tick_count;

            timer->tick_count = 0;
            pMsgQueue->TimerMask &= ~(0x01 << slot);

            if (timer->proc) {
                BOOL ret_timer_proc;

                /* unlock the message queue when calling timer proc */
                UNLOCK_MSGQ (pMsgQueue);

                /* calling the timer callback procedure */
                ret_timer_proc = timer->proc (timer->hWnd, 
                        timer->id, tick_count);

                /* lock the message queue again */
                LOCK_MSGQ (pMsgQueue);

                if (!ret_timer_proc) {
                    /* remove the timer */
                    __mg_remove_timer (timer, slot);
                }
            }
            else {
                pMsg->message = MSG_TIMER;
                pMsg->hwnd = timer->hWnd;
                pMsg->wParam = timer->id;
                pMsg->lParam = tick_count;
                SET_PADD (NULL);

                UNLOCK_MSGQ (pMsgQueue);
                return TRUE;
            }
        }
    }

    UNLOCK_MSGQ (pMsgQueue);

#ifndef _LITE_VERSION
    if (bWait) {
        /* no message, wait again. */
        sem_wait (&pMsgQueue->wait);
        goto checkagain;
    }
#else
    /* no message, idle */
    if (bWait) {
         pMsgQueue->OnIdle (pMsgQueue);
         goto checkagain;
    }
#endif

    /* no message */
    return FALSE;
}

/* 
The following two functions are moved to window.h as inline functions.
int GUIAPI GetMessage (PMSG pMsg, HWND hWnd)
{
    return PeekMessageEx (pMsg, hWnd, 0, 0, TRUE, PM_REMOVE);
}

BOOL GUIAPI PeekMessage (PMSG pMsg, HWND hWnd, int iMsgFilterMin, 
                         int iMsgFilterMax, UINT uRemoveMsg)
{
    return PeekMessageEx (pMsg, hWnd, iMsgFilterMin, iMsgFilterMax, 
                           FALSE, uRemoveMsg);
}
*/

/* wait for message */
BOOL GUIAPI WaitMessage (PMSG pMsg, HWND hWnd)
{
    PMSGQUEUE pMsgQueue;

    if (!pMsg)
        return FALSE;

    if (!(pMsgQueue = GetMsgQueue(hWnd)))
        return FALSE;

    memset (pMsg, 0, sizeof(MSG));

checkagain:

    LOCK_MSGQ (pMsgQueue);

    if (pMsgQueue->dwState & QS_QUIT) {
        goto getit;
    }

#ifndef _LITE_VERSION
    if (pMsgQueue->dwState & QS_SYNCMSG) {
        if (pMsgQueue->pFirstSyncMsg) {
            goto getit;
        }
    }
#endif

    if (pMsgQueue->dwState & QS_NOTIFYMSG) {
        if (pMsgQueue->pFirstNotifyMsg) {
            goto getit;
        }
    }

    if (pMsgQueue->dwState & QS_POSTMSG) {
        if (pMsgQueue->readpos != pMsgQueue->writepos) {
            goto getit;
        }
    }

    if (pMsgQueue->dwState & QS_PAINT) {
        goto getit;
    }
    
#ifdef _LITE_VERSION
    if (pMsgQueue->dwState & QS_DESKTIMER) {
        goto getit;
    }
#endif

    if (pMsgQueue->TimerMask) {
        goto getit;
    }

#ifndef _LITE_VERSION
    /* no message, wait again. */
    UNLOCK_MSGQ (pMsgQueue);
    sem_wait (&pMsgQueue->wait);
#else
    /* no message, idle */
    pMsgQueue->OnIdle (pMsgQueue);
#endif

    goto checkagain;

getit:
    UNLOCK_MSGQ (pMsgQueue);
    return TRUE;
}

BOOL GUIAPI PeekPostMessage (PMSG pMsg, HWND hWnd, int iMsgFilterMin, 
                        int iMsgFilterMax, UINT uRemoveMsg)
{
    PMSGQUEUE pMsgQueue;
    PMSG pPostMsg;

    if (!pMsg)
        return FALSE;

    if (!(pMsgQueue = GetMsgQueue(hWnd)))
        return FALSE;

    LOCK_MSGQ (pMsgQueue);
    memset (pMsg, 0, sizeof(MSG));
	
    if (pMsgQueue->dwState & QS_POSTMSG) {
    
        if (pMsgQueue->readpos != pMsgQueue->writepos) {

            pPostMsg = pMsgQueue->msg + pMsgQueue->readpos;

            if (iMsgFilterMin == 0 && iMsgFilterMax == 0)
                *pMsg = *pPostMsg;
            else if (pPostMsg->message <= iMsgFilterMax &&
                    pPostMsg->message >= iMsgFilterMin)
                *pMsg = *pPostMsg;
            else {
                UNLOCK_MSGQ (pMsgQueue);
                return FALSE;
            }
            
            SET_PADD (NULL);

            if (uRemoveMsg == PM_REMOVE) {
                pMsgQueue->readpos++;
                if (pMsgQueue->readpos >= pMsgQueue->len) 
                    pMsgQueue->readpos = 0;
            }

            UNLOCK_MSGQ (pMsgQueue);
            return TRUE;
        }

    }

    UNLOCK_MSGQ (pMsgQueue);
    return FALSE;
}

int GUIAPI SendMessage (HWND hWnd, int iMsg, WPARAM wParam, LPARAM lParam)
{
    WNDPROC WndProc;

    MG_CHECK_RET (MG_IS_WINDOW(hWnd), -1);

#ifndef _LITE_VERSION
    if (!BE_THIS_THREAD(hWnd))
        return SendSyncMessage (hWnd, iMsg, wParam, lParam);
#endif /* !_LITE_VERSION */
    
    if ( !(WndProc = GetWndProc(hWnd)) )
        return ERR_INV_HWND;

    return (*WndProc)(hWnd, iMsg, wParam, lParam);

}

int GUIAPI SendNotifyMessage (HWND hWnd, int iMsg, WPARAM wParam, LPARAM lParam)
{
    PMSGQUEUE pMsgQueue;
    PQMSG pqmsg;

    MG_CHECK_RET (MG_IS_WINDOW(hWnd), ERR_INV_HWND);

    if (!(pMsgQueue = GetMsgQueue(hWnd)))
        return ERR_INV_HWND;
  
    pqmsg = QMSGAlloc();

    LOCK_MSGQ (pMsgQueue);

    /* queue the notification message. */
    pqmsg->Msg.hwnd = hWnd;
    pqmsg->Msg.message = iMsg;
    pqmsg->Msg.wParam = wParam;
    pqmsg->Msg.lParam = lParam;
    pqmsg->next = NULL;

    if (pMsgQueue->pFirstNotifyMsg == NULL) {
        pMsgQueue->pFirstNotifyMsg = pMsgQueue->pLastNotifyMsg = pqmsg;
    }
    else {
        pMsgQueue->pLastNotifyMsg->next = pqmsg;
        pMsgQueue->pLastNotifyMsg = pqmsg;
    }

    pMsgQueue->dwState |= QS_NOTIFYMSG;

    UNLOCK_MSGQ (pMsgQueue);
#ifndef _LITE_VERSION
    if ( !BE_THIS_THREAD(hWnd) )
        POST_MSGQ(pMsgQueue);
#endif

    return ERR_OK;
}

int GUIAPI PostMessage (HWND hWnd, int iMsg, WPARAM wParam, LPARAM lParam)
{
    PMSGQUEUE pMsgQueue;
    MSG msg;

    if (!(pMsgQueue = GetMsgQueue(hWnd)))
        return ERR_INV_HWND;

    if (iMsg == MSG_PAINT) {
        LOCK_MSGQ (pMsgQueue);
        pMsgQueue->dwState |= QS_PAINT;
        UNLOCK_MSGQ (pMsgQueue);
#ifndef _LITE_VERSION
        if ( !BE_THIS_THREAD(hWnd) )
            POST_MSGQ(pMsgQueue);
#endif
        return ERR_OK;
    }

    msg.hwnd = hWnd;
    msg.message = iMsg;
    msg.wParam = wParam;
    msg.lParam = lParam;

    if (!QueueMessage(pMsgQueue, &msg))
        return ERR_QUEUE_FULL;

    return ERR_OK;
}

int GUIAPI PostQuitMessage (HWND hWnd)
{
    PMSGQUEUE pMsgQueue = NULL;

    if (hWnd && hWnd != HWND_INVALID) {
        PMAINWIN pWin = (PMAINWIN)hWnd;
        if (pWin->DataType == TYPE_HWND || pWin->DataType == TYPE_WINTODEL) {
            pMsgQueue = pWin->pMainWin->pMessages;
        }
    }

    if (pMsgQueue == NULL)
        return ERR_INV_HWND;

    pMsgQueue->loop_depth ++;
    pMsgQueue->dwState |= QS_QUIT;

#ifndef _LITE_VERSION
    if (!BE_THIS_THREAD (hWnd))
        POST_MSGQ (pMsgQueue);
#endif

    return ERR_OK;
}

int GUIAPI DispatchMessage (PMSG pMsg)
{
    WNDPROC WndProc;
#ifndef _LITE_VERSION
    PSYNCMSG pSyncMsg;
#endif
    int iRet;

#ifdef _TRACE_MSG
    fprintf (stderr, "Message, %s: hWnd: %x, wP: %x, lP: %lx. %s\n",
        Message2Str (pMsg->message),
        pMsg->hwnd,
        pMsg->wParam,
        pMsg->lParam,
        SYNMSGNAME);
#endif

    if (pMsg->hwnd == HWND_INVALID) {
#ifndef _LITE_VERSION
        if (pMsg->pAdd) {
            pSyncMsg = (PSYNCMSG)pMsg->pAdd;
            pSyncMsg->retval = ERR_MSG_CANCELED;
            sem_post (pSyncMsg->sem_handle);
        }
#endif
        
#ifdef _TRACE_MSG
        fprintf (stderr, "Message have been thrown away: %s\n",
            Message2Str (pMsg->message));
#endif
        return -1;
    }

    if (pMsg->hwnd == 0)
        return -1;

    if (!(WndProc = GetWndProc (pMsg->hwnd)))
        return -1;

    iRet = (*WndProc)(pMsg->hwnd, pMsg->message, pMsg->wParam, pMsg->lParam);

#ifndef _LITE_VERSION
    /* this is a sync message. */
    if (pMsg->pAdd) {
        pSyncMsg = (PSYNCMSG)pMsg->pAdd;
        pSyncMsg->retval = iRet;
        sem_post (pSyncMsg->sem_handle);
    }
#endif

#ifdef _TRACE_MSG
    fprintf (stderr, "Message, %s done, return value: %x\n",
        Message2Str (pMsg->message), iRet);
#endif

    return iRet;
}

/* define this to print debug info of ThrowAwayMessages */
/* #define _DEBUG_TAM 1 */
#undef _DEBUG_TAM

int GUIAPI ThrowAwayMessages (HWND hWnd)
{
    PMSG        pMsg;
    PMSGQUEUE   pMsgQueue = NULL;
    PQMSG       pQMsg;
    int         nCountN = 0;
    int         nCountS = 0;
    int         nCountP = 0;
    int         readpos;
    int         slot;

    if (hWnd && hWnd != HWND_INVALID) {
        PMAINWIN pWin = (PMAINWIN)hWnd;
        if (pWin->DataType == TYPE_HWND || pWin->DataType == TYPE_WINTODEL) {
            pMsgQueue = pWin->pMainWin->pMessages;
        }
    }

    if (pMsgQueue == NULL)
        return ERR_INV_HWND;

    LOCK_MSGQ (pMsgQueue);

    if (pMsgQueue->pFirstNotifyMsg) {
        pQMsg = pMsgQueue->pFirstNotifyMsg;
        
        while (pQMsg) {
            pMsg = &pQMsg->Msg;

            if (pMsg->hwnd == hWnd || GetMainWindowPtrOfControl (pMsg->hwnd) 
                    == (PMAINWIN)hWnd) {
                pMsg->hwnd = HWND_INVALID;
                nCountN ++;
            }

            pQMsg = pQMsg->next;
        }
    }

#ifdef _DEBUG_TAM
    printf ("ThrowAwayMessages: %d notification messages thrown\n", nCountN);
#endif

#ifndef _LITE_VERSION
    if (pMsgQueue->pFirstSyncMsg) {
        PSYNCMSG pSyncMsg, pSyncPrev = NULL;
        pSyncMsg = pMsgQueue->pFirstSyncMsg;
        
        while (pSyncMsg) {
            pMsg = &pSyncMsg->Msg;

            if (pMsg->hwnd == hWnd || GetMainWindowPtrOfControl (pMsg->hwnd)
                            == (PMAINWIN)hWnd) {
                pMsg->hwnd = HWND_INVALID;
                nCountS ++;

                /* 
                 * notify the waiting thread and remove the node from 
                 * msg queue
                 */
                pSyncMsg->retval = ERR_MSG_CANCELED;
                if (pSyncPrev) {
                    pSyncPrev->pNext = pSyncMsg->pNext ? pSyncMsg->pNext : NULL;
                }
                else {
                    pSyncPrev = pSyncMsg;
                    pSyncMsg = pSyncMsg->pNext;
                    pMsgQueue->pFirstSyncMsg = pSyncMsg;
                    sem_post (pSyncPrev->sem_handle);
                    pSyncPrev = NULL;
                    continue;
                }
                sem_post (pSyncMsg->sem_handle);
            }

            pSyncPrev = pSyncMsg;
            pSyncMsg = pSyncMsg->pNext;
        }
    }
    
#ifdef _DEBUG_TAM
    printf ("ThrowAwayMessages: %d sync messages thrown\n", nCountS);
#endif

#endif

    readpos = pMsgQueue->readpos;
    while (readpos != pMsgQueue->writepos) {
        pMsg = pMsgQueue->msg + readpos;

        if (pMsg->hwnd == hWnd
                || GetMainWindowPtrOfControl (pMsg->hwnd) == (PMAINWIN)hWnd) {
            pMsg->hwnd = HWND_INVALID;
            nCountP ++;
        }
        
        readpos++;
        if (readpos >= pMsgQueue->len) 
            readpos = 0;
    }

#ifdef _DEBUG_TAM
    printf ("ThrowAwayMessages: %d post messages thrown\n", nCountP);
#endif

    /* clear timer message flags of this window */
    for (slot = 0; slot < DEF_NR_TIMERS; slot++) {
        if (pMsgQueue->TimerMask & (0x01 << slot)) {
            HWND timer_wnd = __mg_get_timer_hwnd (slot);
            if (timer_wnd == hWnd 
                    || GetMainWindowPtrOfControl (timer_wnd) == (PMAINWIN)hWnd){
                RemoveMsgQueueTimerFlag (pMsgQueue, slot);
            }
        }
    }

    UNLOCK_MSGQ (pMsgQueue);

    return nCountN + nCountS + nCountP;
}

#ifdef _LITE_VERSION

BOOL GUIAPI EmptyMessageQueue (HWND hWnd)
{
    PMSGQUEUE   pMsgQueue;
    PQMSG       pQMsg, temp;

    if (!(pMsgQueue = GetMsgQueue(hWnd)))
        return FALSE;

    if (pMsgQueue->pFirstNotifyMsg) {
        pQMsg = pMsgQueue->pFirstNotifyMsg;
        while (pQMsg) {
            temp = pQMsg->next;
            FreeQMSG (pQMsg);
            pQMsg = temp;
        }
    }

    pMsgQueue->pFirstNotifyMsg = NULL;
    pMsgQueue->pLastNotifyMsg = NULL;

    pMsgQueue->readpos = 0;
    pMsgQueue->writepos = 0;

    TerminateTimer ();

    pMsgQueue->dwState = QS_EMPTY;
    pMsgQueue->TimerMask = 0;

    return TRUE;
}

#else

/* send a synchronous message to a window in a different thread */
int SendSyncMessage (HWND hWnd, int msg, WPARAM wParam, LPARAM lParam)
{
    PMSGQUEUE pMsgQueue, thinfo = NULL;
    SYNCMSG SyncMsg;
    sem_t sync_msg;

    if (!(pMsgQueue = GetMsgQueue(hWnd)))
        return ERR_INV_HWND;

    if ((thinfo = GetMsgQueueThisThread ())) {
        /* avoid to create a new semaphore object */
        SyncMsg.sem_handle = &thinfo->sync_msg;
    }
    else {
        /* this is not a GUI thread */
        sem_init (&sync_msg, 0, 0);
        SyncMsg.sem_handle = &sync_msg;
    }

    /* queue the sync message. */
    SyncMsg.Msg.hwnd = hWnd;
    SyncMsg.Msg.message = msg;
    SyncMsg.Msg.wParam = wParam;
    SyncMsg.Msg.lParam = lParam;
    SyncMsg.retval = ERR_MSG_CANCELED;
    SyncMsg.pNext = NULL;

    LOCK_MSGQ (pMsgQueue);

    if (pMsgQueue->pFirstSyncMsg == NULL) {
        pMsgQueue->pFirstSyncMsg = pMsgQueue->pLastSyncMsg = &SyncMsg;
    }
    else {
        pMsgQueue->pLastSyncMsg->pNext = &SyncMsg;
        pMsgQueue->pLastSyncMsg = &SyncMsg;
    }

    pMsgQueue->dwState |= QS_SYNCMSG;

    UNLOCK_MSGQ (pMsgQueue);
    POST_MSGQ (pMsgQueue);

    /* add by houhh 20081203, deal with itself's SyncMsg first before 
     * SendSyncMessage to other thread, because other thread maybe wait
     * for SyncMsg for you.*/
    if (thinfo) {
        MSG msg;
        PMSGQUEUE pMsgQueue = thinfo;

        while (TRUE) {
            LOCK_MSGQ (pMsgQueue);
            if (pMsgQueue->dwState & QS_SYNCMSG) {
                if (pMsgQueue->pFirstSyncMsg) {
                    msg = pMsgQueue->pFirstSyncMsg->Msg;
                    msg.pAdd = (pMsgQueue->pFirstSyncMsg);
                    pMsgQueue->pFirstSyncMsg = pMsgQueue->pFirstSyncMsg->pNext;
                    UNLOCK_MSGQ (pMsgQueue);
                    TranslateMessage(&msg);
                    DispatchMessage(&msg);
                }
                else {
                    UNLOCK_MSGQ (pMsgQueue);
                    pMsgQueue->dwState &= ~QS_SYNCMSG;
                    break;
                }
            }
            else {
                UNLOCK_MSGQ (pMsgQueue);
                break;
            }
        }
    }

    /* suspend until the message has been handled. */
    if (sem_wait (SyncMsg.sem_handle) < 0) {
        fprintf (stderr, 
            "SendSyncMessage: thread is interrupted abnormally!\n");
    }

    if (thinfo == NULL)
        sem_destroy (&sync_msg);

    return SyncMsg.retval;
}

int GUIAPI PostSyncMessage (HWND hWnd, int msg, WPARAM wParam, LPARAM lParam)
{
    MG_CHECK_RET (MG_IS_WINDOW(hWnd), -1);
    if (BE_THIS_THREAD(hWnd))
        return -1;

    return SendSyncMessage (hWnd, msg, wParam, lParam);
}

int GUIAPI SendAsyncMessage (HWND hWnd, int iMsg, WPARAM wParam, LPARAM lParam)
{
    WNDPROC WndProc;
    
    MG_CHECK_RET (MG_IS_WINDOW(hWnd), -1);

    if ( !(WndProc = GetWndProc(hWnd)) )
        return -1;

    return (*WndProc)(hWnd, iMsg, wParam, lParam);
}

#endif /* !LITE_VERSION */

