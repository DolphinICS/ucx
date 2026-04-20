
#include <ucs/type/class.h>
#include <ucs/type/status.h>
#include <ucs/sys/string.h>

#include "stdio.h"

#include "pcie_iface.h"
#include "pcie_md.h"

uct_component_t uct_pcie_component;

#define UCT_PCIE_NAME "pcie"

/**
 * @brief self device MD configuration
 */
typedef struct uct_pcie_md_config {
    uct_md_config_t super;
    size_t          num_devices; /* Number of devices to create */
} uct_pcie_md_config_t;

static void uct_pcie_md_close(uct_md_h md) {
    uct_pcie_md_t * sci_md = ucs_derived_of(md, uct_pcie_md_t);
    sci_error_t sci_error;

    printf("uct_pcie_md_close\n");

    SCIClose(sci_md->sci_virtual_device, 0 , &sci_error);
    if (sci_error != SCI_ERR_OK) {
        ucs_error("Error closing Virtual_Device error: %s",
            SCIGetErrorString(sci_error));
    }
}

static ucs_status_t uct_pcie_md_query(uct_md_h md, uct_md_attr_v2_t *attr)
{
    printf("uct_pcie_md_query\n");
    /* Dummy memory registration provided. No real memory handling exists */
    // attr->flags               = UCT_MD_FLAG_NEED_RKEY | UCT_MD_FLAG_ALLOC;
    attr->flags               = 0;
    attr->max_alloc           = 0;
    attr->reg_mem_types       = 0;
    attr->alloc_mem_types     = 0;
    attr->access_mem_types    = 0;
    attr->detect_mem_types    = 0;
    attr->max_reg             = 0;
    attr->rkey_packed_size    = 0;
    attr->reg_cost = ucs_linear_func_make(1e9, 1e9);
    memset(&attr->local_cpus, 0xff, sizeof(attr->local_cpus));
    return UCS_OK;
}

typedef struct {
    void *ptr;
    size_t length;
} uct_pcie_alloc_handle_t;

// static ucs_status_t uct_pcie_mem_alloc(
//     uct_md_h uct_md,
//     size_t *length_p,
//     void **address_p,
//     ucs_memory_type_t mem_type,
//     unsigned flags,
//     const char *alloc_name,
//     uct_mem_h *memh_p)
// {
//     uct_pcie_alloc_handle_t *alloc_handle;

//     printf("uct_pcie_mem_alloc\n");

//     alloc_handle = ucs_malloc(sizeof(*alloc_handle),
//                               "uct_pcie_mem_alloc");
//     if (NULL == alloc_handle) {
//         ucs_error("failed to allocate memory for uct_pcie_mem_alloc");
//         return UCS_ERR_NO_MEMORY;
//     }

//     alloc_handle->ptr = malloc(*length_p);
//     if (alloc_handle->ptr == NULL) {
//         ucs_error("uct_pcie_mem_alloc, malloc failed");
//         ucs_free(alloc_handle);
//         return UCS_ERR_NO_MEMORY;
//     }

//     alloc_handle->length = *length_p;

//     *memh_p    = alloc_handle;
//     *address_p = (void*)alloc_handle->ptr;
//     return UCS_OK;
// }

// static ucs_status_t uct_pcie_mem_free(uct_md_h md, uct_mem_h memh)
// {
//     uct_pcie_alloc_handle_t *alloc_handle = (uct_pcie_alloc_handle_t*) memh;

//     printf("uct_pcie_mem_free\n");

//     free(alloc_handle->ptr);
//     ucs_free(alloc_handle);
//     return UCS_OK;
// }

// static ucs_status_t uct_pcie_md_rkey_unpack(uct_component_t *component,
//     const void *rkey_buffer, uct_rkey_t *rkey_p,
//     void **handle_p)
// {
//     printf("uct_pcie_md_rkey_unpack\n");
//     /**
//     * Pseudo stub function for the key unpacking
//     * Need rkey == 0 due to work with same process to reuse
//     * uct_base_[put|get|atomic]*
//     */
//     *rkey_p   = 0;
//     *handle_p = NULL;
//     return UCS_OK;
// }

#if defined false

static ucs_status_t uct_pcie_mem_reg(
    uct_md_h md,
    void *address,
    size_t length,
    const uct_md_mem_reg_params_t *params,
    uct_mem_h *memh_p)
{
    /* We have to emulate memory registration. Return dummy pointer */
    *memh_p = (void *) 0xdeadbeef;
    return UCS_OK;
}

static ucs_status_t uct_pcie_mem_dereg(
    uct_md_h uct_md,
    const uct_md_mem_dereg_params_t *params)
{
    UCT_MD_MEM_DEREG_CHECK_PARAMS(params, 0);

    ucs_assert(params->memh == (void*)0xdeadbeef);

    return UCS_OK;
}

#endif

static ucs_status_t uct_pcie_md_open(
    uct_component_t *component,
    const char *md_name,
    const uct_md_config_t *config,
    uct_md_h *md_p)
{
    uct_pcie_md_config_t *md_config =
        ucs_derived_of(config, uct_pcie_md_config_t);

    static uct_md_ops_t md_ops = {
        .close              = uct_pcie_md_close, 
        .query              = uct_pcie_md_query,
        .mem_alloc          = ucs_empty_function_return_unsupported,
        .mem_free           = ucs_empty_function_return_unsupported,
        .mkey_pack          = ucs_empty_function_return_unsupported,
        .mem_reg            = ucs_empty_function_return_unsupported,
        .mem_dereg          = ucs_empty_function_return_unsupported,
        .detect_memory_type = ucs_empty_function_return_unsupported
    };

    
    /* create sci memory domain struct */
    static uct_pcie_md_t md;
    sci_error_t errors;
    printf("uct_pcie_md_open\n");
    SCIOpen(&md.sci_virtual_device, 0, &errors);
    if (errors != SCI_ERR_OK) {
        ucs_error("SCIOpen: %s/n", SCIGetErrorString(errors));
        return UCS_ERR_NO_RESOURCE;
    }
    
    md.super.ops       = &md_ops;
    md.super.component = &uct_pcie_component;
    md.num_devices     = md_config->num_devices;
    
    *md_p = &md.super;
    md_name = "pcie";
    return UCS_OK;
}

uct_component_t uct_pcie_component = {
    .query_md_resources = uct_md_query_single_md_resource, 
    .md_open            = uct_pcie_md_open,
    .cm_open            = ucs_empty_function_return_unsupported,
    .rkey_unpack        = ucs_empty_function_return_unsupported,
    .rkey_ptr           = ucs_empty_function_return_unsupported, 
    .rkey_release       = ucs_empty_function_return_unsupported,
    .rkey_compare       = ucs_empty_function_return_unsupported,
    .name               = UCT_PCIE_NAME,
    .md_config          = UCT_MD_DEFAULT_CONFIG_INITIALIZER,
    .tl_list            = UCT_COMPONENT_TL_LIST_INITIALIZER(&uct_pcie_component),
    .flags              = 0,
    .md_vfs_init        = (uct_component_md_vfs_init_func_t)ucs_empty_function
};
UCT_COMPONENT_REGISTER(&uct_pcie_component)