#ifndef UCT_PCIE_MD_H
#define UCT_PCIE_MD_H

#include <uct/api/uct_def.h>
#include <uct/base/uct_md.h>

#include <sisci_api.h>

/**
 * @brief Memory domain descriptor.
 *
 * Holds one shared segment from which mem_alloc bump-allocates.
 * The segment ID and base VA are published in uct_pcie_iface_addr_t so
 * remote EPs can connect to it during handshake without any rkey exchange.
 */
typedef struct {
    uct_md_t             super;
    size_t               num_devices;
    sci_desc_t           sci_virtual_device;
    /* RMA segment — pre-allocated at md_open, size UCT_PCIE_RMA_SEG_SIZE.
     * mem_alloc bump-allocates from it; remote EPs connect to it for put/get. */
    sci_local_segment_t  rma_seg;
    sci_map_t            rma_seg_map;
    void                *rma_buf_local;  /* local VA of segment start (this process) */
    unsigned int         rma_seg_id;
    size_t               rma_allocated;  /* bump allocator pointer (bytes used) */
} uct_pcie_md_t;

/* Handle returned by mem_alloc.  Points into the MD's shared segment;
 * mem_free releases only the handle struct, not the underlying segment. */
typedef struct {
    void   *ptr;    /* = md->rma_buf_local + offset */
    size_t  length;
} uct_pcie_mem_handle_t;


#endif