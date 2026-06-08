#ifndef UCT_PCIE_SISCI_HELPER_H
#define UCT_PCIE_SISCI_HELPER_H

#include "pcie_iface.h"

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
    void **buf);

/**
 * @brief Undos setup by uct_pcie_helper_create_segment.
 * 
 * @details Unmaps and removes local segment set up by
 * uct_pcie_helper_create_segment.
 * 
 * @param[in] segment 
 */
void uct_pcie_helper_remove_segment(
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
int uct_pcie_helper_create_seg_set_avail(
    sci_desc_t sd,
    sci_local_segment_t *segment,
    sci_map_t *segment_map,
    size_t segment_size,
    unsigned int *segment_id,
    void **buf);

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
    sci_map_t segment_map);

/**
 * @brief Connects to and maps remote sisci segment at a given offset and size.
 *
 * @note Tries to connect in a loop until it succeeds.
 */
int uct_pcie_connect_segment(
    sci_desc_t sd,
    size_t offset,
    size_t segment_size,
    unsigned int node_id,
    unsigned int segment_id,
    sci_remote_segment_t *segment,
    sci_map_t *segment_map,
    volatile void **buf);

/**
 * @brief Connects to a remote SISCI segment, queries its full size via
 *        SCIGetRemoteSegmentSize, and maps the entire segment from offset 0.
 *
 * Avoids the caller needing to know the segment size in advance — useful for
 * put/get where size is not carried in the 8-byte wire rkey.
 *
 * @param[in]  sd           Open SISCI virtual device descriptor
 * @param[in]  node_id      Remote SISCI node ID
 * @param[in]  segment_id   Remote SISCI segment ID
 * @param[out] segment      Connected remote segment handle
 * @param[out] segment_map  Map handle for the connected segment
 * @param[out] buf          Local VA of the mapped remote segment window
 * @return 0 on success, -1 on failure
 */
int uct_pcie_connect_segment_full(
    sci_desc_t sd,
    unsigned int node_id,
    unsigned int segment_id,
    sci_remote_segment_t *segment,
    sci_map_t *segment_map,
    volatile void **buf);

/**
 * @brief Unmaps and disconnects from remote segment segment.
 *        (Reverses what is done in uct_pcie_connect_segment)
 * @param[in] segment 
 * @param[in] segment_map 
 */
void uct_pcie_disconnect_segment(
    sci_remote_segment_t segment,
    sci_map_t segment_map);

#endif
