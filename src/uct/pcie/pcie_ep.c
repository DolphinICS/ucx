

#include <uct/base/uct_iov.inl>

#include "pcie_ep.h"
#include "pcie_sisci_helper.h"
#include "pcie_md.h"

/* Discard callback for use when purging the pending queue on EP teardown.
 * UCX's contract requires the user to call ep_pending_purge before destroying
 * an EP, so this should never actually fire. It is here purely as a safety net. */
static ucs_arbiter_cb_result_t
uct_pcie_ep_pending_discard_cb(ucs_arbiter_t *arbiter, ucs_arbiter_group_t *group,
                               ucs_arbiter_elem_t *elem, void *arg)
{
    ucs_warn("pcie EP destroyed with pending request still queued");
    return UCS_ARBITER_CB_RESULT_REMOVE_ELEM;
}

static UCS_CLASS_CLEANUP_FUNC(uct_pcie_ep_t)
{
    uct_pcie_iface_t *iface = ucs_derived_of(self->super.super.iface, uct_pcie_iface_t);

    /* UCX guarantees ep_pending_purge is called before ep_destroy, so the
     * group should already be empty here. Drain defensively just in case. */
    ucs_arbiter_group_purge(&iface->arbiter, &self->pending_q,
                            uct_pcie_ep_pending_discard_cb, NULL);

    uct_pcie_disconnect_segment(self->rma_seg, self->rma_seg_map);

    self->remote_seg_buf = NULL;
    uct_pcie_disconnect_segment(self->remote_segment, self->remote_seg_map);
    ucs_debug("ep deleted segment_id %d node_id %d\n",
        self->remote_seg_id,
        self->remote_node_id);
}


/**
 * @brief 
 * @param[in] iface 
 * @param[in] node_id 
 * @param[in] remote_interrupt_no 
 * @return 
 */
static ucs_status_t uct_pcie_ep_send_conn_request(
    uct_pcie_conn_req_t *request,
    unsigned int node_id,
    unsigned int remote_interrupt_no,
    sci_desc_t sci_virtual_device)
{
    sci_error_t sci_error;
    sci_remote_data_interrupt_t req_interrupt;
    int rc = UCS_OK;

    do {
        SCIConnectDataInterrupt(
            sci_virtual_device,
            &req_interrupt,
            node_id,
            UCT_PCIE_LOCAL_ADAPTER_NO,
            remote_interrupt_no,
            SCI_INFINITE_TIMEOUT,
            UCT_PCIE_NO_FLAGS,
            &sci_error);
    } while (sci_error != SCI_ERR_OK);

    SCITriggerDataInterrupt(
        req_interrupt,
        (void *) request,
        sizeof(uct_pcie_conn_req_t),
        UCT_PCIE_NO_FLAGS,
        &sci_error);
    if(sci_error != SCI_ERR_OK) {
        ucs_error("SCI Trigger Interrupt: %s", SCIGetErrorString(sci_error));
        rc = UCS_ERR_NO_RESOURCE;
    }

    /*  Clean up for connection.  */
    SCIDisconnectDataInterrupt(req_interrupt, UCT_PCIE_NO_FLAGS, &sci_error);
    if(sci_error == SCI_ERR_BUSY) {
        ucs_error("SCIDisconnectDataInterrupt: %s",
            SCIGetErrorString(sci_error));
    }

    return rc;
}

/**
 * @brief 
 * @param[in] request 
 * @param[in] node_id 
 * @param[in] remote_interrupt_no 
 * @param[in] sci_virtual_device 
 * @param[out] answer 
 */
static ucs_status_t uct_pcie_ep_send_recv_conn_request(
    uct_pcie_conn_req_t request,
    unsigned int node_id,
    unsigned int remote_interrupt_no,
    sci_desc_t sci_virtual_device,
    uct_pcie_conn_ans_t *answer)
{
    sci_error_t sci_error;
    ucs_status_t ucs_ret;
    sci_local_data_interrupt_t ans_interrupt;
    unsigned int ans_size = (unsigned int) sizeof(uct_pcie_conn_ans_t);

    /* 1.
     * Create the data interrupt we will use to receive the response of our
     * connection request. We get sisci to give us a free interrupt id.
     * We send that ID along with our connection request such that the server
     * can get back to us. */
    SCICreateDataInterrupt(
        sci_virtual_device,
        &ans_interrupt,
        UCT_PCIE_LOCAL_ADAPTER_NO,
        &request.interrupt,  
        UCT_PCIE_NO_CALLBACK,
        NULL, /* No callback arguments */
        UCT_PCIE_NO_FLAGS,
        &sci_error);
    if(sci_error != SCI_ERR_OK) {
        ucs_error("SCI Trigger Interrupt: %s", SCIGetErrorString(sci_error));
        return UCS_ERR_NO_RESOURCE;
    }
    
    /* 2.
     * Send connection request to the server. This will trigger a callback on
     * the other side, and that callback will get back to us and send us an
     * answer to our connection request. */
    ucs_ret = uct_pcie_ep_send_conn_request(
        &request,
        node_id,
        remote_interrupt_no,
        sci_virtual_device);
    if (ucs_ret != UCS_OK) {
        return ucs_ret;
    }

    /* 3. Wait for connection request answer */
    SCIWaitForDataInterrupt(
        ans_interrupt,
        (void*) answer,
        &ans_size,
        SCI_INFINITE_TIMEOUT,
        UCT_PCIE_NO_FLAGS,
        &sci_error);
    if(sci_error != SCI_ERR_OK) {
        ucs_error("SCI Wait For Interrupt: %s", SCIGetErrorString(sci_error));
        ucs_ret = UCS_ERR_NO_RESOURCE;
    }
    /* Done. Clean up data interrupt made in step 1 */
    SCIRemoveDataInterrupt(ans_interrupt, UCT_PCIE_NO_FLAGS, &sci_error);

    return ucs_ret;
}

static UCS_CLASS_INIT_FUNC(uct_pcie_ep_t, const uct_ep_params_t *params)
{
    ucs_status_t ucs_ret;
    
    uct_pcie_iface_addr_t* iface_addr =
        (uct_pcie_iface_addr_t*) params->iface_addr;

    uct_pcie_device_addr_t* dev_addr =
        (uct_pcie_device_addr_t*) params->dev_addr;
    
    uct_pcie_conn_ans_t answer;
    int ret;
    unsigned int ep_conn_index;
    uct_pcie_conn_req_t request;

    unsigned int remote_interrupt_no;
    uct_pcie_iface_t* iface = ucs_derived_of(params->iface, uct_pcie_iface_t);
    uct_pcie_md_t* md = ucs_derived_of(iface->super.md, uct_pcie_md_t);

    UCT_EP_PARAMS_CHECK_DEV_IFACE_ADDRS(params);

    remote_interrupt_no = (unsigned int) iface_addr->interrupt_no;
    self->remote_node_id = (unsigned int) dev_addr->node_id;

    ucs_debug("EP created remote_interrupt_no %d node_id %d\n",
        remote_interrupt_no,
        self->remote_node_id);

    self->super.super.iface = params->iface;
    
    UCS_CLASS_CALL_SUPER_INIT(uct_base_ep_t, &iface->super);

    /* If multiple processes can run this function simultaneously,
     * then this should be protected with a lock */
    ep_conn_index = iface->eps_init_cnt;
    iface->eps_init_cnt++;

    request.node_id = iface->device_addr; /* send local node ID */
    request.ep_conn_index = ep_conn_index;
    request.ctl_segment_id = iface->ctl_segment_id;

    ucs_ret = uct_pcie_ep_send_recv_conn_request(
        request,
        self->remote_node_id,
        remote_interrupt_no,
        md->sci_virtual_device,
        &answer);
    if (ucs_ret != UCS_OK) {
        return ucs_ret;
    }
    
    /* uct_pcie_ep_t *self */
    self->remote_seg_id = answer.segment_id;
    self->ep_conn_offset  = answer.ep_conn_offset;
    self->ep_conn_index = ep_conn_index;
    self->ep_conn_seq_num = 1;
    ucs_arbiter_group_init(&self->pending_q);

    /* Connect to the remote iface's shared segment now, at EP creation.
     * The segment is guaranteed available: md_open creates and publishes it
     * before any EP can be created, and its ID arrives in iface_addr. */
    ret = uct_pcie_connect_segment_full(
        iface->vdev_ep,
        self->remote_node_id,
        iface_addr->rma_seg_id,
        &self->rma_seg,
        &self->rma_seg_map,
        (volatile void **)&self->rma_buf_local);
    if (ret != 0) {
        ucs_error("EP: failed to connect to remote shared segment %u on node %u",
                  iface_addr->rma_seg_id, self->remote_node_id);
        iface->eps_init_cnt--;
        return UCS_ERR_NO_RESOURCE;
    }
    self->rma_local_base = iface_addr->rma_local_base; /* [LIBPERF_RKEY_QUIRK] */

    ucs_debug("EP shared: connected seg=%u node=%u rma_buf_local=%p",
              iface_addr->rma_seg_id, self->remote_node_id, self->rma_buf_local);

    ret = uct_pcie_connect_segment(
        iface->vdev_ep,
        self->ep_conn_offset,
        iface->packet_size_bytes * iface->packet_queue_len,
        self->remote_node_id,
        self->remote_seg_id,
        &self->remote_segment,
        &self->remote_seg_map,
        (volatile void**)&self->remote_seg_buf);
    if (ret != UCS_OK) {
        ucs_error("Endpoint failed to connect and map to remote sisci segment");
        uct_pcie_disconnect_segment(self->rma_seg, self->rma_seg_map);
        iface->eps_init_cnt--;
        return UCS_ERR_NO_RESOURCE;
    }

    ucs_debug("EP connected to segment %d at node %d\n",
        self->remote_seg_id,
        self->remote_node_id);

    return UCS_OK;
}


UCS_CLASS_DEFINE(uct_pcie_ep_t, uct_base_ep_t);

UCS_CLASS_DEFINE_NEW_FUNC(uct_pcie_ep_t, uct_ep_t, const uct_ep_params_t *);
UCS_CLASS_DEFINE_DELETE_FUNC(uct_pcie_ep_t, uct_ep_t);


/* // SECTION Put / Get (one-sided) */

/*
 * [UCT_REMOTE_ADDR] NOTE on the parameter named "remote_addr" in put/get functions below:
 *
 * Despite its name, remote_addr is NOT a SISCI remote address and does NOT
 * point into any SISCI-remotely-mapped memory window.  In SISCI terms, a
 * "remote address" would mean a pointer obtained via SCIMapRemoteSegment,
 * i.e something you can dereference locally to access physical memory on
 * another node. remote_addr is none of that.
 *
 * The name is the official UCT API parameter name, carried over from the
 * InfiniBand transport model where the NIC uses a remote VA directly.  Here
 * it is simply an opaque integer that encodes a segment offset.  It must be
 * converted before use:
 *
 *   seg_offset = remote_addr - ep->rma_local_base  [LIBPERF_RKEY_QUIRK]
 *   local_ptr  = ep->rma_buf_local + seg_offset
 *
 * ep->rma_buf_local is the actual local VA of the PCIe-mapped window into the
 * remote node's segment.  That is the SISCI remote address.  See
 * [LIBPERF_RKEY_QUIRK] in uct_pcie_iface_addr_t for why rma_local_base exists.
 */

ucs_status_t uct_pcie_ep_put_short(
    uct_ep_h tl_ep,
    const void *buffer,
    unsigned length,
    uint64_t remote_addr,  /* not a SISCI remote address, see [UCT_REMOTE_ADDR] */
    uct_rkey_t rkey)
{
    uct_pcie_ep_t *ep = ucs_derived_of(tl_ep, uct_pcie_ep_t);
    size_t seg_offset;

    UCT_CHECK_LENGTH(length, 0, UCT_PCIE_MAX_PUT_SHORT, "put_short");

    ucs_assert(remote_addr >= ep->rma_local_base);
    seg_offset = (size_t)(remote_addr - ep->rma_local_base); /* [LIBPERF_RKEY_QUIRK] */
    ucs_assert(seg_offset + length <= UCT_PCIE_RMA_SEG_SIZE);

    memcpy((uint8_t *)ep->rma_buf_local + seg_offset, buffer, length);
    SCIFlush(NULL, SCI_FLAG_FLUSH_CPU_BUFFERS_ONLY);
    return UCS_OK;
}

ssize_t uct_pcie_ep_put_bcopy(
    uct_ep_h tl_ep,
    uct_pack_callback_t pack_cb,
    void *arg,
    uint64_t remote_addr,  /* not a SISCI remote address, see [UCT_REMOTE_ADDR] */
    uct_rkey_t rkey)
{
    uct_pcie_ep_t *ep = ucs_derived_of(tl_ep, uct_pcie_ep_t);
    size_t seg_offset;
    ssize_t length;

    ucs_assert(remote_addr >= ep->rma_local_base);
    seg_offset = (size_t)(remote_addr - ep->rma_local_base); /* [LIBPERF_RKEY_QUIRK] */

    length = pack_cb((uint8_t *)ep->rma_buf_local + seg_offset, arg);
    ucs_assert(seg_offset + (size_t)length <= UCT_PCIE_RMA_SEG_SIZE);
    SCIFlush(NULL, SCI_FLAG_FLUSH_CPU_BUFFERS_ONLY);
    return length;
}

/* //!SECTION Put / Get (one-sided) */

ucs_status_t uct_pcie_ep_get_short(
    uct_ep_h tl_ep,
    void *buffer,
    unsigned length,
    uint64_t remote_addr,  /* not a SISCI remote address, see [UCT_REMOTE_ADDR] */
    uct_rkey_t rkey)
{
    uct_pcie_ep_t *ep = ucs_derived_of(tl_ep, uct_pcie_ep_t);
    size_t seg_offset;

    ucs_assert(remote_addr >= ep->rma_local_base);
    seg_offset = (size_t)(remote_addr - ep->rma_local_base); /* [LIBPERF_RKEY_QUIRK] */
    ucs_assert(seg_offset + length <= UCT_PCIE_RMA_SEG_SIZE);

    memcpy(buffer, (const uint8_t *)ep->rma_buf_local + seg_offset, length);
    return UCS_OK;
}

ucs_status_t uct_pcie_ep_put_zcopy(
    uct_ep_h tl_ep,
    const uct_iov_t *iov,
    size_t iovcnt,
    uint64_t remote_addr,  /* not a SISCI remote address, see [UCT_REMOTE_ADDR] */
    uct_rkey_t rkey,
    uct_completion_t *comp)
{
    uct_pcie_ep_t *ep = ucs_derived_of(tl_ep, uct_pcie_ep_t);
    ucs_iov_iter_t iov_iter;
    size_t seg_offset;
    size_t total_len;

    ucs_assert(remote_addr >= ep->rma_local_base);
    seg_offset = (size_t)(remote_addr - ep->rma_local_base); /* [LIBPERF_RKEY_QUIRK] */

    total_len = uct_iov_total_length(iov, iovcnt);
    ucs_assert(seg_offset + total_len <= UCT_PCIE_RMA_SEG_SIZE);
    ucs_iov_iter_init(&iov_iter);
    uct_iov_to_buffer(iov, iovcnt, &iov_iter,
                      (uint8_t *)ep->rma_buf_local + seg_offset, total_len);

    SCIFlush(NULL, SCI_FLAG_FLUSH_CPU_BUFFERS_ONLY);
    if (comp != NULL) {
        uct_invoke_completion(comp, UCS_OK);
    }
    return UCS_OK;
}

ucs_status_t uct_pcie_ep_get_bcopy(
    uct_ep_h tl_ep,
    uct_unpack_callback_t unpack_cb,
    void *arg,
    size_t length,
    uint64_t remote_addr,  /* not a SISCI remote address, see [UCT_REMOTE_ADDR] */
    uct_rkey_t rkey,
    uct_completion_t *comp)
{
    uct_pcie_ep_t *ep = ucs_derived_of(tl_ep, uct_pcie_ep_t);
    size_t seg_offset;

    ucs_assert(remote_addr >= ep->rma_local_base);
    seg_offset = (size_t)(remote_addr - ep->rma_local_base); /* [LIBPERF_RKEY_QUIRK] */
    ucs_assert(seg_offset + length <= UCT_PCIE_RMA_SEG_SIZE);
    unpack_cb(arg, (const uint8_t *)ep->rma_buf_local + seg_offset, length);
    return UCS_OK;
}


/*//!SECTION*/

/*
 * [NO_ATOMICS] Atomic operations are not implemented in this transport and
 * will not be implemented in the foreseeable future.
 *
 * SISCI is a pure shared-memory-over-PCIe API: it provides segment creation,
 * remote mapping, and DMA transfer primitives. It exposes no facility for
 * atomic read-modify-write operations across the PCIe fabric.
 *
 * This transport therefore does not advertise any UCT_IFACE_FLAG_ATOMIC_*
 * capabilities. UCX's upper layers (UCP) detect the absence of those flags and
 * fall back to their own software emulation: atomic operations are carried out
 * via active messages routed through whichever transport supports AM, with the
 * target side performing the operation locally using CPU atomics and returning
 * the result. That path is correct, well-tested, and not meaningfully slower
 * than anything we could build here. The stubs below exist only to satisfy the
 * iface_ops vtable; they are never called in practice.
 */

ucs_status_t uct_pcie_ep_atomic32_post(
    uct_ep_h ep,
    unsigned opcode,
    uint32_t value,
    uint64_t remote_addr,
    uct_rkey_t rkey)
{
    return UCS_ERR_NOT_IMPLEMENTED; /* see [NO_ATOMICS] */
}

ucs_status_t uct_pcie_ep_atomic64_post(
    uct_ep_h ep,
    unsigned opcode,
    uint64_t value,
    uint64_t remote_addr,
    uct_rkey_t rkey)
{
    return UCS_ERR_NOT_IMPLEMENTED; /* see [NO_ATOMICS] */
}

ucs_status_t uct_pcie_ep_atomic64_fetch(
    uct_ep_h ep,
    uct_atomic_op_t opcode,
    uint64_t value,
    uint64_t *result,
    uint64_t remote_addr,
    uct_rkey_t rkey,
    uct_completion_t *comp)
{
    return UCS_ERR_NOT_IMPLEMENTED; /* see [NO_ATOMICS] */
}

ucs_status_t uct_pcie_ep_atomic32_fetch(
    uct_ep_h ep,
    uct_atomic_op_t opcode,
    uint32_t value,
    uint32_t *result,
    uint64_t remote_addr,
    uct_rkey_t rkey,
    uct_completion_t *comp)
{
    return UCS_ERR_NOT_IMPLEMENTED; /* see [NO_ATOMICS] */
}

ucs_status_t uct_pcie_ep_atomic_cswap64(
    uct_ep_h tl_ep,
    uint64_t compare,
    uint64_t swap,
    uint64_t remote_addr,
    uct_rkey_t rkey,
    uint64_t *result,
    uct_completion_t *comp)
{
    return UCS_ERR_NOT_IMPLEMENTED; /* see [NO_ATOMICS] */
}

ucs_status_t uct_pcie_ep_atomic_cswap32(
    uct_ep_h tl_ep,
    uint32_t compare,
    uint32_t swap,
    uint64_t remote_addr,
    uct_rkey_t rkey,
    uint32_t *result,
    uct_completion_t *comp)
{
    return UCS_ERR_NOT_IMPLEMENTED; /* see [NO_ATOMICS] */
}

/* //!SECTION */

/*  // SECTION Active messages */

/**
 * @brief Send a short active message.
 *
 * Packet layout in the remote segment slot:
 *   [ uct_pcie_am_hdr_t | header (uint64_t) | payload ]
 *
 * The header is a fixed-size uint64_t, unlike am_zcopy which takes a
 * variable-length header.
 *
 * @param[inout] tl_ep   Endpoint to send on
 * @param[in]    id      Active message handler ID on the remote side
 * @param[in]    header  Fixed 8-byte user header
 * @param[in]    payload Payload buffer
 * @param[in]    length  Length of payload in bytes
 * @return UCS_OK              on success
 * @return UCS_ERR_NO_RESOURCE if the send queue is full
 */
ucs_status_t uct_pcie_ep_am_short(
    uct_ep_h tl_ep,
    uint8_t id,
    uint64_t header,
    const void *payload,
    unsigned length)
{
    uct_pcie_ep_t     *ep       = ucs_derived_of(tl_ep, uct_pcie_ep_t);
    uct_pcie_iface_t  *iface    = ucs_derived_of(tl_ep->iface, uct_pcie_iface_t);
    uct_pcie_ctl_t    *ctl      = &iface->ctls[ep->ep_conn_index];
    uct_pcie_am_hdr_t *am_hdr;
    uint32_t           slot_offset;
    uint32_t           payload_offset;

    /* Check that the send queue has room for one more packet */
    if (ep->ep_conn_seq_num - ctl->ep_conn_ack > iface->packet_queue_len) {
        return UCS_ERR_NO_RESOURCE;
    }



    UCT_CHECK_LENGTH(sizeof(uct_pcie_am_hdr_t) + sizeof(uint64_t) + length,
                 0, iface->packet_size_bytes, "am_short");
    UCT_CHECK_AM_ID(id);

    /* Locate the next available slot in the remote segment ring buffer.
     * The ring index wraps using modulo, and each slot is packet_size_bytes wide. */
    slot_offset    = iface->packet_size_bytes *
                     (ep->ep_conn_seq_num % iface->packet_queue_len);
    payload_offset = slot_offset + sizeof(uct_pcie_am_hdr_t);

    /* Copy the fixed uint64_t header and payload into the slot */
    uct_am_short_fill_data(
        (void *)&ep->remote_seg_buf[payload_offset],
        header,
        payload,
        length,
        UCS_ARCH_MEMCPY_NT_DEST);

    /* Write the transport AM header and mark the slot as posted */
    am_hdr                    = (uct_pcie_am_hdr_t *) &ep->remote_seg_buf[slot_offset];
    am_hdr->am_id             = id;
    am_hdr->am_length         = length + sizeof(header);
    SCIFlush(NULL, SCI_FLAG_FLUSH_CPU_BUFFERS_ONLY);
    am_hdr->am_message_posted = 1;
    SCIFlush(NULL, SCI_FLAG_FLUSH_CPU_BUFFERS_ONLY);

    ep->ep_conn_seq_num++;

    return UCS_OK;
}

/**
 * @brief Send a short active message from a scatter-gather list.
 *
 * Variant of am_short that takes an iov array instead of a flat
 * header+payload. The iov buffers are gathered into a contiguous
 * region in the remote segment slot.
 *
 * Packet layout in the remote segment slot:
 *   [ uct_pcie_am_hdr_t | iov[0] | iov[1] | ... ]
 *
 * @param[inout] tl_ep  Endpoint to send on
 * @param[in]    id     Active message handler ID on the remote side
 * @param[in]    iov    Scatter-gather buffers, gathered in array order
 * @param[in]    iovcnt Number of entries in iov
 * @return UCS_OK              on success
 * @return UCS_ERR_NO_RESOURCE if the send queue is full
 */
ucs_status_t uct_pcie_ep_am_short_iov(
    uct_ep_h tl_ep,
    uint8_t id,
    const uct_iov_t *iov,
    size_t iovcnt)
{
    uct_pcie_ep_t     *ep      = ucs_derived_of(tl_ep, uct_pcie_ep_t);
    uct_pcie_iface_t  *iface   = ucs_derived_of(tl_ep->iface, uct_pcie_iface_t);
    uct_pcie_ctl_t    *ctl     = &iface->ctls[ep->ep_conn_index];
    uct_pcie_am_hdr_t *am_hdr;
    ucs_iov_iter_t     iov_iter;
    uint32_t           slot_offset;
    uint32_t           payload_offset;
    size_t             iov_total_len;
    size_t             bytes_copied;

    /* Check that the send queue has room for one more packet */
    if (ep->ep_conn_seq_num - ctl->ep_conn_ack > iface->packet_queue_len) {
        return UCS_ERR_NO_RESOURCE;
    }

    iov_total_len = uct_iov_total_length(iov, iovcnt);

    UCT_CHECK_LENGTH(iov_total_len + sizeof(uct_pcie_am_hdr_t),
                     0, iface->packet_size_bytes, "am_short_iov");
    UCT_CHECK_AM_ID(id);

    /* Locate the next available slot in the remote segment ring buffer.
     * The ring index wraps using modulo, and each slot is packet_size_bytes wide. */
    slot_offset    = iface->packet_size_bytes *
                     (ep->ep_conn_seq_num % iface->packet_queue_len);
    payload_offset = slot_offset + sizeof(uct_pcie_am_hdr_t);

    /* Gather-copy each iov buffer into the contiguous payload region of the slot */
    ucs_iov_iter_init(&iov_iter);
    bytes_copied = uct_iov_to_buffer(iov, iovcnt, &iov_iter,
                                     (void *)&ep->remote_seg_buf[payload_offset],
                                     iov_total_len);
    ucs_assert(bytes_copied == iov_total_len);

    /* Write the transport AM header and mark the slot as posted */
    am_hdr                    = (uct_pcie_am_hdr_t *) &ep->remote_seg_buf[slot_offset];
    am_hdr->am_id             = id;
    am_hdr->am_length         = iov_total_len;
    SCIFlush(NULL, SCI_FLAG_FLUSH_CPU_BUFFERS_ONLY);
    am_hdr->am_message_posted = 1;
    SCIFlush(NULL, SCI_FLAG_FLUSH_CPU_BUFFERS_ONLY);

    ep->ep_conn_seq_num++;

    return UCS_OK;
}

/**
 * @brief Send an active message using the bcopy (buffered copy) protocol.
 *
 * Calls a user-provided pack callback to write the payload directly into
 * the remote segment slot, avoiding an intermediate staging buffer.
 * The callback returns the number of bytes written.
 *
 * Packet layout in the remote segment slot:
 *   [ uct_pcie_am_hdr_t | packed payload ]
 *
 * @param[inout] tl_ep   Endpoint to send on
 * @param[in]    id      Active message handler ID on the remote side
 * @param[in]    pack_cb Callback that packs data into the destination buffer
 * @param[in]    arg     Argument passed through to pack_cb
 * @param[in]    flags   UCT flags (unused)
 * @return Number of bytes sent on success (>= 0)
 * @return UCS_ERR_NO_RESOURCE if the send queue is full
 */
ssize_t uct_pcie_ep_am_bcopy(
    uct_ep_h tl_ep,
    uint8_t id,
    uct_pack_callback_t pack_cb,
    void *arg,
    unsigned flags)
{
    uct_pcie_ep_t     *ep      = ucs_derived_of(tl_ep, uct_pcie_ep_t);
    uct_pcie_iface_t  *iface   = ucs_derived_of(tl_ep->iface, uct_pcie_iface_t);
    uct_pcie_ctl_t    *ctl     = &iface->ctls[ep->ep_conn_index];
    uct_pcie_am_hdr_t *am_hdr;
    uint32_t           slot_offset;
    uint32_t           payload_offset;
    ssize_t            length;

    /* Check that the send queue has room for one more packet */
    if (ep->ep_conn_seq_num - ctl->ep_conn_ack > iface->packet_queue_len) {
        return UCS_ERR_NO_RESOURCE;
    }

    UCT_CHECK_AM_ID(id);

    /* Locate the next available slot in the remote segment ring buffer.
     * The ring index wraps using modulo, and each slot is packet_size_bytes wide. */
    slot_offset    = iface->packet_size_bytes *
                     (ep->ep_conn_seq_num % iface->packet_queue_len);
    payload_offset = slot_offset + sizeof(uct_pcie_am_hdr_t);

    am_hdr = (uct_pcie_am_hdr_t *) &ep->remote_seg_buf[slot_offset];

    /* Invoke the pack callback to write the payload directly into the slot */
    length = pack_cb((void *)&ep->remote_seg_buf[payload_offset], arg);

    /* Write the transport AM header and mark the slot as posted */
    am_hdr->am_id             = id;
    am_hdr->am_length         = length;
    SCIFlush(NULL, SCI_FLAG_FLUSH_CPU_BUFFERS_ONLY);
    am_hdr->am_message_posted = 1;
    SCIFlush(NULL, SCI_FLAG_FLUSH_CPU_BUFFERS_ONLY);

    ep->ep_conn_seq_num++;

    return length;
}

/**
 * @brief Send an active message using the zero-copy protocol.
 *
 * Writes directly into the remote segment mapped via SISCI/PCIe.
 * Packet layout in the remote segment slot:
 *   [ uct_pcie_am_hdr_t | header (header_length bytes) | payload (iov) ]
 *
 * @param[inout] uct_ep        Endpoint to send on
 * @param[in]    id            Active message handler ID on the remote side
 * @param[in]    header        User-defined AM header copied before the payload
 * @param[in]    header_length Length of header in bytes (may be 0)
 * @param[in]    iov           Scatter-gather payload buffers
 * @param[in]    iovcnt        Number of entries in iov
 * @param[in]    flags         UCT flags (unused)
 * @param[in]    comp          Completion handle (unused, send is synchronous)
 * @return UCS_OK              on success
 * @return UCS_ERR_NO_RESOURCE if the send queue is full
 */
ucs_status_t uct_pcie_ep_am_zcopy(
    uct_ep_h uct_ep,
    uint8_t id,
    const void *header,
    unsigned header_length,
    const uct_iov_t *iov,
    size_t iovcnt,
    unsigned flags,
    uct_completion_t *comp)
{
    uct_pcie_ep_t     *ep          = ucs_derived_of(uct_ep, uct_pcie_ep_t);
    uct_pcie_iface_t  *iface       = ucs_derived_of(uct_ep->iface, uct_pcie_iface_t);
    uct_pcie_ctl_t    *ctl         = &iface->ctls[ep->ep_conn_index];
    uct_pcie_am_hdr_t *am_hdr;
    uint8_t           *dest_hdr;
    void              *dest_payload;
    ucs_iov_iter_t     iov_iter;
    size_t             bytes_copied;
    uint32_t           slot_offset;
    size_t             iov_total_len = uct_iov_total_length(iov, iovcnt);
    size_t             total_size    = sizeof(uct_pcie_am_hdr_t) + header_length + iov_total_len;

    /* Check that the send queue has room for one more packet */
    if (ep->ep_conn_seq_num - ctl->ep_conn_ack > iface->packet_queue_len) {
        return UCS_ERR_NO_RESOURCE;
    }

    UCT_CHECK_LENGTH(total_size, 0, iface->packet_size_bytes, "am_zcopy");
    UCT_CHECK_AM_ID(id);

    /* Locate the next available slot in the remote segment ring buffer.
     * The ring index wraps using modulo, and each slot is packet_size_bytes wide. */
    slot_offset  = iface->packet_size_bytes *
                   (ep->ep_conn_seq_num % iface->packet_queue_len);

    /* Lay out the three regions of the packet within the slot */
    am_hdr       = (uct_pcie_am_hdr_t *) &ep->remote_seg_buf[slot_offset];
    dest_hdr     = (uint8_t *) &am_hdr[1];
    dest_payload = dest_hdr + header_length;

    /* Write the transport AM header (id and total payload length) */
    am_hdr->am_id     = id;
    am_hdr->am_length = header_length + iov_total_len;

    /* Copy the user-defined header into the slot, immediately after the AM header */
    memcpy(dest_hdr, header, header_length);

    /* Gather-copy each iov buffer into the contiguous payload region of the slot */
    ucs_iov_iter_init(&iov_iter);
    bytes_copied = uct_iov_to_buffer(iov, iovcnt, &iov_iter,
                                     dest_payload, iov_total_len);
    ucs_assert(bytes_copied == iov_total_len);

    /* Update metadata informing the other side that the message has been sent */
    SCIFlush(NULL, SCI_FLAG_FLUSH_CPU_BUFFERS_ONLY);
    am_hdr->am_message_posted = 1;
    SCIFlush(NULL, SCI_FLAG_FLUSH_CPU_BUFFERS_ONLY);

    /* Advance the sequence number */
    ep->ep_conn_seq_num++;

    return UCS_OK;
}

/* //!SECTION*/

/* // SECTION Pending queue */

/*
 * The UCT pending API lets upper layers queue send requests when a transport
 * is temporarily out of resources (e.g. our flow-control window is full).
 *
 * We use the standard ucs_arbiter_t mechanism:
 *   - One ucs_arbiter_t lives on the iface (shared across all EPs).
 *   - One ucs_arbiter_group_t lives on each EP (its private queue).
 *   - The ucs_arbiter_elem_t is stored in-place inside req->priv, which UCT
 *     reserves for transport use — no extra allocation needed.
 *
 * iface_progress drains the arbiter after processing incoming packets, so
 * pending sends are retried as soon as ack slots free up.
 */

/* Recover the uct_pending_req_t from an arbiter element.
 * The element lives at req->priv (cast to ucs_arbiter_elem_t*), so
 * ucs_container_of reverses that cast. */
static inline uct_pending_req_t *
uct_pcie_pending_req_from_elem(ucs_arbiter_elem_t *elem)
{
    return ucs_container_of(elem, uct_pending_req_t, priv);
}

typedef struct {
    uct_pending_purge_callback_t cb;
    void                        *arg;
} uct_pcie_pending_purge_args_t;

/* Callback used by pending_purge: invokes the caller-supplied cancel function
 * for each element then removes it from the arbiter. */
static ucs_arbiter_cb_result_t
uct_pcie_ep_pending_purge_cb(
    ucs_arbiter_t       *arbiter,
    ucs_arbiter_group_t *group,
    ucs_arbiter_elem_t  *elem,
    void                *arg)
{
    uct_pcie_pending_purge_args_t *args = arg;
    uct_pending_req_t             *req  = uct_pcie_pending_req_from_elem(elem);

    if (args->cb != NULL) {
        args->cb(req, args->arg);
    }
    return UCS_ARBITER_CB_RESULT_REMOVE_ELEM;
}

/**
 * @brief Add a send request to the per-EP pending queue.
 *
 * Called by UCX when an AM send returns UCS_ERR_NO_RESOURCE. We store the
 * pending request in the EP's arbiter group so that uct_pcie_iface_progress
 * can retry it once the flow-control window opens up.
 *
 * @return UCS_INPROGRESS  request was queued
 * @return UCS_ERR_BUSY    window has room and no earlier requests are waiting;
 *                         UCX will retry the send immediately without queuing
 */
ucs_status_t uct_pcie_ep_pending_add(
    uct_ep_h tl_ep,
    uct_pending_req_t *req,
    unsigned flags)
{
    uct_pcie_ep_t    *ep    = ucs_derived_of(tl_ep, uct_pcie_ep_t);
    uct_pcie_iface_t *iface = ucs_derived_of(tl_ep->iface, uct_pcie_iface_t);
    uct_pcie_ctl_t   *ctl   = &iface->ctls[ep->ep_conn_index];

    /* If the window has room and no prior requests are waiting (which would
     * need to go first to preserve ordering), tell UCX to retry directly. */
    if (ucs_arbiter_group_is_empty(&ep->pending_q) &&
        (ep->ep_conn_seq_num - ctl->ep_conn_ack <= iface->packet_queue_len)) {
        return UCS_ERR_BUSY;
    }

    /* uct_pending_req_arb_group_push stores the arbiter element in req->priv
     * (UCT-reserved space) and appends it to this EP's arbiter group. */
    uct_pending_req_arb_group_push(&ep->pending_q, req);

    /* Tell the iface arbiter to include this group in the next dispatch.
     * Idempotent: safe to call even if the group is already scheduled. */
    ucs_arbiter_group_schedule(&iface->arbiter, &ep->pending_q);

    /* UCT contract: return UCS_OK (not UCS_INPROGRESS) to signal that the
     * request was successfully queued. UCS_ERR_BUSY means "don't queue, retry
     * directly". Any other value triggers assertion failures in the UCP layer. */
    return UCS_OK;
}

/**
 * @brief Cancel all pending requests for an EP.
 *
 * Drains the EP's arbiter group and invokes the caller-supplied callback for
 * each request so the upper layer can clean up (e.g. free the request).
 * Typically called during EP teardown or MPI error recovery.
 */
void uct_pcie_ep_pending_purge(
    uct_ep_h tl_ep,
    uct_pending_purge_callback_t cb,
    void *arg)
{
    uct_pcie_ep_t                 *ep    = ucs_derived_of(tl_ep, uct_pcie_ep_t);
    uct_pcie_iface_t              *iface = ucs_derived_of(tl_ep->iface, uct_pcie_iface_t);
    uct_pcie_pending_purge_args_t  args  = {.cb = cb, .arg = arg};

    ucs_arbiter_group_purge(&iface->arbiter, &ep->pending_q,
                            uct_pcie_ep_pending_purge_cb,
                            &args);
}

/* //!SECTION */

