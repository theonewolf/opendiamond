/*
 *  The OpenDiamond Platform for Interactive Search
 *  Version 3
 *
 *  Copyright (c) 2002-2005 Intel Corporation
 *  Copyright (c) 2007 Carnegie Mellon University  
 *  All rights reserved.
 *
 *  This software is distributed under the terms of the Eclipse Public
 *  License, Version 1.0 which can be found in the file named LICENSE.
 *  ANY USE, REPRODUCTION OR DISTRIBUTION OF THIS SOFTWARE CONSTITUTES
 *  RECIPIENT'S ACCEPTANCE OF THIS AGREEMENT
 */

#ifndef	_LIB_DCTL_H_
#define	_LIB_DCTL_H_	1


#include <diamond_features.h>

#ifdef __cplusplus
extern "C"
{
#endif



typedef enum {
    DCTL_DT_NODE = 1,
    DCTL_DT_UINT32,
    DCTL_DT_UINT64,
    DCTL_DT_STRING,
    DCTL_DT_CHAR
} dctl_data_type_t;

/* the maximum name of any dctl component (node or leaf) */
#define	MAX_DCTL_COMP_NAME	64


typedef	struct {
    dctl_data_type_t	entry_type;
    char 			    entry_name[MAX_DCTL_COMP_NAME];
} dctl_entry_t;


typedef struct {
  dctl_data_type_t  dt;
  int               len;
  char             *dbuf;
} dctl_rleaf_t;

typedef struct {
  int               err;
  int               num_ents;
  dctl_entry_t     *ent_data;
} dctl_lleaf_t;

typedef struct {
  int               err;
  int               num_ents;
  dctl_entry_t     *ent_data;
} dctl_lnode_t;

/*
 * These are the function prototypes that are associated with the
 * read and write operations for the given data node.
 */
typedef int (*dctl_read_fn)(void *cookie, int *data_len, char *data);
typedef int (*dctl_write_fn)(void *cookie, int data_len, char *data);

/*
 * These are the function prototypes for the callback functions used
 * when establishing a "mount" point.
 */
typedef int (*dctl_fwd_rleaf_fn)(char *leaf_name, dctl_data_type_t *dtype,
				 int *len, char *data, void *cookie);
typedef int (*dctl_fwd_wleaf_fn)(char *leaf_name, int len, char *data,
				 void *cookie);
typedef int (*dctl_fwd_lnodes_fn)(char *parent_node, int *num_ents,
	                                  dctl_entry_t *entry_space, void *cookie);
typedef int (*dctl_fwd_lleafs_fn)(char *parent_node, int *num_ents,
				  dctl_entry_t *entry_space, void *cookie);


typedef	struct {
    dctl_fwd_rleaf_fn   dfwd_rleaf_cb;
    dctl_fwd_wleaf_fn   dfwd_wleaf_cb;
    dctl_fwd_lnodes_fn  dfwd_lnodes_cb;
    dctl_fwd_lleafs_fn  dfwd_lleafs_cb;
    void *              dfwd_cookie;
} dctl_fwd_cbs_t;


diamond_public
int dctl_register_node(char *path, char *node_name);
int dctl_unregister_node(char *path, char *node_name);

diamond_public
int dctl_register_leaf(char *path, char *leaf_name,
       dctl_data_type_t dctl_data_t, dctl_read_fn read_cb,
		       dctl_write_fn write_cb, void *cookie);

diamond_public
int dctl_read_leaf(char *leaf_name, dctl_data_type_t *type,
		   int *len, char *data);
diamond_public
int dctl_write_leaf(char *leaf_name, int len, char *data);

diamond_public
int dctl_list_nodes(char *parent_node, int *num_ents, dctl_entry_t *
		    entry_space);

diamond_public
int dctl_list_leafs(char *parent_node, int *num_ents, dctl_entry_t *
		    entry_space);

/*
 * The following are a set of helper functions for reading, writing
 * commoon data types.  The callers can use these are the read and
 * write functions passed to dctl_register_leaf().  The cookie must
 * be the pointer to the data of the appropriate type.
 */
diamond_public
int dctl_read_uint32(void *cookie, int *len, char *data);

diamond_public
int dctl_write_uint32(void *cookie, int len, char *data);

#ifdef __cplusplus
}
#endif


#endif	/* !defined(_LIB_DCTL_H_) */
