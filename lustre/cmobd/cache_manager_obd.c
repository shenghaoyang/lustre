/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 *  Copyright (c) 2002 Cluster File Systems, Inc. <info@clusterfs.com>
 *
 *   This file is part of Lustre, http://www.lustre.org.
 *
 *   Lustre is free software; you can redistribute it and/or
 *   modify it under the terms of version 2 of the GNU General Public
 *   License as published by the Free Software Foundation.
 *
 *   Lustre is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with Lustre; if not, write to the Free Software
 *   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#define DEBUG_SUBSYSTEM S_CMOBD

#include <linux/version.h>
#include <linux/init.h>
#include <linux/obd_support.h>
#include <linux/lustre_lib.h>
#include <linux/lustre_net.h>
#include <linux/lustre_idl.h>
#include <linux/obd_class.h>
#include <linux/lustre_mds.h>
#include <linux/lustre_cmobd.h>
#include <linux/lprocfs_status.h>
#include <linux/obd_lov.h>

#include "cmobd_internal.h"

static int cmobd_attach(struct obd_device *obd, obd_count len, void *data)
{
        struct lprocfs_static_vars lvars;

        lprocfs_init_vars(cmobd, &lvars);
        return lprocfs_obd_attach(obd, lvars.obd_vars);
}

static int cmobd_detach(struct obd_device *obd)
{
        return lprocfs_obd_detach(obd);
}

static void cmobd_find_master_client_obd(struct obd_device *obd,
                                        struct obd_uuid *uuid)
{             
        struct cache_manager_obd *cmobd = &obd->u.cmobd;
         
        cmobd->cm_master_obd = class_find_client_obd(NULL, OBD_LOV_DEVICENAME,
                                                     uuid);
        if (cmobd->cm_master_obd == NULL) 
               cmobd->cm_master_obd = class_find_client_obd(NULL, 
                                                            LUSTRE_MDC_NAME, 
                                                            uuid);
}        

static int cmobd_setup(struct obd_device *obd, obd_count len, void *buf)
{
        struct cache_manager_obd *cmobd = &obd->u.cmobd;
        struct lustre_cfg* lcfg = buf;
        struct obd_uuid master_uuid, cache_uuid;
        struct lustre_handle conn = { 0 };
        int rc;
        ENTRY;

        if (lcfg->lcfg_inllen1 < 1 || !lcfg->lcfg_inlbuf1) {
                CERROR("CMOBD setup requires master uuid\n");
                RETURN(-EINVAL);
        }
        if (lcfg->lcfg_inllen2 < 1 || !lcfg->lcfg_inlbuf2) {
                CERROR("CMOBD setup requires cache uuid\n");
                RETURN(-EINVAL);
        }
        
        obd_str2uuid(&master_uuid, lcfg->lcfg_inlbuf1);
        obd_str2uuid(&cache_uuid, lcfg->lcfg_inlbuf2);

        cmobd_find_master_client_obd(obd, &master_uuid);
        if (cmobd->cm_master_obd == NULL) {
                CERROR("Can't find master obd %s\n", &master_uuid.uuid[0]);
                RETURN(-EINVAL);
        }
        cmobd->cm_cache_obd = class_uuid2obd(&cache_uuid);
        if (cmobd->cm_cache_obd == NULL) {
                CERROR("Can't find cache obd %s\n", &cache_uuid.uuid[0]);
                RETURN(-EINVAL);
        }

        /* master lov connects to master ost here */
        rc = obd_connect(&conn, cmobd->cm_master_obd, &obd->obd_uuid, 0); 
        if (rc)
                RETURN(rc);
        cmobd->cm_master_exp = class_conn2export(&conn);

        memset(&conn, 0, sizeof(conn));
        rc = class_connect(&conn, cmobd->cm_cache_obd, &obd->obd_uuid);
        if (rc)
                GOTO(put_master, rc);
        cmobd->cm_cache_exp = class_conn2export(&conn);
        if (!strcmp(cmobd->cm_master_obd->obd_type->typ_name, 
                    OBD_LOV_DEVICENAME)){
                /* for master osc remove the recovery flag of oscc */
                rc = obd_set_info(cmobd->cm_master_exp, strlen("unrecovery"),
                                  "unrecovery", 0, NULL); 
                if (rc)
                        GOTO(put_master, rc);
                
                rc = cmobd_init_write_srv(obd);
                if (rc)
                        GOTO(put_cache, rc);

        } else {
                /*FIXME later temp fix here 
                 *Assumation:cache mds only have one ost*/
                cmobd->cm_master_obd->u.cli.cl_max_mds_easize = 
                                                lov_mds_md_size(1);
        }
        /* start n threads for write replay */
        RETURN(0);
put_cache:
        class_disconnect(cmobd->cm_cache_exp, 0);
put_master:
        obd_disconnect(cmobd->cm_master_exp, 0);
        RETURN(rc);
}

static int cmobd_cleanup(struct obd_device *obd, int flags)
{
        struct cache_manager_obd *cmobd = &obd->u.cmobd;
        ENTRY;
        
        if (!strcmp(cmobd->cm_master_obd->obd_type->typ_name,
                    OBD_LOV_DEVICENAME)) {
                cmobd_cleanup_write_srv(obd);
        }
        class_disconnect(cmobd->cm_cache_exp, 0);
        obd_disconnect(cmobd->cm_master_exp, 0);
        
        RETURN(0);
}

static int cmobd_iocontrol(unsigned int cmd, struct obd_export *exp, int len,
                           void *karg, void *uarg)
{
        struct obd_device *obd = exp->exp_obd;
        int rc = 0;
        ENTRY;
        
        switch (cmd) {
        case OBD_IOC_CMOBD_SYNC: /* trigger reintegration */
                rc = cmobd_reintegrate(obd);
                break;
        default:
                CERROR("unrecognized ioctl %#x\n", cmd);
                rc = -EINVAL;
                break;
        }
                
        RETURN(rc);
}

static struct obd_ops cmobd_ops = {
        o_owner:                THIS_MODULE,
        o_attach:               cmobd_attach,
        o_detach:               cmobd_detach,
        o_setup:                cmobd_setup,
        o_cleanup:              cmobd_cleanup,
        o_iocontrol:            cmobd_iocontrol,
};

kmem_cache_t *cmobd_extent_slab;

static int __init cmobd_init(void)
{
        struct lprocfs_static_vars lvars;
        int rc;
        ENTRY;

        printk(KERN_INFO "Lustre: Cache Manager OBD driver; info@clusterfs.com\n");

        lprocfs_init_vars(cmobd, &lvars);
        rc = class_register_type(&cmobd_ops, NULL, lvars.module_vars,
                                 LUSTRE_CMOBD_NAME);
        if (rc)
                RETURN(rc);
        cmobd_extent_slab = kmem_cache_create("cmobd_extents",
                                               sizeof(struct cmobd_extent_info), 0,
                                               SLAB_HWCACHE_ALIGN, NULL, NULL);
        if (cmobd_extent_slab == NULL) {
                class_unregister_type(LUSTRE_CMOBD_NAME);
                RETURN(-ENOMEM);
        }
        RETURN(0);
}

static void /*__exit*/ cmobd_exit(void)
{
        class_unregister_type(LUSTRE_CMOBD_NAME);
        if (kmem_cache_destroy(cmobd_extent_slab) != 0)
                CERROR("couldn't free cmobd extent slab\n");
}

MODULE_AUTHOR("Cluster File Systems, Inc. <info@clusterfs.com>");
MODULE_DESCRIPTION("Lustre Cache Manager OBD driver");
MODULE_LICENSE("GPL");

module_init(cmobd_init);
module_exit(cmobd_exit);
