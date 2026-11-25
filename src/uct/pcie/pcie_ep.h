/*
 * Copyright (C) Dolphin Interconnect Solutions AS, 2025-2026. ALL RIGHTS RESERVED.
 * See file LICENSE for terms.
 */

#ifndef UCT_PCIE_EP_H
#define UCT_PCIE_EP_H

#include "pcie_iface.h"

#include <uct/base/uct_iface.h>


/* How far a connecting EP is, see uct_pcie_ep_progress_connect */
typedef enum {
    UCT_PCIE_EP_CONN_QUEUED,  /* Behind other EPs in connecting_eps */
    UCT_PCIE_EP_CONN_RMA,     /* Trying to map the peer's RMA segment */
    UCT_PCIE_EP_CONN_SENDING, /* RMA segment mapped, trying to send the
                               * connection request */
    UCT_PCIE_EP_CONN_WAITING  /* Request in the peer's inbox, so the peer may
                               * write the ctl slot. Kept after the answer. */
} uct_pcie_ep_conn_step_t;

typedef struct {
    uct_base_ep_t        super;
    unsigned int         remote_seg_id;
    sci_remote_segment_t remote_segment;
    sci_map_t            remote_seg_map;
    volatile uint8_t     *remote_seg_buf;
    unsigned int         ep_conn_index;
    unsigned int         remote_node_id;
    uint64_t             ep_conn_seq_num;

    /* Queue of pending send requests for this endpoint. Requests are added here
     * when the flow-control window is full and drained by iface progress. */
    ucs_arbiter_group_t  pending_q;

    /* UCS_INPROGRESS until rma_seg is mapped, the peer has answered the
     * connection request and remote_segment is mapped, then UCS_OK, or the
     * error that failed it. Every send, put and get needs it. */
    ucs_status_t         conn_status;
    /* Peer iface's connection interrupt and inbox, from
     * uct_pcie_iface_addr_t */
    unsigned int         remote_interrupt_no;
    unsigned int         remote_inbox_seg_id;
    /* Handshake step while conn_status is UCS_INPROGRESS */
    uct_pcie_ep_conn_step_t conn_step;
    /* When to give up on the handshake, set when sending starts */
    ucs_time_t           conn_deadline;
    /* In the iface's connecting_eps while conn_status is UCS_INPROGRESS */
    ucs_list_link_t      conn_list;

    /* Set to 1 at EP creation, cleared to 0 by the SISCI disconnect callback
     * on remote_segment. Read by ep_is_connected. volatile because the
     * callback runs on the SISCI callback thread. */
    volatile int         is_connected;

    /* Peer's RMA segment, target of put and get. Mapped from progress, see
     * uct_pcie_ep_progress_connect. */
    unsigned int         remote_rma_seg_id;
    sci_remote_segment_t rma_seg;
    sci_map_t            rma_seg_map;
    /* Local mapping of the peer's RMA segment; every access crosses PCIe */
    void                 *remote_rma_va;
    /* Peer's rma_base from uct_pcie_iface_addr_t, used to turn remote_addr
     * into a segment offset */
    uint64_t             remote_rma_base;
} uct_pcie_ep_t;


UCS_CLASS_DECLARE_NEW_FUNC(uct_pcie_ep_t, uct_ep_t, const uct_ep_params_t*);
UCS_CLASS_DECLARE_DELETE_FUNC(uct_pcie_ep_t, uct_ep_t);

ucs_status_t uct_pcie_ep_progress_connect(uct_pcie_ep_t *ep);

ucs_status_t uct_pcie_ep_flush(uct_ep_h tl_ep, unsigned flags,
                               uct_completion_t *comp);

ucs_status_t
uct_pcie_ep_pending_add(uct_ep_h tl_ep, uct_pending_req_t *req, unsigned flags);

void uct_pcie_ep_pending_purge(uct_ep_h tl_ep, uct_pending_purge_callback_t cb,
                               void *arg);

ucs_status_t uct_pcie_ep_am_short(uct_ep_h tl_ep, uint8_t id, uint64_t header,
                                  const void *payload, unsigned length);

ssize_t uct_pcie_ep_am_bcopy(uct_ep_h tl_ep, uint8_t id,
                             uct_pack_callback_t pack_cb, void *arg,
                             unsigned flags);

ucs_status_t uct_pcie_ep_am_zcopy(uct_ep_h ep, uint8_t id, const void *header,
                                  unsigned header_length, const uct_iov_t *iov,
                                  size_t iovcnt, unsigned flags,
                                  uct_completion_t *comp);

ucs_status_t uct_pcie_ep_put_short(uct_ep_h tl_ep, const void *buffer,
                                   unsigned length, uint64_t remote_addr,
                                   uct_rkey_t rkey);

ucs_status_t uct_pcie_ep_am_short_iov(uct_ep_h tl_ep, uint8_t id,
                                      const uct_iov_t *iov, size_t iovcnt);

ssize_t uct_pcie_ep_put_bcopy(uct_ep_h ep, uct_pack_callback_t pack_cb,
                              void *arg, uint64_t remote_addr, uct_rkey_t rkey);

ucs_status_t uct_pcie_ep_put_zcopy(uct_ep_h tl_ep, const uct_iov_t *iov,
                                   size_t iovcnt, uint64_t remote_addr,
                                   uct_rkey_t rkey, uct_completion_t *comp);

ucs_status_t uct_pcie_ep_get_zcopy(uct_ep_h tl_ep, const uct_iov_t *iov,
                                   size_t iovcnt, uint64_t remote_addr,
                                   uct_rkey_t rkey, uct_completion_t *comp);

ucs_status_t uct_pcie_ep_get_short(uct_ep_h tl_ep, void *buffer,
                                   unsigned length, uint64_t remote_addr,
                                   uct_rkey_t rkey);

ucs_status_t uct_pcie_ep_get_bcopy(uct_ep_h ep, uct_unpack_callback_t unpack_cb,
                                   void *arg, size_t length,
                                   uint64_t remote_addr, uct_rkey_t rkey,
                                   uct_completion_t *comp);

#endif
