
/**
 * @ingroup UCS_RESOURCE
 * @brief Create an asynchronous execution context
 *
 * Allocate and initialize an asynchronous execution context.
 * This can be used to ensure safe event delivery.
 *
 * @param mode            Indicates whether to use signals or polling threads
 *                        for waiting.
 * @param async_p         Event context pointer to initialize.
 *
 * @return Error code as defined by @ref ucs_status_t.
 */
ucs_status_t ucs_async_context_create(ucs_async_mode_t mode,
                                      ucs_async_context_t **async_p);

/**
 * @ingroup UCT_CONTEXT
 * @brief Create a worker object.
 *
 *  The worker represents a progress engine. Multiple progress engines can be
 * created in an application, for example to be used by multiple threads.
 *  Transports can allocate separate communication resources for every worker,
 * so that every worker can be progressed independently of others.
 *
 * @param [in]  async         Context for async event handlers. Must not be NULL.
 * @param [in]  thread_mode   Thread access mode to the worker and all interfaces
 *                             and endpoints associated with it.
 * @param [out] worker_p      Filled with a pointer to the worker object.
 */
ucs_status_t uct_worker_create(ucs_async_context_t *async,
                               ucs_thread_mode_t thread_mode,
                               uct_worker_h *worker_p);



typedef struct uct_component         *uct_component_h;

/**
 * Defines a UCT component
 */
struct uct_component {
    const char                              name[UCT_COMPONENT_NAME_MAX]; /**< Component name */
    uct_component_query_md_resources_func_t query_md_resources; /**< Query memory domain resources method */
    uct_component_md_open_func_t            md_open;            /**< Memory domain open method */
    uct_component_cm_open_func_t            cm_open;            /**< Connection manager open method */
    uct_component_rkey_unpack_func_t        rkey_unpack;        /**< Remote key unpack method */
    uct_component_rkey_ptr_func_t           rkey_ptr;           /**< Remote key access pointer method */
    uct_component_rkey_release_func_t       rkey_release;       /**< Remote key release method */
    uct_component_rkey_compare_func_t       rkey_compare;       /**< Remote key comparison method */
    ucs_config_global_list_entry_t          md_config;          /**< MD configuration entry */
    ucs_config_global_list_entry_t          cm_config;          /**< CM configuration entry */
    ucs_list_link_t                         tl_list;            /**< List of transports */
    ucs_list_link_t                         list;               /**< Entry in global list of components */
    uint64_t                                flags;              /**< Flags as defined by
                                                                     UCT_COMPONENT_FLAG_xx */
    /**< Memory domain initialize VFS method */
    uct_component_md_vfs_init_func_t        md_vfs_init;
};

/**
 * @ingroup UCT_RESOURCE
 * @brief UCT component attributes field mask
 *
 * The enumeration allows specifying which fields in @ref uct_component_attr_t
 * are present. It is used for backward compatibility support.
 */
enum uct_component_attr_field {
    UCT_COMPONENT_ATTR_FIELD_NAME              = UCS_BIT(0), /**< Component name */
    UCT_COMPONENT_ATTR_FIELD_MD_RESOURCE_COUNT = UCS_BIT(1), /**< MD resource count */
    UCT_COMPONENT_ATTR_FIELD_MD_RESOURCES      = UCS_BIT(2), /**< MD resources array */
    UCT_COMPONENT_ATTR_FIELD_FLAGS             = UCS_BIT(3)  /**< Capability flags */
};


/**
 * @ingroup UCT_RESOURCE
 * @brief UCT component attributes
 *
 * This structure defines the attributes for UCT component. It is used for
 * @ref uct_component_query
 */
typedef struct uct_component_attr {
    /**
     * Mask of valid fields in this structure, using bits from
     * @ref uct_component_attr_field.
     * Fields not specified in this mask will be ignored.
     * Provides ABI compatibility with respect to adding new fields.
     */
    uint64_t               field_mask;

    /** Component name */
    char                   name[UCT_COMPONENT_NAME_MAX];

    /** Number of memory-domain resources */
    unsigned               md_resource_count;

    /**
     * Array of memory domain resources. When used, it should be initialized
     * prior to calling @ref uct_component_query with a pointer to an array,
     * which is large enough to hold all memory domain resource entries. After
     * the call, this array will be filled with information about existing
     * memory domain resources.
     * In order to allocate this array, you can call @ref uct_component_query
     * twice: The first time would only obtain the amount of entries required,
     * by specifying @ref UCT_COMPONENT_ATTR_FIELD_MD_RESOURCE_COUNT in
     * field_mask. Then the array could be allocated with the returned number of
     * entries, and passed to a second call to @ref uct_component_query, this
     * time setting field_mask to @ref UCT_COMPONENT_ATTR_FIELD_MD_RESOURCES.
     */
    uct_md_resource_desc_t *md_resources;

    /**
     * Flags as defined by UCT_COMPONENT_FLAG_xx.
     */
    uint64_t               flags;
} uct_component_attr_t;

/**
 * @ingroup UCT_RESOURCE
 * @brief Query for list of components.
 *
 * Obtain the list of transport components available on the current system.
 *
 * @param [out] components_p      Filled with a pointer to an array of component
 *                                handles.
 * @param [out] num_components_p  Filled with the number of elements in the array.
 *
 * @return UCS_OK if successful, or UCS_ERR_NO_MEMORY if failed to allocate the
 *         array of component handles.
 */
ucs_status_t uct_query_components(uct_component_h **components_p,
                                  unsigned *num_components_p);

/**
 * @ingroup UCT_RESOURCE
 * @brief Get component attributes
 *
 * Query various attributes of a component.
 *
 * @param [in] component          Component handle to query attributes for. The
 *                                handle can be obtained from @ref uct_query_components.
 * @param [inout] component_attr  Filled with component attributes.
 *
 * @return UCS_OK if successful, or nonzero error code in case of failure.
 */
ucs_status_t uct_component_query(uct_component_h component,
                                 uct_component_attr_t *component_attr);



/**
 * @ingroup UCT_RESOURCE
 * @brief Query for transport resources.
 *
 * This routine queries the @ref uct_md_h "memory domain" for communication
 * resources that are available for it.
 *
 * @param [in]  md              Handle to memory domain.
 * @param [out] resources_p     Filled with a pointer to an array of resource
 *                              descriptors.
 * @param [out] num_resources_p Filled with the number of resources in the array.
 *
 * @return Error code.
 */
ucs_status_t uct_md_query_tl_resources(uct_md_h md,
                                       uct_tl_resource_desc_t **resources_p,
                                       unsigned *num_resources_p);


/**
 * @ingroup UCT_MD
 * @brief  Memory domain attributes.
 *
 * This structure defines the attributes of a Memory Domain which includes
 * maximum memory that can be allocated, credentials required for accessing the memory,
 * CPU mask indicating the proximity of CPUs, and bitmaps indicating the types
 * of memory (CPU/CUDA/ROCM) that can be detected, allocated and accessed.
 */
struct uct_md_attr {
    struct {
        uint64_t             max_alloc; /**< Maximal allocation size */
        size_t               max_reg;   /**< Maximal registration size */
        uint64_t             flags;     /**< UCT_MD_FLAG_xx */
        uint64_t             reg_mem_types; /**< Bitmap of memory types that Memory Domain can be registered with */
        uint64_t             detect_mem_types; /**< Bitmap of memory types that Memory Domain can detect if address belongs to it */
        uint64_t             alloc_mem_types;  /**< Bitmap of memory types that Memory Domain can allocate memory on */
        uint64_t             access_mem_types; /**< Memory types that Memory Domain can access */
    } cap;

    ucs_linear_func_t        reg_cost;  /**< Memory registration cost estimation
                                             (time,seconds) as a linear function
                                             of the buffer size. */

    char                     component_name[UCT_COMPONENT_NAME_MAX]; /**< Component name */
    size_t                   rkey_packed_size; /**< Size of buffer needed for packed rkey */
    ucs_cpu_set_t            local_cpus;    /**< Mask of CPUs near the resource */
};


/**
 * @ingroup UCT_MD
 * @brief Query for memory domain attributes.
 *
 * @param [in]  md       Memory domain to query.
 * @param [out] md_attr  Filled with memory domain attributes.
 */
ucs_status_t uct_md_query(uct_md_h md, uct_md_attr_t *md_attr);


/**
 * @ingroup UCT_RESOURCE
 * @brief  List of UCX device types.
 */
typedef enum {
    UCT_DEVICE_TYPE_NET,     /**< Network devices */
    UCT_DEVICE_TYPE_SHM,     /**< Shared memory devices */
    UCT_DEVICE_TYPE_ACC,     /**< Acceleration devices */
    UCT_DEVICE_TYPE_SELF,    /**< Loop-back device */
    UCT_DEVICE_TYPE_LAST
} uct_device_type_t;


/**
 * @ingroup UCT_RESOURCE
 * @brief Communication resource descriptor.
 *
 * Resource descriptor is an object representing the network resource.
 * Resource descriptor could represent a stand-alone communication resource
 * such as an HCA port, network interface, or multiple resources such as
 * multiple network interfaces or communication ports. It could also represent
 * virtual communication resources that are defined over a single physical
 * network interface.
 */
typedef struct uct_tl_resource_desc {
    char                     tl_name[UCT_TL_NAME_MAX];   /**< Transport name */
    char                     dev_name[UCT_DEVICE_NAME_MAX]; /**< Hardware device name */
    uct_device_type_t        dev_type;     /**< The device represented by this resource
                                                (e.g. UCT_DEVICE_TYPE_NET for a network interface) */
    ucs_sys_device_t         sys_device;   /**< The identifier associated with the device
                                                bus_id as captured in ucs_sys_bus_id_t struct */
} uct_tl_resource_desc_t;

/**
 * @ingroup UCT_AM
 * @brief Short io-vector send operation.
 *
 * This routine sends a message using @ref uct_short_protocol_desc "short" protocol.
 * The input data in @a iov array of @ref ::uct_iov_t structures is sent to remote
 * side to contiguous buffer keeping the order of the data in the array.
 *
 * @param [in] ep              Destination endpoint handle.
 * @param [in] id              Active message id. Must be in range 0..UCT_AM_ID_MAX-1.
 * @param [in] iov             Points to an array of @ref ::uct_iov_t structures.
 *                             The @a iov pointer must be a valid address of an array
 *                             of @ref ::uct_iov_t structures. A particular structure
 *                             pointer must be a valid address. A NULL terminated
 *                             array is not required. @a stride and @a count fields in
 *                             @ref ::uct_iov_t structure are ignored in current
 *                             implementation. The total size of the data buffers in
 *                             the array is limited by
 *                             @ref uct_iface_attr_cap_am_max_short
 *                             "uct_iface_attr::cap::am::max_short".
 * @param [in] iovcnt          Size of the @a iov data @ref ::uct_iov_t structures
 *                             array. If @a iovcnt is zero, the data is considered empty.
 *                             @a iovcnt is limited by @ref uct_iface_attr_cap_am_max_iov
 *                             "uct_iface_attr::cap::am::max_iov".
 *
 * @return UCS_OK              Operation completed successfully.
 * @return UCS_ERR_NO_RESOURCE Could not start the operation due to lack of
 *                             send resources.
 * @return otherwise           Error code.
 */
UCT_INLINE_API ucs_status_t uct_ep_am_short_iov(uct_ep_h ep, uint8_t id,
                                                const uct_iov_t *iov, size_t iovcnt);


// Like above but simpler hopefully?
/**
 * @ingroup UCT_AM
 * @brief
 */
UCT_INLINE_API ucs_status_t uct_ep_am_short(uct_ep_h ep, uint8_t id, uint64_t header,
                                            const void *payload, unsigned length);


/**
 * @ingroup UCT_RESOURCE
 * @brief Open a communication interface.
 *
 * @param [in]  md            Memory domain to create the interface on.
 * @param [in]  worker        Handle to worker which will be used to progress
 *                            communications on this interface.
 * @param [in]  params        User defined @ref uct_iface_params_t parameters.
 * @param [in]  config        Interface configuration options. Should be obtained
 *                            from uct_md_iface_config_read() function, or point to
 *                            transport-specific structure which extends uct_iface_config_t.
 * @param [out] iface_p       Filled with a handle to opened communication interface.
 *
 * @return Error code.
 */
ucs_status_t uct_iface_open(uct_md_h md, uct_worker_h worker,
                            const uct_iface_params_t *params,
                            const uct_iface_config_t *config,
                            uct_iface_h *iface_p);

/**
 * @ingroup UCT_RESOURCE
 * @brief Read transport-specific interface configuration.
 *
 * @param [in]  md            Memory domain on which the transport's interface
 *                            was registered.
 * @param [in]  tl_name       Transport name. If @e md supports
 *                            @ref UCT_MD_FLAG_SOCKADDR, the transport name
 *                            is allowed to be NULL. In this case, the configuration
 *                            returned from this routine should be passed to
 *                            @ref uct_iface_open with
 *                            @ref UCT_IFACE_OPEN_MODE_SOCKADDR_SERVER or
 *                            @ref UCT_IFACE_OPEN_MODE_SOCKADDR_CLIENT set in
 *                            @ref uct_iface_params_t.open_mode.
 *                            In addition, if tl_name is not NULL, the configuration
 *                            returned from this routine should be passed to
 *                            @ref uct_iface_open with @ref UCT_IFACE_OPEN_MODE_DEVICE
 *                            set in @ref uct_iface_params_t.open_mode.
 * @param [in]  env_prefix    If non-NULL, search for environment variables
 *                            starting with this UCT_<prefix>_. Otherwise, search
 *                            for environment variables starting with just UCT_.
 * @param [in]  filename      If non-NULL, read configuration from this file. If
 *                            the file does not exist, it will be ignored.
 * @param [out] config_p      Filled with a pointer to configuration.
 *
 * @return Error code.
 */
ucs_status_t uct_md_iface_config_read(uct_md_h md, const char *tl_name,
                                      const char *env_prefix, const char *filename,
                                      uct_iface_config_t **config_p);


/**
 * @ingroup UCT_RESOURCE
 * @brief Enable synchronous progress for the interface
 *
 * Notify the transport that it should actively progress communications during
 * @ref uct_worker_progress().
 *
 * When the interface is created, its progress is initially disabled.
 *
 * @param [in]  iface    The interface to enable progress.
 * @param [in]  flags    The type of progress to enable as defined by
 *                       @ref uct_progress_types
 *
 * @note This function is not thread safe with respect to
 *       @ref ucp_worker_progress(), unless the flag
 *       @ref UCT_PROGRESS_THREAD_SAFE is specified.
 *
 */
UCT_INLINE_API void uct_iface_progress_enable(uct_iface_h iface, unsigned flags);

/**
 * @ingroup UCT_RESOURCE
 * @brief Get interface attributes.
 *
 * @param [in]  iface      Interface to query.
 * @param [out] iface_attr Filled with interface attributes.
 */
ucs_status_t uct_iface_query(uct_iface_h iface, uct_iface_attr_t *iface_attr);

/**
 * @ingroup UCT_AM
 * @brief Set active message handler for the interface.
 *
 * Only one handler can be set of each active message ID, and setting a handler
 * replaces the previous value. If cb == NULL, the current handler is removed.
 *
 *
 * @param [in]  iface    Interface to set the active message handler for.
 * @param [in]  id       Active message id. Must be 0..UCT_AM_ID_MAX-1.
 * @param [in]  cb       Active message callback. NULL to clear.
 * @param [in]  arg      Active message argument.
 * @param [in]  flags    Required @ref uct_cb_flags "callback flags"
 *
 * @return error code if the interface does not support active messages or
 *         requested callback flags
 */
ucs_status_t uct_iface_set_am_handler(uct_iface_h iface, uint8_t id,
                                      uct_am_callback_t cb, void *arg, uint32_t flags);

/**
 * @ingroup UCT_RESOURCE
 * @brief Get address of the device the interface is using.
 *
 *  Get underlying device address of the interface. All interfaces using the same
 * device would return the same address.
 *
 * @param [in]  iface       Interface to query.
 * @param [out] addr        Filled with device address. The size of the buffer
 *                          provided must be at least @ref uct_iface_attr_t::device_addr_len.
 */
ucs_status_t uct_iface_get_device_address(uct_iface_h iface, uct_device_addr_t *addr);


/**
 * @ingroup UCT_RESOURCE
 * @brief Get interface address.
 *
 * requires @ref UCT_IFACE_FLAG_CONNECT_TO_IFACE.
 *
 * @param [in]  iface       Interface to query.
 * @param [out] addr        Filled with interface address. The size of the buffer
 *                          provided must be at least @ref uct_iface_attr_t::iface_addr_len.
 */
ucs_status_t uct_iface_get_address(uct_iface_h iface, uct_iface_addr_t *addr);

/**
 * @ingroup UCT_RESOURCE
 * @brief Check if remote iface address is reachable.
 *
 * This function checks if a remote address can be reached from a local interface.
 * If the function returns true, it does not necessarily mean a connection and/or
 * data transfer would succeed, since the reachability check is a local operation
 * it does not detect issues such as network mis-configuration or lack of connectivity.
 *
 * @param [in]  iface      Interface to check reachability from.
 * @param [in]  dev_addr   Device address to check reachability to. It is NULL
 *                         if iface_attr.dev_addr_len == 0, and must be non-NULL otherwise.
 * @param [in]  iface_addr Interface address to check reachability to. It is
 *                         NULL if iface_attr.iface_addr_len == 0, and must
 *                         be non-NULL otherwise.
 *
 * @return Nonzero if reachable, 0 if not.
 */
int uct_iface_is_reachable(const uct_iface_h iface, const uct_device_addr_t *dev_addr,
                           const uct_iface_addr_t *iface_addr);

/**
 * @ingroup UCT_RESOURCE
 * @brief Create new endpoint.
 *
 * Create a UCT endpoint in one of the available modes:
 * -# Unconnected endpoint: If no any address is present in @ref uct_ep_params,
 *    this creates an unconnected endpoint. To establish a connection to a
 *    remote endpoint, @ref uct_ep_connect_to_ep will need to be called. Use of
 *    this mode requires @ref uct_ep_params_t::iface has the
 *    @ref UCT_IFACE_FLAG_CONNECT_TO_EP capability flag. It may be obtained by
 *    @ref uct_iface_query.
 * -# Connect to a remote interface: If @ref uct_ep_params_t::dev_addr and
 *    @ref uct_ep_params_t::iface_addr are set, this will establish an endpoint
 *    that is connected to a remote interface. This requires that
 *    @ref uct_ep_params_t::iface has the @ref UCT_IFACE_FLAG_CONNECT_TO_IFACE
 *    capability flag. It may be obtained by @ref uct_iface_query.
 * -# Connect to a remote socket address: If @ref uct_ep_params_t::sockaddr is
 *    set, this will create an endpoint that is connected to a remote socket.
 *    This requires that either @ref uct_ep_params::cm, or
 *    @ref uct_ep_params::iface will be set. In the latter case, the interface
 *    has to support @ref UCT_IFACE_FLAG_CONNECT_TO_SOCKADDR flag, which can be
 *    checked by calling @ref uct_iface_query.
 * @param [in]  params  User defined @ref uct_ep_params_t configuration for the
 *                      @a ep_p.
 * @param [out] ep_p    Filled with handle to the new endpoint.
 *
 * @return UCS_OK       The endpoint is created successfully. This does not
 *                      guarantee that the endpoint has been connected to
 *                      the destination defined in @a params; in case of failure,
 *                      the error will be reported to the interface error
 *                      handler callback provided to @ref uct_iface_open
 *                      via @ref uct_iface_params_t.err_handler.
 * @return              Error code as defined by @ref ucs_status_t
 */
ucs_status_t uct_ep_create(const uct_ep_params_t *params, uct_ep_h *ep_p);

/**
 * @ingroup UCT_RESOURCE
 * @brief Get endpoint address.
 *
 * @param [in]  ep       Endpoint to query.
 * @param [out] addr     Filled with endpoint address. The size of the buffer
 *                       provided must be at least @ref uct_iface_attr_t::ep_addr_len.
 */
ucs_status_t uct_ep_get_address(uct_ep_h ep, uct_ep_addr_t *addr);

/**
 * @ingroup UCT_RESOURCE
 * @brief Connect endpoint to a remote endpoint.
 *
 * requires @ref UCT_IFACE_FLAG_CONNECT_TO_EP capability.
 *
 * @param [in] ep           Endpoint to connect.
 * @param [in] dev_addr     Remote device address.
 * @param [in] ep_addr      Remote endpoint address.
 */
ucs_status_t uct_ep_connect_to_ep(uct_ep_h ep, const uct_device_addr_t *dev_addr,
                                  const uct_ep_addr_t *ep_addr);

