#ifndef UCT_PCIE_MD_H
#define UCT_PCIE_MD_H

#include <uct/api/uct_def.h>
#include <uct/base/uct_md.h>
#include <ucs/datastruct/list.h>

#include <sisci_api.h>

/* SISCI refuses a 33rd local segment, or a 33rd remote segment connection, on
 * one virtual device descriptor (SCI_ERR_NOSPC, "Out of local resources").
 * Registrations and remote-key connections therefore come from pools of
 * descriptors with at most this many resources each. */
#define UCT_PCIE_RESOURCES_PER_DESC 32

/**
 * @brief Pool of SISCI virtual device descriptors.
 *
 * Hands out a descriptor that still has room for one more resource of the
 * kind the pool is used for, opening a new descriptor when all are full.
 * Descriptors stay open until the pool is destroyed.
 */
typedef struct {
    sci_desc_t   *descs;
    unsigned     *used;
    unsigned      count;
} uct_pcie_desc_pool_t;

/**
 * @brief Memory domain descriptor.
 */
typedef struct {
    uct_md_t             super;
    /* Local SISCI node id, packed into every rkey */
    unsigned int         node_id;
    /* Descriptor for the transport's own segments (AM rings, control) */
    sci_desc_t           sci_virtual_device;
    /* Descriptors for registered memory, one segment per registration */
    uct_pcie_desc_pool_t reg_descs;
    /* Measured cost of SCICreateSegment + SCIRegisterSegmentMemory +
     * SCIPrepareSegment + SCISetSegmentAvailable on malloc'd memory */
    ucs_linear_func_t    reg_cost;
    /* Deregistered segments (uct_pcie_memh_t), removed by the remover thread
     * once no peer is connected any more, see uct_pcie_mem_dereg. busy_lock
     * guards the list, reg_descs and the remover state. */
    ucs_list_link_t      busy_segments;
    pthread_mutex_t      busy_lock;
    pthread_cond_t       busy_cond;
    pthread_t            remover;
    int                  remover_started;
    int                  remover_stop;
    /* Registrations later ones can share (uct_pcie_memh_t), see
     * uct_pcie_mem_reg; shared_lock also guards their refcounts */
    ucs_list_link_t      shared;
    pthread_mutex_t      shared_lock;
    /* REG_SPAN: registrations cover the aligned window of this size around
     * the buffer, within its mapping; 0 when UCM unmap events are missing */
    size_t               reg_span;
} uct_pcie_md_t;

/**
 * @brief Memory handle: a registered range exported as its own SISCI segment.
 *
 * The range covers the caller's buffer and usually more (see
 * uct_pcie_mem_reg), so local and remote offsets are always computed against
 * base, not against the caller's address. Registrations inside the range
 * share the handle.
 */
typedef struct {
    ucs_list_link_t      list;          /* in md->busy_segments after dereg */
    ucs_list_link_t      shared;        /* in md->shared until retired */
    int                  in_shared;
    unsigned int         refcount;      /* registrations sharing the handle */
    sci_local_segment_t  segment;
    unsigned int         segment_id;
    unsigned int         desc_index;
    uintptr_t            base;
    size_t               length;
    /* Remote connections to the segment, counted by the SISCI local segment
     * callback (SCI_CB_CONNECT / SCI_CB_DISCONNECT) */
    volatile int         connections;
} uct_pcie_memh_t;

/**
 * @brief Packed remote key: where the registered range lives.
 */
typedef struct {
    uint32_t             node_id;
    uint32_t             segment_id;
    uint64_t             base;
    uint64_t             length;
} UCS_S_PACKED uct_pcie_packed_rkey_t;

/**
 * @brief Unpacked remote key: a connected and mapped remote segment.
 *
 * Connecting (~120 us) and mapping (~30 us) cost far more than a transfer, so
 * remote segments are cached per (node id, segment id) for the lifetime of
 * the process and shared between all rkeys that name the same segment. A
 * segment id is not reused while we stay connected (the owner cannot remove
 * the segment, see uct_pcie_mem_dereg), so a cached entry never points at
 * the wrong memory. An entry the owner withdrew or lost is marked invalid by
 * the SISCI callback and reconnected by the next unpack of its key.
 *
 * The cache is bounded (UCX_PCIE_RKEY_CACHE): a peer can only remove a
 * segment it deregistered once nobody is connected to it, and local segments
 * are limited per node (about 1138, all processes together), so stale
 * connections must not accumulate.
 */
typedef struct {
    ucs_list_link_t      lru;          /* in the cache, least recently used first */
    uint64_t             key;          /* node id << 32 | segment id */
    sci_remote_segment_t segment;
    sci_map_t            map;
    volatile uint8_t    *va;           /* PIO window onto the remote range */
    uint64_t             base;         /* remote_addr of the first byte */
    uint64_t             length;
    unsigned int         desc_index;
    unsigned int         refcount;
    volatile int         valid;
    int                  cached;       /* still the hash entry for key */
} uct_pcie_rseg_t;

/* Tries per segment before giving up on SCI_ERR_SEGMENTID_USED */
#define UCT_PCIE_SEGMENT_ID_ATTEMPTS 1024

unsigned uct_pcie_md_next_segment_id(void);

#endif
