#ifndef UCT_PCIE_EP_H
#define UCT_PCIE_EP_H

#include <stdio.h>

#include <uct/base/uct_iface.h>
#include "pcie_iface.h"



typedef struct {
    uct_pcie_device_addr_t device_addr;
    uct_pcie_iface_addr_t iface_addr;
}  UCS_S_PACKED uct_pcie_ep_addr_t;


typedef struct {
    uct_base_ep_t           super;
    unsigned int            remote_seg_id;
    sci_remote_segment_t    remote_segment;
    sci_map_t               remote_seg_map;
    volatile uint8_t*       remote_seg_buf;
    /* ep_conn_offset should be ep_conn_index * (iface->packet_size_bytes * iface->packet_queue_len) */
    unsigned int            ep_conn_offset;
    unsigned int            ep_conn_index;
    unsigned int            remote_node_id;
    uint64_t                ep_conn_seq_num;

    /* Queue of pending send requests for this endpoint. Requests are added here
     * when the flow-control window is full and drained by iface progress. */
    ucs_arbiter_group_t     pending_q;

    /* Connection to the remote iface's shared segment, established at EP
     * creation time using rseg_id and rseg_base_va from iface_addr.
     * All put_short / put_bcopy operations write into this segment. */
    sci_remote_segment_t    remote_rseg;
    sci_map_t               remote_rseg_map;
    void                   *remote_rseg_buf;  /* local VA of remote shared window */
    uint64_t                remote_rseg_base_va; /* remote VA of shared segment start */
} uct_pcie_ep_t;


UCS_CLASS_DECLARE_NEW_FUNC(uct_pcie_ep_t, uct_ep_t, const uct_ep_params_t *);
UCS_CLASS_DECLARE_DELETE_FUNC(uct_pcie_ep_t, uct_ep_t);

ucs_status_t uct_pcie_ep_pending_add(
    uct_ep_h tl_ep,
    uct_pending_req_t *req,
    unsigned flags);

void uct_pcie_ep_pending_purge(
    uct_ep_h tl_ep,
    uct_pending_purge_callback_t cb,
    void *arg);

ucs_status_t uct_pcie_ep_am_short(
    uct_ep_h tl_ep,
    uint8_t id,
    uint64_t header,
    const void *payload,
    unsigned length);

ssize_t uct_pcie_ep_am_bcopy(
    uct_ep_h tl_ep,
    uint8_t id,
    uct_pack_callback_t pack_cb,
    void *arg,
    unsigned flags);

ucs_status_t uct_pcie_ep_am_zcopy(
    uct_ep_h ep,
    uint8_t id,
    const void *header,
    unsigned header_length,
    const uct_iov_t *iov,
    size_t iovcnt,
    unsigned flags,
    uct_completion_t *comp);                                  

ucs_status_t uct_pcie_ep_put_short(
    uct_ep_h tl_ep,
    const void *buffer,
    unsigned length,
    uint64_t remote_addr,
    uct_rkey_t rkey);

ucs_status_t uct_pcie_ep_am_short_iov(
    uct_ep_h tl_ep,
    uint8_t id,
    const uct_iov_t *iov,
    size_t iovcnt);

ssize_t uct_pcie_ep_put_bcopy(
    uct_ep_h ep,
    uct_pack_callback_t pack_cb,
    void *arg,
    uint64_t remote_addr,
    uct_rkey_t rkey);

ucs_status_t uct_pcie_ep_get_bcopy(
    uct_ep_h ep,
    uct_unpack_callback_t unpack_cb,
    void *arg,
    size_t length,
    uint64_t remote_addr,
    uct_rkey_t rkey,
    uct_completion_t *comp);

ucs_status_t uct_pcie_ep_atomic_cswap64(
    uct_ep_h tl_ep,
    uint64_t compare,
    uint64_t swap,
    uint64_t remote_addr,
    uct_rkey_t rkey,
    uint64_t *result,
    uct_completion_t *comp);

ucs_status_t uct_pcie_ep_atomic_cswap32(
    uct_ep_h tl_ep,
    uint32_t compare,
    uint32_t swap,
    uint64_t remote_addr,
    uct_rkey_t rkey,
    uint32_t *result,
    uct_completion_t *comp);

ucs_status_t uct_pcie_ep_atomic64_post(
    uct_ep_h ep,
    unsigned opcode,
    uint64_t value,
    uint64_t remote_addr,
    uct_rkey_t rkey);

ucs_status_t uct_pcie_ep_atomic64_fetch(
    uct_ep_h ep,
    uct_atomic_op_t opcode,
    uint64_t value,
    uint64_t *result,
    uint64_t remote_addr,
    uct_rkey_t rkey,
    uct_completion_t *comp);

ucs_status_t uct_pcie_ep_atomic32_post(
    uct_ep_h ep,
    unsigned opcode,
    uint32_t value,
    uint64_t remote_addr,
    uct_rkey_t rkey);

ucs_status_t uct_pcie_ep_atomic32_fetch(
    uct_ep_h ep,
    uct_atomic_op_t opcode,
    uint32_t value,
    uint32_t *result,
    uint64_t remote_addr,
    uct_rkey_t rkey,
    uct_completion_t *comp);

#endif