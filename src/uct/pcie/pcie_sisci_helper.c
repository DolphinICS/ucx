#include "pcie_sisci_helper.h"

/* To get ucs print outs */
#include <ucs/type/status.h>

#include <sys/mman.h>
#include <unistd.h>

/******************************************************************************/
/*** Local segment operations *************************************************/

/**
 * @brief Creates, prepares and maps a local sisci segment with a segment ID
 *        that is unique on this node (see uct_pcie_md_next_segment_id).
 * @param[in] sd 
 * @param[in] segment 
 * @param[in] segment_map 
 * @param[in] segment_size 
 * @param[out] segment_id 
 * @param[out] buf 
 * @return 
 */
int uct_pcie_helper_create_segment(
    sci_desc_t sd,
    sci_local_segment_t *segment,
    sci_map_t *segment_map,
    size_t segment_size,
    unsigned int *segment_id,
    void **buf)
{
    sci_error_t sci_error = SCI_ERR_SEGMENTID_USED;
    unsigned int attempt;

    for (attempt = 0; attempt < UCT_PCIE_SEGMENT_ID_ATTEMPTS; attempt++) {
        SCICreateSegment(
            sd,
            segment,
            uct_pcie_md_next_segment_id(),
            segment_size,
            UCT_PCIE_NO_CALLBACK,
            NULL, /* callbackArg == NULL */
            UCT_PCIE_NO_FLAGS,
            &sci_error);
        if (sci_error != SCI_ERR_SEGMENTID_USED) {
            break;
        }
    }
    if (sci_error != SCI_ERR_OK) { 
        ucs_error("SCICreateSegment failed: %s", SCIGetErrorString(sci_error));
        return -1;
    }

    SCIPrepareSegment(
        *segment,
        UCT_PCIE_LOCAL_ADAPTER_NO,
        UCT_PCIE_NO_FLAGS,
        &sci_error);
    if (sci_error != SCI_ERR_OK) { 
        ucs_error("SCIPrepareSegment failed: %s", SCIGetErrorString(sci_error));
        SCIRemoveSegment(*segment, UCT_PCIE_NO_FLAGS , &sci_error);
        return -1;
    }

    *buf = SCIMapLocalSegment(
        *segment,
        segment_map,
        0, /* Mapping offset == 0 */
        segment_size,
        NULL, /* No suggested virtual address */
        UCT_PCIE_NO_FLAGS,
        &sci_error);
    if (sci_error != SCI_ERR_OK) { 
        ucs_error("SCIMapLocalSegment failed: %s",
            SCIGetErrorString(sci_error));
        SCIRemoveSegment(*segment, UCT_PCIE_NO_FLAGS, &sci_error);
        return -1;
    }

    *segment_id = SCIGetLocalSegmentId(*segment);

    return 0;
}

/**
 * @brief Undos setup by uct_pcie_helper_create_segment.
 * 
 * @details Unmaps and removes local segment set up by
 *          uct_pcie_helper_create_segment.
 * 
 * @param[in] segment 
 * @param[in] segment_map 
 */
void uct_pcie_helper_remove_segment(
    sci_local_segment_t segment,
    sci_map_t segment_map)
{
    sci_error_t sci_error;

    SCIUnmapSegment(segment_map, UCT_PCIE_NO_FLAGS, &sci_error);
    if (sci_error != SCI_ERR_OK) {
        ucs_warn("SCIUnmapSegment failed: %s\n", SCIGetErrorString(sci_error));
    }

    SCIRemoveSegment(segment, UCT_PCIE_NO_FLAGS, &sci_error);
    if (sci_error != SCI_ERR_OK) {
        ucs_warn("SCIRemoveSegment failed: %s\n", SCIGetErrorString(sci_error));
    }
}

/**
 * @brief Creates, prepares and maps a local sisci segment, then sets it
 *        available for remote connections. The segment ID is unique on this
 *        node (see uct_pcie_md_next_segment_id).
 * @param[in] sd 
 * @param[in] segment 
 * @param[in] segment_map 
 * @param[in] segment_size 
 * @param[out] segment_id 
 * @param[out] buf 
 * @return 
 */
int uct_pcie_helper_create_seg_set_avail(
    sci_desc_t sd,
    sci_local_segment_t *segment,
    sci_map_t *segment_map,
    size_t segment_size,
    unsigned int *segment_id,
    void **buf)
{
    sci_error_t sci_error;
    int ret;
    
    ret = uct_pcie_helper_create_segment(
        sd,
        segment,
        segment_map,
        segment_size,
        segment_id,
        buf);
    if (ret == 0) {
        SCISetSegmentAvailable(*segment, UCT_PCIE_LOCAL_ADAPTER_NO, UCT_PCIE_NO_FLAGS, &sci_error);
        if (sci_error != SCI_ERR_OK) { 
            ucs_error("SCISetSegmentAvailable failed: %s",
                SCIGetErrorString(sci_error));
            uct_pcie_helper_remove_segment(*segment, *segment_map);
            return -1;
        }
    }

    return 0;
}

/**
 * @brief Unmaps, removes local segment (after first setting it to unavailable)
 *        set up by uct_pcie_helper_create_segment.
 *        (Undos setup by uct_pcie_helper_create_segment)
 *
 * @details Unmaps, removes local segment set up by
 *          uct_pcie_helper_create_segment.
 *          (after first setting it to unavailable)
 * 
 * @param[in] segment 
 * @param[in] segment_map 
 */
void uct_pcie_helper_remove_seg_set_unavail(
    sci_local_segment_t segment,
    sci_map_t segment_map)
{
    sci_error_t sci_error;
    SCISetSegmentUnavailable(segment, UCT_PCIE_LOCAL_ADAPTER_NO, UCT_PCIE_NO_FLAGS, &sci_error);
    if (sci_error != SCI_ERR_OK) {
        ucs_warn("SCISetSegmentUnavailable failed: %s",
            SCIGetErrorString(sci_error));
    }
    uct_pcie_helper_remove_segment(segment, segment_map);
}

/**
 * @brief Creates a segment on process memory and sets it available.
 *
 * @details SCICreateSegment allocates from a driver pool of about 164 MiB per
 *          node (fpga1614: 41 segments of 4 MiB, shared by all processes), too
 *          small for one AM ring per connection with many ranks per node. This
 *          segment is anonymous memory registered with SCIRegisterSegmentMemory
 *          instead, like the zero-copy registrations. The memory is mmap'd so
 *          that it never returns to the heap while a peer may still write to it
 *          (see uct_pcie_helper_remove_user_segment).
 *
 * @param[in]  sd           Descriptor for the segment.
 * @param[out] segment      Created segment.
 * @param[in]  segment_size Size in bytes, a multiple of the page size.
 * @param[out] segment_id   Segment ID, unique on this node.
 * @param[out] buf          Zero-filled local memory of the segment.
 * @return 0 on success, -1 on failure (logged).
 */
int uct_pcie_helper_create_user_segment(
    sci_desc_t sd,
    sci_local_segment_t *segment,
    size_t segment_size,
    unsigned int *segment_id,
    void **buf)
{
    sci_error_t  sci_error = SCI_ERR_SEGMENTID_USED;
    const char  *step;
    unsigned int attempt;
    void        *mem;

    mem = mmap(NULL, segment_size, PROT_READ | PROT_WRITE,
               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED) {
        ucs_error("mmap(%zu) for a pcie segment failed: %m", segment_size);
        return -1;
    }

    step = "SCICreateSegment";
    for (attempt = 0; attempt < UCT_PCIE_SEGMENT_ID_ATTEMPTS; attempt++) {
        *segment_id = uct_pcie_md_next_segment_id();
        SCICreateSegment(sd, segment, *segment_id, segment_size,
                         UCT_PCIE_NO_CALLBACK, NULL, SCI_FLAG_EMPTY,
                         &sci_error);
        if (sci_error != SCI_ERR_SEGMENTID_USED) {
            break;
        }
    }
    if (sci_error != SCI_ERR_OK) {
        goto err_unmap;
    }

    step = "SCIRegisterSegmentMemory";
    SCIRegisterSegmentMemory(mem, segment_size, *segment,
                             SCI_FLAG_LOCK_USER_MEM, &sci_error);
    if (sci_error != SCI_ERR_OK) {
        goto err_remove;
    }

    step = "SCIPrepareSegment";
    SCIPrepareSegment(*segment, UCT_PCIE_LOCAL_ADAPTER_NO, UCT_PCIE_NO_FLAGS,
                      &sci_error);
    if (sci_error != SCI_ERR_OK) {
        goto err_remove;
    }

    step = "SCISetSegmentAvailable";
    SCISetSegmentAvailable(*segment, UCT_PCIE_LOCAL_ADAPTER_NO,
                           UCT_PCIE_NO_FLAGS, &sci_error);
    if (sci_error != SCI_ERR_OK) {
        goto err_remove;
    }

    *buf = mem;
    return 0;

err_remove:
    SCIRemoveSegment(*segment, UCT_PCIE_NO_FLAGS, &sci_error);
err_unmap:
    munmap(mem, segment_size);
    ucs_error("pcie segment of %zu bytes: %s failed: %s", segment_size, step,
              SCIGetErrorString(sci_error));
    return -1;
}

/**
 * @brief Removes a segment made by uct_pcie_helper_create_user_segment.
 *
 * @details A segment that a peer still has connected cannot be removed
 *          (SCI_ERR_BUSY), and a second remove blocks until the peer
 *          disconnects, so it is left to the driver: the pages stay pinned
 *          until the process exits. Unmapping the range is safe either way,
 *          since pinned pages are not handed out again while registered.
 *
 * @param[in] segment      Segment to remove.
 * @param[in] buf          Its memory.
 * @param[in] segment_size Its size.
 */
void uct_pcie_helper_remove_user_segment(
    sci_local_segment_t segment,
    void *buf,
    size_t segment_size)
{
    sci_error_t sci_error;

    SCISetSegmentUnavailable(segment, UCT_PCIE_LOCAL_ADAPTER_NO,
                             UCT_PCIE_NO_FLAGS, &sci_error);
    SCIRemoveSegment(segment, UCT_PCIE_NO_FLAGS, &sci_error);
    if (sci_error != SCI_ERR_OK) {
        ucs_debug("pcie segment still connected, left to the driver: %s",
                  SCIGetErrorString(sci_error));
    }
    munmap(buf, segment_size);
}

/******************************************************************************/
/*** Remote segment operations ************************************************/

int uct_pcie_connect_segment(
    sci_desc_t sd,
    size_t offset,
    size_t segment_size,
    unsigned int node_id,
    unsigned int segment_id,
    sci_remote_segment_t *segment,
    sci_map_t *segment_map,
    volatile void **buf,
    sci_cb_remote_segment_t callback,
    void *callback_arg)
{
    sci_error_t  sci_error;
    unsigned int flags = (callback != NULL) ? SCI_FLAG_USE_CALLBACK
                                            : UCT_PCIE_NO_FLAGS;
    unsigned int attempt;

    /* Retry while the segment is not there yet, but not forever: out of
     * connections (SCI_ERR_NOSPC, 828 per node) this looped in the connection
     * handler, which then answered no further requests */
    for (attempt = 0; attempt < UCT_PCIE_CONN_ATTEMPTS; attempt++) {
        SCIConnectSegment(sd,
            segment,
            node_id,
            segment_id,
            UCT_PCIE_LOCAL_ADAPTER_NO,
            callback,
            callback_arg,
            SCI_INFINITE_TIMEOUT,
            flags,
            &sci_error);
        if ((sci_error == SCI_ERR_OK) || (sci_error == SCI_ERR_NOSPC)) {
            break;
        }
        usleep(1000);
    }
    if (sci_error != SCI_ERR_OK) {
        ucs_error("SCIConnectSegment(node %u segment %u): %s", node_id,
                  segment_id, SCIGetErrorString(sci_error));
        return -1;
    }

    /* Todo: maybe not discard volatile property? */
    *buf = SCIMapRemoteSegment(
        *segment,
        segment_map,
        offset,
        segment_size,
        NULL,
        UCT_PCIE_NO_FLAGS,
        &sci_error);
    if (sci_error != SCI_ERR_OK) { 
        SCIDisconnectSegment(*segment, UCT_PCIE_NO_FLAGS, &sci_error);
        ucs_warn("SCIMapRemoteSegment failed: %s",
            SCIGetErrorString(sci_error));
        return -1;
    }

    return 0;
}

void uct_pcie_disconnect_segment(
    sci_remote_segment_t segment,
    sci_map_t segment_map)
{
    sci_error_t sci_error;
    SCIUnmapSegment(segment_map, UCT_PCIE_NO_FLAGS, &sci_error);
    if (sci_error != SCI_ERR_OK) {
        ucs_warn("SCIUnmapSegment failed: %s", SCIGetErrorString(sci_error));
    }

    SCIDisconnectSegment(segment, UCT_PCIE_NO_FLAGS, &sci_error);
    if (sci_error != SCI_ERR_OK) { 
        ucs_warn("SCIDisconnectSegment failed: %s",
            SCIGetErrorString(sci_error));
    }

}
ucs_status_t uct_pcie_desc_pool_get(
    uct_pcie_desc_pool_t *pool,
    sci_desc_t *sd_p,
    unsigned *index_p)
{
    sci_error_t sci_error;
    sci_desc_t *descs;
    unsigned   *used;
    unsigned    i;

    for (i = 0; i < pool->count; i++) {
        if (pool->used[i] < UCT_PCIE_RESOURCES_PER_DESC) {
            goto found;
        }
    }

    descs = ucs_realloc(pool->descs, (i + 1) * sizeof(*descs), "pcie descs");
    if (descs == NULL) {
        return UCS_ERR_NO_MEMORY;
    }
    pool->descs = descs;

    used = ucs_realloc(pool->used, (i + 1) * sizeof(*used), "pcie desc use");
    if (used == NULL) {
        return UCS_ERR_NO_MEMORY;
    }
    pool->used = used;

    SCIOpen(&pool->descs[i], UCT_PCIE_NO_FLAGS, &sci_error);
    if (sci_error != SCI_ERR_OK) {
        ucs_error("SCIOpen: %s", SCIGetErrorString(sci_error));
        return UCS_ERR_NO_RESOURCE;
    }
    pool->used[i] = 0;
    pool->count   = i + 1;

found:
    pool->used[i]++;
    *sd_p    = pool->descs[i];
    *index_p = i;
    return UCS_OK;
}

void uct_pcie_desc_pool_put(
    uct_pcie_desc_pool_t *pool,
    unsigned index)
{
    ucs_assert(index < pool->count);
    ucs_assert(pool->used[index] > 0);
    pool->used[index]--;
}

void uct_pcie_desc_pool_cleanup(
    uct_pcie_desc_pool_t *pool)
{
    sci_error_t sci_error;
    unsigned    i;

    for (i = 0; i < pool->count; i++) {
        if (pool->used[i] != 0) {
            ucs_warn("pcie descriptor %u still holds %u resources", i,
                     pool->used[i]);
        }
        SCIClose(pool->descs[i], UCT_PCIE_NO_FLAGS, &sci_error);
    }
    ucs_free(pool->descs);
    ucs_free(pool->used);
    pool->descs = NULL;
    pool->used  = NULL;
    pool->count = 0;
}
