/*
 * main.c - AmigaOS 4 handler entry wrapper
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "handler.h"

#include <dos/dos.h>
#include <dos/dosextens.h>

#include <proto/exec.h>

static void return_startup_packet(struct DosPacket *pkt,
                                  LONG res1,
                                  LONG res2)
{
    struct MsgPort *replyport;
    struct Message *msg;

    if (!pkt || !pkt->dp_Link || !pkt->dp_Port)
        return;

    replyport = pkt->dp_Port;
    msg = pkt->dp_Link;

    pkt->dp_Res1 = res1;
    pkt->dp_Res2 = res2;
    msg->mn_Node.ln_Name = (char *)pkt;
    msg->mn_Node.ln_Succ = NULL;
    msg->mn_Node.ln_Pred = NULL;
    PutMsg(replyport, msg);
}

int main(void)
{
    struct Process *proc;
    struct Message *msg;
    struct DosPacket *pkt;

    proc = (struct Process *)FindTask(NULL);
    if (!proc)
        return RETURN_FAIL;

    WaitPort(&proc->pr_MsgPort);
    msg = GetMsg(&proc->pr_MsgPort);
    if (!msg) {
        proc->pr_Result2 = ERROR_OBJECT_WRONG_TYPE;
        return RETURN_FAIL;
    }

    if (!msg->mn_Node.ln_Name) {
        ReplyMsg(msg);
        proc->pr_Result2 = ERROR_OBJECT_WRONG_TYPE;
        return RETURN_FAIL;
    }

    pkt = (struct DosPacket *)msg->mn_Node.ln_Name;
    if (pkt->dp_Type != ACTION_STARTUP) {
        return_startup_packet(pkt, DOSFALSE, ERROR_ACTION_NOT_KNOWN);
        proc->pr_Result2 = ERROR_ACTION_NOT_KNOWN;
        return RETURN_FAIL;
    }

    handler_main_startup(msg);
    return RETURN_OK;
}
