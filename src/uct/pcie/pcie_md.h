#ifndef UCT_PCIE_MD_H
#define UCT_PCIE_MD_H

#include <uct/api/uct_def.h>
#include <uct/base/uct_md.h>

#include <sisci_api.h>

/**
 * @brief Memory domain descriptor
 */
typedef struct {
    uct_md_t   super;
    size_t     num_devices;
    sci_desc_t sci_virtual_device;
    uint32_t   node_id; /* local SISCI node ID, needed when packing rkeys */
} uct_pcie_md_t;

/* Local registered memory handle, created by mem_alloc and freed by mem_free.
 * The SISCI segment here is what a remote peer sees as its rseg_cache_entry
 * (uct_pcie_remote_seg_t in pcie_ep.h) when it connects for put/get. */
typedef struct {
    void                *ptr;
    size_t               length;
    sci_local_segment_t  segment;
    sci_map_t            segment_map;
    uint32_t             segment_id;
} uct_pcie_mem_handle_t;


#endif