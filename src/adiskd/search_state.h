/*
 *
 *
 *                          Diamond 1.0
 * 
 *            Copyright (c) 2002-2004, Intel Corporation
 *                         All Rights Reserved
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 * 
 *    * Redistributions of source code must retain the above copyright
 *      notice, this list of conditions and the following disclaimer.
 *
 *    * Redistributions in binary form must reproduce the above
 *      copyright notice, this list of conditions and the following
 *      disclaimer in the documentation and/or other materials provided
 *      with the distribution.
 *
 *    * Neither the name of Intel nor the names of its contributors may
 *      be used to endorse or promote products derived from this software 
 *      without specific prior written permission.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef _SEARCH_STATE_H_
#define _SEARCH_STATE_H_


/* some of the default constants for packet processing */
#define	SSTATE_DEFAULT_PEND_MAX	100


enum split_types_t {
	SPLIT_TYPE_FIXED = 0,	/* Defined fixed ratio of work */
	SPLIT_TYPE_DYNAMIC	/* use dynamic optimization */
};


#define	SPLIT_DEFAULT_BP_THRESH	15
#define	SPLIT_DEFAULT_TYPE		(SPLIT_TYPE_FIXED)
#define	SPLIT_DEFAULT_RATIO		(78)
#define	SPLIT_DEFAULT_AUTO_STEP		5
#define	SPLIT_DEFAULT_PEND_LOW		200
#define	SPLIT_DEFAULT_MULT		20
#define	SPLIT_DEFAULT_PEND_HIGH		10

#define DEV_FLAG_RUNNING                0x01
#define DEV_FLAG_COMPLETE               0x02



typedef struct search_state {
    void           *comm_cookie;	/* cookie from the communication lib */
    pthread_t       thread_id;	
    unsigned int    flags;
    struct odisk_state *ostate;
    struct ceval_state *cstate;
    int             ver_no;
    ring_data_t    *control_ops;
    pthread_mutex_t log_mutex;
    pthread_cond_t  log_cond;
    pthread_t       log_thread;
    pthread_t       bypass_id;
    filter_data_t  *fdata;
    uint            obj_total;
    uint            obj_processed;
    uint            obj_dropped;
    uint            obj_passed;
    uint            obj_skipped;
    uint            network_stalls;
    uint            tx_full_stalls;
    uint            tx_idles;
    uint            pend_objs;
    float	    pend_compute;
    uint            pend_max;
    uint            split_type;		/* policy for the splitting */
    uint            split_ratio;	/* amount of computation to do local */
    uint            split_mult;		/* multiplier for queue size */
    uint            split_auto_step;	/* step to increment ration by */
    uint            split_bp_thresh;	/* below, not enough work for host */
    uint	    avg_int_ratio;	/* average ratio for this run */
    uint	    smoothed_int_ratio;	/* integer smoothed ratio */
    float	    smoothed_ratio;	/* smoothed value */
    uint	    old_proc;		/* last number run */
    float	    avg_ratio;	        /* floating point avg ratio */
    void           *dctl_cookie;
    void           *log_cookie;
    unsigned char  *sig;
} search_state_t;



/*
 * Function prototypes for the search functions.
 */

int             search_new_conn(void *cookie, void **app_cookie);
int             search_close_conn(void *app_cookie);
int             search_start(void *app_cookie, int gen_num);
int             search_stop(void *app_cookie, int gen_num);
int             search_set_searchlet(void *app_cookie, int gen_num,
                                     char *filter, char *spec);
int             search_set_list(void *app_cookie, int gen_num);
int             search_term(void *app_cookie, int gen_num);
void            search_get_stats(void *app_cookie, int gen_num);
int             search_release_obj(void *app_cookie, obj_data_t * obj);
int             search_get_char(void *app_cookie, int gen_num);
int             search_log_done(void *app_cookie, char *buf, int len);
int             search_setlog(void *app_cookie, uint32_t level, uint32_t src);
int             search_read_leaf(void *app_cookie, char *path, int32_t opid);
int             search_write_leaf(void *app_cookie, char *path, int len,
                                  char *data, int32_t opid);
int             search_list_nodes(void *app_cookie, char *path, int32_t opid);
int             search_list_leafs(void *app_cookie, char *path, int32_t opid);
int             search_set_gid(void *app_cookie, int gen, groupid_t gid);
int             search_clear_gids(void *app_cookie, int gen);
int             search_set_blob(void *app_cookie, int gen, char *name,
                                int blob_len, void *blob_data);
int             search_set_offload(void *app_cookie, int gen, uint64_t data);

#endif                          /* ifndef _SEARCH_STATE_H_ */
