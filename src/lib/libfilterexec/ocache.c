/*
 * 	Diamond (Release 1.0)
 *      A system for interactive brute-force search
 *
 *      Copyright (c) 2002-2005, Intel Corporation
 *      All Rights Reserved
 *
 *  This software is distributed under the terms of the Eclipse Public
 *  License, Version 1.0 which can be found in the file named LICENSE.
 *  ANY USE, REPRODUCTION OR DISTRIBUTION OF THIS SOFTWARE CONSTITUTES
 *  RECIPIENT'S ACCEPTANCE OF THIS AGREEMENT
 */

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stddef.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <dirent.h>
#include <assert.h>
#include <sys/uio.h>
#include <limits.h>
#include <openssl/evp.h>

#include "diamond_consts.h"
#include "diamond_types.h"
#include "lib_tools.h"
#include "lib_log.h"
#include "lib_dctl.h"
#include "dctl_common.h"
#include "ocache_priv.h"
#include "obj_attr.h"
#include "obj_attr.h"
#include "lib_filter.h"
#include "lib_filter_sys.h"
#include "lib_odisk.h"
#include "lib_filterexec.h"
#include "lib_ocache.h"
#include "lib_dconfig.h"

static char const cvsid[] = "$Header$";


#define	MAX_FNAME	128
#define TEMP_ATTR_BUF_SIZE	1024

/*
 * dctl variables 
 */
unsigned int    if_cache_table = 0;
unsigned int    if_cache_oattr = 0;
unsigned int    count_thresh = 0;

static int      search_active = 0;
static int      search_done = 0;
static ring_data_t *cache_ring;
static ring_data_t *oattr_ring;


static pthread_mutex_t shared_mutex = PTHREAD_MUTEX_INITIALIZER;
/* queue empty */
static pthread_cond_t fg_data_cv = PTHREAD_COND_INITIALIZER;
/* active */
static pthread_cond_t bg_active_cv = PTHREAD_COND_INITIALIZER;
/* queue full */
static pthread_cond_t bg_queue_cv = PTHREAD_COND_INITIALIZER;

/* queue non-empty */
static pthread_cond_t nem_queue_cv = PTHREAD_COND_INITIALIZER;	
/* queue non empty */
static pthread_cond_t oattr_cv = PTHREAD_COND_INITIALIZER;	
/* queue non empty */
static pthread_cond_t oattr_bg_cv = PTHREAD_COND_INITIALIZER;
/* queue non empty */
static pthread_cond_t wait_lookup_cv = PTHREAD_COND_INITIALIZER;

#define	CACHE_RING_SIZE	128
#define	OATTR_RING_SIZE	128
#define SIG_BUF_SIZE	256
#define MAX_FILTER_ARG_NAME 256
#define CACHE_ENTRY_NUM 4096
#define FCACHE_NUM 50
#define MAX_CACHE_ENTRY_NUM 0X1000000
#define MAX_ENTRY_NUM	(2 * MAX_CACHE_ENTRY_NUM)
#define MAX_IATTR_SIZE	4096

static cache_init_obj **init_table;
static fcache_t *filter_cache_table[FCACHE_NUM];
static int      cache_entry_num = 0;	/* for debug purpose */

static sig_val_t  ocache_sig = {{0,}};
static sig_val_t  oattr_sig = {{0,}};
static char     iattr_buf[MAX_IATTR_SIZE];
static int      iattr_buflen = -1;

static int
search_paths(const char *filename, char *pathbuf)
{
	const char     *envvars[] = { "$DYLD_LIBRARY_PATH",
	                              "$LD_LIBRARY_PATH",
	                              "/usr/lib:/lib", NULL
	                            };
	int             envvar_index;
	const char     *pathspec;
	const char     *element;
	const char     *p;
	char           *q;
	char           *pathbuf_end;
	struct stat     stat_buf;

	pathbuf_end = pathbuf + PATH_MAX - 8;

	for (envvar_index = 0; envvars[envvar_index]; envvar_index++) {
		if (envvars[envvar_index][0] == '$')
			pathspec = getenv(envvars[envvar_index] + 1);
		else
			pathspec = envvars[envvar_index];
		if (pathspec != NULL) {
			element = pathspec;
			while (*element) {
				p = element;
				q = pathbuf;
				while (*p && *p != ':' && q < pathbuf_end)
					*q++ = *p++;
				if (q == pathbuf) {	/* empty element */
					if (*p) {
						element = p + 1;
						continue;
					}
					break;
				}
				if (*p)
					element = p + 1;
				else
					element = p;
				if (*(q - 1) != '/' && q < pathbuf_end)
					*q++ = '/';
				p = filename;
				while (*p && q < pathbuf_end)
					*q++ = *p++;
				*q++ = 0;
				if (q >= pathbuf_end)
					break;
				if (stat(pathbuf, &stat_buf) == 0)
					return (0);
			}
		}
	}
	return (-1);
}

int
digest_cal(char *lib_name, char *filt_name, int numarg, char **filt_args,
           int blob_len, void *blob, sig_val_t *signature)
{
	EVP_MD_CTX      mdctx;
	const EVP_MD   *md;
	unsigned char  *md_value;
	int             md_len = 0, i, len;
	int             lib_fd;
	char            buf[SIG_BUF_SIZE];
	char            pathbuf[PATH_MAX];

	OpenSSL_add_all_digests();
	md = EVP_get_digestbyname("md5");
	if (!md) {
		printf("Unknown message digest md5\n");
		assert(md != NULL);
	}


	if (lib_name == NULL)
		printf("null lib_name\n");
	if (lib_name[0] == '/') {
		lib_fd = open(lib_name, O_RDONLY);
	} else {
		search_paths(lib_name, pathbuf);
		lib_fd = open(pathbuf, O_RDONLY);
	}
	if (lib_fd < 0) {
		printf("fail to open lib file %s, errno %d\n", lib_name, errno);
		assert(0);
		return (EINVAL);
	}

	EVP_MD_CTX_init(&mdctx);
	EVP_DigestInit_ex(&mdctx, md, NULL);

	do {
		len = read(lib_fd, buf, SIG_BUF_SIZE);
		EVP_DigestUpdate(&mdctx, buf, len);
	} while (len > 0);

	close(lib_fd);

	if (filt_name != NULL)
		len = strlen(filt_name);
	else
		len = 0;
	if (len >= MAX_FILTER_FUNC_NAME) {
		/*
		 * XXX error 
		 */
		return (EINVAL);
	}

	EVP_DigestUpdate(&mdctx, filt_name, len);
	for (i = 0; i < numarg; i++) {
		if (filt_args[i] != NULL)
			len = strlen(filt_args[i]);
		else
			len = 0;
		if (len >= MAX_FILTER_ARG_NAME) {
			return (EINVAL);
		}
		EVP_DigestUpdate(&mdctx, filt_args[i], len);
	}
	if (blob_len > 0)
		EVP_DigestUpdate(&mdctx, blob, blob_len);

	md_value = signature->sig;
	EVP_DigestFinal_ex(&mdctx, md_value, &md_len);
	EVP_MD_CTX_cleanup(&mdctx);

	/*
	 * printf("Digest is: "); for(i = 0; i < md_len; i++) printf("%x",
	 * md_value[i]); printf("\n"); 
	 */

	if (md_len == 16)
		return (0);
	else
		return (EINVAL);
}


static int
sig_iattr(cache_attr_set * iattr, sig_val_t * sig)
{
	EVP_MD_CTX      mdctx;
	const EVP_MD   *md;
	unsigned char  *md_value;
	int             md_len = 0, i;
	off_t           buflen = 0;

	OpenSSL_add_all_digests();
	md = EVP_get_digestbyname("md5");
	if (!md) {
		printf("Unknown message digest md5\n");
		assert(md != NULL);
	}

	md_value = sig->sig;

	EVP_MD_CTX_init(&mdctx);
	EVP_DigestInit_ex(&mdctx, md, NULL);

	for (i = 0; i < iattr->entry_num; i++) {
		EVP_DigestUpdate(&mdctx, iattr->entry_data[i]->attr_name,
		                 iattr->entry_data[i]->name_len);
		EVP_DigestUpdate(&mdctx, &iattr->entry_data[i]->attr_sig,
			sizeof(sig_val_t));
		buflen += iattr->entry_data[i]->name_len + 16;
	}

	EVP_DigestFinal_ex(&mdctx, md_value, &md_len);
	EVP_MD_CTX_cleanup(&mdctx);

	if (md_len == 16) {
		return (0);
	} else {
		return (EINVAL);
	}
}


static int
attr_in_set(cache_attr_entry *inattr, cache_attr_set * set)
{
	int	j;
	cache_attr_entry *tattr;

	for (j = 0; j < set->entry_num; j++) {
		tattr = set->entry_data[j];
		if (tattr == NULL) {
			printf("null temp_j, something wrong\n");
			continue;
		}
		if ((tattr->name_len == inattr->name_len) &&
			!strncmp(tattr->attr_name, inattr->attr_name, tattr->name_len)
			&& sig_match(&tattr->attr_sig, &inattr->attr_sig)){
				return(1);
		}
	}
	return(0);	

}

/*
 * This to see if attr1 is a strict subset of attr2, if not, then we
 * return 1, otherwise return 0.
 */

static int
compare_attr_set(cache_attr_set * attr1, cache_attr_set * attr2)
{
	int             i;
	cache_attr_entry *temp_i;

	/*
	 * for each item in attr1, see if it exists in attr2.
	 */
	for (i = 0; i < attr1->entry_num; i++) {
		temp_i = attr1->entry_data[i];
		if (temp_i == NULL) {
			printf("null temp_i, something wrong\n");
			continue;
		}

		/* if this item isn't in the set then return 1 */
		if (attr_in_set(temp_i, attr2) == 0) {
			return 1;
		}
	}
	return 0;
}

int
combine_attr_set(cache_attr_set * attr1, cache_attr_set * attr2)
{
	int             i, j;
	int             found;
	cache_attr_entry *temp_i, *temp_j;
	cache_attr_entry **tmp;

	for (i = 0; i < attr2->entry_num; i++) {
		temp_i = attr2->entry_data[i];
		if (temp_i == NULL) {
			printf("null temp_i, something wrong\n");
			assert(0);
		}
		found = 0;
		for (j = 0; j < attr1->entry_num; j++) {
			temp_j = attr1->entry_data[j];
			if (temp_j == NULL) {
				printf("null temp_j, something wrong\n");
				break;
			}
			if ((temp_i->name_len == temp_j->name_len) &&
			    !strncmp(temp_i->attr_name, temp_j->attr_name,
			             temp_i->name_len)) {
				attr1->entry_data[j] = temp_i;
				found = 1;
				break;
			}
		}
		/*
		 * no found, add to the tail 
		 */
		if (!found) {
			attr1->entry_data[attr1->entry_num] = temp_i;
			attr1->entry_num++;
			if ((attr1->entry_num % ATTR_ENTRY_NUM) == 0) {
				tmp = malloc((attr1->entry_num +
				            ATTR_ENTRY_NUM) * sizeof(char *));
				assert(tmp != NULL);
				memcpy(tmp, attr1->entry_data,
				       attr1->entry_num * sizeof(char *));
				free(attr1->entry_data);
				attr1->entry_data = tmp;
			}
		}
	}
	return 0;
}

static int
ocache_entry_free(cache_obj * cobj)
{
	int             i;

	if (cobj == NULL) {
		return (0);
	}

	for (i = 0; i < cobj->iattr.entry_num; i++) {
		if (cobj->iattr.entry_data[i] != NULL) {
			if (cobj->iattr.entry_data[i]->attr_name != NULL)
				free(cobj->iattr.entry_data[i]->attr_name);
			free(cobj->iattr.entry_data[i]);
		}
	}

	if (cobj->iattr.entry_data != NULL)
		free(cobj->iattr.entry_data);
		for (i = 0; i < cobj->oattr.entry_num; i++) {
		if (cobj->oattr.entry_data[i] != NULL) {
			if (cobj->oattr.entry_data[i]->attr_name != NULL)
				free(cobj->oattr.entry_data[i]->attr_name);
			free(cobj->oattr.entry_data[i]);
		}
	}
	if (cobj->oattr.entry_data != NULL)
		free(cobj->oattr.entry_data);
	free(cobj);
	return (0);
}

int
cache_lookup0(sig_val_t *id_sig, cache_attr_set * change_attr, obj_attr_t *init_attr)
{
	cache_init_obj      *cobj;
	int             found = 0;
	unsigned int    index;

	pthread_mutex_lock(&shared_mutex);
	index = sig_hash(id_sig) % CACHE_ENTRY_NUM;
	cobj = init_table[index];
	while (cobj != NULL) {
		/* if found, update change_attr */
		if (sig_match(&cobj->id_sig, id_sig)) {
			found = 1;
			change_attr->entry_num = cobj->attr.entry_num;
			memcpy(change_attr->entry_data, cobj->attr.entry_data, cobj->attr.entry_num*sizeof(char *));
			break;
		}
		cobj = cobj->next;
	}
	/* if not found, insert entry */
	if( (found == 0 ) && (init_attr != NULL) ) {
		attr_record_t *arec;
		char *buf;
		size_t len;
		void *      cookie;
		int	err;
		cache_init_obj      *p, *q;
		cache_attr_entry *attr_entry;

		err = obj_get_attr_first(init_attr, &buf, &len, &cookie, 0);
		while (err != ENOENT) {
			if( buf == NULL ) {
				printf("can not get attr\n");
				break;
			}
			arec = (attr_record_t *) buf;
			attr_entry = (cache_attr_entry *) malloc(sizeof(cache_attr_entry));
			assert(attr_entry != NULL);
			attr_entry->name_len = arec->name_len;
			attr_entry->attr_name = malloc(arec->name_len);
			assert(attr_entry->attr_name != NULL);
			memcpy(attr_entry->attr_name, arec->data, arec->name_len);
			memcpy(&attr_entry->attr_sig, &arec->attr_sig,
				sizeof(sig_val_t));
			change_attr->entry_data[change_attr->entry_num] = attr_entry;
			change_attr->entry_num++;
			if ((change_attr->entry_num % ATTR_ENTRY_NUM) == 0) {
				cache_attr_entry **tmp;

				tmp = malloc((change_attr->entry_num +
				              ATTR_ENTRY_NUM) * sizeof(char *));
				assert(tmp != NULL);
				memcpy(tmp, change_attr->entry_data,
				       change_attr->entry_num * sizeof(char *));
				free(change_attr->entry_data);
				change_attr->entry_data = tmp;
			}
			err = obj_get_attr_next(init_attr, &buf, &len, &cookie, 0);
		}

		cobj = (cache_init_obj *) malloc( sizeof(*cobj) );
		memcpy(&cobj->id_sig, id_sig, sizeof(sig_val_t));
		cobj->attr.entry_num = change_attr->entry_num;
		cobj->attr.entry_data = malloc(cobj->attr.entry_num * sizeof(char *));
		memcpy(cobj->attr.entry_data, change_attr->entry_data, cobj->attr.entry_num * sizeof(char *) );
		cobj->next = NULL;
		if( init_table[index] == NULL ) {
			init_table[index] = cobj;
		} else {
			p = init_table[index];
			while (p != NULL) {
				q = p;
				p = p->next;
			}
			q->next = cobj;
		}
	}

	pthread_mutex_unlock(&shared_mutex);

	return found;
}

int
cache_lookup(sig_val_t *id_sig, sig_val_t *fsig, void *fcache_table,
             cache_attr_set * change_attr, int *err,
             cache_attr_set ** oattr_set, sig_val_t * iattr_sig)
{
	cache_obj      *cobj;
	int             found = 0;
	unsigned int    index;
	cache_obj     **cache_table = (cache_obj **) fcache_table;

	if (cache_table == NULL)
		return found;
	if (search_done == 1) {
		return (ENOENT);
	}
	pthread_mutex_lock(&shared_mutex);
	index = sig_hash(id_sig) % CACHE_ENTRY_NUM;
	cobj = cache_table[index];
	sig_clear(iattr_sig);

	/*
	 * cache hit if there is a (id_sig, filter sig, input attr sig) match 
	 */
	while (cobj != NULL) {
		if (sig_match(&cobj->id_sig, id_sig)) {
			/*
			 * compare change_attr set with input attr set 
			 */
			if (!compare_attr_set(&cobj->iattr, change_attr)) {
				found = 1;
				*err = cobj->result;
				cobj->ahit_count++;

				memcpy(iattr_sig, &cobj->iattr_sig, 
					sizeof(sig_val_t));
				/*
				 * pass back the output attr set for next evaluation 
				 */
				*oattr_set = &cobj->oattr;
				break;
			} else {
				printf("sig match, but not iattr \n");
				/*
				 * printf("INPUT ATTR SET : \n");
				 * dump_attr_set(&cobj->iattr); printf("CHANGE ATTR SET :
				 * \n"); dump_attr_set(change_attr); 
				 */
			}
		}
		cobj = cobj->next;
	}

	pthread_mutex_unlock(&shared_mutex);

	return found;
}

int
cache_lookup2(sig_val_t *id_sig, sig_val_t *fsig, void *fcache_table,
              cache_attr_set * change_attr, int *conf,
              cache_attr_set ** oattr_set, int *oattr_flag, int flag)
{
	cache_obj      *cobj;
	unsigned int    index;
	int             err = ENOENT;
	unsigned int    oattr_count = 1;
	cache_obj     **cache_table = (cache_obj **) fcache_table;

	if (cache_table == NULL)
		return err;
	if (search_done == 1) {
		return (ENOENT);
	}
	pthread_mutex_lock(&shared_mutex);
	index = sig_hash(id_sig) % CACHE_ENTRY_NUM;
	cobj = cache_table[index];
	*oattr_flag = 0;
	/*
	 * cache hit if there is a (id_sig, filter sig, input attr sig) match 
	 */
	while (cobj != NULL) {
		if (sig_match(&cobj->id_sig, id_sig)) {
			/*
			 * compare change_attr set with input attr set 
			 */
			if (!compare_attr_set(&cobj->iattr, change_attr)) {
				cobj->aeval_count++;
				oattr_count = cobj->eval_count + cobj->aeval_count;
				*oattr_set = &cobj->oattr;
				*conf = cobj->result;
				err = 0;
				break;
			}
		}
		cobj = cobj->next;
	}

	if (if_cache_oattr) {
		if ((oattr_count >= count_thresh) && flag) {
			*oattr_flag = 1;
		}
	}

	pthread_mutex_unlock(&shared_mutex);
	return (err);
}

int
cache_wait_lookup(obj_data_t * lobj, sig_val_t *fsig, void *fcache_table,
                  cache_attr_set * change_attr, cache_attr_set ** oattr_set)
{
	cache_obj      *cobj;
	unsigned int    index;
	cache_obj     **cache_table = (cache_obj **) fcache_table;

	*oattr_set = NULL;
	if (cache_table == NULL) {
		return (ENOENT);
	}
	if (search_done == 1) {
		return (ENOENT);
	}

	if (if_cache_table) {
		/*
		 * XXX we should set a timer here 
		 */
		while (*oattr_set == NULL) {
			pthread_mutex_lock(&shared_mutex);
			index = sig_hash(&lobj->id_sig) % CACHE_ENTRY_NUM;
			cobj = cache_table[index];
			/*
			 * cache hit if there is a (id_sig, filter sig, 
			 * input attr sig) match 
			 */
			while (cobj != NULL) {
				if (sig_match(&cobj->id_sig, &lobj->id_sig)) {
					/*
					 * compare change_attr set with input 
					 * attr set 
					 */
					if (!compare_attr_set(&cobj->iattr, change_attr)) {
						*oattr_set = &cobj->oattr;
						break;
					}
				}
				cobj = cobj->next;
			}
			if (*oattr_set == NULL) {
#ifdef	XXX
				if (cache_entry_num < MAX_ENTRY_NUM) {
					pthread_cond_wait(&wait_lookup_cv, &shared_mutex);
				} else {
					pthread_mutex_unlock(&shared_mutex);
					return (ENOENT);
				}
#else
				pthread_mutex_unlock(&shared_mutex);
				return (ENOENT);
#endif
			}
			pthread_mutex_unlock(&shared_mutex);
		}
		return (0);
	}
	return (ENOENT);
}

static int
time_after(struct timeval *time1, struct timeval *time2)
{
	if (time1->tv_sec > time2->tv_sec)
		return (1);
	if (time1->tv_sec < time2->tv_sec)
		return (0);
	if (time1->tv_usec > time2->tv_usec)
		return (1);
	return (0);
}

static int
ocache_update(int fd, cache_obj ** cache_table, struct stat *stats)
{
	cache_obj      *cobj;
	int             i;
	off_t           size, rsize;
	cache_obj      *p, *q;
	unsigned int    index;
	int             duplicate;

	if (fd < 0) {
		printf("cache file does not exist\n");
		return (EINVAL);
	}
	size = stats->st_size;
	// printf("ocache_update size %ld\n", size);
	rsize = 0;

	pthread_mutex_lock(&shared_mutex);
	while (rsize < size)
	{
		cobj = (cache_obj *) malloc(sizeof(*cobj));
		assert(cobj != NULL);
		read(fd, &cobj->id_sig, sizeof(sig_val_t));
		read(fd, &cobj->iattr_sig, sizeof(sig_val_t));
		read(fd, &cobj->result, sizeof(int));

		read(fd, &cobj->eval_count, sizeof(unsigned short));
		cobj->aeval_count = 0;
		read(fd, &cobj->hit_count, sizeof(unsigned short));
		cobj->ahit_count = 0;
		rsize +=
		    (2 *sizeof(sig_val_t) + sizeof(int) + 2*sizeof(unsigned short));

		read(fd, &cobj->iattr.entry_num, sizeof(unsigned int));
		rsize += sizeof(unsigned int);
		cobj->iattr.entry_data =
		    malloc(cobj->iattr.entry_num * sizeof(char *));
		assert(cobj->iattr.entry_data != NULL);
		for (i = 0; i < cobj->iattr.entry_num; i++) {
			cobj->iattr.entry_data[i] = malloc(sizeof(cache_attr_entry));
			assert(cobj->iattr.entry_data[i] != NULL);
			read(fd, &cobj->iattr.entry_data[i]->name_len,
			     sizeof(unsigned int));
			cobj->iattr.entry_data[i]->attr_name =
			    malloc(cobj->iattr.entry_data[i]->name_len + 1);
			assert((cobj->iattr.entry_data[i]->attr_name) != NULL);
			read(fd, cobj->iattr.entry_data[i]->attr_name,
			     cobj->iattr.entry_data[i]->name_len);
			cobj->iattr.entry_data[i]->attr_name[cobj->iattr.entry_data[i]->name_len] = '\0';
			read(fd, &cobj->iattr.entry_data[i]->attr_sig, 
				sizeof(sig_val_t));
			rsize +=
			    (sizeof(unsigned int) + cobj->iattr.entry_data[i]->name_len +
			     sizeof(sig_val_t));
		}

		read(fd, &cobj->oattr.entry_num, sizeof(unsigned int));
		rsize += sizeof(unsigned int);
		cobj->oattr.entry_data =
		    malloc(cobj->oattr.entry_num * sizeof(char *));
		assert(cobj->oattr.entry_data != NULL);
		for (i = 0; i < cobj->oattr.entry_num; i++) {
			cobj->oattr.entry_data[i] = malloc(sizeof(cache_attr_entry));
			assert(cobj->oattr.entry_data[i] != NULL);
			read(fd, &cobj->oattr.entry_data[i]->name_len,
			     sizeof(unsigned int));
			cobj->oattr.entry_data[i]->attr_name =
			    malloc(cobj->oattr.entry_data[i]->name_len + 1);
			assert(cobj->oattr.entry_data[i]->attr_name != NULL);
			read(fd, cobj->oattr.entry_data[i]->attr_name,
			     cobj->oattr.entry_data[i]->name_len);
			cobj->oattr.entry_data[i]->attr_name[cobj->oattr.entry_data[i]->
			                                     name_len] = '\0';
			read(fd, &cobj->oattr.entry_data[i]->attr_sig, 
				sizeof(sig_val_t));
			rsize +=
			    (sizeof(unsigned int) + cobj->oattr.entry_data[i]->name_len +
			     sizeof(sig_val_t));
		}
		cobj->next = NULL;
		/*
		 * insert it into the cache_table array 
		 */
		index = sig_hash(&cobj->id_sig) % CACHE_ENTRY_NUM;
		if (cache_table[index] == NULL) {
			cache_table[index] = cobj;
			/*
			 * for debug purpose 
			 */
			cache_entry_num++;
			printf("ocache_update cache_entry_num %d\n", cache_entry_num);
		} else {
			p = cache_table[index];
			q = p;
			duplicate = 0;
			while (p != NULL) {
				if (sig_match(&p->id_sig, &cobj->id_sig) &&
				    sig_match(&p->iattr_sig, &cobj->iattr_sig)) {
					if (cobj->eval_count > p->eval_count) {
						p->eval_count += (cobj->eval_count - p->eval_count);
					}
					if (cobj->hit_count > p->hit_count) {
						p->hit_count += (cobj->hit_count - p->hit_count);
					}
					duplicate = 1;
					break;
				}
				q = p;
				p = p->next;
			}
			if (duplicate) {
				ocache_entry_free(cobj);
			} else {
				q->next = cobj;
				/*
				 * for debug purpose 
				 */
				cache_entry_num++;
				printf("ocache_update cache_entry_num %d\n", cache_entry_num);
			}
		}
	}
	pthread_mutex_unlock(&shared_mutex);

	// printf("cache_entry_num %d\n", cache_entry_num);

	return (0);
}

static int
ocache_write_file(char *disk_path, fcache_t * fcache)
{
	char            fpath[PATH_MAX];
	cache_obj      *cobj;
	cache_obj      *tmp;
	int             i, j;
	int             fd;
	int             err;
	cache_obj     **cache_table;
	unsigned int    count;
	struct stat     stats;
	char *		s_str;

	assert(fcache != NULL);
	cache_table = (cache_obj **) fcache->cache_table;
	s_str = sig_string(&fcache->fsig);
	if (s_str == NULL) {
		return(0);
	}
	sprintf(fpath, "%s/%s.%s", disk_path, s_str, CACHE_EXT);
	free(s_str);

	fd = open(fpath, O_CREAT | O_RDWR, 00777);
	if (fd < 0) {
		perror("failed to open cache file\n");
		return (0);
	}
	err = flock(fd, LOCK_EX);
	if (err) {
		perror("failed to lock cache file\n");
		close(fd);
		return (0);
	}
	err = fstat(fd, &stats);
	if (err != 0) {
		perror("failed to stat cache file\n");
		close(fd);
		return (0);
	}

	if (memcmp(&stats.st_mtime, &fcache->mtime, sizeof(time_t))) {
		err = ocache_update(fd, cache_table, &stats);
	}

	close(fd);
	fd = open(fpath, O_CREAT | O_RDWR | O_TRUNC, 00777);
	err = flock(fd, LOCK_EX);
	if (err) {
		perror("failed to lock cache file\n");
		close(fd);
		return (0);
	}

	pthread_mutex_lock(&shared_mutex);
	for (j = 0; j < CACHE_ENTRY_NUM; j++) {
		cobj = cache_table[j];
		while (cobj != NULL) {
			write(fd, &cobj->id_sig, sizeof(sig_val_t));
			write(fd, &cobj->iattr_sig, sizeof(sig_val_t));
			write(fd, &cobj->result, sizeof(int));

			count = cobj->eval_count + cobj->aeval_count;
			write(fd, &count, sizeof(unsigned short));
			count = cobj->hit_count + cobj->ahit_count;
			write(fd, &count, sizeof(unsigned short));

			write(fd, &cobj->iattr.entry_num, sizeof(unsigned int));
			for (i = 0; i < cobj->iattr.entry_num; i++) {
				write(fd, &cobj->iattr.entry_data[i]->name_len,
				      sizeof(unsigned int));
				write(fd, cobj->iattr.entry_data[i]->attr_name,
				      cobj->iattr.entry_data[i]->name_len);
				write(fd, &cobj->iattr.entry_data[i]->attr_sig,
					 sizeof(sig_val_t));
			}

			write(fd, &cobj->oattr.entry_num, sizeof(unsigned int));

			for (i = 0; i < cobj->oattr.entry_num; i++) {
				write(fd, &cobj->oattr.entry_data[i]->name_len,
				      sizeof(unsigned int));
				write(fd, cobj->oattr.entry_data[i]->attr_name,
				      cobj->oattr.entry_data[i]->name_len);
				write(fd, &cobj->oattr.entry_data[i]->attr_sig,
					sizeof(sig_val_t));
			}

			tmp = cobj;
			cobj = cobj->next;
			/*
			 * free 
			 */
			ocache_entry_free(tmp);
			cache_entry_num--;
		}
	}
	for (i = 0; i < CACHE_ENTRY_NUM; i++) {
		cache_table[i] = NULL;
	}
	pthread_mutex_unlock(&shared_mutex);
	close(fd);
	return (0);
}

/*
 * free cache table for unused filters: a very simple LRU 
 */
static int
free_fcache_entry(char *disk_path)
{
	int             i;
	fcache_t       *oldest = NULL;
	int             found = -1;

	// printf("free_fcache_entry\n");
	while (cache_entry_num >= MAX_CACHE_ENTRY_NUM) {
		for (i = 0; i < FCACHE_NUM; i++) {
			if (filter_cache_table[i] == NULL)
				continue;
			if (filter_cache_table[i]->running > 0)
				continue;
			if (oldest == NULL) {
				oldest = filter_cache_table[i];
				found = i;
			} else {
				if (time_after(&oldest->atime, &filter_cache_table[i]->atime)) {
					oldest = filter_cache_table[i];
					found = i;
				}
			}
		}
		if (oldest == NULL) {
			return (-1);
		} else {
			ocache_write_file(disk_path, oldest);
			free(oldest->cache_table);
			free(oldest);
			filter_cache_table[found] = NULL;
			oldest = NULL;
		}
	}
	return (found);
}

static int
ocache_init_read(char *disk_path)
{
	char					fpath[PATH_MAX];
	cache_init_obj		*cobj;
	off_t           	size, rsize;
	int             	fd;
	struct stat     	stats;
	cache_init_obj    *p, *q;
	unsigned int    	index;
	int             	i, err;

	sprintf(fpath, "%s/ATTRSIG", disk_path);

	init_table = (cache_init_obj **) malloc(sizeof(char *) * CACHE_ENTRY_NUM);
	assert(init_table != NULL );
	for (i = 0; i < CACHE_ENTRY_NUM; i++) {
		init_table[i] = NULL;
	}

	fd = open(fpath, O_RDONLY, 00777);
	if (fd < 0) {
		printf("init cache file does not exist\n");
		return (0);
	}

	err = flock(fd, LOCK_EX);
	if (err != 0) {
		perror("failed to lock cache file\n");
		close(fd);
		return (0);
	}
	err = fstat(fd, &stats);
	if (err != 0) {
		perror("failed to stat cache file\n");
		close(fd);
		return (EINVAL);
	}
	size = stats.st_size;

	rsize = 0;
	pthread_mutex_lock(&shared_mutex);
	while (rsize < size) {
		cobj = (cache_init_obj *) malloc(sizeof(*cobj));
		assert(cobj != NULL);
		read(fd, &cobj->id_sig, sizeof(sig_val_t));
		read(fd, &cobj->attr.entry_num, sizeof(unsigned int));
		rsize += (sizeof(sig_val_t) + sizeof(unsigned int));
		cobj->attr.entry_data =
		    malloc(cobj->attr.entry_num * sizeof(char *));
		assert(cobj->attr.entry_data != NULL);
		for (i = 0; i < cobj->attr.entry_num; i++) {
			cobj->attr.entry_data[i] = 
				malloc(sizeof(cache_attr_entry));
			assert(cobj->attr.entry_data[i] != NULL);
			read(fd, &cobj->attr.entry_data[i]->name_len, 	
				sizeof(unsigned int));
			cobj->attr.entry_data[i]->attr_name =
			    malloc(cobj->attr.entry_data[i]->name_len);
			read(fd, cobj->attr.entry_data[i]->attr_name, 
				cobj->attr.entry_data[i]->name_len);
			read(fd, &cobj->attr.entry_data[i]->attr_sig, 
				sizeof(sig_val_t));
			rsize += (sizeof(unsigned int) + 
				cobj->attr.entry_data[i]->name_len + 
				sizeof(sig_val_t));
		}
		cobj->next = NULL;
		index = sig_hash(&cobj->id_sig) % CACHE_ENTRY_NUM;
		if( init_table[index] == NULL ) {
			init_table[index] = cobj;
		} else {
			p = init_table[index];
			while (p != NULL) {
				q = p;
				p = p->next;
			}
			q->next = cobj;
		}
	}
	pthread_mutex_unlock(&shared_mutex);
	close(fd);
	return(0);
}

static int
ocache_init_write(char *disk_path)
{
	char					fpath[PATH_MAX];
	cache_init_obj			*cobj, *tmp;
	int             		fd;
	int             		i, j, err;

	if( init_table == NULL ) {
		return(0);
	}
	sprintf(fpath, "%s/ATTRSIG", disk_path);
	fd = open(fpath, O_CREAT | O_RDWR | O_TRUNC, 00777);
	err = flock(fd, LOCK_EX);
	if (err) {
		perror("failed to lock cache file\n");
		close(fd);
		return (0);
	}
	pthread_mutex_lock(&shared_mutex);
	for (j = 0; j < CACHE_ENTRY_NUM; j++) {
		cobj = init_table[j];
		while (cobj != NULL) {
			write(fd, &cobj->id_sig, sizeof(sig_val_t));
			write(fd, &cobj->attr.entry_num, sizeof(unsigned int));
			for (i = 0; i < cobj->attr.entry_num; i++) {
				write(fd, &cobj->attr.entry_data[i]->name_len,
				      sizeof(unsigned int));
				write(fd, cobj->attr.entry_data[i]->attr_name,
				      cobj->attr.entry_data[i]->name_len);
				write(fd, &cobj->attr.entry_data[i]->attr_sig, 
					sizeof(sig_val_t));
			}
			tmp = cobj;
			cobj = cobj->next;

			for (i = 0; i < tmp->attr.entry_num; i++) {
				if (tmp->attr.entry_data[i] != NULL) {
					if (tmp->attr.entry_data[i]->attr_name != NULL)
						free(tmp->attr.entry_data[i]->attr_name);
					free(tmp->attr.entry_data[i]);
				}
			}
			if(tmp->attr.entry_data != NULL)
				free(tmp->attr.entry_data);
			free(tmp);
		}
	}
	pthread_mutex_unlock(&shared_mutex);
	close(fd);
	return (0);
}

int
ocache_read_file(char *disk_path, sig_val_t *fsig, void **fcache_table,
                 struct timeval *atime)
{
	char            fpath[PATH_MAX];
	cache_obj      *cobj;
	int             i;
	off_t           size, rsize;
	int             fd;
	struct stat     stats;
	cache_obj      *p, *q;
	unsigned int    index;
	fcache_t *	fcache;
	int             err;
	char *		sig_str;
	int             duplicate;
	cache_obj     **cache_table;
	int             filter_cache_table_num = -1;

	*fcache_table = NULL;

	/*
	 * lookup the filter in cached filter array 
	 */
	for (i = 0; i < FCACHE_NUM; i++) {
		if (filter_cache_table[i] == NULL)
			continue;

		if (sig_match(&filter_cache_table[i]->fsig, fsig)) {
			*fcache_table = filter_cache_table[i]->cache_table;
			memcpy(&filter_cache_table[i]->atime, atime,
			       sizeof(struct timeval));
			filter_cache_table[i]->running++;
			return (0);
		}
	}

	/* if not found, try to get a free entry for this filter */
	sig_str = sig_string(fsig);
	if (sig_str == NULL) {
		return(ENOENT);
	}
	/* XXX overflow on buffer*/
	sprintf(fpath, "%s/%s.%s", disk_path, sig_str, CACHE_EXT);
	free(sig_str);

	for (i = 0; i < FCACHE_NUM; i++) {
		if (filter_cache_table[i] == NULL) {
			filter_cache_table_num = i;
			break;
		}
	}

	if ((cache_entry_num > MAX_CACHE_ENTRY_NUM)
	    || (filter_cache_table_num == -1)) {
		err = free_fcache_entry(disk_path);
		if (err < 0) {
			printf("can not find free fcache entry\n");
			return (ENOMEM);
		}
		filter_cache_table_num = err;
	}

	cache_table = (cache_obj **) malloc(sizeof(char *) * CACHE_ENTRY_NUM);
	assert(cache_table != NULL);

	for (i = 0; i < CACHE_ENTRY_NUM; i++) {
		cache_table[i] = NULL;
	}

	fcache = (fcache_t *) malloc(sizeof(fcache_t));
	assert(fcache != NULL);

	filter_cache_table[filter_cache_table_num] = fcache;
	fcache->cache_table = (void *) cache_table;
	memcpy(&fcache->fsig, fsig, sizeof(sig_val_t));

	assert(atime != NULL);
	memcpy(&fcache->atime, atime, sizeof(struct timeval));
	fcache->running = 1;
	*fcache_table = (void *) cache_table;

	fd = open(fpath, O_RDONLY, 00777);
	if (fd < 0) { printf("cache file not found: <%s> \n", fpath);
		memset(&fcache->mtime, 0, sizeof(time_t));
		return (0);
	}
	err = flock(fd, LOCK_EX);
	if (err != 0) {
		perror("failed to lock cache file\n");
		close(fd);
		return (0);
	}
	err = fstat(fd, &stats);
	if (err != 0) {
		perror("failed to stat cache file\n");
		close(fd);
		return (EINVAL);
	}
	size = stats.st_size;
	// printf("ocache_read_file size %ld\n", size);
	memcpy(&fcache->mtime, &stats.st_mtime, sizeof(time_t));
	rsize = 0;

	pthread_mutex_lock(&shared_mutex);
	while (rsize < size) {
		cobj = (cache_obj *) malloc(sizeof(*cobj));
		assert(cobj != NULL);
		read(fd, &cobj->id_sig, sizeof(sig_val_t));
		read(fd, &cobj->iattr_sig, sizeof(sig_val_t));
		read(fd, &cobj->result, sizeof(int));

		read(fd, &cobj->eval_count, sizeof(unsigned short));
		cobj->aeval_count = 0;
		read(fd, &cobj->hit_count, sizeof(unsigned short));
		cobj->ahit_count = 0;
		rsize +=
		    (sizeof(sig_val_t) + sizeof(sig_val_t) + 
			sizeof(int) + 2*sizeof(unsigned short) );

		read(fd, &cobj->iattr.entry_num, sizeof(unsigned int));
		rsize += sizeof(unsigned int);
		cobj->iattr.entry_data =
		    malloc(cobj->iattr.entry_num * sizeof(char *));
		assert(cobj->iattr.entry_data != NULL);
		for (i = 0; i < cobj->iattr.entry_num; i++) {
			cobj->iattr.entry_data[i] = malloc(sizeof(cache_attr_entry));
			assert(cobj->iattr.entry_data[i] != NULL);
			read(fd, &cobj->iattr.entry_data[i]->name_len,
			     sizeof(unsigned int));
			cobj->iattr.entry_data[i]->attr_name =
			    malloc(cobj->iattr.entry_data[i]->name_len + 1);
			assert((cobj->iattr.entry_data[i]->attr_name) != NULL);
			read(fd, cobj->iattr.entry_data[i]->attr_name,
			     cobj->iattr.entry_data[i]->name_len);
			cobj->iattr.entry_data[i]->attr_name[cobj->iattr.entry_data[i]->name_len] = '\0';
			read(fd, &cobj->iattr.entry_data[i]->attr_sig, 
				sizeof(sig_val_t));
			rsize +=
			    (sizeof(unsigned int) + cobj->iattr.entry_data[i]->name_len +
			     sizeof(sig_val_t));
		}

		read(fd, &cobj->oattr.entry_num, sizeof(unsigned int));
		rsize += sizeof(unsigned int);
		cobj->oattr.entry_data =
		    malloc(cobj->oattr.entry_num * sizeof(char *));
		assert(cobj->oattr.entry_data != NULL);
		for (i = 0; i < cobj->oattr.entry_num; i++) {
			cobj->oattr.entry_data[i] = malloc(sizeof(cache_attr_entry));
			assert(cobj->oattr.entry_data[i] != NULL);
			read(fd, &cobj->oattr.entry_data[i]->name_len,
			     sizeof(unsigned int));
			cobj->oattr.entry_data[i]->attr_name =
			    malloc(cobj->oattr.entry_data[i]->name_len + 1);
			assert(cobj->oattr.entry_data[i]->attr_name != NULL);
			read(fd, cobj->oattr.entry_data[i]->attr_name,
			     cobj->oattr.entry_data[i]->name_len);
			cobj->oattr.entry_data[i]->attr_name[cobj->oattr.entry_data[i]->
			                                     name_len] = '\0';
			read(fd, &cobj->oattr.entry_data[i]->attr_sig, 
				sizeof(sig_val_t));
			rsize +=
			    (sizeof(unsigned int) + cobj->oattr.entry_data[i]->name_len +
			     sizeof(sig_val_t));
		}
		cobj->next = NULL;
		/*
		 * insert it into the cache_table array 
		 */
		index = sig_hash(&cobj->id_sig) % CACHE_ENTRY_NUM;
		if (cache_table[index] == NULL) {
			cache_table[index] = cobj;
			/*
			 * for debug purpose 
			 */
			cache_entry_num++;
		} else {
			p = cache_table[index];
			q = p;
			duplicate = 0;
			while (p != NULL) {
				if (sig_match(&p->id_sig, &cobj->id_sig) && 
				    sig_match(&p->iattr_sig, &cobj->iattr_sig)) {
					duplicate = 1;
					break;
				}
				q = p;
				p = p->next;
			}
			if (duplicate) {
				ocache_entry_free(cobj);
			} else {
				q->next = cobj;
				/*
				 * for debug purpose 
				 */
				cache_entry_num++;
			}
		}
	}
	pthread_mutex_unlock(&shared_mutex);
	close(fd);

	printf("cache_entry_num %d\n", cache_entry_num);

	return (0);
}

static int
ocache_lookup_next(cache_ring_entry ** cobj, ocache_state_t * ocache)
{
	pthread_mutex_lock(&shared_mutex);
	while (1) {
		if (!ring_empty(cache_ring)) {
			*cobj = ring_deq(cache_ring);
			pthread_cond_signal(&bg_queue_cv);
			pthread_mutex_unlock(&shared_mutex);
			return (0);
		} else {
			pthread_cond_signal(&nem_queue_cv);
			pthread_cond_wait(&fg_data_cv, &shared_mutex);
		}
	}
}

static int
oattr_lookup_next(oattr_ring_entry ** cobj, ocache_state_t * ocache)
{
	pthread_mutex_lock(&shared_mutex);
	while (1) {
		if (!ring_empty(oattr_ring)) {
			*cobj = ring_deq(oattr_ring);
			pthread_cond_signal(&oattr_bg_cv);
			pthread_mutex_unlock(&shared_mutex);
			return (1);
		} else {
			pthread_cond_wait(&oattr_cv, &shared_mutex);
		}
	}
}

static int
ocache_ring_insert(cache_ring_entry * cobj)
{
	pthread_mutex_lock(&shared_mutex);
	while (1) {
		if (!ring_full(cache_ring)) {
			ring_enq(cache_ring, cobj);
			break;
		} else {
			pthread_cond_wait(&bg_queue_cv, &shared_mutex);
		}
	}
	pthread_cond_signal(&fg_data_cv);
	pthread_mutex_unlock(&shared_mutex);
	return (0);
}


static int
oattr_ring_insert(oattr_ring_entry * cobj)
{
	pthread_mutex_lock(&shared_mutex);
	/*
	 * we do not wait if ring full. just drop it 
	if (!ring_full(oattr_ring)) {
		ring_enq(oattr_ring, cobj);
	} else {
		if (cobj->type == INSERT_OATTR) {
			odisk_release_obj(cobj->u.oattr.obj);
			free(cobj);
		} else if (cobj->type == INSERT_START) {
			free(cobj->u.file_name);
			free(cobj);
		} else {
			free(cobj);
		}
	}
	 */
	while (1) {
		if (!ring_full(oattr_ring)) {
			ring_enq(oattr_ring, cobj);
			break;
		} else {
			pthread_cond_wait(&oattr_bg_cv, &shared_mutex);
		}
	}

	pthread_cond_signal(&oattr_cv);
	pthread_mutex_unlock(&shared_mutex);
	return (0);
}

int
ocache_add_start(char *fhandle, sig_val_t *id_sig, void *cache_table,
                 int lookup, int oattr_flag, sig_val_t *fsig)
{
	cache_ring_entry *new_entry;
	oattr_ring_entry *oattr_entry;

	if (if_cache_table) {
		if ((lookup == ENOENT) && (cache_table != NULL)) {
		    	memcpy(&ocache_sig, id_sig, sizeof(sig_val_t));
			new_entry = (cache_ring_entry *) malloc(sizeof(*new_entry));
			assert(new_entry != NULL);
			new_entry->type = INSERT_START;
		    	memcpy(&new_entry->id_sig, id_sig, sizeof(sig_val_t));
			new_entry->u.start.cache_table = cache_table;
			ocache_ring_insert(new_entry);
		}
	}

	if (if_cache_oattr) {
		if (oattr_flag) {
			memcpy(&oattr_sig, id_sig, sizeof(sig_val_t));
			oattr_entry = (oattr_ring_entry *) malloc(sizeof(*oattr_entry));
			assert(oattr_entry != NULL);
			oattr_entry->type = INSERT_START;
		    	memcpy(&oattr_entry->id_sig, id_sig, sizeof(sig_val_t));
			memcpy(&oattr_entry->u.fsig, fsig, sizeof(sig_val_t));
			iattr_buflen = 0;
			oattr_ring_insert(oattr_entry);
		}
	}

	return (0);
}

static void
ocache_add_iattr(lf_obj_handle_t ohandle,
                 const char *name, off_t len, const char *data)
{
	cache_ring_entry *new_entry;
	unsigned int    name_len;
	int             err;
	obj_data_t     *obj = (obj_data_t *) ohandle;

	if ((if_cache_table) && sig_match(&ocache_sig, &obj->id_sig)) {
		new_entry = (cache_ring_entry *) malloc(sizeof(*new_entry));
		assert(new_entry != NULL);
		new_entry->type = INSERT_IATTR;
		memcpy(&new_entry->id_sig, &obj->id_sig, sizeof(sig_val_t));
		if (name != NULL) {
			name_len = strlen(name);
		} else {
			name_len = 0;
		}
		new_entry->u.iattr.name_len = name_len;
		new_entry->u.iattr.attr_name = malloc(name_len + 1);
		assert(new_entry->u.iattr.attr_name != NULL);
		if (name_len > 0)
			memcpy(new_entry->u.iattr.attr_name, name, name_len);
		new_entry->u.iattr.attr_name[name_len] = '\0';

		err = odisk_get_attr_sig(obj, name, 
				&new_entry->u.iattr.attr_sig);
		if (err) {
			printf("\t\t XXXXXXXXXXXx failed get_attr_size \n");
		}

		{	
			char *sstr;
			sstr = sig_string(&new_entry->u.iattr.attr_sig);
			printf("add : name %s sig %s \n", name, sstr);
			free(sstr);
		}

		if ((if_cache_oattr) && (iattr_buflen >= 0)) {
			if ((iattr_buflen + name_len + sizeof(sig_val_t)) <= MAX_IATTR_SIZE) {
				if (name != NULL)
					memcpy(iattr_buf + iattr_buflen, name, name_len);
				iattr_buflen += name_len;
				memcpy(iattr_buf + iattr_buflen,
				    &new_entry->u.iattr.attr_sig, 
				    sizeof(sig_val_t));
				iattr_buflen += sizeof(sig_val_t);
			} else {
				sig_clear(&oattr_sig);
			}
		}
		ocache_ring_insert(new_entry);
	}

	return;
}

static void
ocache_add_oattr(lf_obj_handle_t ohandle, const char *name,
                 off_t len, const char *data)
{
	cache_ring_entry *new_entry;
	oattr_ring_entry *oattr_entry;
	unsigned int    name_len;
	obj_data_t     *obj = (obj_data_t *) ohandle;
	int             err;


	/*
	 * call function to update stats 
	 */
	ceval_wattr_stats(len);

	if (if_cache_table) {
		if (sig_match(&ocache_sig, &obj->id_sig)) {
			new_entry = (cache_ring_entry *) malloc(sizeof(*new_entry));
			assert(new_entry != NULL);
			new_entry->type = INSERT_OATTR;
			memcpy(&new_entry->id_sig, &obj->id_sig, sizeof(sig_val_t));
			if (name != NULL) {
				name_len = strlen(name);
			} else {
				name_len = 0;
			}
			new_entry->u.oattr.name_len = name_len;
			new_entry->u.oattr.attr_name = malloc(name_len + 1);
			assert(new_entry->u.oattr.attr_name != NULL);
			if (name_len > 0)
				memcpy(new_entry->u.oattr.attr_name, name, name_len);
			new_entry->u.oattr.attr_name[name_len] = '\0';
			err = odisk_get_attr_sig(obj, name,
					 &new_entry->u.oattr.attr_sig);
			ocache_ring_insert(new_entry);
		}
	}

	if ((if_cache_oattr) && sig_match(&oattr_sig, &obj->id_sig)) {
		attr_record_t  *arec;
		if ((name == NULL) || (len < 0)) {
			printf("invalid oattr entry\n");
			return;
		}

		oattr_entry = (oattr_ring_entry *) malloc(sizeof(*oattr_entry));
		assert(oattr_entry != NULL);

		oattr_entry->type = INSERT_OATTR;
		memcpy(&oattr_entry->id_sig, &obj->id_sig, sizeof(sig_val_t));

		arec = odisk_get_arec(obj, name);
		assert(arec != NULL);
		odisk_ref_obj(obj);
		oattr_entry->u.oattr.arec = arec;
		oattr_entry->u.oattr.obj = obj;

		oattr_ring_insert(oattr_entry);
	}
}

int
ocache_add_end(char *fhandle, sig_val_t *id_sig, int conf)
{
	cache_ring_entry *new_entry;
	oattr_ring_entry *oattr_entry;
	sig_val_t	sig;

	if (if_cache_table) {
	    if (sig_match(&ocache_sig, id_sig)) {
			new_entry = (cache_ring_entry *) malloc(sizeof(*new_entry));
			assert(new_entry != NULL);
			new_entry->type = INSERT_END;
			memcpy(&new_entry->id_sig, id_sig, sizeof(sig_val_t));
			new_entry->u.result = conf;
			ocache_ring_insert(new_entry);
			sig_clear(&ocache_sig);
		}
	}

	if ((if_cache_oattr) && sig_match(&oattr_sig, id_sig)) {
		sig_clear(&oattr_sig);
		oattr_entry = (oattr_ring_entry *) malloc(sizeof(*oattr_entry));
		assert(oattr_entry != NULL);
		oattr_entry->type = INSERT_END;
	    	memcpy(&oattr_entry->id_sig, id_sig, sizeof(sig_val_t));
		if (iattr_buflen >= 0) {
			sig_cal(iattr_buf, iattr_buflen, &sig);
			memcpy(&oattr_entry->u.iattr_sig, &sig, 
				sizeof(sig_val_t));
			iattr_buflen = -1;
		}
		oattr_ring_insert(oattr_entry);
	}
	return (0);
}

static void    *
ocache_main(void *arg)
{
	ocache_state_t 	*cstate = (ocache_state_t *) arg;
	int             	err;
	cache_ring_entry 	*tobj;
	cache_obj      		*cobj;
	cache_obj      		*p, *q;
	unsigned int   		index;
	int            		correct;
	sig_val_t		sig;
	cache_obj     		**cache_table;
	cache_attr_entry	**iattr, **oattr, **tmp;
	cache_attr_entry	*attr_entry;

	dctl_thread_register(cstate->dctl_cookie);
	log_thread_register(cstate->log_cookie);


	iattr = malloc(ATTR_ENTRY_NUM * sizeof(char *));
	assert(iattr!=NULL);
	oattr = malloc(ATTR_ENTRY_NUM * sizeof(char *));
	assert(oattr!=NULL);

	while (1) {
		/*
		 * If there is no search don't do anything 
		 */
		pthread_mutex_lock(&shared_mutex);
		while (search_active == 0) {
			err = pthread_cond_wait(&bg_active_cv, &shared_mutex);
		}
		pthread_mutex_unlock(&shared_mutex);

		/* get the next lookup object */
		cache_table = NULL;
		err = ocache_lookup_next(&tobj, cstate);

		if (tobj == NULL) {
			continue;
		}

		if (tobj->type != INSERT_START) {
			free(tobj);
			continue;
		}
		/*
		 * for one thread case, we could do it in this simple way. XXX: do we 
		 * need to change this later? 
		 */
		if (tobj->type == INSERT_START) {
			correct = 0;
			cobj = (cache_obj *) malloc(sizeof(*cobj));
			assert(cobj != NULL);
			memcpy(&cobj->id_sig, &tobj->id_sig, sizeof(sig_val_t));
			cobj->eval_count = 0;
			cobj->aeval_count = 1;
			cobj->hit_count = 0;
			cobj->ahit_count = 1;
			cache_table = (cache_obj **) tobj->u.start.cache_table;
			cobj->iattr.entry_num = 0;
			cobj->oattr.entry_num = 0;
			free(tobj);

			while (1) {
				err = ocache_lookup_next(&tobj, cstate);
				if (tobj->type == INSERT_IATTR) {
					if (!sig_match(&cobj->id_sig, &tobj->id_sig)) {
						if (tobj->u.iattr.attr_name)
							free(tobj->u.iattr.attr_name);
						free(tobj);
						break;
					}
					attr_entry = (cache_attr_entry *) malloc(sizeof(cache_attr_entry));
					assert(attr_entry != NULL);
					attr_entry->name_len = tobj->u.iattr.name_len;
					attr_entry->attr_name = tobj->u.iattr.attr_name;
					memcpy(&attr_entry->attr_sig, &tobj->u.iattr.attr_sig, 
						sizeof(sig_val_t));
					iattr[cobj->iattr.entry_num] = attr_entry;
					cobj->iattr.entry_num++;
					if ((cobj->iattr.entry_num % ATTR_ENTRY_NUM) == 0) {
						tmp =malloc((cobj->iattr.entry_num +
						             ATTR_ENTRY_NUM) * sizeof(char *));
						assert(tmp!=NULL);
						memcpy(tmp, iattr,
						       cobj->iattr.entry_num * sizeof(char *));
						free(iattr);
						iattr = tmp;
					}
					free(tobj);
					continue;
				}
				if (tobj->type == INSERT_OATTR) {
					if (!sig_match(&cobj->id_sig, &tobj->id_sig)) {
						if (tobj->u.oattr.attr_name)
							free(tobj->u.oattr.attr_name);
						free(tobj);
						break;
					}
					attr_entry = (cache_attr_entry *) malloc(sizeof(cache_attr_entry));
					assert(attr_entry != NULL);
					attr_entry->name_len = tobj->u.oattr.name_len;
					attr_entry->attr_name = tobj->u.oattr.attr_name;
					memcpy(&attr_entry->attr_sig, &tobj->u.oattr.attr_sig, 
						sizeof(sig_val_t));
					oattr[cobj->oattr.entry_num] = attr_entry;
					cobj->oattr.entry_num++;
					if ((cobj->oattr.entry_num % ATTR_ENTRY_NUM) == 0) {
						tmp =malloc((cobj->oattr.entry_num +
						             ATTR_ENTRY_NUM) * sizeof(char *));
						assert(tmp!=NULL);
						memcpy(tmp, oattr,
						       cobj->oattr.entry_num * sizeof(char *));
						free(oattr);
						oattr = tmp;
					}
					free(tobj);
					continue;
				}
				if (tobj->type == INSERT_END) {
					if (!sig_match(&cobj->id_sig, &tobj->id_sig)) {
						free(tobj);
						break;
					}
					cobj->result = tobj->u.result;
					correct = 1;
					free(tobj);
					break;
				}
			}
			/*
			 * insert into cache table 
			 */
			if( cobj->iattr.entry_num > 0 ) {
				cobj->iattr.entry_data = malloc(cobj->iattr.entry_num * sizeof(char *));
				assert(cobj->iattr.entry_data != NULL);
				memcpy(cobj->iattr.entry_data, iattr, cobj->iattr.entry_num*sizeof(char *));
			} else {
				cobj->iattr.entry_data = NULL;
			}
			if (cobj->oattr.entry_num > 0) {
				cobj->oattr.entry_data = malloc(cobj->oattr.entry_num * sizeof(char *));
				assert(cobj->oattr.entry_data != NULL);
				memcpy(cobj->oattr.entry_data, oattr, cobj->oattr.entry_num*sizeof(char *));
			} else {
				cobj->oattr.entry_data = NULL;
			}

			if ((correct == 1) && (cache_entry_num < MAX_ENTRY_NUM)) {
				if (cache_table == NULL) {
					printf("invalid entry\n");
					ocache_entry_free(cobj);
					continue;
				}
				sig_iattr(&cobj->iattr, &sig);
				memcpy(&cobj->iattr_sig, &sig, sizeof(sig_val_t));

				cobj->next = NULL;
				index = sig_hash(&cobj->id_sig) % CACHE_ENTRY_NUM;

				pthread_mutex_lock(&shared_mutex);
				if (cache_table[index] == NULL) {
					cache_table[index] = cobj;
				} else {
					p = cache_table[index];
					while (p != NULL) {
						q = p;
						p = p->next;
					}
					q->next = cobj;
				}
				cache_entry_num++;
				pthread_cond_signal(&wait_lookup_cv);
				pthread_mutex_unlock(&shared_mutex);
			} else {
				ocache_entry_free(cobj);
			}
			if (cache_entry_num >= MAX_ENTRY_NUM) {
				free_fcache_entry(cstate->ocache_path);
			}
		}
	}
	free(iattr);
	free(oattr);
}


#define	MAX_VEC_SIZE	10

static void    *
oattr_main(void *arg)
{
	ocache_state_t *cstate = (ocache_state_t *) arg;
	oattr_ring_entry *tobj;
	sig_val_t		id_sig;
	int             fd;
	char            attrbuf[PATH_MAX];
	char            new_attrbuf[PATH_MAX];
	int             err;
	int             correct;
	char *			s_str;
	char *			i_str;

	struct iovec    wvec[MAX_VEC_SIZE];
	obj_data_t     *ovec[MAX_VEC_SIZE];
	int             wcount;
	int             i;

	while (1) {
		/*
		 * If there is no search don't do anything 
		 */
		err = oattr_lookup_next(&tobj, cstate);
		if (err != 1)
			continue;

		if (tobj->type != INSERT_START) {
			if (tobj->type == INSERT_OATTR) {
				odisk_release_obj(tobj->u.oattr.obj);
			}
			free(tobj);
			continue;
		}

		if (tobj->type == INSERT_START) {
			correct = 0;
			memcpy(&id_sig, &tobj->id_sig, sizeof(sig_val_t));
			s_str = sig_string(&tobj->u.fsig);
			i_str = sig_string(&id_sig);
			if (s_str == NULL) {
				continue;
			}
			sprintf(attrbuf, "%s/%s/%s/%s",
			        cstate->ocache_path, CACHE_DIR, s_str, i_str);
			free(s_str);
			free(i_str);

			free(tobj);

			fd = open(attrbuf, O_WRONLY | O_CREAT | O_EXCL, 00777);
			if (fd < 0) {
				if (errno == EEXIST) {
					continue;
				} else {
					perror("error");
					continue;
				}
			}
			err = flock(fd, LOCK_EX);
			if (err) {
				perror("error when locking oattr file\n");
				close(fd);
				continue;
			}

			wcount = 0;
			while (1) {
				err = oattr_lookup_next(&tobj, cstate);
				if (err != 1) {
					printf("something wrong from oattr_lookup_next\n");
					break;
				}
				if (!sig_match(&id_sig, &tobj->id_sig)) {
					free(tobj);
					break;
				}
				if (tobj->type == INSERT_END) {
					/*
					 * write out existing data 
					 */
					if (wcount > 0) {
						err = writev(fd, wvec, wcount);
						assert(err >= 0);
						for (i = 0; i < wcount; i++) {
							odisk_release_obj(ovec[i]);
						}
					}
			
					s_str = sig_string(&tobj->u.iattr_sig);
					if (s_str == NULL) {
						free(tobj);
						break;
					}
					sprintf(new_attrbuf, "%s.%s", attrbuf,
						s_str);
					free(s_str);
					free(tobj);
					correct = 1;
					break;
				}
				if (tobj->type != INSERT_OATTR) {
					printf("something wrong in oattr\n");
					free(tobj);
					break;
				}
				/*
				 * put this record on the list to process 
				 */
				wvec[wcount].iov_base = tobj->u.oattr.arec;
				wvec[wcount].iov_len = tobj->u.oattr.arec->rec_len;
				ovec[wcount] = tobj->u.oattr.obj;
				wcount++;
				free(tobj);


				/*
				 * if the vector is full then flush it 
				 */
				if (wcount == MAX_VEC_SIZE) {
					/*
					 * XXX err ?
					 */
					err = writev(fd, wvec, wcount);
					assert(err >= 0);
					for (i = 0; i < wcount; i++) {
						odisk_release_obj(ovec[i]);
					}
					wcount = 0;
				}
			}

			close(fd);

			if (correct == 0) {
				unlink(attrbuf);
				for (i = 0; i < wcount; i++) {
					odisk_release_obj(ovec[i]);
				}
			} else {
				rename(attrbuf, new_attrbuf);
			}
		}
	}
}

int
ocache_init(char *dirp, void *dctl_cookie, void *log_cookie)
{
	ocache_state_t *new_state;
	int             err;
	char *		dir_path;
	int             i;

	if (dirp == NULL) {
		dir_path = dconf_get_cachedir();
	} else {
		dir_path = dirp;
	}
	if (strlen(dir_path) > (MAX_DIR_PATH - 1)) {
		return (EINVAL);
	}
	err = mkdir(dir_path, 0777);
	if (err && errno != EEXIST) {
		printf("fail to creat cache dir (%s), err %d\n", 
			dir_path, errno);
		return (EPERM);
	}

	/*
	 * dctl control 
	 */
	dctl_register_leaf(DEV_CACHE_PATH, "cache_table", DCTL_DT_UINT32,
		      	dctl_read_uint32, dctl_write_uint32, &if_cache_table);
	dctl_register_leaf(DEV_CACHE_PATH, "cache_oattr", DCTL_DT_UINT32,
	               	dctl_read_uint32, dctl_write_uint32, &if_cache_oattr);
	dctl_register_leaf(DEV_CACHE_PATH, "cache_thresh_hold", DCTL_DT_UINT32,
			dctl_read_uint32, dctl_write_uint32, &count_thresh);
	ring_init(&cache_ring, CACHE_RING_SIZE);


	/* creat output attr ring */
	ring_init(&oattr_ring, OATTR_RING_SIZE);

	new_state = (ocache_state_t *) malloc(sizeof(*new_state));
	assert(new_state != NULL);

	memset(new_state, 0, sizeof(*new_state));

	new_state->dctl_cookie = dctl_cookie;
	new_state->log_cookie = log_cookie;

	/*
	 * set callback functions so we get notifice on read/and writes
	 * to object attributes.
	 */

	lf_set_read_cb(ocache_add_iattr);
	lf_set_write_cb(ocache_add_oattr);

	/* the length has already been tested above */
	strcpy(new_state->ocache_path, dir_path);

	/* initialized the  cache_table */
	for (i = 0; i < FCACHE_NUM; i++) {
		filter_cache_table[i] = NULL;
	}

	sig_cal_init();
	ocache_init_read(dir_path);
	/*
	 * create thread to process inserted entries for cache table 
	 */
	err = pthread_create(&new_state->c_thread_id, PATTR_DEFAULT,
	                     ocache_main, (void *) new_state);

	/*
	 * create thread to process inserted output attrs 
	 */
	err = pthread_create(&new_state->o_thread_id, PATTR_DEFAULT,
	                     oattr_main, (void *) new_state);
	return (0);
}

int
ocache_start()
{
	// printf("ocache_start\n");
	pthread_mutex_lock(&shared_mutex);
	search_active = 1;
	search_done = 0;
	pthread_cond_signal(&bg_active_cv);
	pthread_mutex_unlock(&shared_mutex);
	return (0);
}

/*
 * called by search_close_conn in adiskd/ 
 */
int
ocache_stop(char *dirp)
{
	int             i;
	char *		dir_path;

	if (dirp == NULL) {
		dir_path = dconf_get_cachedir();
	} else {
		dir_path = dirp;
	}

	// printf("ocache_stop\n");
	pthread_mutex_lock(&shared_mutex);
	search_active = 0;
	search_done = 1;
	pthread_mutex_unlock(&shared_mutex);

	for (i = 0; i < FCACHE_NUM; i++) {
		if (filter_cache_table[i] == NULL)
			continue;
		ocache_write_file(dir_path, filter_cache_table[i]);
		free(filter_cache_table[i]->cache_table);
		free(filter_cache_table[i]);
		filter_cache_table[i] = NULL;
	}

	//ocache_init_write(dir_path);
	return (0);
}

/*
 * called by ceval_stop, ceval_stop is called when Stop 
 */
int
ocache_stop_search(sig_val_t *fsig)
{
	int             i;

	for (i = 0; i < FCACHE_NUM; i++) {
		if (filter_cache_table[i] == NULL) {
			continue;
		}
		if (sig_match(&filter_cache_table[i]->fsig, fsig)) {
			filter_cache_table[i]->running--;
		}
	}
	return (0);
}
