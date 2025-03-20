#ifndef UCT_SCI_MD_H
#define UCT_SCI_MD_H

#include <uct/api/uct_def.h>
#include <uct/base/uct_md.h>

#include <sisci_api.h>

/**
 * @brief self device MD descriptor
 */
typedef struct {
    uct_md_t super;
    size_t   num_devices; /* Number of devices to create */
    sci_desc_t sci_virtual_device;
} uct_sci_md_t;


#endif