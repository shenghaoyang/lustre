/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 * GPL HEADER START
 *
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 only,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License version 2 for more details (a copy is included
 * in the LICENSE file that accompanied this code).
 *
 * You should have received a copy of the GNU General Public License
 * version 2 along with this program; If not, see [sun.com URL with a
 * copy of GPLv2].
 *
 * Please contact Sun Microsystems, Inc., 4150 Network Circle, Santa Clara,
 * CA 95054 USA or visit www.sun.com if you need additional information or
 * have any questions.
 *
 * GPL HEADER END
 */
/*
 * Copyright  2008 Sun Microsystems, Inc. All rights reserved
 * Use is subject to license terms.
 */
/*
 * This file is part of Lustre, http://www.lustre.org/
 * Lustre is a trademark of Sun Microsystems, Inc.
 *
 * lustre/lov/lov_pool.c
 *
 * OST pool methods
 *
 * Author: Jacques-Charles LAFOUCRIERE <jc.lafoucriere@cea.fr>
 */

#define DEBUG_SUBSYSTEM S_LOV

#ifdef __KERNEL__
#include <libcfs/libcfs.h>
#else
#include <liblustre.h>
#endif

#include <obd.h>
#include "lov_internal.h"

/*
 * hash function using a Rotating Hash algorithm
 * Knuth, D. The Art of Computer Programming,
 * Volume 3: Sorting and Searching,
 * Chapter 6.4.
 * Addison Wesley, 1973
 */
static __u32 pool_hashfn(lustre_hash_t *hash_body, void *key, unsigned mask)
{
        int i;
        __u32 result;
        char *poolname;

        result = 0;
        poolname = (char *)key;
        for (i = 0; i < MAXPOOLNAME; i++) {
                if (poolname[i] == '\0')
                        break;
                result = (result << 4)^(result >> 28) ^  poolname[i];
        }
        return (result % mask);
}

static void *pool_key(struct hlist_node *hnode)
{
        struct pool_desc *pool;

        pool = hlist_entry(hnode, struct pool_desc, pool_hash);
        return (pool->pool_name);
}

static int pool_hashkey_compare(void *key, struct hlist_node *compared_hnode)
{
        char *pool_name;
        struct pool_desc *pool;
        int rc;

        pool_name = (char *)key;
        pool = hlist_entry(compared_hnode, struct pool_desc, pool_hash);
        rc = strncmp(pool_name, pool->pool_name, MAXPOOLNAME);
        return (!rc);
}

static void *pool_hashrefcount_get(struct hlist_node *hnode)
{
        struct pool_desc *pool;

        pool = hlist_entry(hnode, struct pool_desc, pool_hash);
        return (pool);
}

static void *pool_hashrefcount_put(struct hlist_node *hnode)
{
        struct pool_desc *pool;

        pool = hlist_entry(hnode, struct pool_desc, pool_hash);
        return (pool);
}

lustre_hash_ops_t pool_hash_operations = {
        .lh_hash        = pool_hashfn,
        .lh_key         = pool_key,
        .lh_compare     = pool_hashkey_compare,
        .lh_get         = pool_hashrefcount_get,
        .lh_put         = pool_hashrefcount_put,
};

#ifdef LPROCFS
/* ifdef needed for liblustre support */
/*
 * pool /proc seq_file methods
 */
/*
 * iterator is used to go through the target pool entries
 * index is the current entry index in the lp_array[] array
 * index >= pos returned to the seq_file interface
 * pos is from 0 to (pool->pool_obds.op_count - 1)
 */
#define POOL_IT_MAGIC 0xB001CEA0
struct pool_iterator {
        int magic;
        struct pool_desc *pool;
        int idx;        /* from 0 to pool_tgt_size - 1 */
};

static void *pool_proc_next(struct seq_file *s, void *v, loff_t *pos)
{
        struct pool_iterator *iter = (struct pool_iterator *)s->private;
        int prev_idx;

        LASSERTF(iter->magic == POOL_IT_MAGIC, "%08X", iter->magic);

        /* test if end of file */
        if (*pos >= pool_tgt_count(iter->pool))
                return NULL;

        /* iterate to find a non empty entry */
        prev_idx = iter->idx;
        read_lock(&pool_tgt_rwlock(iter->pool));
        iter->idx++;
        if (iter->idx == pool_tgt_count(iter->pool)) {
                iter->idx = prev_idx; /* we stay on the last entry */
                read_unlock(&pool_tgt_rwlock(iter->pool));
                return NULL;
        }
        read_unlock(&pool_tgt_rwlock(iter->pool));
        (*pos)++;
        /* return != NULL to continue */
        return iter;
}

static void *pool_proc_start(struct seq_file *s, loff_t *pos)
{
        struct pool_desc *pool = (struct pool_desc *)s->private;
        struct pool_iterator *iter;

        if ((pool_tgt_count(pool) == 0) ||
            (*pos >= pool_tgt_count(pool)))
                return NULL;

        OBD_ALLOC(iter, sizeof(struct pool_iterator));
        if (!iter)
                return ERR_PTR(-ENOMEM);
        iter->magic = POOL_IT_MAGIC;
        iter->pool = pool;
        iter->idx = 0;

        /* we use seq_file private field to memorized iterator so
         * we can free it at stop() */
        /* /!\ do not forget to restore it to pool before freeing it */
        s->private = iter;
        if (*pos > 0) {
                loff_t i;
                void *ptr;

                i = 0;
                do {
                     ptr = pool_proc_next(s, &iter, &i);
                } while ((i < *pos) && (ptr != NULL));
                return ptr;
        }
        return iter;
}

static void pool_proc_stop(struct seq_file *s, void *v)
{
        struct pool_iterator *iter = (struct pool_iterator *)s->private;

        /* in some cases stop() method is called 2 times, without
         * calling start() method (see seq_read() from fs/seq_file.c)
         * we have to free only if s->private is an iterator */
        if ((iter) && (iter->magic == POOL_IT_MAGIC)) {
                /* we restore s->private so next call to pool_proc_start()
                 * will work */
                s->private = iter->pool;
                OBD_FREE(iter, sizeof(struct pool_iterator));
        }
        return;
}

static int pool_proc_show(struct seq_file *s, void *v)
{
        struct pool_iterator *iter = (struct pool_iterator *)v;
        struct lov_tgt_desc *tgt;

        LASSERTF(iter->magic == POOL_IT_MAGIC, "%08X", iter->magic);
        LASSERT(iter->pool != NULL);
        LASSERT(iter->idx <= pool_tgt_count(iter->pool));

        read_lock(&pool_tgt_rwlock(iter->pool));
        tgt = pool_tgt(iter->pool, iter->idx);
        read_unlock(&pool_tgt_rwlock(iter->pool));
        if (tgt)
                seq_printf(s, "%s\n", obd_uuid2str(&(tgt->ltd_uuid)));

        return 0;
}

static struct seq_operations pool_proc_ops = {
        .start          = pool_proc_start,
        .next           = pool_proc_next,
        .stop           = pool_proc_stop,
        .show           = pool_proc_show,
};

static int pool_proc_open(struct inode *inode, struct file *file)
{
        int rc;

        rc = seq_open(file, &pool_proc_ops);
        if (!rc) {
                struct seq_file *s = file->private_data;
                s->private = PROC_I(inode)->pde->data;
        }
        return rc;
}

static struct file_operations pool_proc_operations = {
        .open           = pool_proc_open,
        .read           = seq_read,
        .llseek         = seq_lseek,
        .release        = seq_release,
};
#endif /* LPROCFS */

void lov_dump_pool(int level, struct pool_desc *pool)
{
        int i;

        CDEBUG(level, "pool "POOLNAMEF" has %d members\n",
               pool->pool_name, pool->pool_obds.op_count);
        read_lock(&pool_tgt_rwlock(pool));
        for (i = 0; i < pool_tgt_count(pool) ; i++) {
                if (!pool_tgt(pool, i) || !(pool_tgt(pool, i))->ltd_exp)
                        continue;
                CDEBUG(level, "pool "POOLNAMEF"[%d] = %s\n", pool->pool_name,
                       i, obd_uuid2str(&((pool_tgt(pool, i))->ltd_uuid)));
        }
        read_unlock(&pool_tgt_rwlock(pool));
}

#define LOV_POOL_INIT_COUNT 2
int lov_ost_pool_init(struct ost_pool *op, unsigned int count)
{
        if (count == 0)
                count = LOV_POOL_INIT_COUNT;
        op->op_array = NULL;
        op->op_count = 0;
        op->op_rwlock = RW_LOCK_UNLOCKED;
        op->op_size = count;
        OBD_ALLOC(op->op_array, op->op_size * sizeof(op->op_array[0]));
        if (op->op_array == NULL) {
                op->op_size = 0;
                return -ENOMEM;
        }
        return 0;
}

int lov_ost_pool_extend(struct ost_pool *op, unsigned int max_count)
{
        __u32 *new;
        int new_size;

        LASSERT(max_count != 0);

        if (op->op_count < op->op_size)
                return 0;

        new_size = min(max_count, 2 * op->op_size);
        OBD_ALLOC(new, new_size * sizeof(op->op_array[0]));
        if (new == NULL)
                return -ENOMEM;

        /* copy old array to new one */
        memcpy(new, op->op_array, op->op_size * sizeof(op->op_array[0]));
        write_lock(&op->op_rwlock);
        OBD_FREE(op->op_array, op->op_size * sizeof(op->op_array[0]));
        op->op_array = new;
        op->op_size = new_size;
        write_unlock(&op->op_rwlock);
        return 0;
}

int lov_ost_pool_add(struct ost_pool *op, __u32 idx, unsigned int max_count)
{
        int rc, i;

        rc = lov_ost_pool_extend(op, max_count);
        if (rc)
                return rc;

        /* search ost in pool array */
        read_lock(&op->op_rwlock);
        for (i = 0; i < op->op_count; i++) {
                if (op->op_array[i] == idx) {
                        read_unlock(&op->op_rwlock);
                        return -EEXIST;
                }
        }
        /* ost not found we add it */
        op->op_array[op->op_count] = idx;
        op->op_count++;
        read_unlock(&op->op_rwlock);
        return 0;
}

int lov_ost_pool_remove(struct ost_pool *op, __u32 idx)
{
        int i;

        read_lock(&op->op_rwlock);
        for (i = 0; i < op->op_count; i++) {
                if (op->op_array[i] == idx) {
                        memmove(&op->op_array[i], &op->op_array[i + 1],
                                (op->op_count - i - 1) * sizeof(op->op_array[0]));
                        op->op_count--;
                        read_unlock(&op->op_rwlock);
                        return 0;
                }
        }
        read_unlock(&op->op_rwlock);
        return -EINVAL;
}

int lov_ost_pool_free(struct ost_pool *op)
{
        if (op->op_size == 0)
                return 0;

        write_lock(&op->op_rwlock);
        OBD_FREE(op->op_array, op->op_size * sizeof(op->op_array[0]));
        op->op_array = NULL;
        op->op_count = 0;
        op->op_size = 0;
        write_unlock(&op->op_rwlock);
        return 0;
}


int lov_pool_new(struct obd_device *obd, char *poolname)
{
        struct lov_obd *lov;
        struct pool_desc *new_pool;
        int rc;

        lov = &(obd->u.lov);

        OBD_ALLOC(new_pool, sizeof(*new_pool));

        if (new_pool == NULL)
                return -ENOMEM;

        if (strlen(poolname) > MAXPOOLNAME)
                return -ENAMETOOLONG;

        strncpy(new_pool->pool_name, poolname, MAXPOOLNAME);
        new_pool->pool_name[MAXPOOLNAME] = '\0';
        new_pool->pool_lov = lov;
        rc = lov_ost_pool_init(&new_pool->pool_obds, 0);
        if (rc)
                return rc;

        memset(&(new_pool->pool_rr), 0, sizeof(struct lov_qos_rr));
        rc = lov_ost_pool_init(&new_pool->pool_rr.lqr_pool, 0);
        if (rc)
                return rc;

        spin_lock(&obd->obd_dev_lock);
        /* check if pool alreaddy exists */
        if (lustre_hash_lookup(lov->lov_pools_hash_body,
                                poolname) != NULL) {
                spin_unlock(&obd->obd_dev_lock);
                lov_ost_pool_free(&new_pool->pool_obds);
                OBD_FREE(new_pool, sizeof(*new_pool));
                return  -EEXIST;
        }

        INIT_HLIST_NODE(&new_pool->pool_hash);
        lustre_hash_add_unique(lov->lov_pools_hash_body, poolname,
                               &new_pool->pool_hash);
        list_add_tail(&new_pool->pool_list, &lov->lov_pool_list);
        lov->lov_pool_count++;
        spin_unlock(&obd->obd_dev_lock);

        CDEBUG(D_CONFIG, POOLNAMEF" is pool #%d\n",
               poolname, lov->lov_pool_count);

#ifdef LPROCFS
        /* ifdef needed for liblustre */
        new_pool->pool_proc_entry = lprocfs_add_simple(lov->lov_pool_proc_entry,
                                                       poolname,
                                                       NULL, NULL,
                                                       new_pool,
                                                       &pool_proc_operations);
#endif

        if (IS_ERR(new_pool->pool_proc_entry)) {
                CWARN("Cannot add proc pool entry "POOLNAMEF"\n", poolname);
                new_pool->pool_proc_entry = NULL;
        }

        return 0;
}

int lov_pool_del(struct obd_device *obd, char *poolname)
{
        struct lov_obd *lov;
        struct pool_desc *pool;

        lov = &(obd->u.lov);

        spin_lock(&obd->obd_dev_lock);
        pool = lustre_hash_lookup(lov->lov_pools_hash_body,
                                             poolname);
        if (pool == NULL) {
                spin_unlock(&obd->obd_dev_lock);
                return -ENOENT;
        }

#ifdef LPROCFS
        if (pool->pool_proc_entry != NULL)
                remove_proc_entry(pool->pool_proc_entry->name,
                                  pool->pool_proc_entry->parent);
#endif

        /* pool is kept in the list to be freed by lov_cleanup()
         * list_del(&pool->pool_list);
         */
        lustre_hash_del_key(lov->lov_pools_hash_body, poolname);

        lov->lov_pool_count--;

        spin_unlock(&obd->obd_dev_lock);

        /* pool struct is not freed because it may be used by
         * some open in /proc
         * the struct is freed at lov_cleanup()
         */
        /*
        if (pool->pool_rr.lqr_size != 0)
                OBD_FREE(pool->pool_rr.lqr_array, pool->pool_rr.lqr_size);
        lov_ost_pool_free(&pool->pool_obds);
        OBD_FREE(pool, sizeof(*pool));
        */
        return 0;
}


int lov_pool_add(struct obd_device *obd, char *poolname, char *ostname)
{
        struct obd_uuid ost_uuid;
        struct lov_obd *lov;
        struct pool_desc *pool;
        unsigned int i, lov_idx;
        int rc;

        lov = &(obd->u.lov);

        pool = lustre_hash_lookup(lov->lov_pools_hash_body, poolname);
        if (pool == NULL) {
                return -ENOENT;
        }

        /* allocate pool tgt array if needed */
        mutex_down(&lov->lov_lock);
        rc = lov_ost_pool_extend(&pool->pool_obds, lov->lov_tgt_size);
        if (rc) {
                mutex_up(&lov->lov_lock);
                return rc;
        }
        mutex_up(&lov->lov_lock);

        obd_str2uuid(&ost_uuid, ostname);

        spin_lock(&obd->obd_dev_lock);

        /* search ost in lov array */
        for (i = 0; i < lov->desc.ld_tgt_count; i++) {
                if (!lov->lov_tgts[i])
                        continue;

                if (obd_uuid_equals(&ost_uuid, &(lov->lov_tgts[i]->ltd_uuid)))
                        break;
        }

        /* test if ost found in lov */
        if (i == lov->desc.ld_tgt_count) {
                spin_unlock(&obd->obd_dev_lock);
                return -EINVAL;
        }

        spin_unlock(&obd->obd_dev_lock);

        lov_idx = i;

        rc = lov_ost_pool_add(&pool->pool_obds, lov_idx, lov->lov_tgt_size);
        if (rc)
                return rc;

        pool->pool_rr.lqr_dirty = 1;

        CDEBUG(D_CONFIG, "Added %s to "POOLNAMEF" as member %d\n",
               ostname, poolname,  pool_tgt_count(pool));
        return 0;
}

int lov_pool_remove(struct obd_device *obd, char *poolname, char *ostname)
{
        struct obd_uuid ost_uuid;
        struct lov_obd *lov;
        struct pool_desc *pool;
        unsigned int i, lov_idx;

        lov = &(obd->u.lov);

        spin_lock(&obd->obd_dev_lock);
        pool = lustre_hash_lookup(lov->lov_pools_hash_body, poolname);
        if (pool == NULL) {
                spin_unlock(&obd->obd_dev_lock);
                return -ENOENT;
        }

        obd_str2uuid(&ost_uuid, ostname);

        /* search ost in lov array, to get index */
        for (i = 0; i < lov->desc.ld_tgt_count; i++) {
                if (!lov->lov_tgts[i])
                        continue;

                if (obd_uuid_equals(&ost_uuid, &(lov->lov_tgts[i]->ltd_uuid)))
                        break;
        }

        /* test if ost found in lov */
        if (i == lov->desc.ld_tgt_count) {
                spin_unlock(&obd->obd_dev_lock);
                return -EINVAL;
        }

        spin_unlock(&obd->obd_dev_lock);

        lov_idx = i;

        lov_ost_pool_remove(&pool->pool_obds, lov_idx);

        pool->pool_rr.lqr_dirty = 1;

        CDEBUG(D_CONFIG, "%s removed from "POOLNAMEF"\n", ostname, poolname);

        return 0;
}

int lov_check_index_in_pool(__u32 idx, struct pool_desc *pool)
{
        int i;

        read_lock(&pool_tgt_rwlock(pool));
        for (i = 0; i < pool_tgt_count(pool); i++) {
                if (pool_tgt_array(pool)[i] == idx) {
                        read_unlock(&pool_tgt_rwlock(pool));
                        return 0;
                }
        }
        read_unlock(&pool_tgt_rwlock(pool));
        return -ENOENT;
}

struct pool_desc *lov_find_pool(struct lov_obd *lov, char *poolname)
{
        struct pool_desc *pool;

        pool = NULL;
        if (poolname[0] != '\0') {
                pool = lustre_hash_lookup(lov->lov_pools_hash_body, poolname);
                if (pool == NULL)
                        CWARN("Request for an unknown pool ("POOLNAMEF")\n",
                              poolname);
                if ((pool != NULL) && (pool_tgt_count(pool) == 0)) {
                        CWARN("Request for an empty pool ("POOLNAMEF")\n",
                               poolname);
                        pool = NULL;
                }
        }
        return pool;
}
