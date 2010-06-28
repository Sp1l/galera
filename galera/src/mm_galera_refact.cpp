//
// Copyright (C) 2010 Codership Oy <info@codership.com>
//


#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <errno.h>
#include <stdbool.h>
#include <unistd.h>
#include <time.h>
#include <sys/time.h>

#include "wsrep_api.h"
#include "gu_log.h"
#include "gcs.h"

extern "C"
{
#include "galera_info.h"
#include "galera_state.h"
#include "galera_options.h"
#include "galera_status.h"
}

#include "gu_lock.hpp"
#include "gu_uri.hpp"

#include "galera_service_thd.hpp"
#include "apply_monitor.hpp"
#include "certification.hpp"
#include "wsdb.hpp"

// #define GALERA_PROFILING
#ifdef GALERA_PROFILING
#define GU_PROFILE
#endif
#include "gu_profile.hpp"
#ifdef GALERA_PROFILING
static gu::prof::Profile galera_prof("galera");
#else
#define galera_prof ""
#endif // GALERA_PROFILE

using namespace std;
using namespace galera;
static Wsdb* wsdb;
static Certification* cert;

#define GALERA_USLEEP_1_SECOND      1000000 //  1 sec
#define GALERA_USLEEP_10_SECONDS   10000000 // 10 sec
#define GALERA_WORKAROUND_197   1 //w/around for #197: lack of cert db on joiner
//#define GALERA_USE_FLOW_CONTROL 1
#define GALERA_USLEEP_FLOW_CONTROL    1000 //  0.1 sec
#define GALERA_USLEEP_FLOW_CONTROL_MAX (GALERA_USLEEP_1_SECOND*10)

typedef enum galera_repl_state {
    GALERA_UNINITIALIZED,
    GALERA_INITIALIZED,
    GALERA_CONNECTED
} galera_repl_state_t;

/* application's handlers */
static void*                    app_ctx            = NULL;
static wsrep_bf_apply_cb_t      bf_apply_cb        = NULL;
static wsrep_view_cb_t          view_handler_cb    = NULL;
static wsrep_synced_cb_t        synced_cb          = NULL;
static wsrep_sst_donate_cb_t    sst_donate_cb      = NULL;

/* gcs parameters */
static gu_to_t            *cert_queue   = NULL;
static gcs_conn_t         *gcs_conn     = NULL;
static ServiceThd         *service_thd  = 0;

typedef enum
{
    AL_QUERY,
    AL_RBR
} ApplyLevel;

static ApplyLevel preferred_apply_level = AL_RBR;

class ApplyCondition
{
public:
    bool operator()(wsrep_seqno_t last_entered, wsrep_seqno_t last_left,
                    const TrxHandle* trx) const
    {
        // 1) local trx is already applied
        // 2) dependencies have left the monitor
        return (trx->is_local() || last_left >= trx->last_depends_seqno());
    }
};

class CertCondition
{
public:
    bool operator()(wsrep_seqno_t last_entered, wsrep_seqno_t last_left,
                    const TrxHandle* trx) const
    {
        return (last_left + 1 == trx->global_seqno());
    }
};

class CommitCondition
{
public:
    bool operator()(wsrep_seqno_t last_entered, wsrep_seqno_t last_left,
                    const TrxHandle* trx) const
    {
        return (last_left + 1 == trx->global_seqno());
    }
};

static Monitor<ApplyCondition> apply_monitor;

static struct galera_status status =
{
    { { 0 } },
    WSREP_SEQNO_UNDEFINED,
    0, 0, 0, 0, 0, 0, 0, 0, .0, .0, .0, .0,
    GALERA_STAGE_INIT
};

/* state trackers */
static galera_repl_state_t conn_state = GALERA_UNINITIALIZED;
static gcs_seqno_t         sst_seqno;         // received in state transfer
static gu_uuid_t           sst_uuid;          // received in state transfer
static gcs_seqno_t         last_recved  = -1; // last received from group
static wsrep_uuid_t        my_uuid = WSREP_UUID_UNDEFINED;
static long                my_idx = 0;
static galera_stage_t      donor_stage;       // stage to return to from DONOR

static const char* data_dir  = NULL;
static const char* sst_donor = NULL;

static gu_mutex_t sst_mtx;
static gu_cond_t  sst_cond;

static struct galera_options galera_opts;


static inline galera::TrxHandle* get_trx(galera::Wsdb* wsdb,
                                         wsrep_trx_handle_t* trx_handle,
                                         bool create = false)
{
    galera::TrxHandle* trx;

    if (trx_handle->opaque != 0)
    {
        trx = reinterpret_cast<TrxHandle*>(trx_handle->opaque);
        assert(wsdb->get_trx(my_uuid, trx_handle->trx_id) == trx);
        assert(trx->trx_id() == trx_handle->trx_id);
    }
    else
    {
        trx = wsdb->get_trx(my_uuid, trx_handle->trx_id, create);
        trx_handle->opaque = trx;
    }
    return trx;
}


// a wrapper to re-try functions which can return -EAGAIN
static inline long while_eagain (
    long (*function) (gu_to_t*, gcs_seqno_t), gu_to_t* to, gcs_seqno_t seqno
) {
    static const struct timespec period = { 0, 10000000 }; // 10 msec
    long rcode;

    while (-EAGAIN == (rcode = function (to, seqno))) {
        nanosleep (&period, NULL);
    }

    return rcode;
}

static inline long while_eagain_or_trx_abort (
    TrxHandle* trx, long (*function) (gu_to_t*, gcs_seqno_t),
    gu_to_t* to, gcs_seqno_t seqno
) {
    long rcode;

    trx->unlock();
    if (-EAGAIN == (rcode = function (to, seqno)))
    {
        trx->lock();
        assert(trx->local_seqno() == seqno);
        if (trx->state() == WSDB_TRX_MUST_ABORT) {
            gu_debug("WSDB_TRX_MUST_ABORT for trx: %lld %lld",
                     trx->global_seqno(), trx->local_seqno());
            return -EINTR;
        }

        gu_debug("INTRERRUPT for trx: %lld %lld in state: %d",
                trx->global_seqno(),
                 trx->local_seqno(),
                 trx->state());

        return -EINTR;
    }
    trx->lock();
    return rcode;
}

// the following are made as macros to allow for correct line number reporting
#define GALERA_GRAB_QUEUE(queue, seqno)                                 \
    {                                                                   \
        long ret = while_eagain (gu_to_grab, queue, seqno);             \
        if (gu_unlikely(ret)) {                                         \
            gu_fatal("Failed to grab %s at %lld: %ld (%s)",             \
                     #queue, seqno, ret, strerror(-ret));               \
            assert(0);                                                  \
            abort();                                                    \
        }                                                               \
    }

#define GALERA_RELEASE_QUEUE(queue, seqno)                              \
    {                                                                   \
        long ret = gu_to_release (queue, seqno);                        \
        if (gu_unlikely(ret)) {                                         \
            gu_fatal("Failed to release %s at %lld: %ld (%s)",          \
                     #queue, seqno, ret, strerror(-ret));               \
            assert(0);                                                  \
            abort();                                                    \
        }                                                               \
    }

#define GALERA_SELF_CANCEL_QUEUE(queue, seqno)                          \
    {                                                                   \
        long ret = while_eagain (gu_to_self_cancel, queue, seqno);      \
        if (gu_unlikely(ret)) {                                         \
            gu_fatal("Failed to self-cancel %s at %lld: %ld (%s)",      \
                     #queue, seqno, ret, strerror(-ret));               \
            assert(0);                                                  \
            abort();                                                    \
        }                                                               \
    }


static void *galera_wsdb_configurator (
    enum wsdb_conf_param_id id, enum wsdb_conf_param_type type
) {
    switch (id) {
    case WSDB_CONF_LOCAL_CACHE_SIZE:
        return &galera_opts.local_cache_size;
    case WSDB_CONF_WS_PERSISTENCY:
        return &galera_opts.persistent_writesets;
    case WSDB_CONF_MARK_COMMIT_EARLY: // not supported any more?
    default:
        return NULL;
    }
}

#define GALERA_UPDATE_LAST_APPLIED(seqno)                               \
    if (status.last_applied < seqno) status.last_applied = seqno;


static gcs_conn_t* galera_init_gcs (wsrep_t*      gh,
                                    const char*   node_name,
                                    const char*   node_incoming,
                                    gcs_seqno_t   last_recved,
                                    const uint8_t uuid[GCS_UUID_LEN])
{
    gcs_conn_t* ret;
    long rcode;

    GU_DBUG_ENTER("galera_init_gcs");

    ret = gcs_create(node_name, node_incoming);
    if (!ret) {
        gu_error ("Failed to create GCS conection handle");
        GU_DBUG_RETURN(NULL);
    }

    rcode = gcs_init (ret, last_recved, status.state_uuid.data);
    if (rcode) {
        gu_error ("Failed to initialize GCS state: %d (%s)",
                  rcode, strerror (-rcode));
        gcs_destroy (ret);
        GU_DBUG_RETURN(NULL);
    }

    GU_DBUG_RETURN(ret);
}

extern "C"
enum wsrep_status mm_galera_init(wsrep_t* gh,
                                        const struct wsrep_init_args* args)
{
    int rcode;
    galera_state_t saved_state;

    GU_DBUG_ENTER("galera_init");

    data_dir = strdup (args->data_dir);
    if (!data_dir) return WSREP_FATAL;

    /* Set up logging */
    gu_conf_set_log_callback((gu_log_cb_t)args->logger_cb);

    /* Set up options if any */
    if (args->options) galera_options_from_string (&galera_opts, args->options);

    wsdb_set_conf_param_cb(galera_wsdb_configurator);

    /* Set up initial state: */

    /* 1. use passed state arguments */
    status.state_uuid   = *(gu_uuid_t*)args->state_uuid;
    status.last_applied = args->state_seqno;

    /* 2. initialize wsdb */
    // wsdb_init(data_dir, (gu_log_cb_t)args->logger_cb);
    wsdb = new Wsdb;
    cert = new Certification;

    /* 3. try to read saved state from file */
    if (status.last_applied == WSREP_SEQNO_UNDEFINED &&
        !memcmp (&WSREP_UUID_UNDEFINED, &status.state_uuid, sizeof(gu_uuid_t))){

        rcode = galera_restore_state(data_dir, &saved_state);

        if (rcode) {
            gu_warn("GALERA state restore failed");
        } else {
            status.state_uuid   = saved_state.uuid;
            status.last_applied = saved_state.last_applied_seqno;
        }

        gu_info("Found stored state: " GU_UUID_FORMAT ":%lli",
                GU_UUID_ARGS(&status.state_uuid), status.last_applied);
    }

    gu_info("Configured state:   " GU_UUID_FORMAT ":%lli",
            GU_UUID_ARGS(&status.state_uuid), status.last_applied);

    /* 4. create and initialize GCS handle */
    gcs_conn = galera_init_gcs (gh, args->node_name, args->node_incoming,
                                status.last_applied, status.state_uuid.data);
    if (!gcs_conn) {
        gu_error ("Failed to initialize GCS state");
        GU_DBUG_RETURN(WSREP_NODE_FAIL);
    }

    service_thd = new ServiceThd (gcs_conn);

    last_recved = status.last_applied;

    my_idx = 0;

    /* set the rest of callbacks */
    app_ctx           = args->app_ctx;
    bf_apply_cb       = args->bf_apply_cb;
    view_handler_cb   = args->view_handler_cb;
    synced_cb         = args->synced_cb;
    sst_donate_cb     = args->sst_donate_cb;

    gu_mutex_init(&sst_mtx,    NULL);
    gu_cond_init (&sst_cond,   NULL);

    /* initialize total order queue */
    cert_queue = gu_to_create(16384, GCS_SEQNO_FIRST);

    conn_state = GALERA_INITIALIZED;

    GU_DBUG_RETURN(WSREP_OK);
}

extern "C"
enum wsrep_status mm_galera_options_set (wsrep_t* gh, const char* opts_str)
{
    return galera_options_from_string (&galera_opts, opts_str);
}

extern "C"
char* mm_galera_options_get (wsrep_t* gh)
{
    return galera_options_to_string (&galera_opts);
}

extern "C"
enum wsrep_status mm_galera_connect (wsrep_t *gh,
                                     const char* cluster_name,
                                     const char* cluster_url,
                                     const char* state_donor)
{
    int rcode;

    GU_DBUG_ENTER("galera_connect");
    // save donor name for using in configuration handler
    if (sst_donor) free ((char*)sst_donor);
    sst_donor = strdup (state_donor);
    if (!sst_donor) GU_DBUG_RETURN(WSREP_FATAL);

    service_thd->reset();

    rcode = gcs_open(gcs_conn, cluster_name, cluster_url);
    if (rcode) {
	gu_error("gcs_open(%p, %s, %s) failed: %d (%s)",
                 &gcs_conn, cluster_name, cluster_url,
                 rcode, strerror(-rcode));
	GU_DBUG_RETURN(WSREP_NODE_FAIL);
    }

    gu_info("Successfully opened GCS connection to %s", cluster_name);

    status.stage = GALERA_STAGE_JOINING;
    conn_state   = GALERA_CONNECTED;

    GU_DBUG_RETURN(WSREP_OK);
}

extern "C"
enum wsrep_status mm_galera_disconnect(wsrep_t *gh)
{
    int rcode;

    GU_DBUG_ENTER("galera_disconnect");

    if (!gcs_conn) {
        /* Commented out, this changes conn state from UNINITIALIZED to
         * initialized when disconnect is called twice... this is probably
         * not what we want */
        /* conn_state   = GALERA_INITIALIZED; */
        GU_DBUG_RETURN (WSREP_NODE_FAIL); //shouldn't we just ignore it?
    }

    // synchronize with self-leave
    if (GALERA_CONNECTED == conn_state) {

        conn_state   = GALERA_INITIALIZED;
        status.stage = GALERA_STAGE_INIT;

        rcode = gcs_close(gcs_conn);
        if (rcode) {
            gu_error ("Failed to close GCS connection handle: %d (%s)",
                      rcode, strerror(-rcode));
            GU_DBUG_RETURN(WSREP_NODE_FAIL);
        }

        gu_info("Closed GCS connection");
    }

    GU_DBUG_RETURN(WSREP_OK);
}

extern "C"
void mm_galera_tear_down(wsrep_t *gh)
{
    int rcode;
    galera_state_t saved_state;

    switch (conn_state) {

    case GALERA_INITIALIZED:
        /* store persistent state*/
        memcpy(saved_state.uuid.data, status.state_uuid.data, GU_UUID_LEN);
        saved_state.last_applied_seqno = status.last_applied;

        rcode = galera_store_state(data_dir, &saved_state);
        if (rcode) {
            gu_error("GALERA state store failed: %d", rcode);
            gu_info(" State: "GU_UUID_FORMAT":%lli",
                    GU_UUID_ARGS(&status.state_uuid), status.last_applied);
        }

        if (data_dir)  free ((char*)data_dir);
        data_dir = NULL;
        if (sst_donor) free ((char*)sst_donor);
        sst_donor = NULL;
        delete service_thd; service_thd = 0;
        if (gcs_conn)  gcs_destroy (gcs_conn);
        gcs_conn = NULL;
        delete wsdb; wsdb = 0;
        delete cert; cert = 0;
        // wsdb_close();

        if (cert_queue)   gu_to_destroy(&cert_queue);
        cert_queue = NULL;
        conn_state = GALERA_UNINITIALIZED;
    case GALERA_UNINITIALIZED:
        break;

    default:
        gu_fatal ("Unrecognized Galera state on teardown: %d", conn_state);
        assert (0);
        abort();
    }

    log_info << galera_prof;

    return;
}


static wsrep_status_t apply_write_set(void *recv_ctx,
                                      const WriteSet& ws,
                                      wsrep_seqno_t global_seqno)
{
    wsrep_status_t rcode = WSREP_OK;
    assert (global_seqno > 0);

    GU_DBUG_ENTER("apply_write_set");
    if (bf_apply_cb == NULL) {
        gu_error("data applier has not been defined");
        GU_DBUG_RETURN(WSREP_FATAL);
    }

    switch (ws.get_level())
    {
    case WSDB_WS_DATA_RBR:
    {
        if (preferred_apply_level    == AL_RBR ||
            ws.get_queries().empty() == true)
        {
            wsrep_apply_data_t data;
            data.type = WSREP_APPLY_APP;
            data.u.app.buffer = const_cast<uint8_t*>(&ws.get_data()[0]);
            data.u.app.len = ws.get_data().size();
            rcode = bf_apply_cb(recv_ctx, &data, global_seqno);
            break;
        }
        // else fall through
    }
    case WSDB_WS_QUERY:
    {
        const QuerySequence& qs(ws.get_queries());
        for (QuerySequence::const_iterator i = qs.begin(); i != qs.end(); ++i)
        {
            wsrep_apply_data_t data;
            data.type           = WSREP_APPLY_SQL;
            data.u.sql.stm      = reinterpret_cast<const char*>(&i->get_query()[0]);
            data.u.sql.len      = i->get_query().size();
            data.u.sql.timeval  = i->get_tstamp();
            data.u.sql.randseed = i->get_rnd_seed();
            switch (bf_apply_cb(recv_ctx, &data, global_seqno))
            {
            case WSREP_OK: break;
            case WSREP_NOT_IMPLEMENTED:
            {
                log_warn << "bf applier returned not implemented for " << *i;
                break;
            }
            case WSREP_FATAL:
            default:
                gu_error("apply failed for: %d, in level: %d",
                         rcode, ws.get_level());
                GU_DBUG_RETURN(WSREP_FATAL);
            }
        }
        break;
    }
    default:
        gu_error("data replication level %d is not supported yet",
                 ws.get_level());
        GU_DBUG_RETURN(WSREP_FATAL);
    }

    switch (rcode) {
    case WSREP_OK: break;
    case WSREP_NOT_IMPLEMENTED: break;
    case WSREP_FATAL:
    default:
        gu_error("apply failed for: %d, in level: %d", rcode, ws.get_level());
        GU_DBUG_RETURN(WSREP_FATAL);
    }
    GU_DBUG_RETURN(WSREP_OK);
}


static wsrep_status_t apply_query(void *recv_ctx, const char *query, int len,
                                  wsrep_seqno_t seqno_g
) {
    int rcode;
    wsrep_apply_data_t data;

    GU_DBUG_ENTER("apply_commit");

    if (bf_apply_cb == NULL) {
        gu_error("data applier has not been defined");
        GU_DBUG_RETURN(WSREP_FATAL);
    }

    data.type           = WSREP_APPLY_SQL;
    data.u.sql.stm      = query;
    data.u.sql.len      = strlen (data.u.sql.stm) + 1 /* terminating 0 */;
    data.u.sql.timeval  = static_cast<time_t>(0);
    data.u.sql.randseed = 0;

    assert (seqno_g > 0);
    rcode = bf_apply_cb(recv_ctx, &data, seqno_g);
    if (rcode != WSREP_OK) {
        gu_error("query commit failed: %d query '%s'", rcode, query);
        GU_DBUG_RETURN(WSREP_TRX_FAIL);
    }

    GU_DBUG_RETURN(WSREP_OK);
}

static ulong const report_interval = 64;
static ulong       report_counter  = 0;

// fast function to be run inside commit_queue critical section
static inline bool report_check_counter ()
{
    return (++report_counter > report_interval && !(report_counter = 0));
}

// this should be run after commit_queue is released
static inline void report_last_committed (gcs_conn_t* gcs_conn)
{
    const gcs_seqno_t safe_seqno(cert->get_safe_to_discard_seqno());

    gu_debug ("Reporting: safe to discard: %lld, last applied: %lld, "
              "last received: %lld",
              safe_seqno, status.last_applied, last_recved);

    service_thd->report_last_committed (safe_seqno);

#if 0 /* DELETE */
    long ret;
    if ((ret = gcs_set_last_applied(gcs_conn, safe_seqno))) {
        gu_warn ("Failed to report last committed %llu, %d (%s)",
                 safe_seqno, ret, strerror (-ret));
        // failure, set counter to trigger new attempt
        report_counter += report_interval;
    }
#endif
}

static inline void truncate_trx_history (gcs_seqno_t seqno)
{
    static long const  truncate_interval = 64;
    static long const  truncate_offset   = 0;
    static gcs_seqno_t last_truncated    = 0;

    if (last_truncated + truncate_interval + truncate_offset < seqno)
    {
        seqno -= truncate_offset; // avoid purging too much
        gu_debug ("Purging history up to %llu", seqno);
        cert->purge_trxs_upto(seqno);
        last_truncated = seqno;
        gu_debug ("Purging done to %llu", seqno);
    }
}

// returns true if action is to be applied and false if to be skipped
// should always be called while holding cert_queue
static inline bool
galera_update_last_received (gcs_seqno_t seqno)
{
    // Seems like we cannot enforce sanity check here - some replicated
    // writesets get cancelled and never make it to this point (TO monitor).
    // Hence holes in global seqno are inevitable here.
    if (gu_likely (last_recved < seqno)) {
        last_recved = seqno;
        return true;
    }
    else {
        return false;
    }
}

static wsrep_status_t process_conn_write_set(
    void *recv_ctx, TrxHandle* trx,
    gcs_seqno_t seqno_l
) {
    wsrep_status_t rcode = WSREP_OK;

    assert(trx->global_seqno() > 0);
    assert(trx->is_local() == false);

    /* wait for total order */
    GALERA_GRAB_QUEUE (cert_queue, seqno_l);

    if (trx->global_seqno() <= cert->position())
    {
        log_info << "discarding trx below cert position " << *trx;
        GALERA_RELEASE_QUEUE(cert_queue, seqno_l);
        apply_monitor.self_cancel(trx);
        return WSREP_TRX_FAIL;
    }

    int cert_ret(cert->append_trx(trx));
    bool do_report(report_check_counter());

    // @todo the following should be changed to apply_monitor.enter()
    // once table/db level certification is supported
    //
    // drain apply monitor, self cancel apply monitor in to_execute_end()
    apply_monitor.drain(trx->global_seqno() - 1);

    if (cert_ret == WSDB_OK)
    {
        if (gu_likely(galera_update_last_received(trx->global_seqno())))
        {
            /* Global seqno ok, certification ok (not needed?) */
            const MappedBuffer& wscoll(trx->write_set_collection());
            size_t offset(serial_size(*trx));
            WriteSet ws;
            while (offset < wscoll.size())
            {
                offset = unserialize(&wscoll[0], wscoll.size(), offset, ws);
                rcode = apply_write_set(recv_ctx, ws,
                                        trx->global_seqno());
                if (rcode)
                {
                    gu_error ("unknown galera fail: %d trx: %llu",
                              rcode, seqno_l);
                    break;
                }
            }
            assert(offset == wscoll.size() || rcode != WSREP_OK);
        }
    }

    GALERA_UPDATE_LAST_APPLIED (trx->global_seqno());
    /* release total order */
    GALERA_RELEASE_QUEUE (cert_queue, seqno_l);
    apply_monitor.self_cancel(trx);
    cert->set_trx_committed(trx);
    if (do_report) report_last_committed(gcs_conn);

    return rcode;
}

enum wsrep_status process_query_write_set_applying(
    void *recv_ctx,
    TrxHandle* trx,
    gcs_seqno_t seqno_l)
{
    static const size_t max_apply_attempts(10);
    size_t attempts(0);
    int rcode(WSREP_OK);
    const MappedBuffer& wscoll(trx->write_set_collection());

    do
    {
        size_t offset(serial_size(*trx));
        WriteSet ws;
        while (offset < wscoll.size())
        {
            offset = unserialize(&wscoll[0], wscoll.size(), offset, ws);
            if ((rcode = apply_write_set(recv_ctx,
                                         ws,
                                         trx->global_seqno())))
            {
                gu_warn("ws apply failed, rcode: %d, seqno: %lld, "
                        "last_seen: %lld attempt: %zd",
                        rcode, trx->global_seqno(),
                        trx->last_seen_seqno(), attempts);

                if (apply_query(recv_ctx, "rollback\0", 9,
                                trx->global_seqno())) {
                    gu_warn("ws apply rollback failed, seqno: %lld, "
                            "last_seen: %lld",
                            trx->global_seqno(),
                            trx->last_seen_seqno());
                    rcode = WSREP_FATAL;
                }
                ++attempts;

                /* avoid retrying if fatal error happened */
                if (rcode == WSREP_FATAL)
                {
                    attempts = max_apply_attempts;
                    break;
                }
            }
        }
        assert(offset == wscoll.size() || rcode != WSREP_OK);
    }
    while (WSREP_OK != rcode && attempts < max_apply_attempts);

    if (attempts == max_apply_attempts) {
        log_warn << "write set applying not possible for trx " << *trx;
        // We don't release commit queue here to disallow following
        // transactions to commit
        assert(0);
        return WSREP_TRX_FAIL;
    }

    if (rcode == WSREP_OK &&
        (rcode = apply_query(recv_ctx, "commit\0", 7,
                             trx->global_seqno())) != WSREP_OK)
    {
        gu_fatal("ws apply commit failed, seqno: %lld %lld, "
                 "last_seen: %lld",
                 trx->global_seqno(),
                 seqno_l,
                 trx->last_seen_seqno());
        gu_throw_fatal << "ws apply commit failed";
    }

    gu_debug("GALERA ws commit for: %lld %lld", trx->global_seqno(),
             seqno_l);

    return WSREP_OK;
}

//
//  similar to post gcs_repl part of `galera_commit' to apply remote WS
//
static wsrep_status_t process_query_write_set(
    void *recv_ctx, TrxHandle* trx,
    gcs_seqno_t seqno_l
    ) {

    enum wsrep_status ret_code;
    gcs_seqno_t seqno_g = trx->global_seqno();

    assert (seqno_g > 0);
    assert(trx->is_local() == false);

    // wait for total order
    GALERA_GRAB_QUEUE (cert_queue, seqno_l);

    if (trx->global_seqno() <= cert->position())
    {
        log_info << "discarding trx below cert position " << *trx;
        GALERA_RELEASE_QUEUE(cert_queue, seqno_l);
        apply_monitor.self_cancel(trx);
        return WSREP_TRX_FAIL;
    }

    // append into cert index and certify
    int cert_ret(cert->append_trx(trx));
    bool do_report(report_check_counter());
    // release total order
    GALERA_RELEASE_QUEUE (cert_queue, seqno_l);

    switch (cert_ret)
    {
    case WSDB_OK:
        // certification ok
        apply_monitor.enter(trx);
        ret_code = process_query_write_set_applying(recv_ctx, trx, seqno_l);
        apply_monitor.leave(trx);
        // stop for any dbms error
        if (ret_code != WSREP_OK)
        {
            gu_fatal("could not apply trx: %lld %lld", seqno_g, seqno_l);
            return WSREP_FATAL;
        }
        break;

    case WSDB_CERTIFICATION_FAIL:
        // certification failed, release
        gu_debug("trx certification failed: (%lld %lld) last_seen: %lld",
                 seqno_g, seqno_l, trx->last_seen_seqno());
        // cancel apply monitor and commit queue
        apply_monitor.self_cancel(trx);
        ret_code = WSREP_TRX_FAIL;
        break;

    default:
        gu_error("unknown galera fail: %d trdx: %lld %lld",
                 cert_ret, seqno_g, seqno_l);
        ret_code = WSREP_FATAL;
        break;
    }

    GALERA_UPDATE_LAST_APPLIED (trx->global_seqno());
    cert->set_trx_committed(trx);
    if (do_report) report_last_committed(gcs_conn);

    return ret_code;
}

static wsrep_status_t process_write_set(
    void *recv_ctx, uint8_t *data, size_t data_len,
    gcs_seqno_t seqno_g, gcs_seqno_t seqno_l
    ) {

    wsrep_status_t rcode = WSREP_OK;

    TrxHandle* trx(cert->create_trx(data, data_len, seqno_l, seqno_g));
    TrxHandleLock lock(*trx);

    switch (trx->write_set_type())
    {
    case WSDB_WS_TYPE_TRX:
        rcode = process_query_write_set(recv_ctx, trx, seqno_l);
        break;
    case WSDB_WS_TYPE_CONN:
        rcode = process_conn_write_set(recv_ctx, trx, seqno_l);
        break;
    }

    return rcode;
}

/* To be called only in isolation in galera_handle_configuration() */
static void
galera_join()
{
    long ret;

    do {
        ret = gcs_join (gcs_conn, status.last_applied);
    }
    while ((-EAGAIN == ret) &&
           (usleep(GALERA_USLEEP_1_SECOND), true));

    if (ret) {
        galera_state_t st = { status.last_applied, status.state_uuid };

        galera_store_state (data_dir, &st);

        gu_fatal ("Could not send join message: "
                  "%d (%s). Aborting.", ret, strerror(-ret));
        abort(); // TODO: see #170, gcs_join() must be reworked
    }
    else {
        status.stage = GALERA_STAGE_JOINED;
    }
}

/*!
 * This is needed because ST is (and needs to be) _asynchronous_ with
 * replication. As a result, only application thread knows when and how ST
 * completed and if we need another one. See #163
 */
static bool
galera_st_required (const gcs_act_conf_t* conf)
{
    bool st_required           = (conf->my_state == GCS_NODE_STATE_PRIM);
    const gu_uuid_t* conf_uuid =
        reinterpret_cast<const gu_uuid_t*>(conf->group_uuid);

    if (st_required) {

        assert (conf->conf_id >= 0);

        if (!gu_uuid_compare (&status.state_uuid, conf_uuid)) {
            // same history
            if (GALERA_STAGE_JOINED <= status.stage) {
                // if we took ST already, it may exceed conf->seqno
                // (ST is asynchronous!)
                st_required = (status.last_applied < conf->seqno);
            }
            else {
                st_required = (status.last_applied != conf->seqno);
            }
        }
    }
    else {
        /* some sanity checks */
    }

    return st_required;
}

/*!
 * @return
 *        donor index (own index in case when no state transfer needed)
 *        or negative error code (-1 if configuration in non-primary)
 */
static long
galera_handle_configuration (wsrep_t* gh,
                             void* recv_ctx,
                             const gcs_act_conf_t* conf,
                             gcs_seqno_t conf_seqno)
{
    long    ret = 0;
    bool    st_required;
    void*   app_req = NULL;
    ssize_t app_req_len;

    const gu_uuid_t* conf_uuid =
        reinterpret_cast<const gu_uuid_t*>(conf->group_uuid);

    GU_DBUG_ENTER("galera_handle_configuration");

    gu_debug ("New %s configuration: %lld, "
              "seqno: %lld, group UUID: "GU_UUID_FORMAT
              ", members: %zu, my idx: %zd",
              conf->conf_id > 0 ? "PRIMARY" : "NON-PRIMARY",
              static_cast<long long>(conf->conf_id),
	      static_cast<long long>(conf->seqno),
              GU_UUID_ARGS(conf_uuid), conf->memb_num, conf->my_idx);

    my_idx = conf->my_idx; // this is always true.

    /* sanity check */
    switch (status.stage) {
    case GALERA_STAGE_INIT:
    case GALERA_STAGE_JOINING:
    case GALERA_STAGE_JOINED:
    case GALERA_STAGE_SYNCED:
    case GALERA_STAGE_DONOR:
        break;
    default:
        gu_fatal ("Internal replication error: unexpected stage %u",
                  status.stage);
        assert(0);
        abort();
    }

    st_required = galera_st_required (conf);

#ifdef GALERA_WORKAROUND_197
    if (conf->seqno >= 0)
    {
        cert->assign_initial_position(conf->seqno);
        apply_monitor.assign_initial_position(conf->seqno);
        galera_update_last_received(conf->seqno);
    }
#endif

    wsrep_view_info_t* view_info(galera_view_info_create(conf, st_required));
    if (view_info->my_idx >= 0)
    {
        my_uuid = view_info->members[view_info->my_idx].id;
        assert(my_uuid != WSREP_UUID_UNDEFINED);
    }
    // @todo view handler consumes view_info, this should be changed so
    // that handler has responsibility of taking copy if it wants to
    // store info permanently
    view_handler_cb (NULL, recv_ctx, view_info, NULL, 0,
                     &app_req, &app_req_len);

    if (conf->conf_id >= 0) {                // PRIMARY configuration

        if (st_required)
        {
            int const retry_sec = 1;

            // GCS determined that we need to request state transfer.
            gu_info ("State transfer required:"
                     "\n\tGroup state: "GU_UUID_FORMAT":%lld"
                     "\n\tLocal state: "GU_UUID_FORMAT":%lld",
                     GU_UUID_ARGS(conf_uuid), conf->seqno,
                     GU_UUID_ARGS(&status.state_uuid), status.last_applied);

            /* Starting state snapshot transfer */
            gu_mutex_lock (&sst_mtx);

            status.stage = GALERA_STAGE_SST_PREPARE;
            // app_req_len = sst_prepare_cb (&app_req);

            if (app_req_len < 0) {

                status.stage = GALERA_STAGE_JOINING;
                ret = app_req_len;
                gu_error ("Application failed to prepare for receiving "
                          "state transfer: %d (%s)", ret, strerror(-ret));
            }
            else do {
                // Actually this loop can be done even in this thread:
                // until we succeed in sending state transfer request, there's
                // nothing else for us to do.
                gcs_seqno_t seqno_l;

                assert (app_req);

                if (galera_invalidate_state (data_dir)) abort();

                status.stage = GALERA_STAGE_RST_SENT;
                ret = gcs_request_state_transfer (gcs_conn,
                                                  app_req, app_req_len,
                                                  sst_donor, &seqno_l);
                if (ret < 0) {
                    if (ret != -EAGAIN) {
                        galera_state_t st =
                            { status.last_applied, status.state_uuid };

                        status.stage = GALERA_STAGE_RST_FAILED;
                        gu_error ("Requesting state snapshot transfer failed: "
                                  "%d (%s)", ret, strerror(-ret));
                        // try not to lose state information if RST fails
                        galera_store_state (data_dir, &st);
                    }
                    else {
                        gu_info ("Requesting state snapshot transfer failed: "
                                 "%d (%s). Retrying in %d seconds",
                                 ret, strerror(-ret), retry_sec);
                    }
                }

                if (seqno_l > GCS_SEQNO_NIL) {

                    GALERA_SELF_CANCEL_QUEUE (cert_queue,   seqno_l);
                }

            } while ((ret == -EAGAIN) &&
                     (usleep(retry_sec * GALERA_USLEEP_1_SECOND), true));

            if (app_req) free (app_req);

            if (ret >= 0) {

                status.stage = GALERA_STAGE_SST_WAIT;
                gu_info ("Requesting state transfer: success, donor %ld", ret);
                /* Commented out, assertion is not correct in case
                 * of rapid successive conf changes */
                /* assert (ret != my_idx); */

                /* Here wait for application to call galera_state_received(),
                 * which will set my_uuid and my_seqno */
                gu_cond_wait (&sst_cond, &sst_mtx);

                if (gu_uuid_compare (&sst_uuid, conf_uuid) ||
                    sst_seqno < conf->seqno)
                {
                    status.stage = GALERA_STAGE_SST_FAILED;
                    gu_fatal ("Application received wrong state:"
                              "\n\tReceived: "GU_UUID_FORMAT" :    %lld"
                              "\n\tRequired: "GU_UUID_FORMAT" : >= %lld",
                              GU_UUID_ARGS(&sst_uuid), sst_seqno,
                              GU_UUID_ARGS(conf_uuid), conf->seqno);
                    assert(0);
                    abort(); // just abort for now. Ideally reconnect to group.
                }
                else {
                    status.state_uuid   = sst_uuid;
                    status.last_applied = sst_seqno;
                    last_recved = status.last_applied;

                    gu_info ("Application state transfer complete: "
                             GU_UUID_FORMAT":%lld",
                             GU_UUID_ARGS(&status.state_uuid),
                             status.last_applied);

                    galera_join();
                }
            }
            else {
                // Until we develop a clean way out of this situation: return
                // from this function means unlocking queues and letting
                // slave threads to apply actions over unsynchronized state.
                gu_fatal ("Aborting to prevent state corruption.");
                assert(0);
                abort();
            }

            gu_mutex_unlock (&sst_mtx);

            service_thd->reset();
        }
        else {                              /* no state transfer required */
            /* sanity check */
            if (GALERA_STAGE_JOINING < status.stage) {
                if ((GCS_NODE_STATE_JOINED == conf->my_state &&
                     GALERA_STAGE_JOINED   != status.stage) ||
                    (GCS_NODE_STATE_SYNCED == conf->my_state &&
                     GALERA_STAGE_SYNCED   != status.stage)) {
                    gu_fatal ("Internal replication error: unexpected stage %u "
                              "when conf_id == 1 and my_state: %s",status.stage,
                              gcs_node_state_to_str(conf->my_state));
                    assert(0);
                    abort();
                }
            }

            ret = my_idx;

            if (1 == conf->conf_id) {
                /* sanity check */
                if (GALERA_STAGE_JOINING < status.stage) {
                    gu_fatal ("Internal replication error: unexpected stage %u "
                              "when conf_id == 1 and my_state: %s",status.stage,
                              gcs_node_state_to_str(conf->my_state));
                    assert(0);
                    abort();
                }

                status.state_uuid   = *conf_uuid;
                status.last_applied = conf->seqno;

                last_recved = status.last_applied;
            }
            else {
                /* Here we can't test for seqno equality,
                 * there can be gaps because of BFs and stuff */
                if (/*last_recved < conf->seqno || */
                    gu_uuid_compare (&status.state_uuid, conf_uuid)) {

                    gu_fatal ("Internal replication error: no state transfer "
                              "required while this node state is not "
                              "the same as the group."
                              "\n\tGroup: "GU_UUID_FORMAT":%lld"
                              "\n\tNode:  "GU_UUID_FORMAT":%lld",
                              GU_UUID_ARGS(conf_uuid),          conf->seqno,
                              GU_UUID_ARGS(&status.state_uuid), last_recved);
                    assert(0);
                    abort(); // just abort for now. Ideally reconnect to group.
                }

                // workaround for #182, should be safe since we're in isolation
                last_recved = conf->seqno;

            }

            if (GALERA_STAGE_JOINING >= status.stage) {
                switch (conf->my_state) {
                case GCS_NODE_STATE_JOINED:
                    status.stage = GALERA_STAGE_JOINED; break;
                case GCS_NODE_STATE_SYNCED:
                    status.stage = GALERA_STAGE_SYNCED; break;
                default: break;
                }
            }
            else if (GALERA_STAGE_DONOR == status.stage) {
                switch (conf->my_state) {
                case GCS_NODE_STATE_JOINED:
                    donor_stage = GALERA_STAGE_JOINED; break;
                case GCS_NODE_STATE_SYNCED:
                    donor_stage = GALERA_STAGE_SYNCED; break;
                default: break;
                }
            }

            if (galera_invalidate_state (data_dir)) abort();
        }
    }
    else {
        // NON PRIMARY configuraiton
        if (status.last_applied >= 0 &&
            gu_uuid_compare (&status.state_uuid, &GU_UUID_NIL)) {
            // try not to lose state information
            galera_state_t st = { status.last_applied, status.state_uuid };
            galera_store_state (data_dir, &st);
        }
        else {
            assert (status.last_applied < 0 &&
                    !gu_uuid_compare (&status.state_uuid, &GU_UUID_NIL));
        }

        if (status.stage != GALERA_STAGE_DONOR) {
            status.stage = GALERA_STAGE_JOINING;
        }
        else {
            donor_stage = GALERA_STAGE_JOINING;
        }

        ret = -1;
    }

    GU_DBUG_RETURN(ret);
}

extern "C"
enum wsrep_status mm_galera_recv(wsrep_t *gh, void *recv_ctx)
{
    int                rcode;
    bool               shutdown = false;
    enum wsrep_status  ret_code = WSREP_NODE_FAIL;

    if (GALERA_CONNECTED != conn_state) {
        gu_info("recv method cannot start, gcs not connected");

        /* return with success code to avoid node shutdown */
        return WSREP_OK;
    }

    assert(gcs_conn);

    while (!shutdown) {
        gcs_act_type_t  action_type;
        size_t          action_size;
        void*           action;
        gcs_seqno_t     seqno_g, seqno_l;

        rcode = gcs_recv(
            gcs_conn, &action, &action_size, &action_type, &seqno_g, &seqno_l
        );

#ifdef EXTRA_DEBUG
        gu_info ("gcs_recv(): act_type: %u, act_size: %u, act_id: %lld, "
                 "local: %lld, rcode: %d (%s)",
                 action_type, action_size, seqno_g, seqno_l, rcode,
                 strerror(-rcode));
#endif
	if (rcode <= 0) {
            /* must return immediately, seqnos are not usable */
            gu_info ("gcs_recv() returned %d (%s)", rcode, strerror(-rcode));
            return WSREP_CONN_FAIL;
        }

        assert (GCS_SEQNO_ILL != seqno_l);

        gu_debug("worker with seqno: (%lld - %lld) type: %s recvd",
                 seqno_g, seqno_l, gcs_act_type_to_str(action_type));

        if (gu_likely(GCS_ACT_TORDERED == action_type)) {

            assert (GCS_SEQNO_ILL != seqno_g);

            status.received++;
            status.received_bytes += action_size;

            ret_code = process_write_set(
                recv_ctx, (uint8_t*)action, action_size, seqno_g, seqno_l
                );

            /* catch node failure */
            if (ret_code == WSREP_FATAL || ret_code == WSREP_NODE_FAIL) {
              shutdown = true;
            }
        }
        else if (GCS_ACT_COMMIT_CUT == action_type) {
            /* This one can be quite frequent, so we optimize it a little */
            assert (0 <= seqno_l);
            GALERA_GRAB_QUEUE    (cert_queue,   seqno_l);
            truncate_trx_history (*(gcs_seqno_t*)action);
            GALERA_RELEASE_QUEUE (cert_queue,   seqno_l);
        }
        else if (0 <= seqno_l) {
            /* Actions processed below are special and very rare, so they are
             * processed in isolation */
            GALERA_GRAB_QUEUE (cert_queue, seqno_l);
            if (cert->position() != -1)
            {
                apply_monitor.drain(cert->position());
            }

            switch (action_type) {
            case GCS_ACT_CONF:
            {
                galera_handle_configuration (gh, recv_ctx,
                                             (gcs_act_conf_t*)action, seqno_l);
                if (-1 == my_idx /* self-leave */)
                {
                    status.stage = GALERA_STAGE_INIT;
                    shutdown = true;
                    ret_code = WSREP_OK;
                }
                ret_code = WSREP_OK;
                break;
            }
            case GCS_ACT_STATE_REQ:
                gu_info ("Got state transfer request.");

                status.stage = GALERA_STAGE_DONOR;
                /* To snap out of donor state application must call
                 * wsrep->sst_sent() when it is really done */
                sst_donate_cb (NULL,
                               recv_ctx,
                               action,
                               action_size,
                               (wsrep_uuid_t*)&status.state_uuid,
                               /* status.last_applied, see #182 */ last_recved,
                               NULL, 0);
                ret_code = WSREP_OK;
                break;
            case GCS_ACT_JOIN:
                gu_debug ("#303 Galera joined group");
                status.stage = GALERA_STAGE_JOINED;
                ret_code = WSREP_OK;
                break;
            case GCS_ACT_SYNC:
                gu_debug ("#301 Galera synchronized with group");
                status.stage = GALERA_STAGE_SYNCED;
                ret_code = WSREP_OK;
                synced_cb(app_ctx);
                break;
            default:
                gu_error("Unexpected gcs action value: %s, must abort.",
                         gcs_act_type_to_str(action_type));
                assert(0);
                return WSREP_FATAL; /* returning without releasing TO queues
                                     * on purpose. */
            }

            GALERA_RELEASE_QUEUE (cert_queue,   seqno_l);
        }
        else {
            /* Negative seqno_t means error code */
            gu_error ("Got %s with code: %lld (%s)",
                      gcs_act_type_to_str(action_type),
                      seqno_l, strerror (-seqno_l));
        }

        free (action); /* TODO: cache DATA actions at the end of commit queue
                        * processing. Therefore do not free them here. */
    }

    gu_info("mm_galera_recv(): return %d", ret_code);
    /* returning WSREP_NODE_FAIL or WSREP_FATAL */
    return ret_code;
}

extern "C"
enum wsrep_status mm_galera_abort_pre_commit(wsrep_t *gh,
    const wsrep_seqno_t bf_seqno, const wsrep_trx_id_t victim_trx
) {
    enum wsrep_status ret_code = WSREP_OK;
    int rcode;

    /*
     * this call must be allowed even if conn_state != GALERA_CONNECTED,
     * slave applier must be able to kill remaining conflicting trxs
     */

    /* take commit mutex to be sure, committing trx does not
     * conflict with us
     */
    TrxHandle* victim(wsdb->get_trx(my_uuid, victim_trx));

    if (victim == 0)
    {
        // Victim has probably already aborted
        return WSREP_OK;
    }

    TrxHandleLock lock(*victim);

    gu_debug("abort_pre_commit trx state: %d seqno: %lld trx %lld bf_seqno %lld",
            victim->state(), victim->local_seqno(), victim_trx, bf_seqno);

    /* continue to kill the victim */
    switch (victim->state()) {
    case WSDB_TRX_ABORTING_REPL:
    case WSDB_TRX_ABORTING_NONREPL:
    case WSDB_TRX_MUST_REPLAY:
    case WSDB_TRX_MUST_ABORT:
    case WSDB_TRX_MISSING:
        /* MUST_ABORT has been acknowledged or trx does not exist */
        break;

    case WSDB_TRX_VOID:
        victim->set_state(WSDB_TRX_MUST_ABORT);
        ret_code = WSREP_WARNING;
        gu_debug("no seqno for trx, marked trx aborted: %llu", victim_trx);
        break;

    case WSDB_TRX_REPLICATING:
        gu_debug("victim trx is replicating: %lld", victim->local_seqno());
        victim->set_state(WSDB_TRX_MUST_ABORT);
        int rc;
        if (victim->gcs_handle() > 0 &&
            (rc = gcs_interrupt(gcs_conn, victim->gcs_handle())) != 0)
        {
            log_debug << "gcs_interrupt(): handle "
                      << victim->gcs_handle()
                      << " trx id " << victim->trx_id()
                      << ": " << strerror(-rc);
        }
        ret_code = WSREP_OK;
        break;

    default:
        assert(victim->global_seqno() != -1);
        if (victim->global_seqno() < bf_seqno)
        {
            gu_debug("trying to interrupt earlier trx:  %lld - %lld",
                     victim->global_seqno(), bf_seqno);
            ret_code = WSREP_WARNING;
        }
        else
        {
            gu_debug("interrupting trx commit: trx_id %llu, seqno %lld",
                     victim_trx, victim->local_seqno());

            /*
             * MUST_ABORT needs to be set only if TO interrupt does not work
             */
            rcode = gu_to_interrupt(cert_queue, victim->local_seqno());
            switch (rcode) {
            case 0:
                ret_code = WSREP_OK;
                break;
            case -EAGAIN:
                /*
                 * victim does not yet fit in TO queue,
                 * slave queue has grown too long, we flag trx with
                 * must abort and assume he will abort himself
                 */
                gu_warn("victim trx does not fit cert TO queue");

                victim->set_state(WSDB_TRX_MUST_ABORT);
                ret_code = WSREP_OK;

                break;
            case -ERANGE:
                /* victim was canceled or used already */
                gu_debug("trx interupt fail in cert_queue for %lld: %d (%s)",
                         victim->local_seqno(), rcode, strerror(-rcode));
                ret_code = WSREP_OK;
            }
        }
        if (victim->global_seqno() != GCS_SEQNO_ILL)
        {
            apply_monitor.interrupt(victim);
        }
    }

    return ret_code;
}

extern "C"
enum wsrep_status mm_galera_abort_slave_trx(
    wsrep_t *gh, wsrep_seqno_t bf_seqno, wsrep_seqno_t victim_seqno
    )
{
    enum wsrep_status ret_code = WSREP_OK;
    int rcode;

    TrxHandle* victim(cert->get_trx(victim_seqno));
    TrxHandleLock vlock(*victim);
    TrxHandle* bf(cert->get_trx(bf_seqno));
    TrxHandleLock block(*victim);

    log_warn << "Trx " << bf << " tries to abort";
    log_warn << "Trx " << victim;
    log_warn << "This call is bogus and should be removed from API. See #335";
    return WSREP_WARNING;

//    gu_throw_fatal << "abort slave trx " << bf_seqno << " " << victim_seqno;

    /* take commit mutex to be sure, committing trx does not
     * conflict with us
     */

    if (victim_seqno < bf_seqno) {
        gu_debug("trying to interrupt earlier trx:  %lld - %lld",
                 victim_seqno, bf_seqno);
        ret_code = WSREP_WARNING;
    } else {
        gu_debug("interrupting trx commit: seqno_g %lld seqno_l %lld",
                 victim_seqno, victim->local_seqno());

        rcode = gu_to_interrupt(cert_queue, victim->local_seqno());
        if (rcode) {
            gu_debug("BF trx interupt fail in cert_queue: %d", rcode);
        }
    }

    return ret_code;
}




extern "C"
enum wsrep_status mm_galera_post_commit(
    wsrep_t *gh, wsrep_trx_handle_t* trx_handle
) {
    TrxHandle* trx(get_trx(wsdb, trx_handle));

    if (trx == 0)
    {
        return WSREP_OK;
    }

    TrxHandleLock lock(*trx);

    GU_DBUG_ENTER("galera_post_commit");

    GU_DBUG_PRINT("galera", ("trx: %llu", trx->trx_id()));

    assert (trx->state() == WSDB_TRX_REPLICATED);
    assert (trx->global_seqno() != WSREP_SEQNO_UNDEFINED);

    GALERA_UPDATE_LAST_APPLIED (trx->global_seqno());

    bool do_report(report_check_counter());

    status.local_commits++;

    apply_monitor.leave(trx);

    cert->set_trx_committed(trx);

    if (do_report) report_last_committed (gcs_conn);

    // Additional ref so that trx is not freed during discard
    trx->ref();
    wsdb->discard_trx(trx->trx_id());
    // trx should still be owned by cert
    assert(trx->refcnt() == 2);
    trx->unref();
    trx_handle->opaque = 0;

    GU_DBUG_RETURN(WSREP_OK);
}

extern "C"
enum wsrep_status mm_galera_post_rollback(
    wsrep_t *gh, wsrep_trx_handle_t* trx_handle
    ) {

    TrxHandle* trx(get_trx(wsdb, trx_handle));

    if (trx == 0)
    {
        return WSREP_OK;
    }

    trx->lock();

    GU_DBUG_ENTER("galera_post_rollback");

    GU_DBUG_PRINT("galera", ("trx: %llu", trx->trx_id()));

    gu_debug("trx state: %d at post_rollback for: %lld",
             trx->state(),
             trx->local_seqno());

    switch (trx->state())
    {
    case WSDB_TRX_MISSING:
        trx->unlock();
        break;
    case WSDB_TRX_MUST_REPLAY:
        trx->unlock();
        /* we need trx info for replaying */
        break;
    case WSDB_TRX_MUST_ABORT:
    case WSDB_TRX_REPLICATED:
    case WSDB_TRX_ABORTING_REPL:
        /* these have replicated */
        assert (trx->local_seqno() > 0);
        cert->set_trx_committed(trx);
        // trx->clear();
        // Note: commit queue should have been released already in
        // pre_commit()
    case WSDB_TRX_VOID:
    case WSDB_TRX_ABORTING_NONREPL:
        /* local trx was removed in pre_commit phase already */
        assert(trx->refcnt() >= 1);
        trx->unlock();
        wsdb->discard_trx(trx->trx_id());
        break;
    default:
        trx->unlock();
        gu_debug("trx state: %d at galera_rolledback for: %lld",
                 trx->state(), trx->local_seqno()
            );
        break;
    }

    trx_handle->opaque = 0;
    GU_DBUG_RETURN(WSREP_OK);
}

static int check_certification_status_for_aborted(TrxHandle* trx)
{
    int rcode;
    wsrep_seqno_t seqno_l(trx->local_seqno());
    /*
     * not sure if certification needs to wait for total order or not.
     * local trx has conflicted with some remote trx and we are interested
     * to find out if this is a true conflict or dbms lock granularity issue.
     *
     * It would be safe to wait for all preceding trxs to certificate before
     * us. However, this is not simple to guarantee. There is a limited number
     * of slave threads and each slave will eventually end up waiting for
     * commit_queue. Therefore, all preceding remote trxs might not have passed
     * cert_queue and we would hang here waiting for cert_queue => deadlock.
     *
     * for the time being, I just certificate here with all trxs which were
     * fast enough to certificate. This is wrong, and must be fixed.
     *
     * Maybe 'light weight' cert_queue would work here: We would just check that
     * seqno_l - 1 has certified and then do our certification.
     */

    rcode = cert->test(trx, false);
    switch (rcode) {
    case WSDB_OK:
        gu_debug ("BF conflicting local trx %lld has certified, "
                  "cert. interval: %lld - %lld",
                  seqno_l,
                  trx->last_seen_seqno(),
                  trx->global_seqno());
        /* certification ok */
        return WSREP_OK;

    case WSDB_CERTIFICATION_FAIL:
        /* certification failed, release */
        gu_debug("BF conflicting local trx %lld certification failed: "
                 "%lld - %lld", seqno_l,
                 trx->last_seen_seqno(),
                 trx->global_seqno());
        return WSREP_TRX_FAIL;

    default:
        gu_fatal("wsdb append failed: seqno_g %lld seqno_l %lld",
                 trx->global_seqno(), seqno_l);
        abort();
        break;
    }
}

static wsrep_status_t trx_repl(TrxHandle* trx)
{
    long rcode;
    gcs_seqno_t seqno_l(GCS_SEQNO_ILL), seqno_g(GCS_SEQNO_ILL);
    const MappedBuffer& wscoll(trx->write_set_collection());

    do
    {
        assert (WSREP_SEQNO_UNDEFINED == trx->global_seqno());
        // grab gcs resource before setting last applied to ensure
        // monotonous increase of last seen seqno
        const long gcs_handle(gcs_schedule(gcs_conn));
        if (gcs_handle < 0)
        {
            log_warn << "gcs_schedule(): " << strerror(-gcs_handle);
            return WSREP_CONN_FAIL;
        }

        trx->set_gcs_handle(gcs_handle);
        trx->set_last_seen_seqno(apply_monitor.last_left());
        // rewrite trx header
        wsdb->flush_trx(trx, true);



        trx->unlock();
        rcode = gcs_repl(gcs_conn, &wscoll[0], wscoll.size(),
                         GCS_ACT_TORDERED, true, &seqno_g, &seqno_l);
        trx->lock();
    }
    while (rcode == -EAGAIN && trx->state() != WSDB_TRX_MUST_ABORT &&
           (usleep(GALERA_USLEEP_FLOW_CONTROL), true));


    if (rcode < 0)
    {
        if (rcode != -EINTR)
        {
            log_info << "gcs_repl() failed with '" << strerror(-rcode)
                     << "' for trx " << *trx;
        }
        else
        {
            log_debug << "gcs_repl() interrupted for: " << trx->trx_id()
                      << " state " << trx->state();
        }
        assert(rcode != -EINTR || trx->state() == WSDB_TRX_MUST_ABORT);
        assert (GCS_SEQNO_ILL == seqno_l);
        assert (GCS_SEQNO_ILL == seqno_g);
        trx->clear();
        trx->set_state(WSDB_TRX_ABORTING_NONREPL);
        trx->set_gcs_handle(-1);
        return WSREP_CONN_FAIL;
    }

    assert (GCS_SEQNO_ILL != seqno_g);
    assert (GCS_SEQNO_ILL != seqno_l);

    trx->set_gcs_handle(-1);
    trx->set_seqnos(seqno_l, seqno_g);

    status.replicated++;
    status.replicated_bytes += wscoll.size();

    return WSREP_OK;
}


extern "C"
enum wsrep_status mm_galera_pre_commit(
    wsrep_t *gh, wsrep_conn_id_t conn_id,
    wsrep_trx_handle_t* trx_handle,
    const void *rbr_data, size_t rbr_data_len, wsrep_seqno_t* global_seqno
    )
{
    wsrep_status_t retcode;

    GU_DBUG_ENTER("galera_pre_commit");

    *global_seqno = WSREP_SEQNO_UNDEFINED;

    if (gu_unlikely(conn_state != GALERA_CONNECTED)) return WSREP_CONN_FAIL;

    TrxHandle* trx(get_trx(wsdb, trx_handle));
    if (trx == 0)
    {
        // Transaction does not have active write set
        return WSREP_OK;
    }

    GU_DBUG_PRINT("galera", ("trx: %llu", trx->trx_id()));

    TrxHandleLock lock(*trx);

    if (trx->state() == WSDB_TRX_MUST_ABORT)
    {
        trx->set_state(WSDB_TRX_ABORTING_NONREPL);
        return WSREP_TRX_FAIL;
    }
    assert(trx->state() == WSDB_TRX_VOID);
    trx->set_state(WSDB_TRX_REPLICATING);

    // set up trx metadata and create write set
    trx->set_conn_id(conn_id);
    trx->set_write_set_type(WSDB_WS_TYPE_TRX);
    trx->set_flags(TrxHandle::F_COMMIT);
    wsdb->create_write_set(trx, rbr_data, rbr_data_len);

    retcode = trx_repl(trx);
    if (retcode != WSREP_OK)
    {
        // replication failed
        return retcode;
    }
    *global_seqno = trx->global_seqno();

    bool do_report(false);
    int cert_ret;
    long rcode;

    if (trx->state() != WSDB_TRX_REPLICATING)
    {
        log_debug << "replicating trx changed state " << *trx;
        if (check_certification_status_for_aborted(trx) == WSREP_OK)
        {
            trx->set_position(WSDB_TRX_POS_CERT_QUEUE);
            trx->set_state(WSDB_TRX_MUST_REPLAY);
            GU_DBUG_RETURN(WSREP_BF_ABORT);
        }
        else
        {
            GALERA_SELF_CANCEL_QUEUE(cert_queue, trx->local_seqno());
            // state set to non-repl since this won't enter certification
            trx->set_state(WSDB_TRX_ABORTING_NONREPL);
            // apply monitor is self canceled in post_repl_out
            retcode = WSREP_TRX_FAIL;
            goto post_repl_out;
        }
    }

    trx->set_state(WSDB_TRX_REPLICATED);

    // wait for total order
    rcode = while_eagain_or_trx_abort(
        trx, gu_to_grab, cert_queue, trx->local_seqno());

    if (rcode != 0)
    {
        log_debug << "cert queue grab failed for trx " << *trx;
        if (check_certification_status_for_aborted(trx) == WSREP_OK)
        {
            trx->set_position(WSDB_TRX_POS_CERT_QUEUE);
            trx->set_state(WSDB_TRX_MUST_REPLAY);
            GU_DBUG_RETURN(WSREP_BF_ABORT);
        }
        else
        {
            GALERA_SELF_CANCEL_QUEUE(cert_queue, trx->local_seqno());
            // state set to non-repl since this won't enter certification
            trx->set_state(WSDB_TRX_ABORTING_NONREPL);
            // apply monitor is self canceled in post_repl_out
            retcode = WSREP_TRX_FAIL;
            goto post_repl_out;
        }
    }

    do_report = report_check_counter();

    if (trx->global_seqno() <= cert->position())
    {
        log_info << "discarding trx below cert position " << *trx;
        GALERA_RELEASE_QUEUE(cert_queue, trx->local_seqno());
        apply_monitor.self_cancel(trx);
        // state set to non-repl since this won't enter certification
        trx->set_state(WSDB_TRX_ABORTING_NONREPL);
        return WSREP_TRX_FAIL;
    }

    cert_ret = cert->append_trx(trx);

    switch (cert_ret) {
    case WSDB_OK:
        log_debug << "local certified " << *trx;
        /* certification ok */
        retcode = WSREP_OK;
        break;
    case WSDB_CERTIFICATION_FAIL:
        log_debug << "local certification failed " << *trx;
        status.local_cert_failures++;
        /* certification failed, release */
        retcode = WSREP_TRX_FAIL;
        break;
    default:
        gu_throw_fatal << "unknown certification return code " << cert_ret;
        break;
    }

    // call release only if grab was successfull
    GALERA_RELEASE_QUEUE (cert_queue, trx->local_seqno());

    if (retcode == WSREP_OK)
    {
        rcode = apply_monitor.enter(trx);
        assert(rcode == 0 || rcode == -EINTR);
        switch (rcode)
        {
        case 0:
            break;
        case -EINTR:
            log_debug << "aborting certified " << *trx;
            trx->set_position(WSDB_TRX_POS_COMMIT_QUEUE);
            trx->set_state(WSDB_TRX_MUST_REPLAY);
            retcode = WSREP_BF_ABORT;
            break;
        }
    }

post_repl_out:

    switch (retcode)
    {
    case WSREP_OK:
        trx->clear();
        // fall through
    case WSREP_BF_ABORT:
        // must keep trx data for replay
        break;
    case WSREP_TRX_FAIL:
    {
        apply_monitor.self_cancel(trx);
        if (do_report) report_last_committed (gcs_conn);
        break;
    }
    default:
        gu_throw_fatal << "unhandled retcode " << retcode;
        throw;
    }

    GU_DBUG_RETURN(retcode);
}


extern "C"
enum wsrep_status mm_galera_append_query(
    wsrep_t *gh, wsrep_trx_handle_t* trx_handle,
    const char *query, const time_t timeval, const uint32_t randseed
    ) {

    if (gu_unlikely(conn_state != GALERA_CONNECTED)) return WSREP_OK;

    TrxHandle* trx(get_trx(wsdb, trx_handle, true));
    TrxHandleLock lock(*trx);

    try
    {
        wsdb->append_query(trx, query, strlen(query), timeval, randseed);
    }
    catch (...)
    {
        return WSREP_CONN_FAIL;
    }

    return WSREP_OK;
}


extern "C"
enum wsrep_status mm_galera_append_row_key(
    wsrep_t *gh,
    wsrep_trx_handle_t* trx_handle,
    const char    *dbtable,
    const size_t dbtable_len,
    const char *key,
    const size_t key_len,
    const enum wsrep_action action
    ) {
    char wsdb_action;

    if (gu_unlikely(conn_state != GALERA_CONNECTED)) return WSREP_OK;

    TrxHandle* trx(get_trx(wsdb, trx_handle, true));
    TrxHandleLock lock(*trx);

    switch (action) {
    case WSREP_UPDATE: wsdb_action=WSDB_ACTION_UPDATE; break;
    case WSREP_DELETE: wsdb_action=WSDB_ACTION_DELETE; break;
    case WSREP_INSERT: wsdb_action=WSDB_ACTION_INSERT; break;
    }

    try
    {
        wsdb->append_row_key(trx, dbtable, dbtable_len, key, key_len, action);
    }
    catch (...)
    {
        return WSREP_CONN_FAIL;
    }
    return WSREP_OK;
}


extern "C"
wsrep_status_t mm_galera_append_data(
    wsrep_t*            wsrep,
    wsrep_trx_handle_t* trx_handle,
    const void*         data,
    size_t              data_len)
{
    return WSREP_NOT_IMPLEMENTED;
    if (gu_unlikely(conn_state != GALERA_CONNECTED)) return WSREP_OK;

    TrxHandle* trx(get_trx(wsdb, trx_handle));

    if (trx == 0)
    {
        return WSREP_TRX_MISSING;
    }

    TrxHandleLock lock(*trx);

    try
    {
        wsdb->append_data(trx, data, data_len);
    }
    catch (...)
    {
        return WSREP_CONN_FAIL;
    }
    return WSREP_OK;
}

extern "C"
wsrep_status_t mm_galera_causal_read(
    wsrep_t* wsrep,
    wsrep_seqno_t* seqno)
{
    return WSREP_NOT_IMPLEMENTED;
}


extern "C"
enum wsrep_status mm_galera_set_variable(
    wsrep_t *gh,
    const wsrep_conn_id_t  conn_id,
    const char *key,   const size_t key_len, // why is it not 0-terminated?
    const char *query, const size_t query_len)
{
    if (gu_unlikely(conn_state != GALERA_CONNECTED)) return WSREP_OK;

    /*
     * an ugly way to provide dynamic way to change galera_debug parameter.
     *
     * we catch here galera_debug variable change, and alter debugging state
     * correspondingly.
     *
     * Note that galera-set_variable() is called for every SET command in
     * session. We should avoid calling this for all variables in RBR session
     * and for many un-meaningful variables in SQL session. When these
     * optimisations are made, galera_debug changing should be implemented
     * in some other way. We should have separate calls for storing session
     * variables for connections and for setting galera configuration variables.
     *
     */
    if (!strncmp(key, "wsrep_debug", key_len)) {
        char value[256] = {0,};
        gu_debug("GALERA set value: %s" , value);
        strncpy(value, query, query_len);
        const char *set_query= "wsrep_debug=ON";

        if (strstr(value, set_query)) {
            gu_info("GALERA enabling debug logging: %s" , value);
            gu_conf_debug_on();
        } else {
            gu_info("GALERA disabling debug logging: %s %s" , value, set_query);
            gu_conf_debug_off();
        }
        return WSREP_OK;
    }

    {
        char var[256] = {0,};
        strncpy(var, key, sizeof(var) - 1);
        gu_debug("GALERA set var: %s" , var);
    }


    try
    {
        TrxHandle* trx(wsdb->get_conn_query(my_uuid, conn_id, true));
        TrxHandleLock lock(*trx);
        wsdb->set_conn_variable(trx, key, key_len, query, query_len);
    }
    catch (...)
    {
        return WSREP_CONN_FAIL;
    }



    return WSREP_OK;
}

extern "C"
enum wsrep_status mm_galera_set_database(wsrep_t *gh,
                                         const wsrep_conn_id_t conn_id,
                                         const char *query,
                                         const size_t query_len)
{
    if (gu_unlikely(conn_state != GALERA_CONNECTED)) return WSREP_OK;

    try
    {
        if (query == 0)
        {
            // 0 query means the end of connection
            wsdb->discard_conn(conn_id);
        }
        else
        {
            TrxHandle* trx(wsdb->get_conn_query(my_uuid, conn_id, true));
            TrxHandleLock lock(*trx);
            wsdb->set_conn_database(trx, query, query_len);
        }
    }
    catch (...)
    {
        return WSREP_CONN_FAIL;
    }

    return WSREP_OK;
}


extern "C"
enum wsrep_status mm_galera_to_execute_start(
    wsrep_t *gh,
    const wsrep_conn_id_t conn_id,
    const void *query,
    const size_t query_len,
    wsrep_seqno_t* global_seqno)
{

    wsrep_status_t retcode;

    GU_DBUG_ENTER("galera_to_execute_start");

    *global_seqno = WSREP_SEQNO_UNDEFINED;

    if (gu_unlikely(conn_state != GALERA_CONNECTED)) return WSREP_CONN_FAIL;

    GU_DBUG_PRINT("galera", ("conn: %llu", conn_id));

    TrxHandle* trx(wsdb->get_conn_query(my_uuid, conn_id, true));
    TrxHandleLock lock(*trx);

    // generate write set
    wsdb->append_conn_query(trx, query, query_len);
    trx->set_write_set_type(WSDB_WS_TYPE_CONN);
    trx->set_flags(TrxHandle::F_COMMIT);
    wsdb->create_write_set(trx);

    retcode = trx_repl(trx);
    // trx write set data is not needed anymore
    // but keep it for debug purposes
    // trx->clear();

    if (retcode != WSREP_OK)
    {
        return retcode;
    }
    *global_seqno = trx->global_seqno();

    log_debug << "TO isolation starting for " << *trx;

    /* wait for total order */
    GALERA_GRAB_QUEUE(cert_queue, trx->local_seqno());

    /* update global seqno */
    galera_update_last_received(trx->global_seqno());

    if (trx->global_seqno() <= cert->position())
    {
        log_info << "discarding trx below cert position " << *trx;
        GALERA_RELEASE_QUEUE(cert_queue, trx->local_seqno());
        apply_monitor.self_cancel(trx);
        // state set to non-repl since this won't enter certification
        trx->set_state(WSDB_TRX_ABORTING_NONREPL);
        return WSREP_TRX_FAIL;
    }

    int cert_ret(cert->append_trx(trx));

    if (cert_ret == WSDB_OK)
    {
        // @todo the following should be changed to apply_monitor.enter()
        // once table/db level certification is supported
        //
        // drain apply monitor, self cancel apply monitor in to_execute_end()
        apply_monitor.drain(trx->global_seqno() - 1);
        // apply_monitor.enter(trx);
        retcode = WSREP_OK;
    }
    else
    {
        log_warn << "TO isolation certification failed for trx " << *trx;
        GALERA_RELEASE_QUEUE (cert_queue, trx->local_seqno());
        apply_monitor.self_cancel(trx);
        cert->set_trx_committed(trx);
        // this situation is as good as failed gcs_repl() call.
        retcode = WSREP_CONN_FAIL;
    }

    log_debug << "TO isolation applied for trx " << *trx;

    GU_DBUG_RETURN(retcode);
}


extern "C"
enum wsrep_status mm_galera_to_execute_end(
    wsrep_t *gh, const wsrep_conn_id_t conn_id)
{
    bool do_report;

    GU_DBUG_ENTER("galera_to_execute_end");

    if (gu_unlikely(conn_state != GALERA_CONNECTED)) return WSREP_OK;


    TrxHandle* trx(wsdb->get_conn_query(my_uuid, conn_id));
    if (trx == 0)
    {
        gu_throw_fatal << "conn query " << conn_id << " not found from wsdb";

    }
    TrxHandleLock lock(*trx);

    /* release monitors */
    do_report = report_check_counter();
    GALERA_RELEASE_QUEUE (cert_queue, trx->local_seqno());
    apply_monitor.self_cancel(trx);

    gu_debug("TO applier ending  seqnos: %lld - %lld",
             trx->local_seqno(), trx->global_seqno());

    GALERA_UPDATE_LAST_APPLIED (trx->global_seqno());

    cert->set_trx_committed(trx);
    if (do_report) report_last_committed (gcs_conn);

    wsdb->discard_conn_query(trx->conn_id());
    GU_DBUG_RETURN(WSREP_OK);
}

extern "C"
enum wsrep_status mm_galera_replay_trx(
    wsrep_t *gh, wsrep_trx_handle_t* trx_handle, void *recv_ctx
    ) {

    TrxHandle* trx(get_trx(wsdb, trx_handle));
    TrxHandleLock lock(*trx);

    log_debug << "replaying trx " << *trx;

    if (trx->state() != WSDB_TRX_MUST_REPLAY) {
        gu_error("replayed trx in bad state: %d", trx->state());
        return WSREP_NODE_FAIL;
    }

    trx->set_state(WSDB_TRX_REPLAYING);
    // Adjust last depends seqno to ensure that all possibly conflicting
    // transactions have finished before applying.
    gu_debug("replaying applier starting");

    status.local_replays++;

    if (trx->write_set_type() == WSDB_WS_TYPE_TRX)
    {
        switch (trx->position())
        {
        case WSDB_TRX_POS_CERT_QUEUE:
        {
            GALERA_GRAB_QUEUE(cert_queue, trx->local_seqno());
            if (trx->global_seqno() <= cert->position())
            {
                log_info << "discarding trx below cert position " << *trx;
                GALERA_RELEASE_QUEUE(cert_queue, trx->local_seqno());
                apply_monitor.self_cancel(trx);
                trx->set_state(WSDB_TRX_ABORTING_NONREPL);
                return WSREP_TRX_FAIL;
            }
            int cert_ret(cert->append_trx(trx));
            GALERA_RELEASE_QUEUE(cert_queue, trx->local_seqno());
            if (cert_ret != WSDB_OK)
            {
                apply_monitor.self_cancel(trx);
                trx->set_state(WSDB_TRX_MUST_ABORT);
                return WSREP_TRX_FAIL;
            }
            // fall through
        }
        case WSDB_TRX_POS_COMMIT_QUEUE:
        {
            // safety measure: make sure that all preceding trxs have
            // committed in case there are hidden dependencies
            trx->set_last_depends_seqno(trx->global_seqno() - 1);
            apply_monitor.enter(trx);
            int ret_code(process_query_write_set_applying(
                             recv_ctx, trx, trx->local_seqno()));
            // apply monitor is released in post_commit()
            // apply_monitor.leave(trx);
            if (ret_code != WSREP_OK) {
                log_error << "could not replay trx " << *trx;
                return WSREP_FATAL;
            }
            break;
        }
        default:
            log_fatal << "bad position in replay for trx " << *trx;
            abort();
        }
    }
    else
    {
        gu_error("replayed trx ws has bad type: %d", trx->write_set_type());
        return WSREP_NODE_FAIL;
    }
    trx->set_state(WSDB_TRX_REPLICATED);

    log_debug << "replaying over for trx " << *trx;

    return WSREP_OK;
}

extern "C"
wsrep_status_t mm_galera_sst_sent (wsrep_t* gh,
                                   const wsrep_uuid_t* uuid,
                                   wsrep_seqno_t seqno)
{
    long err;

    if (GALERA_STAGE_DONOR != status.stage) {
        gu_error ("wsrep_sst_sent() called when not SST donor. Stage: %u",
                  status.stage);
        return WSREP_CONN_FAIL;
    }

    if (memcmp (uuid, &status.state_uuid, sizeof(wsrep_uuid_t)) &&
        seqno >= 0) {
        // state we have sent no longer corresponds to the current group state
        // mark an error.
        seqno = -EREMCHG;
    }

    // WARNING: Here we have application block on this call which
    //          may prevent application from resolving the issue.
    //          (Not that we expect that application can resolve it.)
    while (-EAGAIN == (err = gcs_join (gcs_conn, seqno))) usleep (100000);

    if (!err) {
        return WSREP_OK;
    }

    gu_error ("Failed to recover from DONOR state");
    return WSREP_CONN_FAIL;
}

extern "C"
wsrep_status_t mm_galera_sst_received (wsrep_t* gh,
                                       const wsrep_uuid_t* uuid,
                                       wsrep_seqno_t seqno,
                                       const char* state,
                                       size_t state_len)
{
    gu_mutex_lock (&sst_mtx);
    {
        sst_uuid  = *(gu_uuid_t*)uuid;
        sst_seqno = seqno;
    }
    gu_cond_signal  (&sst_cond);
    gu_mutex_unlock (&sst_mtx);

    return WSREP_OK;
}

extern "C"
wsrep_status_t mm_galera_snapshot(
    wsrep_t*     wsrep,
    const void*  msg,
    const size_t msg_len,
    const char*  donor_spec)
{
    return WSREP_NOT_IMPLEMENTED;
}

extern "C"
struct wsrep_status_var* mm_galera_status_get (wsrep_t* gh)
{
    status.cert_deps_dist = cert->get_avg_deps_dist();
    apply_monitor.get_stats(&status.apply_oooe,
                            &status.apply_oool,
                            &status.apply_window);
    return galera_status_get (&status);
}

extern "C"
void mm_galera_status_free (wsrep_t* gh,
                            struct wsrep_status_var* s)
{
    galera_status_free (s);
}

extern "C" long
galera_slave_queue()
{
    return (gcs_conn != 0 ? gcs_queue_len(gcs_conn) : 0);
}


static wsrep_t mm_galera_str = {
    WSREP_INTERFACE_VERSION,
    &mm_galera_init,
    &mm_galera_options_set,
    &mm_galera_options_get,
    &mm_galera_connect,
    &mm_galera_disconnect,
    &mm_galera_recv,
    &mm_galera_pre_commit,
    &mm_galera_post_commit,
    &mm_galera_post_rollback,
    &mm_galera_replay_trx,
    &mm_galera_abort_pre_commit,
    &mm_galera_abort_slave_trx,
    &mm_galera_append_query,
    &mm_galera_append_row_key,
    &mm_galera_append_data,
    &mm_galera_causal_read,
    &mm_galera_set_variable,
    &mm_galera_set_database,
    &mm_galera_to_execute_start,
    &mm_galera_to_execute_end,
    &mm_galera_sst_sent,
    &mm_galera_sst_received,
    &mm_galera_snapshot,
    &mm_galera_status_get,
    &mm_galera_status_free,
    "Galera",
    "0.8pre",
    "Codership Oy <info@codership.com>",
    &mm_galera_tear_down,
    NULL,
    NULL
};

/* Prototype to make compiler happy */
extern "C"
int wsrep_loader(wsrep_t *hptr);

extern "C"
int wsrep_loader(wsrep_t *hptr)
{
    if (!hptr)
        return EINVAL;
    *hptr = mm_galera_str;
    return WSREP_OK;
}
