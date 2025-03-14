

#include "sisci_ep.h"
#include "sisci_iface.h"

static UCS_CLASS_CLEANUP_FUNC(uct_sci_ep_t)
{   
    sci_error_t sci_error;
    
    SCIUnmapSegment(self->remote_map, 0, &sci_error);
    
    self->buf = NULL;

    if (sci_error != SCI_ERR_OK) { 
        printf("SCI_UNMAP_SEGMENT: %s\n", SCIGetErrorString(sci_error));
    }
    
    SCIDisconnectSegment(self->remote_segment, 0, &sci_error);

    if (sci_error != SCI_ERR_OK) { 
        printf("SCI_DISCONNECT_SEGMENT: %s\n", SCIGetErrorString(sci_error));
    }
    
    DEBUG_PRINT("ep deleted segment_id %d node_id %d\n", self->remote_segment_id, self->remote_node_id);
}


/**
 * @brief 
 * @param[in] iface 
 * @param[in] node_id 
 * @param[in] remote_interrupt_no 
 * @param[in] local_interrupt_no 
 * @return 
 */
static ucs_status_t uct_sci_ep_send_conn_request(
    uct_sci_iface_t* iface,
    unsigned int node_id,
    unsigned int remote_interrupt_no,
    unsigned int local_interrupt_no,
    sci_desc_t sci_virtual_device)
{
    sci_error_t sci_error;
    uct_sci_conn_req_t request;
    sci_remote_data_interrupt_t req_interrupt;
    int rc = UCS_OK;

    do {
        SCIConnectDataInterrupt(sci_virtual_device, &req_interrupt, node_id, 0, remote_interrupt_no, 0, 0, &sci_error);
    } while (sci_error != SCI_ERR_OK);

    //printf("%d connected to remote interrupt!, ret_int %d\n", getpid(),local_interrupt_no);
    //printf("size of answer %zd size of struct answer %zd\n", sizeof(answer), sizeof(uct_sci_conn_ans_t));
    request.status     = 1;
    request.interrupt  = local_interrupt_no;
    request.node_id    = iface->device_addr;
    request.ctl_offset = iface->eps * sizeof(uct_sci_ctl_t);
    request.ctl_id     = iface->ctl_id;

    SCITriggerDataInterrupt(req_interrupt, (void *) &request, sizeof(request), UCT_SCI_NO_FLAGS, &sci_error);
    if(sci_error != SCI_ERR_OK) {
        printf("SCI Trigger Interrupt: %s\n", SCIGetErrorString(sci_error));
        rc = UCS_ERR_NO_RESOURCE;
    }

    /* Should probably retry then? */
    /*  Clean up for connection.  */
    SCIDisconnectDataInterrupt(req_interrupt, UCT_SCI_NO_FLAGS, &sci_error);
    if(sci_error == SCI_ERR_BUSY) {
        printf("SCIRemoveDataInterrupt: Interrupt still being used by another proccess");
    }

    return rc;
}

/**
 * @brief 
 * @param[in] iface 
 * @param[in] node_id 
 * @param[in] remote_interrupt_no 
 * @param[in] sci_virtual_device 
 * @param[out] answer 
 */
static ucs_status_t uct_sci_ep_send_recv_conn_request(
    uct_sci_iface_t* iface,
    unsigned int node_id,
    unsigned int remote_interrupt_no,
    sci_desc_t sci_virtual_device,
    uct_sci_conn_ans_t *answer)
{
    sci_error_t sci_error;
    unsigned int local_interrupt_no;
    ucs_status_t ucs_ret;
    sci_local_data_interrupt_t ans_interrupt;
    unsigned int ans_size = (unsigned int) sizeof(uct_sci_conn_ans_t);

    /* 1.
     * Create the data interrupt we will use to receive the response of our
     * connection request. We get sisci to give us a free interrupt id.
     * We send that ID along with our connection request such that the server
     * can get back to us. */
    SCICreateDataInterrupt(
        sci_virtual_device,
        &ans_interrupt,
        UCT_SCI_LOCAL_ADAPTER_NO,
        &local_interrupt_no,  
        NULL,
        NULL,
        UCT_SCI_NO_FLAGS,
        &sci_error);
    if(sci_error != SCI_ERR_OK) {
        printf("SCI Trigger Interrupt: %s\n", SCIGetErrorString(sci_error));
        return UCS_ERR_NO_RESOURCE;
    }
    
    /* 2.
     * Send connection request to the server. This will trigger a callback on
     * the other side, and that callback will get back to us and send us an
     * answer to our connection request. */
    ucs_ret = uct_sci_ep_send_conn_request(iface, node_id, remote_interrupt_no, local_interrupt_no, sci_virtual_device);
    if (ucs_ret != UCS_OK) {
        return ucs_ret;
    }

    /* 3. Wait for connection request answer */
    SCIWaitForDataInterrupt(ans_interrupt, (void*) answer, &ans_size, SCI_INFINITE_TIMEOUT, 0, &sci_error);
    if(sci_error != SCI_ERR_OK) {
        printf("SCI Wait For Interrupt: %s\n", SCIGetErrorString(sci_error));
        ucs_ret = UCS_ERR_NO_RESOURCE;
    }
    /* Done. Clean up data interrupt made in step 1 */
    SCIRemoveDataInterrupt(ans_interrupt, UCT_SCI_NO_FLAGS, &sci_error);

    return ucs_ret;
}

static UCS_CLASS_INIT_FUNC(uct_sci_ep_t, const uct_ep_params_t *params)
{
    sci_error_t sci_error;
    ucs_status_t ucs_ret;
    uct_sci_iface_addr_t* iface_addr =  (uct_sci_iface_addr_t*) params->iface_addr;
    uct_sci_device_addr_t* dev_addr = (uct_sci_device_addr_t*) params->dev_addr;
    uct_sci_conn_ans_t answer;

    unsigned int remote_interrupt_no;
    unsigned int node_id;
    uct_sci_iface_t* iface = ucs_derived_of(params->iface, uct_sci_iface_t);
    uct_sci_md_t* md = ucs_derived_of(iface->super.md, uct_sci_md_t);

    UCT_EP_PARAMS_CHECK_DEV_IFACE_ADDRS(params);

    remote_interrupt_no = (unsigned int) iface_addr->interrupt_no;
    node_id = (unsigned int) dev_addr->node_id;

    DEBUG_PRINT("EP created remote_interrupt_no %d node_id %d\n", remote_interrupt_no, node_id);

    self->super.super.iface = params->iface;
    
    UCS_CLASS_CALL_SUPER_INIT(uct_base_ep_t, &iface->super); //segfaults without this line, probably has something to do with the stats member...

    ucs_ret = uct_sci_ep_send_recv_conn_request(iface, node_id, remote_interrupt_no, md->sci_virtual_device, &answer);
    if (ucs_ret != UCS_OK) {
        return ucs_ret;
    }
    
    /* uct_sci_ep_t *self */
    self->remote_node_id    = answer.node_id;
    self->remote_segment_id = answer.segment_id;
    self->offset            = answer.offset;
    self->packet_size_bytes = answer.packet_size_bytes;
    self->packet_queue_len  = answer.packet_queue_len;
    self->ctl_offset        = iface->eps * sizeof(uct_sci_ctl_t);
    /* quick fix for weird behaviour when queue size was 1...*/
    self->seq               = self->packet_queue_len > 1 ? 1 : 0;

    do {
        DEBUG_PRINT("waiting to connect %d %s\n", sci_error,  SCIGetErrorString(sci_error));
        
        SCIConnectSegment(iface->vdev_ep, &self->remote_segment, self->remote_node_id, self->remote_segment_id, 
                    UCT_SCI_LOCAL_ADAPTER_NO, NULL, NULL, 0, 0, &sci_error);
    } while (sci_error != SCI_ERR_OK);

    self->buf = (uint8_t *) SCIMapRemoteSegment(self->remote_segment, &self->remote_map, self->offset, iface->packet_size_bytes * self->packet_queue_len, NULL, 0, &sci_error);
    if (sci_error != SCI_ERR_OK) { 
        ucs_error("SCI_MAP_REM_SEG: %s", SCIGetErrorString(sci_error));
        return UCS_ERR_NO_RESOURCE;
    }

    iface->eps += 1;    
    DEBUG_PRINT("EP connected to segment %d at node %d\n",  self->remote_segment_id, self->remote_node_id);
    return UCS_OK;
}


UCS_CLASS_DEFINE(uct_sci_ep_t, uct_base_ep_t);

UCS_CLASS_DEFINE_NEW_FUNC(uct_sci_ep_t, uct_ep_t, const uct_ep_params_t *);
UCS_CLASS_DEFINE_DELETE_FUNC(uct_sci_ep_t, uct_ep_t);


/* //SECTION RDMA*/
ucs_status_t uct_sci_ep_put_short (uct_ep_h tl_ep, const void *buffer,
                                 unsigned length, uint64_t remote_addr,
                                 uct_rkey_t rkey)
{
    //TODO
    printf("uct_sci_ep_put_short()\n");
    return UCS_ERR_NOT_IMPLEMENTED;
}

ssize_t uct_sci_ep_put_bcopy(uct_ep_h tl_ep, uct_pack_callback_t pack_cb,
                            void *arg, uint64_t remote_addr, uct_rkey_t rkey)
{
    //TODO
    printf("uct_sci_ep_put_bcopy()\n");
    return UCS_ERR_NOT_IMPLEMENTED;
}

ucs_status_t uct_sci_ep_get_bcopy(uct_ep_h tl_ep, uct_unpack_callback_t unpack_cb,
                                 void *arg, size_t length,
                                 uint64_t remote_addr, uct_rkey_t rkey,
                                 uct_completion_t *comp)
{
    //TODO
    printf("uct_sci_ep_get_bcopy()\n");
    return UCS_ERR_NOT_IMPLEMENTED;
}


/*//!SECTION*/

/* //SECTION ATOMICS*/

ucs_status_t uct_sci_ep_atomic32_post(uct_ep_h ep, unsigned opcode, uint32_t value,
                                     uint64_t remote_addr, uct_rkey_t rkey)
{
    //TODO
    printf("uct_sci_ep_atomic32_post()\n");
    return UCS_ERR_NOT_IMPLEMENTED;
}

ucs_status_t uct_sci_ep_atomic64_post(uct_ep_h ep, unsigned opcode, uint64_t value,
                                     uint64_t remote_addr, uct_rkey_t rkey)
{
    //TODO
    printf("uct_sci_ep_atomic64_post()\n");
    return UCS_ERR_NOT_IMPLEMENTED;
}

ucs_status_t uct_sci_ep_atomic64_fetch(uct_ep_h ep, uct_atomic_op_t opcode,
                                      uint64_t value, uint64_t *result,
                                      uint64_t remote_addr, uct_rkey_t rkey,
                                      uct_completion_t *comp)
{
    //TODO
    printf("uct_sci_ep_atomic64_fetch()\n");
    return UCS_ERR_NOT_IMPLEMENTED;
}

ucs_status_t uct_sci_ep_atomic32_fetch(uct_ep_h ep, uct_atomic_op_t opcode,
                                      uint32_t value, uint32_t *result,
                                      uint64_t remote_addr, uct_rkey_t rkey,
                                      uct_completion_t *comp)
{
    //TODO
    printf("uct_sci_ep_atomic32_fetch()\n");
    return UCS_ERR_NOT_IMPLEMENTED;
}

ucs_status_t uct_sci_ep_atomic_cswap64(uct_ep_h tl_ep, uint64_t compare,
                                      uint64_t swap, uint64_t remote_addr,
                                      uct_rkey_t rkey, uint64_t *result,
                                      uct_completion_t *comp)
{
    //TODO
    printf("uct_sci_ep_atomic_cswap64()\n");
    return UCS_ERR_NOT_IMPLEMENTED;
}

ucs_status_t uct_sci_ep_atomic_cswap32(uct_ep_h tl_ep, uint32_t compare,
                                      uint32_t swap, uint64_t remote_addr,
                                      uct_rkey_t rkey, uint32_t *result,
                                      uct_completion_t *comp)
{
    //TODO
    printf("uct_sci_ep_atomic_cswap32()\n");
    return UCS_ERR_NOT_IMPLEMENTED;
}

/* //!SECTION */

/*  // SECTION Active messages */

/**
 * @brief 
 * @param[inout] tl_ep 
 * @param[in] id 
 * @param[in] header 
 * @param[in] payload 
 * @param[in] length 
 * @return 
 */
ucs_status_t uct_sci_ep_am_short(uct_ep_h tl_ep, uint8_t id, uint64_t header,
                                  const void *payload, unsigned length)
{

    uct_sci_ep_t* ep       = ucs_derived_of(tl_ep, uct_sci_ep_t);
    uct_sci_am_hdr_t* packet_am_hdr; 
    uct_sci_iface_t* iface = ucs_derived_of(tl_ep->iface, uct_sci_iface_t);
    uct_sci_ctl_t* ctl         = iface->ctls + ep->ctl_offset;
    uint32_t packet_buf_offset;
    uint32_t send_start_buf_offset;
    
    if (ep->seq - ctl->ack >= iface->packet_queue_len) {
        return UCS_ERR_NO_RESOURCE;
    }
        
    packet_buf_offset = ep->packet_size_bytes * (ep->seq % ep->packet_queue_len);
    packet_am_hdr = (uct_sci_am_hdr_t*) &ep->buf[packet_buf_offset];
    ctl->status = 1;
    packet_am_hdr->am_id = id;
    packet_am_hdr->length = length + sizeof(header);

    send_start_buf_offset = packet_buf_offset + sizeof(uct_sci_am_hdr_t);

    uct_am_short_fill_data(&ep->buf[send_start_buf_offset], header, payload, length, UCS_ARCH_MEMCPY_NT_DEST);
    SCIFlush(NULL, SCI_FLAG_FLUSH_CPU_BUFFERS_ONLY);    
    packet_am_hdr->status = 1;
    SCIFlush(NULL, SCI_FLAG_FLUSH_CPU_BUFFERS_ONLY);
    ep->seq++;
    DEBUG_PRINT("EP_SEG %d EP_NOD %d AM_ID %d size %d SEQ:%d\n", ep->remote_segment_id, ep->remote_node_id, id, packet_am_hdr->length, ep->seq);
    return UCS_OK;
}

ucs_status_t uct_sci_ep_am_short_iov(uct_ep_h tl_ep, uint8_t id,
                                      const uct_iov_t *iov, size_t iovcnt)
{
    //TODO short_iov
    printf("uct_sci_ep_am_short_iov()\n");
    return UCS_ERR_NOT_IMPLEMENTED;
}

/**
 * @brief 
 * @param[inout] tl_ep
 * @param[in] id 
 * @param[in] pack_cb Callback to put specified data into 
 * @param[in] arg Data to copy.
 * @param[in] flags 
 * @return 
 */
ssize_t uct_sci_ep_am_bcopy(
    uct_ep_h tl_ep,
    uint8_t id,
    uct_pack_callback_t pack_cb,
    void *arg,
    unsigned flags)
{
    uct_sci_ep_t* ep = ucs_derived_of(tl_ep, uct_sci_ep_t);
    uct_sci_am_hdr_t* packet_am_hdr;
    uct_sci_iface_t* iface = ucs_derived_of(tl_ep->iface, uct_sci_iface_t);
    uct_sci_ctl_t* ctl = iface->ctls + ep->ctl_offset;
    ssize_t length;
    uint32_t packet_buf_offset;
    uint32_t send_start_buf_offset;

    if(ep->seq - ctl->ack >= iface->packet_queue_len) {
        return UCS_ERR_NO_RESOURCE;
    }

    packet_buf_offset = ep->packet_size_bytes * (ep->seq % ep->packet_queue_len);
    packet_am_hdr = (uct_sci_am_hdr_t*) &ep->buf[packet_buf_offset];
    
    ctl->status = 1;
    
    send_start_buf_offset = packet_buf_offset + sizeof(uct_sci_am_hdr_t);
    
    /* This is where the sending of the real data happends */
    length = pack_cb(&ep->buf[send_start_buf_offset],  arg);

    /* Update meta information */
    packet_am_hdr->am_id = id;
    packet_am_hdr->length = length;
    SCIFlush(NULL, SCI_FLAG_FLUSH_CPU_BUFFERS_ONLY);
    packet_am_hdr->status = 1;
    SCIFlush(NULL, SCI_FLAG_FLUSH_CPU_BUFFERS_ONLY);
    ep->seq++;

    DEBUG_PRINT("EP_SEG %d EP_NOD %d AM_ID %d size %d \n", ep->remote_segment_id, ep->remote_node_id, id, packet_am_hdr->length);

    return length;
}

/**
 * @brief 
 * @param[in] header 
 * @param[in] header_length 
 * @param[in] id 
 * @param[in] iov 
 * @param[in] iovcnt
 * @param[in] bytes_to_send
 * @param[out] tx_buf 
 */
static void uct_sci_fill_buffer_with_packet(
    const void *header,
    unsigned header_length,
    uint8_t id,
    const uct_iov_t *iov,
    size_t iovcnt,
    size_t iov_total_len,
    size_t bytes_to_send,
    uint8_t* tx_buf)
{
    size_t bytes_copied;
    ucs_iov_iter_t uct_iov_iter;
    uct_sci_am_hdr_t* tx_packet_am_hdr = (uct_sci_am_hdr_t*) tx_buf;

    /* Convert the iov into a contiguous buffer */
    ucs_iov_iter_init(&uct_iov_iter);
    
    /* Set uct_sci packet prefix values, stored directly into the DMA buffer ready for sending */
    tx_packet_am_hdr->am_id = id;
    tx_packet_am_hdr->length = iov_total_len + header_length;
    
    /* Copy the uct header to the transfer buffer after prefix. Copied after uct_sci packet prefix*/
    if (header_length != 0) {
        memcpy(&tx_buf[sizeof(uct_sci_am_hdr_t)], header, header_length);
    }
    
    /* Copy package from iov to the the DMA buffer. The rest of the data, after uct_sci packet prefix, and uct header */
    bytes_copied = uct_iov_to_buffer(
        iov,
        iovcnt,
        &uct_iov_iter,
        &tx_buf[sizeof(uct_sci_am_hdr_t) + header_length],
        bytes_to_send);
    assert(bytes_copied != iov_total_len);
}

/**
 * @brief 
 * @param[inout] uct_ep 
 * @param[in] id 
 * @param[in] header 
 * @param[in] header_length 
 * @param[in] iov 
 * @param[in] iovcnt 
 * @param[in] flags 
 * @param comp Unused 
 * @return 
 */
ucs_status_t uct_sci_ep_am_zcopy(
    uct_ep_h uct_ep,
    uint8_t id,
    const void *header,
    unsigned header_length,
    const uct_iov_t *iov,
    size_t iovcnt,
    unsigned flags,
    uct_completion_t *comp) 
{

    uct_sci_ep_t* ep = ucs_derived_of(uct_ep, uct_sci_ep_t);
    uct_sci_iface_t* iface = ucs_derived_of(uct_ep->iface, uct_sci_iface_t);
    uct_sci_am_hdr_t* packet_am_hdr; 
    uct_sci_ctl_t* ctl = iface->ctls + ep->ctl_offset;
    uint32_t packet_buf_offset;
    size_t bytes_to_send;
    sci_error_t sci_error;

    uint8_t* tx_buf = iface->dma_buf;

    size_t iov_total_len = uct_iov_total_length(iov, iovcnt);
    
    if(ep->seq - ctl->ack >= iface->packet_queue_len) {
        return UCS_ERR_NO_RESOURCE;
    }

    ctl->status = 1;

    bytes_to_send = iov_total_len + header_length + sizeof(uct_sci_am_hdr_t);
    UCT_CHECK_LENGTH(bytes_to_send, 0 , iface->packet_size_bytes, "am_zcopy");
    UCT_CHECK_AM_ID(id);

    uct_sci_fill_buffer_with_packet(
        header,
        header_length,
        id,
        iov,
        iovcnt,
        iov_total_len,
        bytes_to_send,
        tx_buf);
    
    packet_buf_offset = ep->packet_size_bytes * (ep->seq % ep->packet_queue_len);
    /* Send all the data  */
    SCIStartDmaTransfer(
        iface->dma_queue,
        iface->dma_segment,
        ep->remote_segment, 
        0,
        bytes_to_send,
        packet_buf_offset,
        UCT_SCI_NO_CALLBACK,
        NULL,
        UCT_SCI_NO_FLAGS,
        &sci_error);
    if(sci_error != SCI_ERR_OK) {
        printf("DMA Transfer Error: %s\n", SCIGetErrorString(sci_error));
    }
        
    /* Need to wait for transfer to finish first? */
    packet_am_hdr = (uct_sci_am_hdr_t*)&ep->buf[packet_buf_offset];
    ep->seq++;
    packet_am_hdr->status = 1;
    SCIFlush(NULL, SCI_FLAG_FLUSH_CPU_BUFFERS_ONLY);

    DEBUG_PRINT("EP_SEG %d EP_NOD %d AM_ID %d size %d \n", ep->remote_segment_id, ep->remote_node_id, id, packet_am_hdr->length);

    return UCS_OK;    
}

/* //!SECTION*/

