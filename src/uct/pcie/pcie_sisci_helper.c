#include "pcie_sisci_helper.h"

/* To get ucs print outs */
#include <ucs/type/status.h>

/**
 * @brief Creates, prepares and maps a local sisci segment. Gets sisci to
 *        automatically assign a segment ID that is unique to this node.
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
    sci_error_t sci_error;

    SCICreateSegment(
        sd,
        segment,
        0,  /* segment_id, but by specifying SCI_FLAG_AUTO_ID
             * we are asking sisci to give us an available one from [0,128] */
        segment_size,
        UCT_PCIE_NO_CALLBACK,
        NULL, /* callbackArg == NULL */
        SCI_FLAG_AUTO_ID,
        &sci_error);
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
        SCIRemoveSegment(*segment, UCT_PCIE_NO_FLAGS , &sci_error);
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

    SCIUnmapSegment(segment_map, 0, &sci_error);
    if (sci_error != SCI_ERR_OK) {
        ucs_warn("SCIUnmapSegment failed: %s\n", SCIGetErrorString(sci_error));
    }

    SCIRemoveSegment(segment, SCI_FLAG_FORCE_REMOVE , &sci_error);
    if (sci_error != SCI_ERR_OK) {
        ucs_warn("SCIRemoveSegment failed: %s\n", SCIGetErrorString(sci_error));
    }
}

/**
 * @brief Creates, prepares and maps a local sisci segment, then sets it
 *        available for remote connections. Gets sisci to automatically assign
 *        a segment ID that is unique to this node.
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
        SCISetSegmentAvailable(*segment, 0, 0, &sci_error);
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
    SCISetSegmentUnavailable(segment, 0, UCT_PCIE_NO_FLAGS, &sci_error);
    if (sci_error != SCI_ERR_OK) {
        ucs_warn("SCISetSegmentUnavailable failed: %s",
            SCIGetErrorString(sci_error));
    }
    uct_pcie_helper_remove_segment(segment, segment_map);
}

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
    do {
        SCIConnectSegment(sd,
            segment,
            node_id,
            segment_id,
            UCT_PCIE_LOCAL_ADAPTER_NO,
            callback,
            callback_arg,
            0,
            flags,
            &sci_error);
    } while (sci_error != SCI_ERR_OK);

    /* Todo: maybe not discard volatile property? */
    *buf = SCIMapRemoteSegment(
        *segment,
        segment_map,
        offset,
        segment_size,
        NULL,
        0,
        &sci_error);
    if (sci_error != SCI_ERR_OK) { 
        SCIDisconnectSegment(*segment, 0, &sci_error);
        ucs_warn("SCIMapRemoteSegment failed: %s",
            SCIGetErrorString(sci_error));
        return -1;
    }

    return 0;
}

int uct_pcie_connect_segment_full(
    sci_desc_t sd,
    unsigned int node_id,
    unsigned int segment_id,
    sci_remote_segment_t *segment,
    sci_map_t *segment_map,
    volatile void **buf)
{
    sci_error_t sci_error;
    size_t seg_size;

    do {
        SCIConnectSegment(sd, segment, node_id, segment_id,
                          UCT_PCIE_LOCAL_ADAPTER_NO,
                          UCT_PCIE_NO_CALLBACK, NULL, 0,
                          UCT_PCIE_NO_FLAGS, &sci_error);
    } while (sci_error != SCI_ERR_OK);

    seg_size = SCIGetRemoteSegmentSize(*segment);

    *buf = SCIMapRemoteSegment(*segment, segment_map, 0, seg_size,
                               NULL, 0, &sci_error);
    if (sci_error != SCI_ERR_OK) {
        ucs_error("SCIMapRemoteSegment: %s", SCIGetErrorString(sci_error));
        SCIDisconnectSegment(*segment, UCT_PCIE_NO_FLAGS, &sci_error);
        return -1;
    }

    return 0;
}

void uct_pcie_disconnect_segment(
    sci_remote_segment_t segment,
    sci_map_t segment_map)
{
    sci_error_t sci_error;
    SCIUnmapSegment(segment_map, 0, &sci_error);
    if (sci_error != SCI_ERR_OK) { 
        ucs_warn("SCIUnmapSegment failed: %s", SCIGetErrorString(sci_error));
    }
    
    SCIDisconnectSegment(segment, 0, &sci_error);
    if (sci_error != SCI_ERR_OK) { 
        ucs_warn("SCIDisconnectSegment failed: %s",
            SCIGetErrorString(sci_error));
    }

}