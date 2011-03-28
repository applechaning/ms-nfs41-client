/* Copyright (c) 2010
 * The Regents of the University of Michigan
 * All Rights Reserved
 *
 * Permission is granted to use, copy and redistribute this software
 * for noncommercial education and research purposes, so long as no
 * fee is charged, and so long as the name of the University of Michigan
 * is not used in any advertising or publicity pertaining to the use
 * or distribution of this software without specific, written prior
 * authorization.  Permission to modify or otherwise create derivative
 * works of this software is not granted.
 *
 * This software is provided as is, without representation or warranty
 * of any kind either express or implied, including without limitation
 * the implied warranties of merchantability, fitness for a particular
 * purpose, or noninfringement.  The Regents of the University of
 * Michigan shall not be liable for any damages, including special,
 * indirect, incidental, or consequential damages, with respect to any
 * claim arising out of or in connection with the use of the software,
 * even if it has been or is hereafter advised of the possibility of
 * such damages.
 */
#include <Windows.h>
#include <strsafe.h>

#include "nfs41_ops.h"
#include "nfs41_callback.h"
#include "daemon_debug.h"


#define CBSLVL 2 /* dprintf level for callback server logging */


static const char g_server_tag[] = "ms-nfs41-callback";


/* callback session */
static void replay_cache_write(
    IN nfs41_cb_session *session,
    IN struct cb_compound_args *args,
    IN struct cb_compound_res *res,
    IN bool_t cachethis);

void nfs41_callback_session_init(
    IN nfs41_session *session)
{
    struct cb_compound_res *res;

    session->cb_session.cb_sessionid = session->session_id;

    /* initialize the replay cache with status NFS4ERR_SEQ_MISORDERED */
    res = calloc(1, sizeof(struct cb_compound_res));
    if (res == NULL) {
        /* don't need to return failure, just leave cb_replay_cached=0 */
        return;
    }

    StringCchCopyA(res->tag.str, CB_COMPOUND_MAX_TAG, g_server_tag);
    res->tag.len = sizeof(g_server_tag);
    res->status = NFS4ERR_SEQ_MISORDERED;

    replay_cache_write(&session->cb_session, NULL, res, FALSE);
    free(res);
}


/* OP_CB_LAYOUTRECALL */
static enum_t handle_cb_layoutrecall(
    IN nfs41_rpc_clnt *rpc_clnt,
    IN struct cb_layoutrecall_args *args,
    OUT struct cb_layoutrecall_res *res)
{
    enum pnfs_status status;

    status = pnfs_file_layout_recall(rpc_clnt->client, args);
    switch (status) {
    case PNFS_PENDING:
        /* not enough information to process the recall yet */
        res->status = NFS4ERR_DELAY;
        break;
    default:
        /* forgetful model for layout recalls */
        res->status = NFS4ERR_NOMATCHING_LAYOUT;
        break;
    }

    dprintf(CBSLVL, "  OP_CB_LAYOUTRECALL { %s, %s, recall %u } %s\n",
        pnfs_layout_type_string(args->type),
        pnfs_iomode_string(args->iomode), args->recall.type,
        nfs_error_string(res->status));
    return res->status;
}

/* OP_CB_RECALL_SLOT */
static enum_t handle_cb_recall_slot(
    IN struct cb_recall_slot_args *args,
    OUT struct cb_recall_slot_res *res)
{
    res->status = NFS4_OK;

    dprintf(CBSLVL, "  OP_CB_RECALL_SLOT { %u } %s\n",
        args->target_highest_slotid, nfs_error_string(res->status));
    return res->status;
}

/* OP_CB_SEQUENCE */
static enum_t handle_cb_sequence(
    IN nfs41_rpc_clnt *rpc_clnt,
    IN struct cb_sequence_args *args,
    OUT struct cb_sequence_res *res,
    OUT nfs41_cb_session **session_out,
    OUT bool_t *cachethis)
{
    nfs41_cb_session *cb_session = &rpc_clnt->client->session->cb_session;
    uint32_t status = NFS4_OK;
    res->status = NFS4_OK;

    *session_out = cb_session;

    /* validate the sessionid */
    if (memcmp(cb_session->cb_sessionid, args->sessionid,
            NFS4_SESSIONID_SIZE)) {
        eprintf("[cb] received sessionid doesn't match session\n");
        res->status = NFS4ERR_BADSESSION;
        goto out;
    }

    /* we only support 1 slot for the back channel so slotid MUST be 0 */
    if (args->slotid != 0) {
        eprintf("[cb] received unexpected slotid=%d\n", args->slotid);
        res->status = NFS4ERR_BADSLOT;
        goto out;
    }
    if (args->highest_slotid != 0) {
        eprintf("[cb] received unexpected highest_slotid=%d\n", 
            args->highest_slotid);
        res->status = NFS4ERR_BAD_HIGH_SLOT;
        goto out;
    }

    /* check for a retry with the same seqid */
    if (args->sequenceid == cb_session->cb_seqnum) {
        if (!cb_session->replay.res.length) {
            /* return success for sequence, but fail the next operation */
            res->status = NFS4_OK;
            status = NFS4ERR_RETRY_UNCACHED_REP;
        } else {
            /* return NFS4ERR_SEQ_FALSE_RETRY for all replays; if the retry
             * turns out to be valid, this response will be replaced anyway */
            status = res->status = NFS4ERR_SEQ_FALSE_RETRY;
        }
        goto out;
    }

    /* error on any unexpected seqids */
    if (args->sequenceid != cb_session->cb_seqnum+1) {
        eprintf("[cb] bad received seq#=%d, expected=%d\n", 
            args->sequenceid, cb_session->cb_seqnum+1);
        res->status = NFS4ERR_SEQ_MISORDERED;
        goto out;
    }

    cb_session->cb_seqnum = args->sequenceid;
    *cachethis = args->cachethis;

    memcpy(res->ok.sessionid, args->sessionid, NFS4_SESSIONID_SIZE);
    res->ok.sequenceid = args->sequenceid;
    res->ok.slotid = args->slotid;
    res->ok.highest_slotid = args->highest_slotid;
    res->ok.target_highest_slotid = args->highest_slotid;

out:
    dprintf(CBSLVL, "  OP_CB_SEQUENCE { seqid %u, slot %u, cachethis %d } "
        "%s\n", args->sequenceid, args->slotid, args->cachethis, 
        nfs_error_string(res->status));
    return status;
}

/* OP_CB_RECALL */
typedef struct _nfs41_cb_recall {
    nfs41_rpc_clnt *rpc_clnt;
    struct cb_recall_args *args;
} nfs41_cb_recall;

static unsigned int WINAPI _handle_cb_recall(void *args)
{
    nfs41_cb_recall *cb_args = (nfs41_cb_recall *)args;
    nfs41_path_fh path_fh;

    dprintf(1, "_handle_cb_recall: start\n");
    print_hexbuf(3, (unsigned char *)"_handle_cb_recall: fh ", 
        cb_args->args->fh.fh, cb_args->args->fh.len);
    print_hexbuf(3, (unsigned char *)"_handle_cb_recall: stateid ", 
        cb_args->args->stateid.other, NFS4_STATEID_OTHER);
    ZeroMemory(&path_fh, sizeof(nfs41_path_fh));
    memcpy(&path_fh.fh, &cb_args->args->fh, sizeof(nfs41_fh));
    path_fh.fh.superblock = NULL;
    path_fh.path = NULL;
    path_fh.name.len = 0;
    dprintf(1, "_handle_cb_recall: sending nfs41_delegreturn\n");
    nfs41_delegreturn(cb_args->rpc_clnt->client->session, &path_fh, 
        &cb_args->args->stateid);
    nfs41_root_deref(cb_args->rpc_clnt->client->root);
    free(cb_args->args);
    free(cb_args);
    dprintf(1, "_handle_cb_recall: end\n");
    return 1;
}

static enum_t handle_cb_recall(
    IN nfs41_rpc_clnt *rpc_clnt,
    IN struct cb_recall_args *args,
    OUT struct cb_recall_res *res)
{
    nfs41_cb_recall *cb_args;
    uintptr_t status;
    res->status = NFS4_OK;

    dprintf(CBSLVL, "OP_CB_RECALL\n");
    cb_args = calloc(1, sizeof(nfs41_cb_recall));
    if (cb_args == NULL) {
        res->status = NFS4ERR_SERVERFAULT;
        goto out;
    }
    cb_args->rpc_clnt = rpc_clnt;
    cb_args->args = calloc(1, sizeof(struct cb_recall_args));
    if (cb_args->args == NULL) {
        free(cb_args);
        res->status = NFS4ERR_SERVERFAULT;
        goto out;
    }
    memcpy(cb_args->args, args, sizeof(struct cb_recall_args));
    status = _beginthreadex(NULL, 0, _handle_cb_recall, cb_args, 0, NULL);
    if (status == -1L || !status) {
        eprintf("_beginthreadex failed to start for _handle_cb_recall %d", 
            status);
        free(cb_args->args);
        free(cb_args);
        res->status = NFS4ERR_SERVERFAULT;
        goto out;
    }
    nfs41_root_ref(rpc_clnt->client->root);

out:
    return res->status;
}

static void replay_cache_write(
    IN nfs41_cb_session *session,
    IN OPTIONAL struct cb_compound_args *args,
    IN struct cb_compound_res *res,
    IN bool_t cachethis)
{
    XDR xdr;
    uint32_t i;

    session->replay.arg.length = 0;
    session->replay.res.length = 0;

    /* encode the reply directly into the replay cache */
    xdrmem_create(&xdr, (char*)session->replay.res.buffer,
        NFS41_MAX_SERVER_CACHE, XDR_ENCODE);

    /* always try to cache the result */
    if (proc_cb_compound_res(&xdr, res)) {
        session->replay.res.length = XDR_GETPOS(&xdr);

        if (args) {
            /* encode the arguments into the request cache */
            xdrmem_create(&xdr, (char*)session->replay.arg.buffer,
                NFS41_MAX_SERVER_CACHE, XDR_ENCODE);

            if (proc_cb_compound_args(&xdr, args))
                session->replay.arg.length = XDR_GETPOS(&xdr);
        }
    } else if (cachethis) {
        /* on failure, only return errors if caching was requested */
        res->status = NFS4ERR_REP_TOO_BIG_TO_CACHE;

        /* find the first operation that failed to encode */
        for (i = 0; i < res->resarray_count; i++) {
            if (!res->resarray[i].xdr_ok) {
                res->resarray[i].res.status = NFS4ERR_REP_TOO_BIG_TO_CACHE;
                res->resarray_count = i + 1;
                break;
            }
        }
    }
}

static bool_t replay_validate_args(
    IN struct cb_compound_args *args,
    IN const struct replay_cache *cache)
{
    char buffer[NFS41_MAX_SERVER_CACHE];
    XDR xdr;

    /* encode the current arguments into a temporary buffer */
    xdrmem_create(&xdr, buffer, NFS41_MAX_SERVER_CACHE, XDR_ENCODE);

    if (!proc_cb_compound_args(&xdr, args))
        return FALSE;

    /* must match the cached length */
    if (XDR_GETPOS(&xdr) != cache->length)
        return FALSE;

    /* must match the cached buffer contents */
    return memcmp(cache->buffer, buffer, cache->length) == 0;
}

static bool_t replay_validate_ops(
    IN const struct cb_compound_args *args,
    IN const struct cb_compound_res *res)
{
    uint32_t i;
    for (i = 0; i < res->resarray_count; i++) {
        /* can't have more operations than the request */
        if (i >= args->argarray_count)
            return FALSE;

        /* each opnum must match the request */
        if (args->argarray[i].opnum != res->resarray[i].opnum)
            return FALSE;

        if (res->resarray[i].res.status)
            break;
    }
    return TRUE;
}

static int replay_cache_read(
    IN nfs41_cb_session *session,
    IN struct cb_compound_args *args,
    OUT struct cb_compound_res **res_out)
{
    XDR xdr;
    struct cb_compound_res *replay;
    struct cb_compound_res *res = *res_out;
    uint32_t status = NFS4_OK;

    replay = calloc(1, sizeof(struct cb_compound_res));
    if (replay == NULL) {
        eprintf("[cb] failed to allocate replay buffer\n");
        status = NFS4ERR_SERVERFAULT;
        goto out;
    }

    /* decode the response from the replay cache */
    xdrmem_create(&xdr, (char*)session->replay.res.buffer,
        NFS41_MAX_SERVER_CACHE, XDR_DECODE);
    if (!proc_cb_compound_res(&xdr, replay)) {
        eprintf("[cb] failed to decode replay buffer\n");
        status = NFS4ERR_SEQ_FALSE_RETRY;
        goto out_free_replay;
    }

    /* if we cached the arguments, use them to validate the retry */
    if (session->replay.arg.length) {
        if (!replay_validate_args(args, &session->replay.arg)) {
            eprintf("[cb] retry attempt with different arguments\n");
            status = NFS4ERR_SEQ_FALSE_RETRY;
            goto out_free_replay;
        }
    } else { /* otherwise, comparing opnums is the best we can do */
        if (!replay_validate_ops(args, replay)) {
            eprintf("[cb] retry attempt with different operations\n");
            status = NFS4ERR_SEQ_FALSE_RETRY;
            goto out_free_replay;
        }
    }

    /* free previous response and replace it with the replay */
    xdr.x_op = XDR_FREE;
    proc_cb_compound_res(&xdr, res);

    dprintf(2, "[cb] retry: returning cached response\n");

    *res_out = replay;
out:
    return status;

out_free_replay:
    xdr.x_op = XDR_FREE;
    proc_cb_compound_res(&xdr, replay);
    goto out;
}

/* CB_COMPOUND */
static void handle_cb_compound(nfs41_rpc_clnt *rpc_clnt, cb_req *req, struct cb_compound_res **reply)
{
    struct cb_compound_args args = { 0 };
    struct cb_compound_res *res = NULL;
    struct cb_argop *argop;
    struct cb_resop *resop;
    XDR *xdr = (XDR*)req->xdr;
    nfs41_cb_session *session = NULL;
    bool_t cachethis = FALSE;
    uint32_t i, status = NFS4_OK;

    dprintf(CBSLVL, "--> handle_cb_compound()\n");

    /* decode the arguments */
    if (!proc_cb_compound_args(xdr, &args)) {
        status = NFS4ERR_BADXDR;
        eprintf("failed to decode compound arguments\n");
    }

    /* allocate the compound results */
    res = calloc(1, sizeof(struct cb_compound_res));
    if (res == NULL) {
        status = NFS4ERR_SERVERFAULT;
        goto out;
    }
    res->status = status;
    StringCchCopyA(res->tag.str, CB_COMPOUND_MAX_TAG, g_server_tag);
    res->tag.str[CB_COMPOUND_MAX_TAG-1] = 0;
    res->tag.len = (uint32_t)strlen(res->tag.str);
    res->resarray = calloc(args.argarray_count, sizeof(struct cb_resop));
    if (res->resarray == NULL) {
        res->status = NFS4ERR_SERVERFAULT;
        goto out;
    }

    dprintf(CBSLVL, "CB_COMPOUND('%s', %u)\n", args.tag.str, args.argarray_count);
    if (args.minorversion != 1) {
        res->status = NFS4ERR_MINOR_VERS_MISMATCH; //XXXXX
        eprintf("args.minorversion %u != 1\n", args.minorversion);
        goto out;
    }

    /* handle each operation in the compound */
    for (i = 0; i < args.argarray_count && res->status == NFS4_OK; i++) {
        argop = &args.argarray[i];
        resop = &res->resarray[i];
        resop->opnum = argop->opnum;
        res->resarray_count++;

        /* 20.9.3: The error NFS4ERR_SEQUENCE_POS MUST be returned
         * when CB_SEQUENCE is found in any position in a CB_COMPOUND 
         * beyond the first.  If any other operation is in the first 
         * position of CB_COMPOUND, NFS4ERR_OP_NOT_IN_SESSION MUST 
         * be returned.
         */
        if (i == 0 && argop->opnum != OP_CB_SEQUENCE) {
            res->status = resop->res.status = NFS4ERR_OP_NOT_IN_SESSION;
            break;
        }
        if (i != 0 && argop->opnum == OP_CB_SEQUENCE) {
            res->status = resop->res.status = NFS4ERR_SEQUENCE_POS;
            break;
        }
        if (status == NFS4ERR_RETRY_UNCACHED_REP) {
            res->status = resop->res.status = status;
            break;
        }

        switch (argop->opnum) {
        case OP_CB_LAYOUTRECALL:
            dprintf(1, "OP_CB_LAYOUTRECALL\n");
            res->status = handle_cb_layoutrecall(rpc_clnt,
                &argop->args.layoutrecall, &resop->res.layoutrecall);
            break;
        case OP_CB_RECALL_SLOT:
            dprintf(1, "OP_CB_RECALL_SLOT\n");
            res->status = handle_cb_recall_slot(&argop->args.recall_slot,
                &resop->res.recall_slot);
            break;
        case OP_CB_SEQUENCE:
            dprintf(1, "OP_CB_SEQUENCE\n");
            status = handle_cb_sequence(rpc_clnt, &argop->args.sequence,
                &resop->res.sequence, &session, &cachethis);

            if (status == NFS4ERR_SEQ_FALSE_RETRY) {
                /* replace the current results with the cached response */
                status = replay_cache_read(session, &args, &res);
                if (status) res->status = status;
                goto out;
            }

            if (status == NFS4_OK)
                res->status = resop->res.sequence.status;
            break;
        case OP_CB_GETATTR:
            dprintf(1, "OP_CB_GETATTR\n");
            res->status = NFS4ERR_NOTSUPP;
            break;
        case OP_CB_RECALL:
            dprintf(1, "OP_CB_RECALL\n");
            res->status = handle_cb_recall(rpc_clnt,
                &argop->args.recall, &resop->res.recall);
            break;
        case OP_CB_NOTIFY:
            dprintf(1, "OP_CB_NOTIFY\n");
            res->status = NFS4ERR_NOTSUPP;
            break;
        case OP_CB_PUSH_DELEG:
            dprintf(1, "OP_CB_PUSH_DELEG\n");
            res->status = NFS4ERR_NOTSUPP;
            break;
        case OP_CB_RECALL_ANY:
            dprintf(1, "OP_CB_RECALL_ANY\n");
            res->status = NFS4ERR_NOTSUPP;
            break;
        case OP_CB_RECALLABLE_OBJ_AVAIL:
            dprintf(1, "OP_CB_RECALLABLE_OBJ_AVAIL\n");
            res->status = NFS4ERR_NOTSUPP;
            break;
        case OP_CB_WANTS_CANCELLED:
            dprintf(1, "OP_CB_WANTS_CANCELLED\n");
            res->status = NFS4ERR_NOTSUPP;
            break;
        case OP_CB_NOTIFY_LOCK:
            dprintf(1, "OP_CB_NOTIFY_LOCK\n");
            res->status = NFS4ERR_NOTSUPP;
            break;
        case OP_CB_NOTIFY_DEVICEID:
            dprintf(1, "OP_CB_NOTIFY_DEVICEID\n");
            res->status = NFS4ERR_NOTSUPP;
            break;
        case OP_CB_ILLEGAL:
            dprintf(1, "OP_CB_ILLEGAL\n");
            res->status = NFS4ERR_NOTSUPP;
            break;
        default:
            eprintf("operation %u not supported\n", argop->opnum);
            res->status = NFS4ERR_NOTSUPP;
            break;
        }
    }

    /* always attempt to cache the reply */
    if (session)
        replay_cache_write(session, &args, res, cachethis);
out:
    /* free the arguments */
    xdr->x_op = XDR_FREE;
    proc_cb_compound_args(xdr, &args);

    *reply = res;
    dprintf(CBSLVL, "<-- handle_cb_compound() returning %s (%u results)\n",
        nfs_error_string(res ? res->status : status),
        res ? res->resarray_count : 0);
}

int nfs41_handle_callback(void *rpc_clnt, void *cb, struct cb_compound_res **reply)
{
    nfs41_rpc_clnt *rpc = (nfs41_rpc_clnt *)rpc_clnt;
    cb_req *request = (cb_req *)cb;
    uint32_t status = 0;

    dprintf(1, "nfs41_handle_callback: received call\n");
    if (request->rq_prog != NFS41_RPC_CBPROGRAM) {
        eprintf("invalid rpc program %u\n", request->rq_prog);
        status = 2;
        goto out;
    }

    switch (request->rq_proc) {
    case CB_NULL:
        dprintf(1, "CB_NULL\n");
        break;

    case CB_COMPOUND:
        dprintf(1, "CB_COMPOUND\n");
        handle_cb_compound(rpc, request, reply);
        break;

    default:
        dprintf(1, "invalid rpc procedure %u\n", request->rq_proc);
        status = 3;
        goto out;
    }
out:
    return status;
}