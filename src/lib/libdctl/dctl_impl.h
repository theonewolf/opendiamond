#ifndef	_DCTL_IMPL_H_
#define	_DCTL_IMPL_H_	1



typedef struct dctl_leaf {
	LIST_ENTRY(dctl_leaf)	leaf_link;
	char *			        leaf_name;
	dctl_data_type_t	    leaf_type;
	dctl_write_fn		    leaf_write_cb;
	dctl_read_fn		    leaf_read_cb;
	void *			        leaf_cookie;
} dctl_leaf_t;

typedef enum {
        DCTL_NODE_LOCAL,
        DCTL_NODE_FWD,
} dctl_node_type_t;



typedef struct dctl_node {
    dctl_node_type_t                    node_type;
	LIST_ENTRY(dctl_node)	            node_link;
	LIST_HEAD(node_children, dctl_node) node_children;
	LIST_HEAD(node_leafs, dctl_leaf)    node_leafs;
	char *			                    node_name;
    void *                              node_cookie;
    dctl_fwd_rleaf_fn                   node_rleaf_cb;
    dctl_fwd_wleaf_fn                   node_wleaf_cb;
    dctl_fwd_lnodes_fn                  node_lnodes_cb;
    dctl_fwd_lleafs_fn                  node_lleafs_cb;
} dctl_node_t;

	
#endif	/* !defined(_DCTL_IMPL_H_) */


