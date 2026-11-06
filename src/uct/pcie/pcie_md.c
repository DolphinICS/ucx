#include <ucs/type/class.h>
#include <ucs/type/status.h>
#include <ucs/sys/string.h>
#include <ucs/sys/sys.h>
#include <ucs/datastruct/khash.h>
#include <ucs/arch/atomic.h>
#include <ucm/api/ucm.h>

#include <limits.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>

#include "pcie_iface.h"
#include "pcie_md.h"
#include "pcie_sisci_helper.h"

uct_component_t uct_pcie_component;

#define UCT_PCIE_NAME "pcie"

/* Segment ids: 0x40000000 + 16384 per process (by pid) */
#define UCT_PCIE_SEGMENT_ID_BASE      0x40000000u
#define UCT_PCIE_SEGMENT_ID_RANGE     16384u

typedef struct uct_pcie_md_config {
    uct_md_config_t super;
    unsigned        rkey_cache;
    size_t          reg_span;
} uct_pcie_md_config_t;

static ucs_config_field_t uct_pcie_md_config_table[] = {
    {"", "", NULL,
     ucs_offsetof(uct_pcie_md_config_t, super),
     UCS_CONFIG_TYPE_TABLE(uct_md_config_table)},

    {"RKEY_CACHE", "16",
     "Remote registered segments a process keeps connected while no rkey uses\n"
     "them, least recently used first out; all of them are given up when the\n"
     "node runs out of connections. Connections are limited per node (828 on\n"
     "fpga1614, all processes together), and with P processes on each of two\n"
     "nodes the AM rings and ack lines already take 2*P*P of them, so P*RKEY_CACHE\n"
     "must fit in the rest. Each cached connection also keeps a segment on the\n"
     "other node from being removed after it is deregistered (a peer cannot be\n"
     "told to disconnect: SCI_FLAG_NOTIFY is not implemented), and local\n"
     "segments are limited per node too (about 830-1138).",
     ucs_offsetof(uct_pcie_md_config_t, rkey_cache), UCS_CONFIG_TYPE_UINT},

    {"REG_SPAN", "0",
     "When a buffer is registered in the aligned window of this size around an\n"
     "existing registration, register the whole window, within the anonymous\n"
     "read-write mapping that holds it, and let later registrations inside it\n"
     "share the segment. Each registration is a local segment, limited per\n"
     "node; with one per buffer, MPI_Alltoall over 32 ranks\n"
     "registered every rank's slice of the send and receive buffers on its own\n"
     "and ran out of them. But collectives also register temporary buffers in\n"
     "every call, and pinning a 4 MiB window for each made\n"
     "MPI_Reduce_scatter_block over 8 ranks 6 times slower (9.4 instead of 1.5\n"
     "ms at 128 KiB). 0 (off) registers just the page-aligned buffer.",
     ucs_offsetof(uct_pcie_md_config_t, reg_span), UCS_CONFIG_TYPE_MEMUNITS},

    {NULL}
};

/* Remote segments connected for unpacked rkeys, shared by all MDs of the
 * process (rkey_unpack only gets the component). See uct_pcie_rseg_t. */
KHASH_INIT(uct_pcie_rseg, uint64_t, uct_pcie_rseg_t*, 1, kh_int64_hash_func,
           kh_int64_hash_equal)

static khash_t(uct_pcie_rseg) uct_pcie_rseg_hash = KHASH_STATIC_INITIALIZER;
static UCS_LIST_HEAD(uct_pcie_rseg_lru);
static unsigned               uct_pcie_rseg_cached;
/* Bound of uct_pcie_rseg_cached (RKEY_CACHE of the first MD opened) */
static unsigned               uct_pcie_rseg_cache_max;
static uct_pcie_desc_pool_t   uct_pcie_rseg_descs;
static pthread_mutex_t        uct_pcie_rseg_lock = PTHREAD_MUTEX_INITIALIZER;
/* Open MDs; the remote segment cache is emptied when the last one closes */
static unsigned               uct_pcie_md_count;
/* Next segment id to try, see uct_pcie_md_next_segment_id */
static volatile uint32_t      uct_pcie_next_segment_id;
/* SISCI node id of this node; set when the first MD opens */
static unsigned               uct_pcie_local_node_id = UINT_MAX;
/* rkey for memory registered on this node. SCIMapRemoteSegment cannot map a
 * registered (malloc'd) segment of the own node ("kcMmapShm failed",
 * "Functionality not implemented"), but UCP unpacks the rkeys of every MD, also
 * for its own and same-node peers' memory, which go through self/sm, never
 * pcie. Unpacking such a key succeeds without connecting; put/get on it fail. */
static uct_pcie_rseg_t        uct_pcie_rseg_same_node = {.valid = 0};

static void uct_pcie_rseg_destroy(uct_pcie_rseg_t *rseg);
static void uct_pcie_md_unmapped_cb(ucm_event_type_t event_type,
                                    ucm_event_t *event, void *arg);

/* Segment ids must be unique on the node. SCI_FLAG_AUTO_ID does not ensure
 * that: it numbers per descriptor (the 33rd registration, on the second
 * descriptor of the registration pool, got an id already in use), and two
 * processes on one node both got receive segment 1026, so a remote EP
 * connected to the wrong process. Ids therefore come from a per-process range,
 * and callers retry with the next id on SCI_ERR_SEGMENTID_USED (another
 * process whose pid maps to the same range). Thread safe: connection requests
 * create segments from the SISCI callback thread. */
unsigned uct_pcie_md_next_segment_id(void)
{
    return ucs_atomic_fadd32(&uct_pcie_next_segment_id, 1);
}

/* Called with uct_pcie_rseg_lock held */
static void uct_pcie_rseg_uncache(uct_pcie_rseg_t *rseg)
{
    khiter_t iter;

    iter = kh_get(uct_pcie_rseg, &uct_pcie_rseg_hash, rseg->key);
    ucs_assert(iter != kh_end(&uct_pcie_rseg_hash));
    kh_del(uct_pcie_rseg, &uct_pcie_rseg_hash, iter);
    ucs_list_del(&rseg->lru);
    uct_pcie_rseg_cached--;
    rseg->cached = 0;
}

/* Disconnect least recently used segments no rkey uses until at most max are
 * cached. Called with uct_pcie_rseg_lock held. */
static void uct_pcie_rseg_cache_trim(unsigned max)
{
    uct_pcie_rseg_t *rseg, *tmp;

    ucs_list_for_each_safe(rseg, tmp, &uct_pcie_rseg_lru, lru) {
        if (uct_pcie_rseg_cached <= max) {
            break;
        }
        if (rseg->refcount == 0) {
            uct_pcie_rseg_uncache(rseg);
            uct_pcie_rseg_destroy(rseg);
        }
    }
}

/******************************************************************************/
/*** MD operations ************************************************************/

/* SISCI thread: a peer connected to or disconnected from a registration */
static sci_callback_action_t
uct_pcie_memh_cb(void *arg, sci_local_segment_t segment,
                 sci_segment_cb_reason_t reason, unsigned int node_id,
                 unsigned int adapter_no, sci_error_t error)
{
    uct_pcie_memh_t *memh = arg;

    if (reason == SCI_CB_CONNECT) {
        __sync_fetch_and_add(&memh->connections, 1);
    } else if (reason == SCI_CB_DISCONNECT) {
        __sync_fetch_and_sub(&memh->connections, 1);
    }
    return SCI_CALLBACK_CONTINUE;
}

/**
 * @brief Remove deregistered segments that no peer is connected to any more.
 *
 * SCIRemoveSegment is only called once the connection count is 0. With a
 * peer connected it returns SCI_ERR_BUSY the first time, but a second call on
 * the same segment then blocks until the peer disconnects. Forcing peers off
 * (SCI_FLAG_FORCE_DISCONNECT) is not an option either: two processes doing so
 * at the same time, each connected to the other's segment, hung both in the
 * driver (unkillable, spinning in the kernel) on fpga1614/fpga1610.
 *
 * Called with busy_lock held, which is released around each SCIRemoveSegment
 * (1 ms and more, see uct_pcie_md_remover); the scan starts over afterwards,
 * since another thread may have removed segments meanwhile.
 */
static void uct_pcie_md_remove_busy_segments(uct_pcie_md_t *md)
{
    uct_pcie_memh_t *memh;
    sci_error_t      sci_error;
    int              found;

    do {
        found = 0;
        ucs_list_for_each(memh, &md->busy_segments, list) {
            if (memh->connections == 0) {
                found = 1;
                break;
            }
        }
        if (!found) {
            break;
        }
        ucs_list_del(&memh->list);
        pthread_mutex_unlock(&md->busy_lock);
        SCIRemoveSegment(memh->segment, UCT_PCIE_NO_FLAGS, &sci_error);
        pthread_mutex_lock(&md->busy_lock);
        if (sci_error != SCI_ERR_OK) {
            ucs_warn("SCIRemoveSegment(segment %u): %s; left to process exit",
                     memh->segment_id, SCIGetErrorString(sci_error));
        }
        uct_pcie_desc_pool_put(&md->reg_descs, memh->desc_index);
        if (sci_error == SCI_ERR_OK) {
            ucs_free(memh); /* else the callback may still fire */
        }
    } while (1);
}

/* How long the remover waits for peers to disconnect before checking again */
#define UCT_PCIE_REMOVER_POLL_NS 10000000

/**
 * @brief Thread that removes deregistered segments.
 *
 * SCIRemoveSegment of a registered segment took 1 ms with no peer connected
 * (more for large ones: it unpins the pages), and MPI collectives deregister
 * temporary buffers in every call: MPI_Reduce_scatter_block over 8 ranks
 * spent 4-25 ms per call removing segments. Deregistration therefore only
 * withdraws the segment and leaves the removal to this thread.
 */
static void *uct_pcie_md_remover(void *arg)
{
    uct_pcie_md_t  *md = arg;
    struct timespec deadline;

    pthread_mutex_lock(&md->busy_lock);
    while (!md->remover_stop) {
        uct_pcie_md_remove_busy_segments(md);
        if (md->remover_stop) {
            break;
        }
        if (ucs_list_is_empty(&md->busy_segments)) {
            pthread_cond_wait(&md->busy_cond, &md->busy_lock);
        } else {
            /* segments peers are still connected to */
            clock_gettime(CLOCK_REALTIME, &deadline);
            deadline.tv_nsec += UCT_PCIE_REMOVER_POLL_NS;
            if (deadline.tv_nsec >= 1000000000) {
                deadline.tv_sec++;
                deadline.tv_nsec -= 1000000000;
            }
            pthread_cond_timedwait(&md->busy_cond, &md->busy_lock, &deadline);
        }
    }
    pthread_mutex_unlock(&md->busy_lock);
    return NULL;
}

/**
 * @brief Close the memory domain and release all SISCI resources.
 *
 * All registrations must have been released by UCX before close, so only the
 * (then empty) registration descriptors and the transport descriptor remain.
 *
 * @param[in] md  Memory domain handle.
 */
/**
 * @brief Disconnect every cached remote segment no rkey uses any more.
 *
 * Must run before the process exits: two processes that exit at the same time
 * while each is still connected to the other's registered segments hang in
 * the SISCI driver (unkillable, spinning in the kernel) on fpga1614/fpga1610.
 * Called with uct_pcie_rseg_lock held.
 */
static void uct_pcie_rseg_cache_purge(void)
{
    uct_pcie_rseg_t *rseg, *tmp;

    ucs_list_for_each_safe(rseg, tmp, &uct_pcie_rseg_lru, lru) {
        if (rseg->refcount != 0) {
            ucs_warn("pcie remote segment 0x%"PRIx64" still has %u rkeys",
                     rseg->key, rseg->refcount);
            continue;
        }
        uct_pcie_rseg_uncache(rseg);
        uct_pcie_rseg_destroy(rseg);
    }
}

static void uct_pcie_md_close(uct_md_h md) {
    uct_pcie_md_t *sci_md = ucs_derived_of(md, uct_pcie_md_t);
    sci_error_t sci_error;

    pthread_mutex_lock(&uct_pcie_rseg_lock);
    if (--uct_pcie_md_count == 0) {
        uct_pcie_rseg_cache_purge();
    }
    pthread_mutex_unlock(&uct_pcie_rseg_lock);

    if (sci_md->reg_span != 0) {
        ucm_unset_event_handler(UCM_EVENT_VM_UNMAPPED, uct_pcie_md_unmapped_cb,
                                sci_md);
    }
    pthread_mutex_destroy(&sci_md->shared_lock);

    pthread_mutex_lock(&sci_md->busy_lock);
    sci_md->remover_stop = 1;
    pthread_cond_signal(&sci_md->busy_cond);
    pthread_mutex_unlock(&sci_md->busy_lock);
    if (sci_md->remover_started) {
        pthread_join(sci_md->remover, NULL);
    }

    /* Segments peers still hold are released by the driver at process exit;
     * their descriptors must stay open until then. */
    pthread_mutex_lock(&sci_md->busy_lock);
    uct_pcie_md_remove_busy_segments(sci_md);
    if (ucs_list_is_empty(&sci_md->busy_segments)) {
        uct_pcie_desc_pool_cleanup(&sci_md->reg_descs);
    }
    pthread_mutex_unlock(&sci_md->busy_lock);
    pthread_cond_destroy(&sci_md->busy_cond);
    pthread_mutex_destroy(&sci_md->busy_lock);

    SCIClose(sci_md->sci_virtual_device, UCT_PCIE_NO_FLAGS, &sci_error);
    if (sci_error != SCI_ERR_OK) {
        ucs_error("Error closing Virtual_Device error: %s",
            SCIGetErrorString(sci_error));
    }

    ucs_free(sci_md);
}

/**
 * @brief Report memory domain capabilities to UCX.
 *
 * Host memory can be registered: each registration becomes an exported SISCI
 * segment (SCIRegisterSegmentMemory, needs the IOMMU in translated mode), so
 * the peer can DMA into or out of it. Zero-copy operations need the memh of
 * the local buffer (the local side of a DMA is a segment too) and an rkey for
 * the remote one.
 *
 * @param[in]  md    Memory domain handle.
 * @param[out] attr  Attribute structure to populate.
 */
static ucs_status_t uct_pcie_md_query(uct_md_h md, uct_md_attr_v2_t *attr)
{
    uct_pcie_md_t *sci_md = ucs_derived_of(md, uct_pcie_md_t);

    uct_md_base_md_query(attr);
    attr->flags            = UCT_MD_FLAG_REG | UCT_MD_FLAG_NEED_MEMH |
                             UCT_MD_FLAG_NEED_RKEY;
    attr->reg_mem_types    = UCS_BIT(UCS_MEMORY_TYPE_HOST);
    attr->cache_mem_types  = UCS_BIT(UCS_MEMORY_TYPE_HOST);
    attr->access_mem_types = UCS_BIT(UCS_MEMORY_TYPE_HOST);
    attr->rkey_packed_size = sizeof(uct_pcie_packed_rkey_t);
    attr->reg_cost         = sci_md->reg_cost;
    return UCS_OK;
}

/* Take a reference to a shared registration that covers [start, end). If
 * there is none, *neighbour_p tells whether one lies in the same
 * reg_span-aligned window. */
static uct_pcie_memh_t *
uct_pcie_md_get_shared(uct_pcie_md_t *md, uintptr_t start, uintptr_t end,
                       int *neighbour_p)
{
    uintptr_t        win_start = 0, win_end = 0;
    uct_pcie_memh_t *memh;

    if (md->reg_span != 0) {
        win_start = ucs_align_down_pow2(start, md->reg_span);
        win_end   = ucs_align_up_pow2(end, md->reg_span);
    }

    *neighbour_p = 0;
    pthread_mutex_lock(&md->shared_lock);
    ucs_list_for_each(memh, &md->shared, shared) {
        if ((memh->base <= start) && (end <= memh->base + memh->length)) {
            memh->refcount++;
            pthread_mutex_unlock(&md->shared_lock);
            return memh;
        }
        if ((memh->base < win_end) && (win_start < memh->base + memh->length)) {
            *neighbour_p = 1;
        }
    }
    pthread_mutex_unlock(&md->shared_lock);
    return NULL;
}

/* UCM: pages are about to be unmapped or released (munmap, mremap, brk,
 * madvise). A registration over them keeps the old pages pinned, so it must
 * not be shared any more; its present users keep it until they deregister,
 * as with any registration (the UCP rcache invalidates theirs). */
static void uct_pcie_md_unmapped_cb(ucm_event_type_t event_type,
                                    ucm_event_t *event, void *arg)
{
    uct_pcie_md_t   *md    = arg;
    uintptr_t        start = (uintptr_t)event->vm_unmapped.address;
    uintptr_t        end   = start + event->vm_unmapped.size;
    uct_pcie_memh_t *memh, *tmp;

    pthread_mutex_lock(&md->shared_lock);
    ucs_list_for_each_safe(memh, tmp, &md->shared, shared) {
        if ((memh->base < end) && (start < memh->base + memh->length)) {
            ucs_list_del(&memh->shared);
            memh->in_shared = 0;
        }
    }
    pthread_mutex_unlock(&md->shared_lock);
}

/* The range to register for [start, end): the page-aligned buffer, or with
 * window set, the reg_span-aligned window around it, clipped to the anonymous
 * read-write mapping that holds it (not for file or shared memory mappings,
 * the stack, a buffer across mappings). */
static void uct_pcie_md_reg_range(const uct_pcie_md_t *md, uintptr_t start,
                                  uintptr_t end, int window, uintptr_t *base_p,
                                  size_t *length_p)
{
    size_t        page_size = ucs_get_page_size();
    uintptr_t     base      = ucs_align_down_pow2(start, page_size);
    uintptr_t     top       = ucs_align_up_pow2(end, page_size);
    unsigned long vm_start, vm_end, inode;
    char          line[1024], perms[5], path[256];
    FILE         *maps;
    int           n;

    if (window && (md->reg_span > page_size) && ucs_is_pow2(md->reg_span) &&
        ((maps = fopen("/proc/self/maps", "r")) != NULL)) {
        while (fgets(line, sizeof(line), maps) != NULL) {
            path[0] = '\0';
            n = sscanf(line, "%lx-%lx %4s %*s %*s %lu %255s", &vm_start,
                       &vm_end, perms, &inode, path);
            if ((n < 4) || (vm_end <= start)) {
                continue;
            }
            if ((vm_start <= start) && (end <= vm_end) &&
                (perms[0] == 'r') && (perms[1] == 'w') && (inode == 0) &&
                ((path[0] == '\0') || !strcmp(path, "[heap]"))) {
                base = ucs_max(vm_start,
                               ucs_align_down_pow2(start, md->reg_span));
                top  = ucs_min(vm_end, ucs_align_up_pow2(end, md->reg_span));
            }
            break;
        }
        fclose(maps);
    }

    *base_p   = base;
    *length_p = top - base;
}

/**
 * @brief Register a host buffer as an exported SISCI segment.
 *
 * Local segments are limited per node (about 1138, all processes together).
 * A registration inside a registered range therefore takes a reference to
 * that handle instead of a segment of its own. With REG_SPAN set (off by
 * default, see there for why), the second registration in a reg_span-aligned
 * window covers the whole window within its mapping (uct_pcie_md_reg_range),
 * so that the slices of one buffer, as in MPI_Alltoall, share a few segments.
 * The range becomes a segment with a unique id on a descriptor from the
 * registration pool.
 *
 * @param[in]  uct_md   Memory domain handle.
 * @param[in]  address  Start of the buffer.
 * @param[in]  length   Length of the buffer.
 * @param[in]  params   Registration parameters (flags).
 * @param[out] memh_p   Set to the new uct_pcie_memh_t.
 */
static ucs_status_t uct_pcie_mem_reg(uct_md_h uct_md, void *address,
                                     size_t length,
                                     const uct_md_mem_reg_params_t *params,
                                     uct_mem_h *memh_p)
{
    uct_pcie_md_t   *md        = ucs_derived_of(uct_md, uct_pcie_md_t);
    uint64_t         flags     = UCT_MD_MEM_REG_FIELD_VALUE(params, flags,
                                                            FIELD_FLAGS, 0);
    ucs_log_level_t  log_level = (flags & UCT_MD_MEM_FLAG_HIDE_ERRORS) ?
                                 UCS_LOG_LEVEL_DIAG : UCS_LOG_LEVEL_ERROR;
    uct_pcie_memh_t *memh;
    sci_desc_t       sd;
    sci_error_t      sci_error;
    const char      *step;
    ucs_status_t     status;
    unsigned         attempt;
    int              window;

    memh = uct_pcie_md_get_shared(md, (uintptr_t)address,
                                  (uintptr_t)address + length, &window);
    if (memh != NULL) {
        *memh_p = memh;
        return UCS_OK;
    }

    memh = ucs_malloc(sizeof(*memh), "uct_pcie_memh_t");
    if (memh == NULL) {
        return UCS_ERR_NO_MEMORY;
    }

    uct_pcie_md_reg_range(md, (uintptr_t)address, (uintptr_t)address + length,
                          window, &memh->base, &memh->length);

    pthread_mutex_lock(&md->busy_lock);
    status = uct_pcie_desc_pool_get(&md->reg_descs, &sd, &memh->desc_index);
    pthread_mutex_unlock(&md->busy_lock);
    if (status != UCS_OK) {
        goto err_free;
    }

    memh->connections = 0;
    step              = "SCICreateSegment";
    for (attempt = 0; attempt < UCT_PCIE_SEGMENT_ID_ATTEMPTS; attempt++) {
        memh->segment_id = uct_pcie_md_next_segment_id();
        SCICreateSegment(sd, &memh->segment, memh->segment_id, memh->length,
                         uct_pcie_memh_cb, memh, SCI_FLAG_EMPTY |
                         SCI_FLAG_USE_CALLBACK, &sci_error);
        if (sci_error == SCI_ERR_NOSPC) {
            /* Out of segments on this node: the remover may be behind
             * (buffers registered and deregistered in every call of a
             * collective); remove what can be removed now and retry */
            pthread_mutex_lock(&md->busy_lock);
            uct_pcie_md_remove_busy_segments(md);
            pthread_mutex_unlock(&md->busy_lock);
            SCICreateSegment(sd, &memh->segment, memh->segment_id,
                             memh->length, uct_pcie_memh_cb, memh,
                             SCI_FLAG_EMPTY | SCI_FLAG_USE_CALLBACK,
                             &sci_error);
        }
        if (sci_error != SCI_ERR_SEGMENTID_USED) {
            break;
        }
    }
    if (sci_error != SCI_ERR_OK) {
        goto err_put_desc;
    }

    step = "SCIRegisterSegmentMemory";
    SCIRegisterSegmentMemory((void*)memh->base, memh->length, memh->segment,
                             SCI_FLAG_LOCK_USER_MEM, &sci_error);
    if (sci_error != SCI_ERR_OK) {
        goto err_remove;
    }

    step = "SCIPrepareSegment";
    SCIPrepareSegment(memh->segment, UCT_PCIE_LOCAL_ADAPTER_NO,
                      UCT_PCIE_NO_FLAGS, &sci_error);
    if (sci_error != SCI_ERR_OK) {
        goto err_remove;
    }

    step = "SCISetSegmentAvailable";
    SCISetSegmentAvailable(memh->segment, UCT_PCIE_LOCAL_ADAPTER_NO,
                           UCT_PCIE_NO_FLAGS, &sci_error);
    if (sci_error != SCI_ERR_OK) {
        goto err_remove;
    }

    ucs_debug("pcie registered %p..%p as segment %u",
              (void*)memh->base, (void*)(memh->base + memh->length),
              memh->segment_id);
    memh->refcount = 1;
    pthread_mutex_lock(&md->shared_lock);
    ucs_list_add_tail(&md->shared, &memh->shared);
    memh->in_shared = 1;
    pthread_mutex_unlock(&md->shared_lock);
    *memh_p = memh;
    return UCS_OK;

err_remove:
    SCIRemoveSegment(memh->segment, UCT_PCIE_NO_FLAGS, &sci_error);
err_put_desc:
    pthread_mutex_lock(&md->busy_lock);
    uct_pcie_desc_pool_put(&md->reg_descs, memh->desc_index);
    pthread_mutex_unlock(&md->busy_lock);
    ucs_log(log_level, "pcie failed to register %p length %zu: %s: %s",
            address, length, step, SCIGetErrorString(sci_error));
    status = UCS_ERR_IO_ERROR;
err_free:
    ucs_free(memh);
    return status;
}

/**
 * @brief Withdraw a registration.
 *
 * Drops one reference; when the last registration sharing the handle is gone,
 * the segment stops accepting connections and goes to the remover thread
 * (uct_pcie_md_remover). If a peer still has it connected (a cached remote
 * key), it cannot be removed yet: it stays pinned and keeps its id, so no new
 * registration can be mistaken for it, and it is removed after the peer
 * disconnected (peers disconnect cached segments when their last MD closes).
 * The pages stay pinned by the driver until then, at the latest until
 * process exit.
 *
 * @param[in] uct_md  Memory domain handle.
 * @param[in] params  Deregistration parameters (memh).
 */
static ucs_status_t uct_pcie_mem_dereg(uct_md_h uct_md,
                                       const uct_md_mem_dereg_params_t *params)
{
    uct_pcie_md_t   *md = ucs_derived_of(uct_md, uct_pcie_md_t);
    uct_pcie_memh_t *memh;
    sci_error_t      sci_error;

    UCT_MD_MEM_DEREG_CHECK_PARAMS(params, 0);
    memh = params->memh;

    pthread_mutex_lock(&md->shared_lock);
    if (--memh->refcount > 0) {
        pthread_mutex_unlock(&md->shared_lock);
        return UCS_OK;
    }
    if (memh->in_shared) {
        ucs_list_del(&memh->shared);
        memh->in_shared = 0;
    }
    pthread_mutex_unlock(&md->shared_lock);

    SCISetSegmentUnavailable(memh->segment, UCT_PCIE_LOCAL_ADAPTER_NO,
                             UCT_PCIE_NO_FLAGS, &sci_error);
    if (sci_error != SCI_ERR_OK) {
        ucs_warn("SCISetSegmentUnavailable(segment %u): %s", memh->segment_id,
                 SCIGetErrorString(sci_error));
    }

    pthread_mutex_lock(&md->busy_lock);
    ucs_list_add_tail(&md->busy_segments, &memh->list);
    if (!md->remover_started) {
        md->remover_started =
            (pthread_create(&md->remover, NULL, uct_pcie_md_remover, md) == 0);
        if (!md->remover_started) {
            uct_pcie_md_remove_busy_segments(md); /* remove it here instead */
        }
    }
    pthread_cond_signal(&md->busy_cond);
    pthread_mutex_unlock(&md->busy_lock);
    return UCS_OK;
}

/******************************************************************************/
/*** Rkeys ********************************************************************/

static ucs_status_t uct_pcie_mkey_pack(uct_md_h uct_md, uct_mem_h memh,
                                       void *address, size_t length,
                                       const uct_md_mkey_pack_params_t *params,
                                       void *mkey_buffer)
{
    uct_pcie_md_t          *md     = ucs_derived_of(uct_md, uct_pcie_md_t);
    uct_pcie_memh_t        *pmemh  = memh;
    uct_pcie_packed_rkey_t *packed = mkey_buffer;

    packed->node_id    = md->node_id;
    packed->segment_id = pmemh->segment_id;
    packed->base       = pmemh->base;
    packed->length     = pmemh->length;
    return UCS_OK;
}

/* SISCI thread: the owner withdrew the segment (mem_dereg) or the link died */
static sci_callback_action_t
uct_pcie_rseg_cb(void *arg, sci_remote_segment_t segment,
                 sci_segment_cb_reason_t reason, sci_error_t status)
{
    uct_pcie_rseg_t *rseg = arg;

    if ((reason == SCI_CB_DISCONNECT) || (reason == SCI_CB_LOST)) {
        rseg->valid = 0;
    }
    return SCI_CALLBACK_CONTINUE;
}

static void uct_pcie_rseg_destroy(uct_pcie_rseg_t *rseg)
{
    sci_error_t sci_error;

    if (rseg->va != NULL) {
        SCIUnmapSegment(rseg->map, UCT_PCIE_NO_FLAGS, &sci_error);
    }
    SCIDisconnectSegment(rseg->segment, UCT_PCIE_NO_FLAGS, &sci_error);
    uct_pcie_desc_pool_put(&uct_pcie_rseg_descs, rseg->desc_index);
    ucs_free(rseg);
}

/* Connect and map a remote segment. Returns UCS_ERR_NO_RESOURCE if the node
 * ran out of connections (SISCI reports SCI_ERR_NOSPC), logged at
 * nospc_log_level. */
static ucs_status_t uct_pcie_rseg_create(const uct_pcie_packed_rkey_t *packed,
                                         ucs_log_level_t nospc_log_level,
                                         uct_pcie_rseg_t **rseg_p)
{
    uct_pcie_rseg_t *rseg;
    sci_desc_t       sd;
    sci_error_t      sci_error;
    ucs_status_t     status;

    rseg = ucs_calloc(1, sizeof(*rseg), "uct_pcie_rseg_t");
    if (rseg == NULL) {
        return UCS_ERR_NO_MEMORY;
    }

    status = uct_pcie_desc_pool_get(&uct_pcie_rseg_descs, &sd,
                                    &rseg->desc_index);
    if (status != UCS_OK) {
        ucs_free(rseg);
        return status;
    }

    rseg->key    = ((uint64_t)packed->node_id << 32) | packed->segment_id;
    rseg->base   = packed->base;
    rseg->length = packed->length;
    rseg->valid  = 1;

    /* SISCI only implements SCI_INFINITE_TIMEOUT here */
    ucs_debug("pcie connecting node %u segment %u (%"PRIu64" bytes)",
              packed->node_id, packed->segment_id, packed->length);
    SCIConnectSegment(sd, &rseg->segment, packed->node_id, packed->segment_id,
                      UCT_PCIE_LOCAL_ADAPTER_NO, uct_pcie_rseg_cb, rseg,
                      SCI_INFINITE_TIMEOUT, SCI_FLAG_USE_CALLBACK, &sci_error);
    if (sci_error != SCI_ERR_OK) {
        ucs_log((sci_error == SCI_ERR_NOSPC) ? nospc_log_level :
                                               UCS_LOG_LEVEL_ERROR,
                "SCIConnectSegment(node %u segment %u): %s", packed->node_id,
                packed->segment_id, SCIGetErrorString(sci_error));
        uct_pcie_desc_pool_put(&uct_pcie_rseg_descs, rseg->desc_index);
        ucs_free(rseg);
        return (sci_error == SCI_ERR_NOSPC) ? UCS_ERR_NO_RESOURCE :
                                              UCS_ERR_UNREACHABLE;
    }

    /* The adapter DMA engine needs the remote segment mapped, and PIO puts
     * write through the mapping. */
    rseg->va = SCIMapRemoteSegment(rseg->segment, &rseg->map, 0, rseg->length,
                                   NULL, UCT_PCIE_NO_FLAGS, &sci_error);
    if (sci_error != SCI_ERR_OK) {
        ucs_log((sci_error == SCI_ERR_NOSPC) ? nospc_log_level :
                                               UCS_LOG_LEVEL_ERROR,
                "SCIMapRemoteSegment(node %u segment %u, %"PRIu64" bytes): %s",
                packed->node_id, packed->segment_id, rseg->length,
                SCIGetErrorString(sci_error));
        rseg->va = NULL;
        uct_pcie_rseg_destroy(rseg);
        return UCS_ERR_NO_RESOURCE;
    }

    ucs_debug("pcie connected node %u segment %u at %p", packed->node_id,
              packed->segment_id, rseg->va);
    *rseg_p = rseg;
    return UCS_OK;
}

/**
 * @brief Unpack an rkey: find or create the connected remote segment.
 *
 * The returned rkey is the uct_pcie_rseg_t pointer; put/get translate
 * remote_addr with remote_addr - rseg->base.
 */
static ucs_status_t uct_pcie_md_rkey_unpack(uct_component_t *component,
    const void *rkey_buffer, const uct_rkey_unpack_params_t *params,
    uct_rkey_t *rkey_p, void **handle_p)
{
    const uct_pcie_packed_rkey_t *packed = rkey_buffer;
    uint64_t                      key    = ((uint64_t)packed->node_id << 32) |
                                           packed->segment_id;
    uct_pcie_rseg_t              *rseg   = NULL;
    ucs_status_t                  status = UCS_OK;
    khiter_t                      iter;
    int                           ret;

    if (packed->node_id == uct_pcie_local_node_id) {
        *rkey_p   = (uintptr_t)&uct_pcie_rseg_same_node;
        *handle_p = NULL;
        return UCS_OK;
    }

    pthread_mutex_lock(&uct_pcie_rseg_lock);

    iter = kh_get(uct_pcie_rseg, &uct_pcie_rseg_hash, key);
    if (iter != kh_end(&uct_pcie_rseg_hash)) {
        rseg = kh_val(&uct_pcie_rseg_hash, iter);
        if (!rseg->valid || (rseg->base != packed->base) ||
            (rseg->length != packed->length)) {
            /* The owner withdrew it, or re-registered under the same id */
            uct_pcie_rseg_uncache(rseg);
            if (rseg->refcount == 0) {
                uct_pcie_rseg_destroy(rseg);
            }
            rseg = NULL;
        } else {
            ucs_list_del(&rseg->lru);
            ucs_list_add_tail(&uct_pcie_rseg_lru, &rseg->lru);
        }
    }

    if (rseg == NULL) {
        uct_pcie_rseg_cache_trim(uct_pcie_rseg_cache_max);
        status = uct_pcie_rseg_create(packed, UCS_LOG_LEVEL_DIAG, &rseg);
        if (status == UCS_ERR_NO_RESOURCE) {
            /* Connections are limited per node (828 on fpga1614, all
             * processes together, see RKEY_CACHE): give up the idle ones */
            uct_pcie_rseg_cache_trim(0);
            status = uct_pcie_rseg_create(packed, UCS_LOG_LEVEL_ERROR, &rseg);
        }
        if (status != UCS_OK) {
            goto out;
        }
        iter = kh_put(uct_pcie_rseg, &uct_pcie_rseg_hash, key, &ret);
        ucs_assert_always(iter != kh_end(&uct_pcie_rseg_hash));
        kh_val(&uct_pcie_rseg_hash, iter) = rseg;
        ucs_list_add_tail(&uct_pcie_rseg_lru, &rseg->lru);
        uct_pcie_rseg_cached++;
        rseg->cached = 1;
    }

    rseg->refcount++;
    *rkey_p   = (uintptr_t)rseg;
    *handle_p = NULL;

out:
    pthread_mutex_unlock(&uct_pcie_rseg_lock);
    return status;
}

/* The connection stays cached; it is released when replaced (see unpack) */
static ucs_status_t uct_pcie_rkey_release(uct_component_t *component,
    uct_rkey_t rkey, void *handle)
{
    uct_pcie_rseg_t *rseg = (uct_pcie_rseg_t*)rkey;

    if (rseg == &uct_pcie_rseg_same_node) {
        return UCS_OK;
    }

    pthread_mutex_lock(&uct_pcie_rseg_lock);
    ucs_assert(rseg->refcount > 0);
    if ((--rseg->refcount == 0) && !rseg->cached) {
        uct_pcie_rseg_destroy(rseg);
    }
    pthread_mutex_unlock(&uct_pcie_rseg_lock);
    return UCS_OK;
}

/******************************************************************************/
/*** MD open ******************************************************************/

/**
 * @brief Open the SISCI PCIe memory domain.
 *
 * Each call returns an independent MD instance: multiple ucp_contexts (and
 * therefore multiple open pcie MDs) can coexist in the same process.
 *
 * @param[in]  component  UCX component handle.
 * @param[in]  md_name    Device name (ignored; always "pcie").
 * @param[in]  config     MD configuration table.
 * @param[out] md_p       Set to the opened MD on success.
 */
static ucs_status_t uct_pcie_md_open(
    uct_component_t *component,
    const char *md_name,
    const uct_md_config_t *config,
    uct_md_h *md_p)
{
    uct_pcie_md_config_t *md_config =
        ucs_derived_of(config, uct_pcie_md_config_t);

    static uct_md_ops_t md_ops = {
        .close              = uct_pcie_md_close,
        .query              = uct_pcie_md_query,
        .mem_alloc          = (uct_md_mem_alloc_func_t)ucs_empty_function_return_unsupported,
        .mem_free           = (uct_md_mem_free_func_t)ucs_empty_function_return_unsupported,
        .mem_advise         = (uct_md_mem_advise_func_t)ucs_empty_function_return_unsupported,
        .mem_reg            = uct_pcie_mem_reg,
        .mem_dereg          = uct_pcie_mem_dereg,
        .mem_query          = (uct_md_mem_query_func_t)ucs_empty_function_return_unsupported,
        .mkey_pack          = uct_pcie_mkey_pack,
        .mem_attach         = (uct_md_mem_attach_func_t)ucs_empty_function_return_unsupported,
        .detect_memory_type = (uct_md_detect_memory_type_func_t)ucs_empty_function_return_unsupported
    };

    uct_pcie_md_t *md;
    sci_error_t errors;

    /* One md_open call per ucp_context/process is not a safe assumption:
     * multiple independent contexts (e.g. an MPI library's own UCX context
     * alongside a separate UCC context) can coexist in the same process, so
     * the MD must be a distinct heap instance per open, not a shared static. */
    md = ucs_calloc(1, sizeof(*md), "uct_pcie_md_t");
    if (md == NULL) {
        ucs_error("failed to allocate uct_pcie_md_t");
        return UCS_ERR_NO_MEMORY;
    }

    SCIOpen(&md->sci_virtual_device, UCT_PCIE_NO_FLAGS, &errors);
    if (errors != SCI_ERR_OK) {
        ucs_error("SCIOpen: %s", SCIGetErrorString(errors));
        ucs_free(md);
        return UCS_ERR_NO_RESOURCE;
    }

    SCIGetLocalNodeId(UCT_PCIE_LOCAL_ADAPTER_NO, &md->node_id,
                      UCT_PCIE_NO_FLAGS, &errors);
    if (errors != SCI_ERR_OK) {
        ucs_error("SCIGetLocalNodeId: %s", SCIGetErrorString(errors));
        SCIClose(md->sci_virtual_device, UCT_PCIE_NO_FLAGS, &errors);
        ucs_free(md);
        return UCS_ERR_NO_RESOURCE;
    }

    /* fpga1614 <-> fpga1610, MXH930: 2-3 us fixed, ~18.6 us per MiB */
    md->reg_cost        = ucs_linear_func_make(3e-6, 18.6e-6 / UCS_MBYTE);
    ucs_list_head_init(&md->busy_segments);
    pthread_mutex_init(&md->busy_lock, NULL);
    pthread_cond_init(&md->busy_cond, NULL);
    ucs_list_head_init(&md->shared);
    pthread_mutex_init(&md->shared_lock, NULL);

    /* Sharing a registration is only safe while unmap events retire it */
    md->reg_span = md_config->reg_span;
    if ((md->reg_span != 0) &&
        (ucm_set_event_handler(UCM_EVENT_VM_UNMAPPED, 1000,
                               uct_pcie_md_unmapped_cb, md) != UCS_OK)) {
        ucs_diag("pcie: no UCM unmap events, registering buffers one by one");
        md->reg_span = 0;
    }

    pthread_mutex_lock(&uct_pcie_rseg_lock);
    uct_pcie_local_node_id = md->node_id;
    if (uct_pcie_md_count++ == 0) {
        uct_pcie_next_segment_id = UCT_PCIE_SEGMENT_ID_BASE +
                                   (getpid() % 0x10000) *
                                   UCT_PCIE_SEGMENT_ID_RANGE;
        uct_pcie_rseg_cache_max  = md_config->rkey_cache;
    }
    pthread_mutex_unlock(&uct_pcie_rseg_lock);
    md->super.ops       = &md_ops;
    md->super.component = &uct_pcie_component;

    *md_p = &md->super;
    return UCS_OK;
}

uct_component_t uct_pcie_component = {
    .query_md_resources = uct_md_query_single_md_resource,
    .md_open            = uct_pcie_md_open,
    .cm_open            = (uct_component_cm_open_func_t)ucs_empty_function_return_unsupported,
    .rkey_unpack        = uct_pcie_md_rkey_unpack,
    .rkey_ptr           = (uct_component_rkey_ptr_func_t)ucs_empty_function_return_unsupported,
    .rkey_release       = uct_pcie_rkey_release,
    .rkey_compare       = uct_base_rkey_compare,
    .name               = UCT_PCIE_NAME,
    .md_config          = {
        .name           = "PCIe memory domain",
        .prefix         = "PCIE_",
        .table          = uct_pcie_md_config_table,
        .size           = sizeof(uct_pcie_md_config_t),
    },
    .tl_list            = UCT_COMPONENT_TL_LIST_INITIALIZER(&uct_pcie_component),
    .flags              = 0,
    .md_vfs_init        = (uct_component_md_vfs_init_func_t)ucs_empty_function
};
UCT_COMPONENT_REGISTER(&uct_pcie_component)
