

#include <uct/base/uct_iov.inl>

#include "pcie_ep.h"
#include "pcie_sisci_helper.h"
#include "pcie_md.h"

static UCS_CLASS_CLEANUP_FUNC(uct_pcie_ep_t)
{   
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
    /* quick fix for weird behaviour when queue size was 1...*/
    self->ep_conn_seq_num               = iface->packet_queue_len > 1 ? 1 : 0;

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


/* //SECTION RDMA*/
ucs_status_t uct_pcie_ep_put_short(
    uct_ep_h tl_ep,
    const void *buffer,
    unsigned length,
    uint64_t remote_addr,
    uct_rkey_t rkey)
{
    //TODO
    printf("uct_pcie_ep_put_short()\n");
    return UCS_ERR_NOT_IMPLEMENTED;
}

ssize_t uct_pcie_ep_put_bcopy(
    uct_ep_h tl_ep,
    uct_pack_callback_t pack_cb,
    void *arg,
    uint64_t remote_addr,
    uct_rkey_t rkey)
{
    //TODO
    printf("uct_pcie_ep_put_bcopy()\n");
    return UCS_ERR_NOT_IMPLEMENTED;
}

ucs_status_t uct_pcie_ep_get_bcopy(
    uct_ep_h tl_ep,
    uct_unpack_callback_t unpack_cb,
    void *arg,
    size_t length,
    uint64_t remote_addr,
    uct_rkey_t rkey,
    uct_completion_t *comp)
{
    //TODO
    printf("uct_pcie_ep_get_bcopy()\n");
    return UCS_ERR_NOT_IMPLEMENTED;
}


/*//!SECTION*/

/* //SECTION ATOMICS*/

ucs_status_t uct_pcie_ep_atomic32_post(
    uct_ep_h ep,
    unsigned opcode,
    uint32_t value,
    uint64_t remote_addr,
    uct_rkey_t rkey)
{
    //TODO
    printf("uct_pcie_ep_atomic32_post()\n");
    return UCS_ERR_NOT_IMPLEMENTED;
}

ucs_status_t uct_pcie_ep_atomic64_post(
    uct_ep_h ep,
    unsigned opcode,
    uint64_t value,
    uint64_t remote_addr,
    uct_rkey_t rkey)
{
    //TODO
    printf("uct_pcie_ep_atomic64_post()\n");
    return UCS_ERR_NOT_IMPLEMENTED;
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
    //TODO
    printf("uct_pcie_ep_atomic64_fetch()\n");
    return UCS_ERR_NOT_IMPLEMENTED;
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
    //TODO
    printf("uct_pcie_ep_atomic32_fetch()\n");
    return UCS_ERR_NOT_IMPLEMENTED;
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
    //TODO
    printf("uct_pcie_ep_atomic_cswap64()\n");
    return UCS_ERR_NOT_IMPLEMENTED;
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
    //TODO
    printf("uct_pcie_ep_atomic_cswap32()\n");
    return UCS_ERR_NOT_IMPLEMENTED;
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
    if (ep->ep_conn_seq_num - ctl->ep_conn_ack >= iface->packet_queue_len) {
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

    ucs_debug("EP_SEG %d EP_NOD %d AM_ID %d size %u ep_conn_seq_num:%d\n",
              ep->remote_seg_id, ep->remote_node_id, id,
              am_hdr->am_length, ep->ep_conn_seq_num);

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
    if (ep->ep_conn_seq_num - ctl->ep_conn_ack >= iface->packet_queue_len) {
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

    ucs_debug("EP_SEG %d EP_NOD %d AM_ID %d size %u ep_conn_seq_num:%d\n",
              ep->remote_seg_id, ep->remote_node_id, id,
              am_hdr->am_length, ep->ep_conn_seq_num);

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
    if (ep->ep_conn_seq_num - ctl->ep_conn_ack >= iface->packet_queue_len) {
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

    ucs_debug("EP_SEG %d EP_NOD %d AM_ID %d size %u",
              ep->remote_seg_id, ep->remote_node_id, id, am_hdr->am_length);

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
    if (ep->ep_conn_seq_num - ctl->ep_conn_ack >= iface->packet_queue_len) {
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

    ucs_debug("EP_SEG %d EP_NOD %d AM_ID %d size %u",
              ep->remote_seg_id, ep->remote_node_id, id, am_hdr->am_length);

    return UCS_OK;
}

/* //!SECTION*/

