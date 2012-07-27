/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

#include "common.h"
#include <fcntl.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>

#include <ccnet.h>
#include "utils.h"

#include "seafile-session.h"
#include "fs-mgr.h"
#include "processors/objecttx-common.h"
#include "recvfs-proc.h"
#include "seaf-utils.h"

#define CHECK_INTERVAL 100      /* 100ms */
#define MAX_NUM_BATCH  64

enum {
    RECV_ROOT,
    FETCH_OBJECT
};

typedef struct  {
    int inspect_objects;
    int pending_objects;
    char buf[4096];
    char *bufptr;
    int  n_batch;
    GHashTable  *fs_objects;

    gboolean registered;
    guint32  reader_id;
    guint32  writer_id;
    guint32  stat_id;
} SeafileRecvfsProcPriv;

#define GET_PRIV(o)  \
   (G_TYPE_INSTANCE_GET_PRIVATE ((o), SEAFILE_TYPE_RECVFS_PROC, SeafileRecvfsProcPriv))

#define USE_PRIV \
    SeafileRecvfsProcPriv *priv = GET_PRIV(processor);


G_DEFINE_TYPE (SeafileRecvfsProc, seafile_recvfs_proc, CCNET_TYPE_PROCESSOR)

static int start (CcnetProcessor *processor, int argc, char **argv);
static void handle_update (CcnetProcessor *processor,
                           char *code, char *code_msg,
                           char *content, int clen);

static void
release_resource(CcnetProcessor *processor)
{
    USE_PRIV;
    g_hash_table_destroy (priv->fs_objects);
    if (priv->registered) {
        seaf_obj_store_unregister_async_read (seaf->fs_mgr->obj_store,
                                              priv->reader_id);
        seaf_obj_store_unregister_async_write (seaf->fs_mgr->obj_store,
                                               priv->writer_id);
        seaf_obj_store_unregister_async_stat (seaf->fs_mgr->obj_store,
                                              priv->stat_id);
    }

    CCNET_PROCESSOR_CLASS (seafile_recvfs_proc_parent_class)->release_resource (processor);
}

static void
seafile_recvfs_proc_class_init (SeafileRecvfsProcClass *klass)
{
    CcnetProcessorClass *proc_class = CCNET_PROCESSOR_CLASS (klass);

    proc_class->name = "recvfs-proc";
    proc_class->start = start;
    proc_class->handle_update = handle_update;
    proc_class->release_resource = release_resource;

    g_type_class_add_private (klass, sizeof (SeafileRecvfsProcPriv));
}

static void
seafile_recvfs_proc_init (SeafileRecvfsProc *processor)
{
}

inline static void
request_object_batch_begin (SeafileRecvfsProcPriv *priv)
{
    priv->bufptr = priv->buf;
    priv->n_batch = 0;
}

inline static void
request_object_batch_flush (CcnetProcessor *processor,
                            SeafileRecvfsProcPriv *priv)
{
    if (priv->bufptr == priv->buf)
        return;
    *priv->bufptr = '\0';       /* add ending '\0' */
    priv->bufptr++;
    ccnet_processor_send_response (processor, SC_GET_OBJECT, SS_GET_OBJECT,
                                   priv->buf, priv->bufptr - priv->buf);

    /* Clean state */
    priv->n_batch = 0;
    priv->bufptr = priv->buf;
}

inline static void
request_object_batch (CcnetProcessor *processor,
                      SeafileRecvfsProcPriv *priv, const char *id)
{
    g_assert(priv->bufptr - priv->buf <= (4096-41));

    if (g_hash_table_lookup(priv->fs_objects, id))
        return;

    memcpy (priv->bufptr, id, 40);
    priv->bufptr += 40;
    *priv->bufptr = '\n';
    priv->bufptr++;

    g_hash_table_insert (priv->fs_objects, g_strdup(id), (gpointer)1);
    /* Flush when too many objects batched. */
    if (++priv->n_batch == MAX_NUM_BATCH)
        request_object_batch_flush (processor, priv);
    ++priv->pending_objects;
}

static int
check_seafdir (CcnetProcessor *processor, SeafDir *dir)
{
    USE_PRIV;
    GList *ptr;
    SeafDirent *dent;

    for (ptr = dir->entries; ptr != NULL; ptr = ptr->next) {
        dent = ptr->data;

        if (strcmp (dent->id, EMPTY_SHA1) == 0)
            continue;

#ifdef DEBUG
        g_debug ("[recvfs] Inspect object %s.\n", dent->id);
#endif

        if (S_ISDIR(dent->mode)) {
            if (seaf_obj_store_async_read (seaf->fs_mgr->obj_store,
                                           priv->reader_id,
                                           dent->id) < 0) {
                g_warning ("[recvfs] Failed to start async read of %s.\n",
                           dent->id);
                goto bad;
            }
        } else {
            /* For file, we just need to check existence. */
            if (seaf_obj_store_async_stat (seaf->fs_mgr->obj_store,
                                           priv->stat_id,
                                           dent->id) < 0) {
                g_warning ("[recvfs] Failed to start async stat of %s.\n",
                           dent->id);
                goto bad;
            }
        }
        ++(priv->inspect_objects);
    }

    return 0;

bad:
    ccnet_processor_send_response (processor, SC_BAD_OBJECT, SS_BAD_OBJECT,
                                   NULL, 0);
    ccnet_processor_done (processor, FALSE);
    return -1;
}

static void
on_seafdir_read (OSAsyncResult *res, void *cb_data)
{
    CcnetProcessor *processor = cb_data;
    SeafDir *dir;
    USE_PRIV;

    --(priv->inspect_objects);

    if (!res->success) {
        request_object_batch (processor, priv, res->obj_id);
        return;
    }

#ifdef DEBUG
    g_debug ("[recvfs] Read seafdir %s.\n", res->obj_id);
#endif

    dir = seaf_dir_from_data (res->obj_id, res->data, res->len);
    if (!dir) {
        g_warning ("[recvfs] Corrupt dir object %s.\n", res->obj_id);
        request_object_batch (processor, priv, res->obj_id);
        return;
    }
    check_seafdir (processor, dir);
    seaf_dir_free (dir);
}

static void
on_seafile_stat (OSAsyncResult *res, void *cb_data)
{
    CcnetProcessor *processor = cb_data;
    USE_PRIV;

    --(priv->inspect_objects);

#ifdef DEBUG
    g_debug ("[recvfs] Stat seafile %s.\n", res->obj_id);
#endif

    if (!res->success)
        request_object_batch (processor, priv, res->obj_id);
}

static void
on_fs_write (OSAsyncResult *res, void *cb_data)
{
    CcnetProcessor *processor = cb_data;

    if (!res->success) {
        g_warning ("[recvfs] Failed to write %s.\n", res->obj_id);
        ccnet_processor_send_response (processor, SC_BAD_OBJECT, SS_BAD_OBJECT,
                                       NULL, 0);
        ccnet_processor_done (processor, FALSE);
    }

#ifdef DEBUG
    g_debug ("[recvfs] Wrote fs object %s.\n", res->obj_id);
#endif
}

static int
check_end_condition (CcnetProcessor *processor)
{
    USE_PRIV;

    /* Flush periodically. */
    request_object_batch_flush (processor, priv);

    if (priv->pending_objects == 0 && priv->inspect_objects == 0) {
        ccnet_processor_send_response (processor, SC_END, SS_END, NULL, 0);
        ccnet_processor_done (processor, TRUE);
        return FALSE;
    } else
        return TRUE;
}

static void
register_async_io (CcnetProcessor *processor)
{
    USE_PRIV;

    priv->registered = TRUE;
    priv->reader_id = seaf_obj_store_register_async_read (seaf->fs_mgr->obj_store,
                                                          on_seafdir_read,
                                                          processor);
    priv->stat_id = seaf_obj_store_register_async_stat (seaf->fs_mgr->obj_store,
                                                          on_seafile_stat,
                                                          processor);
    priv->writer_id = seaf_obj_store_register_async_write (seaf->fs_mgr->obj_store,
                                                           on_fs_write,
                                                           processor);
}

static int
start (CcnetProcessor *processor, int argc, char **argv)
{
    char *session_token;
    USE_PRIV;

    if (argc != 1) {
        ccnet_processor_send_response (processor, SC_BAD_ARGS, SS_BAD_ARGS, NULL, 0);
        ccnet_processor_done (processor, FALSE);
        return -1;
    }

    session_token = argv[0];
    if (seaf_token_manager_verify_token (seaf->token_mgr,
                                         processor->peer_id,
                                         session_token, NULL) == 0) {
        ccnet_processor_send_response (processor, SC_OK, SS_OK, NULL, 0);
        processor->state = RECV_ROOT;
        priv->fs_objects = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                  g_free, NULL);
        register_async_io (processor);
        return 0;
    } else {
        ccnet_processor_send_response (processor, 
                                       SC_ACCESS_DENIED, SS_ACCESS_DENIED,
                                       NULL, 0);
        ccnet_processor_done (processor, FALSE);
        return -1;
    }
}

static int
save_fs_object (CcnetProcessor *processor, ObjectPack *pack, int len)
{
    USE_PRIV;

    return seaf_obj_store_async_write (seaf->fs_mgr->obj_store,
                                       priv->writer_id,
                                       pack->id,
                                       pack->object,
                                       len - 41);
}

static void
recv_fs_object (CcnetProcessor *processor, char *content, int clen)
{
    USE_PRIV;
    ObjectPack *pack = (ObjectPack *)content;
    uint32_t type;

    if (clen < sizeof(ObjectPack)) {
        g_warning ("invalid object id.\n");
        goto bad;
    }

#ifdef DEBUG
    g_debug ("[recvfs] Recv fs object %s.\n", pack->id);
#endif

    --priv->pending_objects;

    type = seaf_metadata_type_from_data(pack->object, clen);
    if (type == SEAF_METADATA_TYPE_DIR) {
        SeafDir *dir;
        dir = seaf_dir_from_data (pack->id, pack->object, clen - 41);
        if (!dir) {
            g_warning ("Bad directory object %s.\n", pack->id);
            goto bad;
        }
        int ret = check_seafdir (processor, dir);
        seaf_dir_free (dir);
        if (ret < 0)
            goto bad;
    } else if (type == SEAF_METADATA_TYPE_FILE) {
        /* TODO: check seafile format. */
#if 0
        int ret = seafile_check_data_format (pack->object, clen - 41);
        if (ret < 0) {
            goto bad;
        }
#endif
    } else {
        g_warning ("Invalid object type.\n");
        goto bad;
    }

    if (save_fs_object (processor, pack, clen) < 0) {
        goto bad;
    }

    g_hash_table_remove (priv->fs_objects, pack->id);
    return;

bad:
    ccnet_processor_send_response (processor, SC_BAD_OBJECT,
                                   SS_BAD_OBJECT, NULL, 0);
    g_warning ("[recvfs] Bad fs object received.\n");
    ccnet_processor_done (processor, FALSE);
}

static void
process_fsroot_list (CcnetProcessor *processor, char *content, int clen)
{
    USE_PRIV;
    char *object_id;
    int n_objects;
    int i;

    if (clen % 41 != 0) {
        g_warning ("Bad fs root list.\n");
        ccnet_processor_send_response (processor, SC_BAD_OL, SS_BAD_OL, NULL, 0);
        ccnet_processor_done (processor, FALSE);
        return;
    }

    n_objects = clen/41;

    request_object_batch_begin (priv);

    object_id = content;
    for (i = 0; i < n_objects; ++i) {
        object_id[40] = '\0';

        /* Empty dir or file alwasys exists. */
        if (strcmp (object_id, EMPTY_SHA1) == 0) {
            object_id += 41;
            continue;
        }

#ifdef DEBUG
        g_debug ("[recvfs] Inspect object %s.\n", object_id);
#endif

        if (seaf_obj_store_async_read (seaf->fs_mgr->obj_store,
                                       priv->reader_id,
                                       object_id) < 0) {
            ccnet_processor_send_response (processor,
                                           SC_BAD_OBJECT, SS_BAD_OBJECT,
                                           NULL, 0);
            ccnet_processor_done (processor, FALSE);
            return;
        }
        ++(priv->inspect_objects);

        object_id += 41;
    }

    ccnet_processor_send_response (processor, SC_OK, SS_OK, NULL, 0);
}

static void
handle_update (CcnetProcessor *processor,
               char *code, char *code_msg,
               char *content, int clen)
{
   switch (processor->state) {
   case RECV_ROOT:
        if (strncmp(code, SC_ROOT, 3) == 0) {
            process_fsroot_list (processor, content, clen);
        } else if (strncmp(code, SC_ROOT_END, 3) == 0) {
            /* change state to FETCH_OBJECT */
            processor->timer = ccnet_timer_new (
                (TimerCB)check_end_condition, processor, CHECK_INTERVAL);
            processor->state = FETCH_OBJECT;
        } else {
            g_warning ("Bad response: %s %s\n", code, code_msg);
            ccnet_processor_send_response (processor,
                                           SC_BAD_UPDATE_CODE, SS_BAD_UPDATE_CODE,
                                           NULL, 0);
            ccnet_processor_done (processor, FALSE);
        }
        break;
    case FETCH_OBJECT:
        if (strncmp(code, SC_OBJECT, 3) == 0) {
            recv_fs_object (processor, content, clen);
        } else {
            g_warning ("Bad response: %s %s\n", code, code_msg);
            ccnet_processor_send_response (processor,
                                           SC_BAD_UPDATE_CODE, SS_BAD_UPDATE_CODE,
                                           NULL, 0);
            ccnet_processor_done (processor, FALSE);
        }
        break;
    default:
        g_assert (0);
    }
}
