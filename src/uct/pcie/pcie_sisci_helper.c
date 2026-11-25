/*
 * Copyright (C) Dolphin Interconnect Solutions AS, 2025-2026. ALL RIGHTS RESERVED.
 * See file LICENSE for terms.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pcie_sisci_helper.h"

#include <ucs/debug/log.h>

/* Create, prepare and map a local segment with SCICreateSegment's segment_id
 * and flags. UCS_ERR_BUSY, not logged, if segment_id is taken. */
static ucs_status_t
uct_pcie_segment_create_common(sci_desc_t sd, size_t size,
                               unsigned int segment_id, unsigned int flags,
                               sci_cb_local_segment_t callback,
                               void *callback_arg,
                               sci_local_segment_t *segment_p,
                               sci_map_t *segment_map_p,
                               unsigned int *segment_id_p, void **buf_p)
{
    sci_local_segment_t segment;
    sci_map_t segment_map;
    sci_error_t sci_error;
    void *buf;

    if (callback != NULL) {
        flags |= SCI_FLAG_USE_CALLBACK;
    }

    SCICreateSegment(sd, &segment, segment_id, size, callback, callback_arg,
                     flags, &sci_error);
    if (sci_error == SCI_ERR_SEGMENTID_USED) {
        return UCS_ERR_BUSY;
    } else if (sci_error != SCI_ERR_OK) {
        ucs_error("SCICreateSegment failed: %s", SCIGetErrorString(sci_error));
        return UCS_ERR_NO_RESOURCE;
    }

    SCIPrepareSegment(segment, UCT_PCIE_LOCAL_ADAPTER_NO, UCT_PCIE_NO_FLAGS,
                      &sci_error);
    if (sci_error != SCI_ERR_OK) {
        ucs_error("SCIPrepareSegment failed: %s", SCIGetErrorString(sci_error));
        goto err_remove_segment;
    }

    buf = SCIMapLocalSegment(segment, &segment_map, 0, size, NULL,
                             UCT_PCIE_NO_FLAGS, &sci_error);
    if (sci_error != SCI_ERR_OK) {
        ucs_error("SCIMapLocalSegment failed: %s",
                  SCIGetErrorString(sci_error));
        goto err_remove_segment;
    }

    *segment_p     = segment;
    *segment_map_p = segment_map;
    *segment_id_p  = SCIGetLocalSegmentId(segment);
    *buf_p         = buf;

    return UCS_OK;

err_remove_segment:
    SCIRemoveSegment(segment, UCT_PCIE_NO_FLAGS, &sci_error);
    return UCS_ERR_NO_RESOURCE;
}

/* With SCI_FLAG_AUTO_ID, SISCI picks a free ID from [1024,1152] */
static ucs_status_t
uct_pcie_segment_create_auto_id(sci_desc_t sd, size_t size,
                                sci_cb_local_segment_t callback,
                                void *callback_arg,
                                sci_local_segment_t *segment_p,
                                sci_map_t *segment_map_p,
                                unsigned int *segment_id_p, void **buf_p)
{
    ucs_status_t status;

    status = uct_pcie_segment_create_common(sd, size,
                                            UCT_PCIE_AUTO_SEG_ID_START,
                                            SCI_FLAG_AUTO_ID, callback,
                                            callback_arg, segment_p,
                                            segment_map_p, segment_id_p,
                                            buf_p);
    if (status == UCS_ERR_BUSY) {
        ucs_error("SCICreateSegment found no free segment ID from %u",
                  UCT_PCIE_AUTO_SEG_ID_START);
        return UCS_ERR_NO_RESOURCE;
    }

    return status;
}

ucs_status_t uct_pcie_segment_create(sci_desc_t sd, size_t size,
                                     sci_local_segment_t *segment_p,
                                     sci_map_t *segment_map_p,
                                     unsigned int *segment_id_p, void **buf_p)
{
    return uct_pcie_segment_create_auto_id(sd, size, NULL, NULL, segment_p,
                                           segment_map_p, segment_id_p, buf_p);
}

void uct_pcie_segment_remove(sci_local_segment_t segment, sci_map_t segment_map)
{
    sci_error_t sci_error;

    SCIUnmapSegment(segment_map, UCT_PCIE_NO_FLAGS, &sci_error);
    if (sci_error != SCI_ERR_OK) {
        ucs_warn("SCIUnmapSegment failed: %s", SCIGetErrorString(sci_error));
    }

    SCIRemoveSegment(segment, UCT_PCIE_NO_FLAGS, &sci_error);
    if (sci_error != SCI_ERR_OK) {
        ucs_warn("SCIRemoveSegment failed: %s", SCIGetErrorString(sci_error));
    }
}

static ucs_status_t uct_pcie_segment_set_available(sci_local_segment_t segment,
                                                   sci_map_t segment_map)
{
    sci_error_t sci_error;

    SCISetSegmentAvailable(segment, UCT_PCIE_LOCAL_ADAPTER_NO,
                           UCT_PCIE_NO_FLAGS, &sci_error);
    if (sci_error != SCI_ERR_OK) {
        ucs_error("SCISetSegmentAvailable failed: %s",
                  SCIGetErrorString(sci_error));
        uct_pcie_segment_remove(segment, segment_map);
        return UCS_ERR_NO_RESOURCE;
    }

    return UCS_OK;
}

ucs_status_t
uct_pcie_segment_create_available(sci_desc_t sd, size_t size,
                                  sci_cb_local_segment_t callback,
                                  void *callback_arg,
                                  sci_local_segment_t *segment_p,
                                  sci_map_t *segment_map_p,
                                  unsigned int *segment_id_p, void **buf_p)
{
    ucs_status_t status;

    status = uct_pcie_segment_create_auto_id(sd, size, callback, callback_arg,
                                             segment_p, segment_map_p,
                                             segment_id_p, buf_p);
    if (status != UCS_OK) {
        return status;
    }

    return uct_pcie_segment_set_available(*segment_p, *segment_map_p);
}

ucs_status_t
uct_pcie_segment_create_available_id(sci_desc_t sd, size_t size,
                                     unsigned int segment_id,
                                     sci_local_segment_t *segment_p,
                                     sci_map_t *segment_map_p, void **buf_p)
{
    unsigned int created_id;
    ucs_status_t status;

    status = uct_pcie_segment_create_common(sd, size, segment_id,
                                            UCT_PCIE_NO_FLAGS, NULL, NULL,
                                            segment_p, segment_map_p,
                                            &created_id, buf_p);
    if (status != UCS_OK) {
        return status;
    }

    return uct_pcie_segment_set_available(*segment_p, *segment_map_p);
}

void uct_pcie_segment_remove_available(sci_local_segment_t segment,
                                       sci_map_t segment_map)
{
    sci_error_t sci_error;

    SCISetSegmentUnavailable(segment, UCT_PCIE_LOCAL_ADAPTER_NO,
                             UCT_PCIE_NO_FLAGS, &sci_error);
    if (sci_error != SCI_ERR_OK) {
        ucs_warn("SCISetSegmentUnavailable failed: %s",
                 SCIGetErrorString(sci_error));
    }

    uct_pcie_segment_remove(segment, segment_map);
}

ucs_status_t
uct_pcie_segment_try_connect(sci_desc_t sd, unsigned int node_id,
                             unsigned int segment_id, size_t offset,
                             size_t size, sci_cb_remote_segment_t callback,
                             void *callback_arg,
                             sci_remote_segment_t *segment_p,
                             sci_map_t *segment_map_p, volatile void **buf_p,
                             sci_error_t *connect_error)
{
    unsigned int flags = (callback != NULL) ? SCI_FLAG_USE_CALLBACK :
                                              UCT_PCIE_NO_FLAGS;
    sci_remote_segment_t segment;
    sci_map_t segment_map;
    sci_error_t sci_error;
    volatile void *buf;

    SCIConnectSegment(sd, &segment, node_id, segment_id,
                      UCT_PCIE_LOCAL_ADAPTER_NO, callback, callback_arg, 0,
                      flags, &sci_error);
    if (sci_error != SCI_ERR_OK) {
        *connect_error = sci_error;
        return UCS_ERR_BUSY;
    }

    /* TODO: maybe keep the volatile qualifier? */
    buf = SCIMapRemoteSegment(segment, &segment_map, offset, size, NULL,
                              UCT_PCIE_NO_FLAGS, &sci_error);
    if (sci_error != SCI_ERR_OK) {
        ucs_warn("SCIMapRemoteSegment failed: %s",
                 SCIGetErrorString(sci_error));
        goto err_disconnect_segment;
    }

    *segment_p     = segment;
    *segment_map_p = segment_map;
    *buf_p         = buf;

    return UCS_OK;

err_disconnect_segment:
    SCIDisconnectSegment(segment, UCT_PCIE_NO_FLAGS, &sci_error);
    return UCS_ERR_NO_RESOURCE;
}

ucs_status_t
uct_pcie_segment_connect(sci_desc_t sd, unsigned int node_id,
                         unsigned int segment_id, size_t offset, size_t size,
                         sci_cb_remote_segment_t callback, void *callback_arg,
                         sci_remote_segment_t *segment_p,
                         sci_map_t *segment_map_p, volatile void **buf_p)
{
    ucs_time_t deadline;
    sci_error_t sci_error;
    ucs_status_t status;

    deadline = uct_pcie_connect_deadline();
    do {
        status = uct_pcie_segment_try_connect(sd, node_id, segment_id, offset,
                                              size, callback, callback_arg,
                                              segment_p, segment_map_p, buf_p,
                                              &sci_error);
    } while ((status == UCS_ERR_BUSY) && (ucs_get_time() < deadline));

    if (status == UCS_ERR_BUSY) {
        ucs_error("SCIConnectSegment failed after %d ms: %s",
                  UCT_PCIE_CONNECT_TIMEOUT_MS, SCIGetErrorString(sci_error));
        return UCS_ERR_TIMED_OUT;
    }

    return status;
}

ucs_status_t
uct_pcie_segment_try_connect_full(sci_desc_t sd, unsigned int node_id,
                                  unsigned int segment_id,
                                  sci_remote_segment_t *segment_p,
                                  sci_map_t *segment_map_p,
                                  volatile void **buf_p,
                                  sci_error_t *connect_error)
{
    sci_remote_segment_t segment;
    sci_map_t segment_map;
    sci_error_t sci_error;
    volatile void *buf;
    size_t size;

    SCIConnectSegment(sd, &segment, node_id, segment_id,
                      UCT_PCIE_LOCAL_ADAPTER_NO, NULL, NULL, 0,
                      UCT_PCIE_NO_FLAGS, &sci_error);
    if (sci_error != SCI_ERR_OK) {
        *connect_error = sci_error;
        return UCS_ERR_BUSY;
    }

    size = SCIGetRemoteSegmentSize(segment);

    buf = SCIMapRemoteSegment(segment, &segment_map, 0, size, NULL,
                              UCT_PCIE_NO_FLAGS, &sci_error);
    if (sci_error != SCI_ERR_OK) {
        ucs_error("SCIMapRemoteSegment failed: %s",
                  SCIGetErrorString(sci_error));
        goto err_disconnect_segment;
    }

    *segment_p     = segment;
    *segment_map_p = segment_map;
    *buf_p         = buf;

    return UCS_OK;

err_disconnect_segment:
    SCIDisconnectSegment(segment, UCT_PCIE_NO_FLAGS, &sci_error);
    return UCS_ERR_NO_RESOURCE;
}

void uct_pcie_segment_disconnect(sci_remote_segment_t segment,
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
