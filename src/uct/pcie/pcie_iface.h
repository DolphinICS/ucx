/*
 * Copyright (C) Dolphin Interconnect Solutions AS, 2025-2026. ALL RIGHTS RESERVED.
 * See file LICENSE for terms.
 */

#ifndef UCT_PCIE_IFACE_H
#define UCT_PCIE_IFACE_H

#include <uct/base/uct_iface.h>
#include <ucs/datastruct/arbiter.h>
#include <ucs/datastruct/list.h>

#include <sisci_error.h>
#include <sisci_api.h>
#include <pthread.h>

#define UCT_PCIE_NAME "pcie"

#define UCT_PCIE_LOCAL_ADAPTER_NO 0
#define UCT_PCIE_NO_FLAGS         0
/* Size of sci_cds[]. The MAX_NUM_EPS and MAX_EPS config defaults must match */
#define UCT_PCIE_MAX_EPS 32

/* Maximum bytes for put_short. put_short and put_bcopy both copy into the
 * PIO window, so this limit only decides where the caller switches from one
 * to the other. */
#define UCT_PCIE_MAX_PUT_SHORT 4096

/* Size of the MD's RMA segment and of the iface's DMA staging segment. Needs
 * the driver's ntb_memory_preallocation_size_mb raised above its default. */
#define UCT_PCIE_RMA_SEG_SIZE (16 * 1024 * 1024)

/* get above this size uses DMA instead of PIO reads */
#define UCT_PCIE_GET_DMA_THRESHOLD 255

/* put_zcopy from the MD pool above this size uses DMA. PIO writes are posted,
 * so DMA only wins for large transfers. */
#define UCT_PCIE_PUT_DMA_THRESHOLD (512 * 1024)

/* DMA queue length. Transfers are waited for before the op returns. */
#define UCT_PCIE_DMA_QUEUE_MAX_ENTRIES 10

/* Maximum iov count for active messages */
#define UCT_PCIE_AM_MAX_IOV 10

/* How long connection setup retries a SISCI connect, or waits for the peer's
 * answer, before giving up */
#define UCT_PCIE_CONNECT_TIMEOUT_MS 10000

/* Most pcie ifaces that can be open on one node at a time, across all
 * processes. Each iface's inbox has a request slot per iface on every node, and
 * iface open fails past this. */
#define UCT_PCIE_MAX_IFACES_PER_NODE 16

/* Inbox slots per iface index: node IDs are multiples of 4, below 256 */
#define UCT_PCIE_INBOX_NODES 64
#define UCT_PCIE_INBOX_SLOTS \
    (UCT_PCIE_INBOX_NODES * UCT_PCIE_MAX_IFACES_PER_NODE)

/* Segments get IDs from [UCT_PCIE_AUTO_SEG_ID_START, +128] with
 * SCI_FLAG_AUTO_ID. Inbox segments take fixed IDs from
 * UCT_PCIE_INBOX_SEG_ID_BASE, outside that range, and the offset is the
 * iface's index on its node. */
#define UCT_PCIE_AUTO_SEG_ID_START 1024
#define UCT_PCIE_INBOX_SEG_ID_BASE 2048

/* Iface address exchanged before EP creation. The MD has a single RMA segment
 * per process, so its identity is carried here rather than in the rkey, and
 * rkey_packed_size is 0. */
typedef struct {
    unsigned int interrupt_no; /* Connection interrupt */
    unsigned int inbox_seg_id; /* Where connection requests are written */

    /* SISCI segment ID of the MD's RMA segment */
    unsigned int rma_seg_id;

    /* Local VA of the RMA segment on this node. mem_alloc returns addresses
     * inside the segment, so a peer turns remote_addr into a segment offset
     * with remote_addr - rma_base. */
    uint64_t     rma_base;
} UCS_S_PACKED uct_pcie_iface_addr_t;

typedef struct {
    unsigned int node_id;
} UCS_S_PACKED uct_pcie_device_addr_t;

/* Connection state in an EP's ctl slot, written by the peer */
typedef enum {
    UCT_PCIE_CONN_NONE,     /* No answer yet */
    UCT_PCIE_CONN_ACCEPTED, /* recv_seg_id is valid, acks follow */
    UCT_PCIE_CONN_REFUSED,  /* The peer won't write the slot again */
    UCT_PCIE_CONN_CLOSED    /* The peer has closed its side and won't write the
                             * slot again */
} uct_pcie_conn_state_t;

/* One EP's slot in its iface's control segment, written by the peer */
typedef struct {
    volatile uint64_t ep_conn_ack;
    /* Answer to the connection request: the receive segment the peer created
     * for this EP. recv_seg_id is written before conn_state. */
    volatile uint32_t recv_seg_id;
    volatile uint32_t conn_state; /* uct_pcie_conn_state_t */
} uct_pcie_ctl_t;

/* Who owns a slot in the iface's control segment */
typedef enum {
    UCT_PCIE_CTL_SLOT_FREE,
    UCT_PCIE_CTL_SLOT_IN_USE, /* By an EP */
    UCT_PCIE_CTL_SLOT_CLOSING /* EP destroyed, the peer may still write the
                               * slot until it's REFUSED or CLOSED */
} uct_pcie_ctl_slot_state_t;

typedef struct {
    uint8_t          am_id;
    volatile uint8_t am_message_posted;
    unsigned         am_length;
} UCS_S_PACKED uct_pcie_am_hdr_t;

typedef enum {
    UCT_PCIE_CD_AVAILABLE,
    UCT_PCIE_CD_RESERVED,
    UCT_PCIE_CD_READY,
} uct_pcie_conn_desc_status_t;

/* Incoming connection, owns the receive segment created for it */
typedef struct {
    volatile uct_pcie_conn_desc_status_t cd_status;
    uint64_t                    ep_conn_last_ack;

    /* Set by uct_pcie_iface_recv_segment_cb on the SISCI callback thread:
     * the EP has mapped recv_segment, and has unmapped it again or its node
     * is gone. Progress closes the connection on either of the last two. */
    volatile int                peer_connected;
    volatile int                peer_gone;
    /* Close the connection if the EP hasn't mapped recv_segment by then */
    ucs_time_t                  connect_deadline;

    sci_local_segment_t         recv_segment;
    sci_map_t                   recv_segment_map;
    unsigned int                recv_segment_id;

    /* Mapped base of recv_segment, the ring buffer the remote EP writes to */
    uint8_t                     *packet_queue_buf;

    /* Peer EP's ack slot in its iface's control segment */
    uint32_t                    ctl_segment_id;
    sci_remote_segment_t        ctl_segment;
    sci_map_t                   ctl_segment_map;
    uct_pcie_ctl_t              *ctl_buf;
} uct_pcie_conn_desc_t;

typedef struct {
    int node_id;
    int ctl_segment_id;
    int ep_conn_index;
} uct_pcie_conn_req_t;

/* The initiator iface's slot in the peer's inbox. The initiator writes request,
 * then sets pending and triggers the peer's interrupt. The peer clears pending
 * once it has handled the request. */
typedef struct {
    uct_pcie_conn_req_t request;
    volatile uint32_t   pending;
} uct_pcie_inbox_slot_t;

typedef struct {
    uct_iface_config_t super;
    /* AM slot size in bytes, the largest AM that fits */
    size_t             packet_size_bytes;
    /* Number of AM slots per connection */
    unsigned int       packet_queue_len;
    /* Maximum number of endpoints supported by the iface */
    unsigned int       max_eps;
} uct_pcie_iface_config_t;

typedef struct {
    uct_base_iface_t           super;

    /* Local SISCI node ID, obtained from SCIGetLocalNodeId at init time. */
    unsigned int               device_addr;

    /* Per-slot byte size for the AM ring buffer. Each AM send occupies one
     * slot; the slot holds a uct_pcie_am_hdr_t followed by the payload. */
    size_t                     packet_size_bytes;
    /* Number of ring-buffer slots per endpoint connection. */
    uint32_t                   packet_queue_len;
    /* Maximum number of simultaneously connected endpoints. */
    unsigned int               max_eps;

    /* DMA for get and put_zcopy. Transfers finish before the op returns, so
     * at most one is queued. */
    sci_dma_queue_t            dma_queue;
    sci_local_segment_t        dma_segment;
    sci_map_t                  dma_map;
    void                       *dma_buffer;

    /* One descriptor per incoming connection */
    uct_pcie_conn_desc_t       sci_cds[UCT_PCIE_MAX_EPS];

    /* Interrupt peers trigger after writing a connection request into the
     * inbox. Its number is published in iface_addr. */
    sci_local_interrupt_t      interrupt;
    unsigned int               interrupt_no;

    /* Connection requests, one slot per iface on every node, see
     * uct_pcie_inbox_slot_t. Its ID is published in iface_addr. */
    sci_local_segment_t        inbox_segment;
    sci_map_t                  inbox_map;
    unsigned int               inbox_seg_id;
    uct_pcie_inbox_slot_t      *inbox;
    /* This iface's index on its node, inbox_seg_id -
     * UCT_PCIE_INBOX_SEG_ID_BASE. Picks this iface's slot in peers' inboxes. */
    unsigned int               local_index;

    /* A SISCI descriptor holds at most 32 resources of a type, so each
     * segment role gets its own to allow 32 EPs */
    sci_desc_t                 vdev_recv; /* per-connection recv segments */
    sci_desc_t                 vdev_ep; /* AM receive segment connections */
    sci_desc_t                 vdev_rma; /* RMA segment connections */
    sci_desc_t                 vdev_ctl; /* control segment connections */
    sci_desc_t                 vdev_setup; /* peer inbox and interrupt,
                                            * during a connection request */
    sci_desc_t                 vdev_refuse; /* peer ctl slot, while refusing
                                             * its request */

    /* Protects sci_cds slot allocation: the connection handler on the SISCI
     * callback thread reserves, progress releases. */
    pthread_mutex_t            lock;

    /* Pending sends, retried from progress */
    ucs_arbiter_t              arbiter;

    /* EPs waiting for the peer's answer, polled from progress */
    ucs_list_link_t            connecting_eps;

    /* Owner of each slot in ctls. A CLOSING slot is freed once the peer has
     * written REFUSED or CLOSED into it, see uct_pcie_iface_ctl_slot_get. */
    uct_pcie_ctl_slot_state_t  ctl_slot_state[UCT_PCIE_MAX_EPS];

    /* Control segment: a local segment that remote EPs map and write
     * acknowledgement counters into. One uct_pcie_ctl_t slot per EP. */
    unsigned int               ctl_segment_id;
    sci_local_segment_t        ctl_segment;
    sci_map_t                  ctl_segment_map;
    uct_pcie_ctl_t             *ctls;
} uct_pcie_iface_t;


ucs_status_t
uct_pcie_iface_ctl_slot_get(uct_pcie_iface_t *iface, unsigned int *index_p);

void uct_pcie_iface_ctl_slot_put(uct_pcie_iface_t *iface, unsigned int index,
                                 int request_sent);

#endif
