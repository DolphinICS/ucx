/*
 * Copyright (C) Dolphin Interconnect Solutions AS, 2025-2026. ALL RIGHTS RESERVED.
 * See file LICENSE for terms.
 */

#ifndef UCT_PCIE_SISCI_HELPER_H
#define UCT_PCIE_SISCI_HELPER_H

#include "pcie_iface.h"


/**
 * Create, prepare and map a local SISCI segment. SISCI assigns a segment ID
 * that is unique on this node.
 *
 * @param [in]  sd             SISCI virtual device descriptor.
 * @param [in]  size           Segment size in bytes.
 * @param [out] segment_p      Created segment.
 * @param [out] segment_map_p  Local mapping of the segment.
 * @param [out] segment_id_p   Assigned segment ID.
 * @param [out] buf_p          Local VA of the mapping.
 *
 * @return UCS_OK on success, UCS_ERR_NO_RESOURCE on failure.
 */
ucs_status_t uct_pcie_segment_create(sci_desc_t sd, size_t size,
                                     sci_local_segment_t *segment_p,
                                     sci_map_t *segment_map_p,
                                     unsigned int *segment_id_p, void **buf_p);


/**
 * Unmap and remove a segment created by @ref uct_pcie_segment_create.
 */
void uct_pcie_segment_remove(sci_local_segment_t segment,
                             sci_map_t segment_map);


/**
 * Same as @ref uct_pcie_segment_create, then make the segment available for
 * remote connections.
 */
ucs_status_t uct_pcie_segment_create_available(sci_desc_t sd, size_t size,
                                               sci_local_segment_t *segment_p,
                                               sci_map_t *segment_map_p,
                                               unsigned int *segment_id_p,
                                               void **buf_p);


/**
 * Make a segment created by @ref uct_pcie_segment_create_available
 * unavailable, then unmap and remove it.
 */
void uct_pcie_segment_remove_available(sci_local_segment_t segment,
                                       sci_map_t segment_map);


/**
 * Connect to a remote SISCI segment and map size bytes of it from offset.
 * Retries the connect until it succeeds.
 *
 * @param [in]  sd             SISCI virtual device descriptor.
 * @param [in]  node_id        Remote SISCI node ID.
 * @param [in]  segment_id     Remote segment ID.
 * @param [in]  offset         Offset in the remote segment to map from.
 * @param [in]  size           Number of bytes to map.
 * @param [in]  callback       Optional remote segment callback, NULL for none.
 *                             SCI_FLAG_USE_CALLBACK is set when non-NULL.
 * @param [in]  callback_arg   Argument passed to callback.
 * @param [out] segment_p      Connected remote segment.
 * @param [out] segment_map_p  Local mapping of the remote segment.
 * @param [out] buf_p          Local VA of the mapping.
 *
 * @return UCS_OK on success, UCS_ERR_NO_RESOURCE on failure.
 */
ucs_status_t
uct_pcie_segment_connect(sci_desc_t sd, unsigned int node_id,
                         unsigned int segment_id, size_t offset, size_t size,
                         sci_cb_remote_segment_t callback, void *callback_arg,
                         sci_remote_segment_t *segment_p,
                         sci_map_t *segment_map_p, volatile void **buf_p);


/**
 * Connect to a remote SISCI segment and map all of it. The size is queried
 * with SCIGetRemoteSegmentSize, so the caller doesn't need to know it.
 * Retries the connect until it succeeds.
 *
 * @return UCS_OK on success, UCS_ERR_NO_RESOURCE on failure.
 */
ucs_status_t uct_pcie_segment_connect_full(sci_desc_t sd, unsigned int node_id,
                                           unsigned int segment_id,
                                           sci_remote_segment_t *segment_p,
                                           sci_map_t *segment_map_p,
                                           volatile void **buf_p);


/**
 * Unmap and disconnect a segment connected by @ref uct_pcie_segment_connect or
 * @ref uct_pcie_segment_connect_full.
 */
void uct_pcie_segment_disconnect(sci_remote_segment_t segment,
                                 sci_map_t segment_map);

#endif
