#include "sisci_helper_funcs.h"

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
ucs_status_t uct_sci_helper_create_segment(
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
        UCT_SCI_NO_CALLBACK,
        NULL, /* callbackArg == NULL */
        SCI_FLAG_AUTO_ID,
        &sci_error);
    if (sci_error != SCI_ERR_OK) { 
            printf("SCI_CREATE_RECV_SEGMENT: %s\n", SCIGetErrorString(sci_error));
            return UCS_ERR_NO_RESOURCE;
    }

    SCIPrepareSegment(*segment, UCT_SCI_LOCAL_ADAPTER_NO, UCT_SCI_NO_FLAGS, &sci_error);
    if (sci_error != SCI_ERR_OK) { 
        printf("SCI_PREPARE_SEGMENT: %s\n", SCIGetErrorString(sci_error));
        SCIRemoveSegment(*segment, UCT_SCI_NO_FLAGS , &sci_error);
        return UCS_ERR_NO_RESOURCE;
    }

    *buf = SCIMapLocalSegment(
        *segment,
        segment_map,
        0, /* Mapping offset == 0 */
        segment_size,
        NULL, /* No suggested virtual address */
        UCT_SCI_NO_FLAGS,
        &sci_error);
    if (sci_error != SCI_ERR_OK) { 
        printf("SCI_MAP_LOCAL_SEG: %s\n", SCIGetErrorString(sci_error));
        SCIRemoveSegment(*segment, UCT_SCI_NO_FLAGS , &sci_error);
        return UCS_ERR_NO_RESOURCE;
    }

    *segment_id = SCIGetLocalSegmentId(*segment);

    return UCS_OK;
}

/**
 * @brief Undos setup by uct_sci_helper_create_segment.
 * 
 * @details Unmaps and removes local segment set up by uct_sci_helper_create_segment.
 * 
 * @param[in] segment 
 * @param[in] segment_map 
 */
void uct_sci_helper_remove_segment(
    sci_local_segment_t segment,
    sci_map_t segment_map)
{
    sci_error_t sci_error;

    SCIUnmapSegment(segment_map, 0, &sci_error);
    if (sci_error != SCI_ERR_OK) {
        ucs_warn("Failed to unmap segment\n");
    }

    SCIRemoveSegment(segment, SCI_FLAG_FORCE_REMOVE , &sci_error);
    if (sci_error != SCI_ERR_OK) {
        ucs_warn("Failed to remove segment\n");
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
ucs_status_t uct_sci_helper_create_seg_set_avail(
    sci_desc_t sd,
    sci_local_segment_t *segment,
    sci_map_t *segment_map,
    size_t segment_size,
    unsigned int *segment_id,
    void **buf)
{
    ucs_status_t ret;
    sci_error_t sci_error;
    
    ret = uct_sci_helper_create_segment(sd, segment, segment_map, segment_size, segment_id, buf);
    if (ret == UCS_OK) {
        SCISetSegmentAvailable(*segment, 0, 0, &sci_error);
        if (sci_error != SCI_ERR_OK) { 
            ucs_error("Failed to set segment as available");
            uct_sci_helper_remove_segment(*segment, *segment_map);
            return UCS_ERR_NO_RESOURCE;
        }
    }

    return UCS_OK;
}

/**
 * @brief Unmaps, removes local segment (after first setting it to unavailable)
 *        set up by uct_sci_helper_create_segment.
 *        (Undos setup by uct_sci_helper_create_segment)
 *
 * @details Unmaps, removes local segment set up by uct_sci_helper_create_segment.
 *          (after first setting it to unavailable)
 * 
 * @param[in] segment 
 * @param[in] segment_map 
 */
void uct_sci_helper_remove_seg_set_unavail(
    sci_local_segment_t segment,
    sci_map_t segment_map)
{
    sci_error_t sci_error;
    SCISetSegmentUnavailable(segment, 0, UCT_SCI_NO_FLAGS, &sci_error);
    if (sci_error != SCI_ERR_OK) {
        ucs_warn("Failed to set segment unavailable\n");
    }
    uct_sci_helper_remove_segment(segment, segment_map);
}
