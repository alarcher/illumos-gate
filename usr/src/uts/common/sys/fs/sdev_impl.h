/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */
/*
 * Copyright 2006 Sun Microsystems, Inc.  All rights reserved.
 * Use is subject to license terms.
 */

#ifndef _SYS_SDEV_IMPL_H
#define	_SYS_SDEV_IMPL_H

#pragma ident	"%Z%%M%	%I%	%E% SMI"

#ifdef __cplusplus
extern "C" {
#endif

#include <rpc/rpc.h>
#include <sys/dirent.h>
#include <sys/vfs.h>
#include <sys/list.h>
#include <sys/nvpair.h>

/*
 * sdev_nodes are the file-system specific part of the
 * vnodes for the device filesystem.
 *
 * The device filesystem exports two node types:
 *
 * VDIR nodes		to represent directories
 * VCHR & VBLK nodes	to represent devices
 */

/*
 * /dev mount arguments
 */
struct sdev_mountargs {
	uint64_t sdev_attrdir;
};


/*
 * Nvpair names of profile information (list of device files available) of
 * non-global /dev mounts.  These strings must be unique among them.
 */
#define	SDEV_NVNAME_MOUNTPT	"prof_mountpt"
#define	SDEV_NVNAME_INCLUDE	"prof_include"
#define	SDEV_NVNAME_EXCLUDE	"prof_exclude"
#define	SDEV_NVNAME_SYMLINK	"prof_symlink"
#define	SDEV_NVNAME_MAP		"prof_map"

/*
 * supported devfsadm_cmd
 */
#define	DEVFSADMD_RUN_ALL	1
#define	DEVFSADMD_NS_LOOKUP	2
#define	DEVFSADMD_NS_READDIR	3

/*
 * supported protocols
 */
typedef enum devname_spec {
	DEVNAME_NS_NONE = 0,
	DEVNAME_NS_PATH,	/* physical /devices path */
	DEVNAME_NS_DEV		/* /dev path */
} devname_spec_t;

/*
 * devfsadm_error codes
 */
#define	DEVFSADM_RUN_INVALID		1
#define	DEVFSADM_RUN_EPERM		2
#define	DEVFSADM_RUN_NOTSUP		3
#define	DEVFSADM_NS_FAILED		4

typedef struct sdev_ns_handle {
	char	ns_name[MAXPATHLEN];	/* device to be looked up */
	char	ns_map[MAXPATHLEN];
} sdev_ns_handle_t;

typedef struct sdev_lkp_handle {
	devname_spec_t	devfsadm_spec;	/* returned path property */
	char	devfsadm_link[MAXPATHLEN]; /* symlink destination */
} sdev_lkp_handle_t;

typedef struct sdev_rdr_handle {
	uint32_t ns_mapcount;	/* number of entries in the map */
	uint32_t maplist_size;
} sdev_rdr_handle_t;

/*
 * devfsadm/devname door data structures
 */
typedef struct sdev_door_arg {
	uint8_t devfsadm_cmd;	/* what to do for devfsadm[d] */
	sdev_ns_handle_t ns_hdl;
} sdev_door_arg_t;

typedef struct sdev_door_res {
	int32_t devfsadm_error;
	sdev_lkp_handle_t ns_lkp_hdl;
	sdev_rdr_handle_t ns_rdr_hdl;
} sdev_door_res_t;

#ifdef _KERNEL

struct sdev_dprof {
	int has_glob;
	nvlist_t *dev_name;
	nvlist_t *dev_map;
	nvlist_t *dev_symlink;
	nvlist_t *dev_glob_incdir;
	nvlist_t *dev_glob_excdir;
};

/*
 * devname_handle_t
 */
struct devname_handle {
	struct sdev_node *dh_data;	/* the sdev_node */
	devname_spec_t dh_spec;
	void    *dh_args;
};
typedef struct devname_handle devname_handle_t;

/*
 * Per-instance node data for the global zone instance
 * Only one mount of /dev in the global zone
 */
typedef struct sdev_global_data {
	struct devname_handle sdev_ghandle;
	struct devname_nsmap *sdev_gmapinfo;	/* VDIR name service info */
	ulong_t		sdev_dir_ggen;		/* name space generation # */
} sdev_global_data_t;

/*
 * Per-instance node data - profile data per non-global zone mount instance
 */
typedef struct sdev_local_data {
	ulong_t sdev_dir_lgen;		/* cached generation # of /dev dir */
	ulong_t sdev_devtree_lgen;	/* cached generation # of devtree */
	struct sdev_node *sdev_lorigin;	/* corresponding global sdev_node */
	struct sdev_dprof sdev_lprof;	/* profile for multi-inst */
} sdev_local_data_t;

/*
 * /dev filesystem sdev_node defines
 */
typedef struct sdev_node {
	char		*sdev_name;	/* node name */
	size_t		sdev_namelen;	/* strlen(sdev_name) */
	char		*sdev_path;	/* absolute path */
	char		*sdev_symlink;	/* source for a symlink */
	struct vnode	*sdev_vnode;	/* vnode */

	krwlock_t	sdev_contents;	/* rw lock for this data structure */
	struct sdev_node *sdev_dotdot;	/* parent */
	struct sdev_node *sdev_dot;	/* child: VDIR: head of children */
	struct sdev_node *sdev_next;	/* sibling: next in this directory */

	struct vnode	*sdev_attrvp;	/* backing store vnode if persisted */
	struct vattr	*sdev_attr;	/* memory copy of the vattr */

	ino64_t		sdev_ino;	/* inode */
	uint_t		sdev_nlink;	/* link count */
	int		sdev_state;	/* state of this node */
	int		sdev_flags;	/* flags bit */

	kmutex_t	sdev_lookup_lock; /* node creation synch lock */
	kcondvar_t	sdev_lookup_cv;	/* node creation sync cv */
	int		sdev_lookup_flags; /* node creation flags */

	/* per-instance data, either global or non-global zone */
	union {
		struct sdev_global_data	sdev_globaldata;
		struct sdev_local_data	sdev_localdata;
	} sdev_instance_data;
} sdev_node_t;

#define	sdev_ldata sdev_instance_data.sdev_localdata
#define	sdev_gdata sdev_instance_data.sdev_globaldata

#define	sdev_handle		sdev_gdata.sdev_ghandle
#define	sdev_mapinfo		sdev_gdata.sdev_gmapinfo
#define	sdev_gdir_gen		sdev_gdata.sdev_dir_ggen

#define	sdev_ldir_gen		sdev_ldata.sdev_dir_lgen
#define	sdev_devtree_gen	sdev_ldata.sdev_devtree_lgen
#define	sdev_origin		sdev_ldata.sdev_lorigin
#define	sdev_prof		sdev_ldata.sdev_lprof

/*
 * sdev_state
 *
 * A sdev_node may go through 3 states:
 *	SDEV_INIT: When a new /dev file is first looked up, a sdev_node
 *		   is allocated, initialized and added to the directory's
 *		   sdev_node cache. A node at this state will also
 *		   have the SDEV_LOOKUP flag set.
 *
 *		   Other threads that are trying to look up a node at
 *		   this state will be blocked until the SDEV_LOOKUP flag
 *		   is cleared.
 *
 *		   When the SDEV_LOOKUP flag is cleared, the node may
 *		   transition into the SDEV_READY state for a successful
 *		   lookup or the node is removed from the directory cache
 *		   and destroyed if the named node can not be found.
 *		   An ENOENT error is returned for the second case.
 *
 *	SDEV_READY: A /dev file has been successfully looked up and
 *		    associated with a vnode. The /dev file is available
 *		    for the supported /dev filesystem operations.
 *
 *	SDEV_ZOMBIE: Deletion of a /dev file has been explicitely issued
 *		    to an SDEV_READY node. The node is transitioned into
 *		    the SDEV_ZOMBIE state if the vnode reference count
 *		    is still held. A SDEV_ZOMBIE node does not support
 *		    any of the /dev filesystem operations. A SDEV_ZOMBIE
 *		    node is removed from the directory cache and destroyed
 *		    once the reference count reaches "zero".
 */
typedef enum {
	SDEV_ZOMBIE = -1,
	SDEV_INIT = 0,
	SDEV_READY
} sdev_node_state_t;

/* sdev_flags */
#define	SDEV_BUILD	0x0001	/* directory cache out-of-date */
#define	SDEV_STALE	0x0002	/* stale sdev nodes */
#define	SDEV_GLOBAL	0x0004	/* global /dev nodes */
#define	SDEV_PERSIST	0x0008	/* backing store persisted node */
#define	SDEV_NO_NCACHE	0x0010	/* do not include in neg. cache */
#define	SDEV_DYNAMIC	0x0020	/* special-purpose vnode ops (ex: pts) */
#define	SDEV_VTOR	0x0040	/* validate sdev_nodes during search */

/* sdev_lookup_flags */
#define	SDEV_LOOKUP	0x0001	/* node creation in progress */
#define	SDEV_READDIR	0x0002	/* VDIR readdir in progress */
#define	SDEV_LGWAITING	0x0004	/* waiting for devfsadm completion */

#define	SDEV_VTOR_INVALID	-1
#define	SDEV_VTOR_SKIP		0
#define	SDEV_VTOR_VALID		1

/* convenient macros */
#define	SDEV_IS_GLOBAL(dv)	\
	(dv->sdev_flags & SDEV_GLOBAL)
#define	SDEV_IS_PERSIST(dv)	\
	(dv->sdev_flags & SDEV_PERSIST)
#define	SDEV_IS_DYNAMIC(dv)	\
	(dv->sdev_flags & SDEV_DYNAMIC)
#define	SDEV_IS_NO_NCACHE(dv)	\
	(dv->sdev_flags & SDEV_NO_NCACHE)

#define	SDEV_IS_LOOKUP(dv)	\
	(dv->sdev_lookup_flags & SDEV_LOOKUP)
#define	SDEV_IS_READDIR(dv)	\
	(dv->sdev_lookup_flags & SDEV_READDIR)
#define	SDEV_IS_LGWAITING(dv)	\
	(dv->sdev_lookup_flags  & SDEV_LGWAITING)

#define	SDEVTOV(n)	((struct vnode *)(n)->sdev_vnode)
#define	VTOSDEV(vp)	((struct sdev_node *)(vp)->v_data)
#define	VN_HELD(v)	((v)->v_count != 0)
#define	SDEV_HELD(dv)	(VN_HELD(SDEVTOV(dv)))
#define	SDEV_HOLD(dv)	VN_HOLD(SDEVTOV(dv))
#define	SDEV_RELE(dv)	VN_RELE(SDEVTOV(dv))
#define	SDEV_SIMPLE_RELE(dv)	{	\
	mutex_enter(&SDEVTOV(dv)->v_lock);	\
	SDEVTOV(dv)->v_count--;	\
	mutex_exit(&SDEVTOV(dv)->v_lock);	\
}

#define	SDEV_ACL_FLAVOR(vp)	(VFSTOSDEVFS(vp->v_vfsp)->sdev_acl_flavor)

/*
 * some defaults
 */
#define	SDEV_ROOTINO		((ino_t)2)
#define	SDEV_UID_DEFAULT	(0)
#define	SDEV_GID_DEFAULT	(3)
#define	SDEV_DIRMODE_DEFAULT	(S_IFDIR |0755)
#define	SDEV_DEVMODE_DEFAULT	(0600)
#define	SDEV_LNKMODE_DEFAULT	(S_IFLNK | 0777)

extern struct vattr sdev_vattr_dir;
extern struct vattr sdev_vattr_lnk;
extern struct vattr sdev_vattr_blk;
extern struct vattr sdev_vattr_chr;

/*
 * devname_lookup_func()
 */
extern int devname_lookup_func(struct sdev_node *, char *, struct vnode **,
    struct cred *, int (*)(struct sdev_node *, char *, void **, struct cred *,
    void *, char *), int);

/*
 * flags used by devname_lookup_func callbacks
 */
#define	SDEV_PATH	0x1	/* callback returning /devices physical path */
#define	SDEV_VNODE	0x2	/* callback returning backing store vnode */
#define	SDEV_VATTR	0x4	/* callback returning node vattr */

/*
 * devname_readdir_func()
 */
extern int devname_readdir_func(vnode_t *, uio_t *, cred_t *, int *, int);

/*
 * flags for devname_readdir_func
 */
#define	SDEV_BROWSE	0x1	/* fetch all entries from backing store */

/*
 * devname_setattr_func()
 */
extern int devname_setattr_func(struct vnode *, struct vattr *, int,
    struct cred *, int (*)(struct sdev_node *, struct vattr *, int), int);

/*
 * /dev file system instance defines
 */
/*
 * /dev version of vfs_data
 */
struct sdev_data {
	struct sdev_data	*sdev_prev;
	struct sdev_data	*sdev_next;
	struct sdev_node	*sdev_root;
	struct vfs		*sdev_vfsp;
	struct sdev_mountargs	*sdev_mountargs;
	ulong_t			sdev_acl_flavor;
};

#define	VFSTOSDEVFS(vfsp)	((struct sdev_data *)((vfsp)->vfs_data))

/*
 * sdev_fid overlays the fid structure (for VFS_VGET)
 */
struct sdev_fid {
	uint16_t	sdevfid_len;
	ino32_t		sdevfid_ino;
	int32_t		sdevfid_gen;
};

/*
 * devfsadm and devname communication defines
 */
typedef enum {
	DEVNAME_DEVFSADM_STOPPED = 0,	/* devfsadm has never run */
	DEVNAME_DEVFSADM_RUNNING,	/* devfsadm is running */
	DEVNAME_DEVFSADM_RUN		/* devfsadm ran once */
} devname_devfsadm_state_t;

extern volatile uint_t  devfsadm_state; /* atomic mask for devfsadm status */

#define	DEVNAME_DEVFSADM_SET_RUNNING(devfsadm_state)	\
	devfsadm_state = DEVNAME_DEVFSADM_RUNNING
#define	DEVNAME_DEVFSADM_SET_STOP(devfsadm_state)	\
	devfsadm_state = DEVNAME_DEVFSADM_STOPPED
#define	DEVNAME_DEVFSADM_SET_RUN(devfsadm_state)	\
	devfsadm_state = DEVNAME_DEVFSADM_RUN
#define	DEVNAME_DEVFSADM_IS_RUNNING(devfsadm_state)	\
	devfsadm_state == DEVNAME_DEVFSADM_RUNNING
#define	DEVNAME_DEVFSADM_HAS_RUN(devfsadm_state)	\
	(devfsadm_state == DEVNAME_DEVFSADM_RUN)

#define	SDEV_BLOCK_OTHERS(dv, cmd)	{	\
	ASSERT(MUTEX_HELD(&dv->sdev_lookup_lock));	\
	dv->sdev_lookup_flags |= cmd;			\
}
extern void sdev_unblock_others(struct sdev_node *, uint_t);
#define	SDEV_UNBLOCK_OTHERS(dv, cmd)	{	\
	sdev_unblock_others(dv, cmd);		\
}

#define	SDEV_CLEAR_LOOKUP_FLAGS(dv, cmd)	{	\
	dv->sdev_lookup_flags &= ~cmd;	\
}

extern int sdev_wait4lookup(struct sdev_node *, int);
extern int devname_filename_register(int, char *);
extern int devname_nsmaps_register(char *, size_t);
extern void sdev_devfsadm_lockinit(void);
extern void sdev_devfsadm_lockdestroy(void);
extern void devname_add_devfsadm_node(char *);
extern void sdev_devfsadmd_thread(struct sdev_node *, struct sdev_node *,
    struct cred *);
extern int devname_profile_update(char *, size_t);
extern struct sdev_data *sdev_find_mntinfo(char *);
void sdev_mntinfo_rele(struct sdev_data *);
extern struct vnodeops *devpts_getvnodeops(void);

/*
 * Directory Based Device Naming (DBNR) defines
 */

#define	ETC_DEV_DIR		"/etc/dev"
#define	DEVNAME_NSCONFIG	"sdev_nsconfig_mod"

/*
 * directory name rule
 */
struct devname_nsmap {
	struct devname_nsmap	*prev;	/* previous entry */
	struct devname_nsmap	*next;	/* next entry */
	char	*dir_name;	/* /dev subdir name, e.g. /dev/disk */
	char	*dir_module;	/* devname module impl the operations */
	char	*dir_map;	/* dev naming rules, e.g. /etc/dev/disks */
	struct devname_ops *dir_ops;	/* directory specific vnode ops */
	char    *dir_newmodule; /* to be reloaded  */
	char    *dir_newmap;    /* to be reloaded */
	int	dir_invalid;    /* map entry obsolete */
	int	dir_maploaded;	/* map contents read */
	krwlock_t dir_lock;	/* protects the data structure */
};

/*
 * name-property pairs to be looked up
 */
typedef struct devname_lkp_arg {
	char *devname_dir;	/* the directory to look */
	char *devname_name;	/* the device name to be looked up */
	char *devname_map;	/* the directory device naming map */
	int reserved;
} devname_lkp_arg_t;

/*
 * name-value-property restured
 */
typedef struct devname_lkp_result {
	devname_spec_t	devname_spec;	/* link to /devices or /dev */
	char	*devname_link;		/* the source path */
	int	reserved;
} devname_lkp_result_t;

/*
 * directory name-value populating results
 */
typedef struct devname_rdr_result {
	uint32_t	ns_mapcount;
} devname_rdr_result_t;

/*
 * sdev_nsrdr work
 */
typedef struct sdev_nsrdr_work {
	char *dir_name;
	char *dir_map;
	struct sdev_node *dir_dv;
	devname_rdr_result_t **result;
	struct sdev_nsrdr_work *next;
} sdev_nsrdr_work_t;


/*
 * boot states - warning, the ordering here is significant
 *
 * the difference between "system available" and "boot complete"
 * is a debounce timeout to catch some daemon issuing a readdir
 * triggering a nuisance implict reconfig on each boot.
 */
#define	SDEV_BOOT_STATE_INITIAL		0
#define	SDEV_BOOT_STATE_RECONFIG	1	/* reconfig */
#define	SDEV_BOOT_STATE_SYSAVAIL	2	/* system available */
#define	SDEV_BOOT_STATE_COMPLETE	3	/* boot complete */

/*
 * Negative cache list and list element
 * The mutex protects the flags against multiple accesses and
 * must only be acquired when already holding the r/w lock.
 */
typedef struct sdev_nc_list {
	list_t		ncl_list;	/* the list itself */
	kmutex_t	ncl_mutex;	/* protects ncl_flags */
	krwlock_t	ncl_lock;	/* protects ncl_list */
	int		ncl_flags;
	int		ncl_nentries;
} sdev_nc_list_t;

typedef struct sdev_nc_node {
	char		*ncn_name;	/* name of the node */
	int		ncn_flags;	/* state information */
	int		ncn_expirecnt;	/* remove once expired */
	list_node_t	ncn_link;	/* link to next in list */
} sdev_nc_node_t;

/* ncl_flags */
#define	NCL_LIST_DIRTY		0x01	/* needs to be flushed */
#define	NCL_LIST_WRITING	0x02	/* write in progress */
#define	NCL_LIST_WENABLE	0x04	/* write-enabled post boot */

/* ncn_flags */
#define	NCN_ACTIVE	0x01	/* a lookup has occurred */
#define	NCN_SRC_STORE	0x02	/* src: persistent store */
#define	NCN_SRC_CURRENT	0x04	/* src: current boot */

/* sdev_lookup_failed flags */
#define	SLF_NO_NCACHE	0x01	/* node should not be added to ncache */
#define	SLF_REBUILT	0x02	/* reconfig performed during lookup attempt */

/*
 * The nvlist name and nvpair identifiers in the
 * /etc/devices/devname_cache nvlist format
 */
#define	DP_DEVNAME_ID			"devname"
#define	DP_DEVNAME_NCACHE_ID		"ncache"
#define	DP_DEVNAME_NC_EXPIRECNT_ID	"expire-counts"

/* devname-cache list element */
typedef struct nvp_devname {
	char			**nvp_paths;
	int			*nvp_expirecnts;
	int			nvp_npaths;
	list_node_t		nvp_link;
} nvp_devname_t;

/*
 * name service globals and prototypes
 */

extern struct devname_ops *devname_ns_ops;
extern int devname_nsmaps_loaded;
extern kmutex_t devname_nsmaps_lock;

extern void sdev_invalidate_nsmaps(void);
extern void sdev_validate_nsmaps(void);
extern int sdev_module_register(char *, struct devname_ops *);
extern struct devname_nsmap *sdev_get_nsmap_by_dir(char *, int);
extern struct devname_nsmap *sdev_get_nsmap_by_module(char *);
extern void sdev_dispatch_to_nsrdr_thread(struct sdev_node *, char *,
    devname_rdr_result_t *);
extern void sdev_insert_nsmap(char *, char *, char *);
extern int devname_nsmap_lookup(devname_lkp_arg_t *, devname_lkp_result_t **);
extern struct devname_nsmap *sdev_get_map(struct sdev_node *, int);
extern int sdev_nsmaps_loaded(void);
extern void sdev_replace_nsmap(struct devname_nsmap *, char *, char *);
extern int sdev_nsmaps_reloaded(void);
extern int devname_get_dir_nsmap(devname_handle_t *, struct devname_nsmap **);

/*
 * vnodeops and vfsops helpers
 */

typedef enum {
	SDEV_CACHE_ADD = 0,
	SDEV_CACHE_DELETE
} sdev_cache_ops_t;

extern struct sdev_node *sdev_cache_lookup(struct sdev_node *, char *);
extern int sdev_cache_update(struct sdev_node *, struct sdev_node **, char *,
    sdev_cache_ops_t);
extern void sdev_node_cache_init(void);
extern void sdev_node_cache_fini(void);
extern struct sdev_node *sdev_mkroot(struct vfs *, dev_t, struct vnode *,
    struct vnode *, struct cred *);
extern int sdev_mknode(struct sdev_node *, char *, struct sdev_node **,
    struct vattr *, struct vnode *, void *, struct cred *, sdev_node_state_t);
extern int sdev_nodeinit(struct sdev_node *, char *, struct sdev_node **,
    vattr_t *);
extern int sdev_nodeready(struct sdev_node *, vattr_t *, vnode_t *, void *,
    cred_t *);
extern int sdev_shadow_node(struct sdev_node *, struct cred *);
extern void sdev_nodedestroy(struct sdev_node *, uint_t);
extern void sdev_update_timestamps(struct vnode *, cred_t *, uint_t);
extern void sdev_vattr_merge(struct sdev_node *, struct vattr *);
extern void sdev_devstate_change(void);
extern int sdev_lookup_filter(sdev_node_t *, char *);
extern void sdev_lookup_failed(sdev_node_t *, char *, int);
extern int sdev_unlocked_access(void *, int, struct cred *);

#define	SDEV_ENFORCE	0x1
extern void sdev_stale(struct sdev_node *);
extern int sdev_cleandir(struct sdev_node *, char *, uint_t);
extern int sdev_rnmnode(struct sdev_node *, struct sdev_node *,
    struct sdev_node *, struct sdev_node **, char *, struct cred *);
extern size_t add_dir_entry(dirent64_t *, char *, size_t, ino_t, offset_t);
extern int sdev_to_vp(struct sdev_node *, struct vnode **);
extern ino_t sdev_mkino(struct sdev_node *);
extern int devname_backstore_lookup(struct sdev_node *, char *,
    struct vnode **);
extern int sdev_is_devfs_node(char *);
extern int sdev_copyin_mountargs(struct mounta *, struct sdev_mountargs *);
extern int sdev_reserve_subdirs(struct sdev_node *);
extern int prof_lookup();
extern void prof_filldir(struct sdev_node *);
extern int devpts_validate(struct sdev_node *dv);
extern void *sdev_get_vtor(struct sdev_node *dv);

/*
 * devinfo helpers
 */
extern int sdev_modctl_readdir(const char *, char ***, int *, int *);
extern void sdev_modctl_readdir_free(char **, int, int);
extern int sdev_modctl_devexists(const char *);


/*
 * ncache handlers
 */

extern void sdev_ncache_init(void);
extern void sdev_ncache_setup(void);
extern void sdev_ncache_teardown(void);
extern void sdev_nc_addname(sdev_nc_list_t *, sdev_node_t *, char *, int);
extern void sdev_nc_node_exists(sdev_node_t *);
extern void sdev_nc_path_exists(sdev_nc_list_t *, char *);
extern void sdev_modctl_dump_files(void);

/*
 * globals
 */
extern kmutex_t sdev_lock;
extern int devtype;
extern kmem_cache_t *sdev_node_cache;
extern struct vnodeops		*sdev_vnodeops;
extern struct vnodeops		*devpts_vnodeops;
extern struct sdev_data *sdev_origins; /* mount info for global /dev instance */
extern const fs_operation_def_t	sdev_vnodeops_tbl[];
extern const fs_operation_def_t	devpts_vnodeops_tbl[];
extern const fs_operation_def_t	devsys_vnodeops_tbl[];
extern const fs_operation_def_t	devpseudo_vnodeops_tbl[];

extern sdev_nc_list_t	*sdev_ncache;
extern int		sdev_reconfig_boot;
extern int		sdev_boot_state;
extern int		sdev_reconfig_verbose;
extern int		sdev_reconfig_disable;
extern int		sdev_nc_disable;
extern int		sdev_nc_disable_reset;
extern int		sdev_nc_verbose;

/*
 * misc. defines
 */
#ifdef DEBUG
extern int sdev_debug;
#define	SDEV_DEBUG		0x01	/* error messages to console/log */
#define	SDEV_DEBUG_VOPS 	0x02	/* vnode ops errors */
#define	SDEV_DEBUG_DLF		0x04	/* trace devname_lookup_func */
#define	SDEV_DEBUG_DRF		0x08	/* trace devname_readdir_func */
#define	SDEV_DEBUG_NCACHE	0x10	/* negative cache tracing */
#define	SDEV_DEBUG_DEVFSADMD	0x20	/* comm. of devnamefs & devfsadm */
#define	SDEV_DEBUG_PTS		0x40	/* /dev/pts tracing */
#define	SDEV_DEBUG_RECONFIG	0x80	/* events triggering reconfig */
#define	SDEV_DEBUG_SDEV_NODE	0x100	/* trace sdev_node activities */
#define	SDEV_DEBUG_PROFILE	0x200	/* trace sdev_profile */
#define	SDEV_DEBUG_MODCTL	0x400	/* trace modctl activity */
#define	SDEV_DEBUG_FLK		0x800	/* trace failed lookups */

#define	sdcmn_err(args)  if (sdev_debug & SDEV_DEBUG) printf args
#define	sdcmn_err2(args) if (sdev_debug & SDEV_DEBUG_VOPS) printf args
#define	sdcmn_err3(args) if (sdev_debug & SDEV_DEBUG_DLF) printf args
#define	sdcmn_err4(args) if (sdev_debug & SDEV_DEBUG_DRF) printf args
#define	sdcmn_err5(args) if (sdev_debug & SDEV_DEBUG_NCACHE) printf args
#define	sdcmn_err6(args) if (sdev_debug & SDEV_DEBUG_DEVFSADMD) printf args
#define	sdcmn_err7(args) if (sdev_debug & SDEV_DEBUG_PTS) printf args
#define	sdcmn_err8(args) if (sdev_debug & SDEV_DEBUG_RECONFIG) printf args
#define	sdcmn_err9(args) if (sdev_debug & SDEV_DEBUG_SDEV_NODE) printf args
#define	sdcmn_err10(args) if (sdev_debug & SDEV_DEBUG_PROFILE) printf args
#define	sdcmn_err11(args) if (sdev_debug & SDEV_DEBUG_MODCTL) printf args
#define	impossible(args) printf args
#else
#define	sdcmn_err(args)		/* does nothing */
#define	sdcmn_err2(args)	/* does nothing */
#define	sdcmn_err3(args)	/* does nothing */
#define	sdcmn_err4(args)	/* does nothing */
#define	sdcmn_err5(args)	/* does nothing */
#define	sdcmn_err6(args)	/* does nothing */
#define	sdcmn_err7(args)	/* does nothing */
#define	sdcmn_err8(args)	/* does nothing */
#define	sdcmn_err9(args)	/* does nothing */
#define	sdcmn_err10(args)	/* does nothing */
#define	sdcmn_err11(args)	/* does nothing */
#define	impossible(args)	/* does nothing */
#endif

#ifdef DEBUG
#define	SD_TRACE_FAILED_LOOKUP(ddv, nm, retried)			\
	if ((sdev_debug & SDEV_DEBUG_FLK) ||				\
	    ((retried) && (sdev_debug & SDEV_DEBUG_RECONFIG))) {	\
		printf("lookup of %s/%s by %s failed, line %d\n",	\
		    (ddv)->sdev_name, (nm), curproc->p_user.u_comm,	\
		    __LINE__);						\
	}
#else
#define	SD_TRACE_FAILED_LOOKUP(ddv, nm, retried)
#endif

#endif	/* _KERNEL */

#ifdef __cplusplus
}
#endif

#endif	/* _SYS_SDEV_IMPL_H */
