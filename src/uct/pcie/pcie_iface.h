#ifndef UCT_PCIE_IFACE_H
#define UCT_PCIE_IFACE_H

#include "pthread.h"

#include <uct/base/uct_iface.h>
#include <ucs/datastruct/arbiter.h>
#include <ucs/datastruct/queue.h>

#include <sisci_error.h>
#include <sisci_api.h>

#define UCT_PCIE_NAME "pcie"

#define UCT_PCIE_LOCAL_ADAPTER_NO 0
#define UCT_PCIE_NO_FLAGS 0
#define UCT_PCIE_NO_CALLBACK 0
/* Compile-time array size for SISCI connection slots (sci_cds[] in
 * uct_pcie_iface_t). Must match the PCIE_MAX_EPS and MAX_NUM_EPS config
 * defaults in uct_pcie_iface_config_table; change all three together.
 *
 * MAX_NUM_EPS is advisory: UCX upper layers use it for planning but the UCT
 * framework does not gate ep_create calls against it. The transport refuses
 * new connections when it runs out of sci_cds[] slots. */
#define UCT_PCIE_MAX_EPS 32

/* Maximum bytes for put_short. This threshold is arbitrary for a PIO
 * transport: put_short and put_bcopy both do a memcpy into the MMIO window,
 * so there is no physical reason to prefer one over the other at any
 * particular size. The UCT distinction is API shape: put_short takes a plain
 * (buffer, length) pair while put_bcopy takes a pack callback that writes
 * directly into the destination. 4096 is a reasonable default; it can be
 * raised further without consequence. */
#define UCT_PCIE_MAX_PUT_SHORT 4096

/* Upper bound for the DMA queue pool (config DMA_QUEUES). Each queue carries
 * one transfer at a time; on the MXH930 several queues in flight overlap the
 * ~7 us per-transfer start-up (64 KiB transfers: 6.5 GB/s with one queue,
 * 21.4 GB/s with eight). */
#define UCT_PCIE_MAX_DMA_QUEUES 32

/* Iface address exchanged OOB before EP creation. Remote memory is named by
 * rkeys (see uct_pcie_packed_rkey_t), so only the connection interrupt is
 * needed here. */
typedef struct {
    unsigned int interrupt_no;   /* Connection interrupt */
} UCS_S_PACKED uct_pcie_iface_addr_t;

typedef struct {
    unsigned int node_id;
} UCS_S_PACKED uct_pcie_device_addr_t;

/* Size of the lines written with one 64-byte store (uct_pcie_iface_t.store_line) */
#define UCT_PCIE_LINE_SIZE 64

/* Connection handshake: an endpoint waits UCT_PCIE_CONN_WAIT_MS for the answer
 * and sends its request again, up to UCT_PCIE_CONN_ATTEMPTS times. With 16
 * ranks on each of two nodes, one request in MPI_Alltoall was never answered
 * and the endpoint waited forever. */
#define UCT_PCIE_CONN_WAIT_MS  2000
#define UCT_PCIE_CONN_ATTEMPTS 30

/* AM payload carried inside the header line */
#define UCT_PCIE_AM_INLINE_MAX 48

/* AM payload carried in each continuation line (uct_pcie_am_line_t) */
#define UCT_PCIE_AM_LINE_DATA 56

/* Lines (header + continuation) of a message sent without any fence */
#define UCT_PCIE_AM_LINES_MAX 32

/* Largest payload sent as lines: 48 + 31 * 56 = 1784 bytes */
#define UCT_PCIE_AM_LINES_PAYLOAD_MAX \
    (UCT_PCIE_AM_INLINE_MAX + (UCT_PCIE_AM_LINES_MAX - 1) * UCT_PCIE_AM_LINE_DATA)

/* uct_pcie_am_hdr_t.am_flags: payload is in the header line, not after it */
#define UCT_PCIE_AM_FLAG_INLINE UCS_BIT(0)

/* uct_pcie_am_hdr_t.am_flags: payload continues in the lines after the
 * header line, each carrying the message's seq in its last 8 bytes; the
 * message is complete when every line shows it. Lines need no ordering
 * between them, so such messages need no fence (config AM_LINES). */
#define UCT_PCIE_AM_FLAG_LINES  UCS_BIT(1)

/* Push out lines stored without a fence (see uct_pcie_iface_t.wc_dirty) */
#define uct_pcie_iface_wc_flush(_iface) \
    do { \
        if ((_iface)->wc_dirty) { \
            SCIFlush(NULL, SCI_FLAG_FLUSH_CPU_BUFFERS_ONLY); \
            (_iface)->wc_dirty = 0; \
        } \
    } while (0)

/* Ack counter the receiver writes into the sender's control segment, one line
 * per endpoint. Like the AM header, written as one 64-byte line with the
 * value in the last 8 bytes. */
typedef struct {
    uint8_t           reserved[UCT_PCIE_LINE_SIZE - sizeof(uint64_t)];
    volatile uint64_t ep_conn_ack;
} UCS_V_ALIGNED(UCT_PCIE_LINE_SIZE) uct_pcie_ctl_t;

/* First line of an AM ring slot. The sender writes it last, with a single
 * 64-byte store, after any payload beyond the line (slot + 64) has been
 * fenced. The receiver polls seq, the last 8 bytes: once it equals the
 * expected sequence number, the whole message is there. A one-line message
 * (payload up to UCT_PCIE_AM_INLINE_MAX bytes) needs no fence at all; each
 * fence (SCIFlush, sfence) costs ~0.21 us, a 64-byte store ~0.004 us. */
typedef struct {
    uint8_t           am_id;
    uint8_t           am_flags;
    uint8_t           reserved[2];
    uint32_t          am_length;
    uint8_t           data[UCT_PCIE_AM_INLINE_MAX];
    volatile uint64_t seq;
} UCS_V_ALIGNED(UCT_PCIE_LINE_SIZE) uct_pcie_am_hdr_t;

/* Continuation line of a UCT_PCIE_AM_FLAG_LINES message */
typedef struct {
    uint8_t           data[UCT_PCIE_AM_LINE_DATA];
    volatile uint64_t seq;
} UCS_V_ALIGNED(UCT_PCIE_LINE_SIZE) uct_pcie_am_line_t;

/* Write one 64-byte line (src aligned to 64) to a remote mapping such that
 * its last 8 bytes cannot become visible before the rest */
typedef void (*uct_pcie_store_line_func_t)(volatile void *dst, const void *src);

/* Copy whole 64-byte lines (dst aligned to 64, src any alignment) to a remote
 * mapping, in no particular order */
typedef void (*uct_pcie_store_lines_func_t)(volatile void *dst, const void *src,
                                            size_t lines);

typedef enum {
    UCT_PCIE_CD_AVAILABLE,
    UCT_PCIE_CD_RESERVED,
    UCT_PCIE_CD_READY,
} conn_desc_status_t;

/*
 * Connection Descriptor.
 *
 * Each incoming connection gets its own window (AM ring) of the iface's
 * receive segment, the window of the cd's index.
 */
typedef struct {
    conn_desc_status_t      cd_status;
    /* Who connected (uct_pcie_conn_req_t): a repeated request gets the same
     * slot again, see uct_pcie_conn_handler */
    int                     remote_node;
    int                     remote_ctl_segment_id;
    int                     remote_ep_conn_index;
    uint64_t                ep_conn_last_ack;
    /* Last ack value written to the sender's control line */
    uint64_t                ep_conn_sent_ack;

    /* This connection's window of the iface's recv_segment: the ring buffer
     * the remote EP writes into. */
    uint8_t*                packet_queue_buf;

    /* Control segment (used by iface to send signals to endpoints).
     * Currently, the only signals are ack messages */
    uint32_t                ctl_segment_id;
    sci_remote_segment_t    ctl_segment;
    sci_map_t               ctl_segment_map;
    uct_pcie_ctl_t*          ctl_buf;
} uct_pcie_conn_desc_t;

typedef struct {
    int     node_id;
    int     interrupt;
    int     ctl_segment_id;
    int     ep_conn_index;
} uct_pcie_conn_req_t;

typedef struct {
    unsigned int segment_id;
    /* Offset of the connection's window (AM ring) in the segment */
    uint64_t     offset;
} uct_pcie_conn_ans_t;

typedef struct {
    uct_iface_config_t    super;
    /* Size of packet in bytes (Maximal send size) */
    size_t                packet_size_bytes;
    /* Number of packets in the packet queue */
    unsigned int          packet_queue_len;
    /* Maximum number of endpoints supported by the iface */
    unsigned int          max_eps;
    /* Number of DMA queues, i.e. zero-copy transfers in flight */
    unsigned int          dma_queues;
    /* put_zcopy up to this size is written with CPU stores, not DMA */
    size_t                put_pio_max;
    /* Gets up to this size are read with CPU loads (get_bcopy), not DMA */
    size_t                get_pio_max;
    /* Largest single put/get zero-copy transfer; UCP splits larger ones */
    size_t                zcopy_max;
    /* Most lines (header + continuation) of a fence-free AM */
    unsigned int          am_lines;
    /* DMA engine time per transfer beyond the data, for UCP's estimates */
    double                dma_overhead;
} uct_pcie_iface_config_t;

struct uct_pcie_ep;

/* One DMA queue of the iface pool and the zero-copy operation it carries */
typedef struct {
    sci_dma_queue_t     queue;
    uct_completion_t   *comp;
    struct uct_pcie_ep *ep;
    /* Get through the staging buffer (see uct_pcie_ep_get_zcopy): copy
     * copy_len bytes from copy_src to copy_dst when the transfer is done */
    void               *copy_dst;
    const void         *copy_src;
    size_t              copy_len;
} uct_pcie_dma_t;

/* The DMA engine runs at 27.4 GB/s only if source and destination have the
 * same offset modulo this; otherwise at 4.5 GB/s (any other offset, measured
 * with MPI_Put of 4 MiB) */
#define UCT_PCIE_DMA_ALIGN 64

/* get_bcopy requests queued for one round of CPU loads (see
 * uct_pcie_iface_progress_gets) */
#define UCT_PCIE_GET_QUEUE 64

/* A get_bcopy waiting for its CPU loads */
typedef struct {
    struct uct_pcie_ep     *ep;
    uct_unpack_callback_t   unpack_cb;
    void                   *arg;
    const volatile uint8_t *src;    /* line-aligned start in the mapping */
    size_t                  head;   /* payload offset in the first line */
    size_t                  length;
    uct_completion_t       *comp;
} uct_pcie_get_req_t;

/* An ep flush that waits for the endpoint's outstanding DMA transfers */
typedef struct {
    ucs_queue_elem_t    queue;
    uct_completion_t   *comp;
    struct uct_pcie_ep *ep;
} uct_pcie_flush_req_t;

typedef struct {
    uct_pcie_am_hdr_t              super;
    /* Local UCT completion object */
    uct_completion_t              *comp;
    /* Current IOV index */
    size_t                        iov_index;
    /* Number of IOVs that should be sent */
    size_t                        iov_cnt;
    /* IOVs that should be sent */
    struct iovec                  iov[0];
} uct_pcie_ep_zcopy_tx_t;

typedef struct {
    uct_base_iface_t            super;

    /* Local SISCI node ID, obtained from SCIGetLocalNodeId at init time. */
    unsigned int                device_addr;

    /* Per-slot byte size for the AM ring buffer. Each AM send occupies one
     * slot; the slot holds a uct_pcie_am_hdr_t followed by the payload. */
    size_t                      packet_size_bytes;
    /* Number of ring-buffer slots per endpoint connection. */
    uint32_t                    packet_queue_len;
    /* Received messages after which an ack is written even mid-drain */
    uint32_t                    ack_batch;
    /* 64-byte line store: AVX-512 where available, else two fences */
    uct_pcie_store_line_func_t  store_line;
    /* Payload copy to the remote ring. 64-byte stores halve the cost of
     * glibc memcpy into the mapping for 64 B - 4 KiB (1 KiB: 0.21 vs 0.41 us
     * per message including the fence). */
    uct_pcie_store_lines_func_t store_lines;
    /* get_bcopy: 64-byte streaming loads from the remote mapping into
     * get_buf, one slot of get_slot bytes per queued request and one more
     * for gets done at once; get_pio_max is 0 without AVX-512 (plain loads
     * are 2.6 us per 8 bytes) */
    void (*load_lines)(void *dst, const volatile void *src, size_t lines);
    size_t                      get_pio_max;
    size_t                      get_slot;
    uint8_t                    *get_buf;
    uct_pcie_get_req_t          gets[UCT_PCIE_GET_QUEUE];
    unsigned int                num_gets;
    int                         gets_busy;
    /* Lines were stored without a fence since the last one. A full line
     * normally leaves the write-combining buffers on its own, but the last
     * lines of a burst can stay there until something forces them out: a
     * stream of 1 KiB line messages averaged 2.3 us each instead of 0.07.
     * Progress, flush and a full ring therefore fence when this is set. */
    int                         wc_dirty;
    /* am_bcopy packs here first: pack callbacks use memcpy */
    void                       *bcopy_buf;
    /* Line messages: payload gathered on send, lines built on send, payload
     * reassembled on receive */
    void                       *lines_tx_buf;
    uct_pcie_am_line_t         *lines_tx;
    void                       *lines_rx_buf;
    /* Maximum number of simultaneously connected endpoints. */
    unsigned int                max_eps;

    /* DMA engine for put/get zero-copy between registered memory (memh) and
     * remote registered memory (rkey). Transfers are asynchronous: a queue
     * is taken from the free list, the transfer started, and progress polls
     * the queue state and completes the operation. */
    uct_pcie_dma_t              dmas[UCT_PCIE_MAX_DMA_QUEUES];
    /* Registered staging buffer for gets whose local buffer is not aligned
     * like the remote one (UCT_PCIE_DMA_ALIGN): one slot of
     * zcopy_max + UCT_PCIE_DMA_ALIGN bytes per DMA queue, made on first use */
    uint8_t                    *bounce_buf;
    uct_mem_h                   bounce_memh;
    size_t                      bounce_slot;
    unsigned int                num_dmas;
    unsigned int                free_dmas[UCT_PCIE_MAX_DMA_QUEUES];
    unsigned int                num_free_dmas;
    /* ep flushes waiting for that endpoint's transfers (uct_pcie_flush_req_t) */
    ucs_queue_head_t            flush_reqs;
    sci_desc_t                  vdev_dma;
    size_t                      put_pio_max;
    size_t                      zcopy_max;
    /* Largest AM payload sent as lines (UCT_PCIE_AM_FLAG_LINES) */
    size_t                      am_lines_payload_max;
    double                      dma_overhead;

    /* One descriptor per potential incoming connection, allocated round-robin
     * by the connection handler. */
    uct_pcie_conn_desc_t        sci_cds[UCT_PCIE_MAX_EPS];

    /* Data interrupt that receives incoming connection requests. The interrupt
     * number is published in iface_addr so remote EPs know where to send them. */
    sci_local_data_interrupt_t  interrupt;
    unsigned int                interrupt_no;

    /* SISCI virtual devices, one per segment role, so that each
     * descriptor holds at most one segment of its type per EP connection.
     * SISCI limits a descriptor to 32 resources of the same type; without
     * this split two remote-segment types on one descriptor would halve the
     * EP limit to 16, and local recv segments on the MD descriptor would
     * overflow its local-segment quota at 32 connections.
     * To scale past 32 EPs, turn each of these into an array of descriptors. */
    sci_desc_t                  vdev_recv; /* local recv segment                 */
    sci_desc_t                  vdev_ep;   /* AM receive segment connections     */
    sci_desc_t                  vdev_ctl;  /* control segment connections        */

    /* Protects connections count and sci_cds slot allocation in the
     * connection handler, which runs on SISCI's interrupt thread. */
    pthread_mutex_t             lock;

    /* Arbiter for pending send requests. When an AM send returns
     * UCS_ERR_NO_RESOURCE (flow-control window full), ep_pending_add queues
     * the request here. uct_pcie_iface_progress drains it each tick. */
    ucs_arbiter_t               arbiter;

    /* Monotonically increasing count of EPs ever created on this iface.
     * Used to assign each new EP a unique ep_conn_index. */
    unsigned int                eps_init_cnt;
    /* Number of currently active incoming connections. */
    volatile unsigned int       connections;

    /* Receive segment on registered process memory: one AM ring per
     * connection slot (sci_cds index). One segment per iface, not one per
     * connection: local segments are limited per node (about 1138 on
     * fpga1614, all processes together), and per-connection segments for 16
     * processes on each of two nodes already took 256 of them. */
    sci_local_segment_t         recv_segment;
    unsigned int                recv_segment_id;
    uint8_t*                    recv_buf;

    /* Control segment: a local segment that remote EPs map and write
     * acknowledgement counters into. One uct_pcie_ctl_t slot per EP. */
    unsigned int                ctl_segment_id;
    sci_local_segment_t         ctl_segment;
    sci_map_t                   ctl_segment_map;
    uct_pcie_ctl_t*             ctls;
} uct_pcie_iface_t;



#endif