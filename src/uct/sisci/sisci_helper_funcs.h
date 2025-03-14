#ifndef UCT_SISCI_HELPER_FUNCS_H
#define UCT_SISCI_HELPER_FUNCS_H

#include <ucs/type/status.h>

#include "sisci.h"

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
    void **buf);

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
    sci_map_t segment_map);

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
    void **buf);

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
    sci_map_t segment_map);


#endif
