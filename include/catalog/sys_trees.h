/*-------------------------------------------------------------------------
 *
 * sys_trees.h
 *		Headers for system trees
 *
 * Copyright (c) 2021-2022, Oriole DB Inc.
 *
 * IDENTIFICATION
 *	  contrib/orioledb/include/catalog/sys_trees.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef __SYS_TREES_H__
#define __SYS_TREES_H__

#include "btree/btree.h"
#include "btree/print.h"

#include "utils/catcache.h"

#define SYS_TREES_DATOID				(1)

#define SYS_TREES_SHARED_ROOT_INFO		(1)
#define SYS_TREES_O_TABLES				(2)
#define SYS_TREES_O_INDICES				(3)
#define SYS_TREES_OPCLASS_CACHE			(4)
#define SYS_TREES_ENUM_CACHE			(5)
#define SYS_TREES_ENUMOID_CACHE			(6)
#define SYS_TREES_RANGE_CACHE			(7)
#define SYS_TREES_CLASS_CACHE			(8)
#define SYS_TREES_EXTENTS_OFF_LEN		(9)
#define SYS_TREES_EXTENTS_LEN_OFF		(10)
#define SYS_TREES_PROC_CACHE			(11)
#define SYS_TREES_TYPE_CACHE			(12)
#define SYS_TREES_AGG_CACHE				(13)
#define SYS_TREES_OPER_CACHE			(14)
#define	SYS_TREES_AMOP_CACHE			(15)
#define	SYS_TREES_AMPROC_CACHE			(16)
#define SYS_TREES_NUM					(16)

#define IS_SYS_TREE_OIDS(oids) \
	((oids).datoid == SYS_TREES_DATOID)

#define O_OPCLASS_PROSRC_MAXLEN 512

typedef struct
{
	Oid			datoid;
	Oid			relnode;
} SharedRootInfoKey;

typedef struct
{
	SharedRootInfoKey key;
	BTreeRootInfo rootInfo;
} SharedRootInfo;

#define O_TABLE_INVALID_VERSION UINT32_MAX

typedef struct
{
	ORelOids	oids;
	uint32		offset;
	uint32		version;
} OTableChunkKey;

typedef struct
{
	OTableChunkKey key;
	uint32		dataLength;
	char		data[FLEXIBLE_ARRAY_MEMBER];
} OTableChunk;

typedef struct
{
	OIndexType	type;
	ORelOids	oids;
	uint32		offset;
} OIndexChunkKey;

typedef struct
{
	OIndexChunkKey key;
	uint32		dataLength;
	char		data[FLEXIBLE_ARRAY_MEMBER];
} OIndexChunk;

/*
 * FileExtent type stores length of an extent inside unsigned 16-bit value.
 * It enough for FileExtent purposes but extents inside free B-trees
 * can be more than 2^16.
 */
typedef struct
{
	uint64		offset;
	uint64		length;
} FreeTreeFileExtent;

/*
 * Tuple stored in a free B-tree nodes and tuples.
 */
typedef struct
{
	FreeTreeFileExtent extent;
	OIndexType	ixType;
	Oid			datoid;
	Oid			relnode;
} FreeTreeTuple;

extern Size sys_trees_shmem_needs(void);
extern void sys_trees_shmem_init(Pointer ptr, bool found);
extern BTreeDescr *get_sys_tree(int tree_num);
extern BTreeStorageType sys_tree_get_storage_type(int tree_num);
extern bool sys_tree_is_temporary(int tree_num);
extern bool sys_tree_supports_transactions(int tree_num);
extern PrintFunc sys_tree_key_print(BTreeDescr *desc);
extern PrintFunc sys_tree_tup_print(BTreeDescr *desc);

#endif							/* __SYS_TREES_H__ */
