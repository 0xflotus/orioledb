/*-------------------------------------------------------------------------
 *
 * orioledb.h
 *		Common declarations for orioledb engine.
 *
 * Copyright (c) 2021-2022, Oriole DB Inc.
 *
 * IDENTIFICATION
 *	  contrib/orioledb/include/orioledb.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef __ORIOLEDB_H__
#define __ORIOLEDB_H__

#include "access/xact.h"
#include "access/xlog.h"
#include "port/atomics.h"
#include "storage/bufpage.h"
#include "storage/fd.h"
#include "storage/lock.h"
#include "storage/procarray.h"
#include "storage/spin.h"
#include "utils/jsonb.h"
#include "utils/typcache.h"
#include "utils/relcache.h"

#define ORIOLEDB_VERSION "OrioleDB public alpha 8"
#define ORIOLEDB_BINARY_VERSION 3
#define ORIOLEDB_DATA_DIR "orioledb_data"
#define ORIOLEDB_UNDO_DIR "orioledb_undo"
#define ORIOLEDB_EVT_EXTENSION "evt"

/*
 * perform_page_split() removes a key data from first right page downlink.
 * But the data can be useful for debug. The macro controls this behavior.
 *
 * See usage in perform_page_split().
 */
#define ORIOLEDB_CUT_FIRST_KEY 1
/* max a BTree depth */
#define ORIOLEDB_MAX_DEPTH		32
/* size of OrioleDB BTree page */
#define ORIOLEDB_BLCKSZ		8192
/* size of on disk compressed page chunk */
#define ORIOLEDB_COMP_BLCKSZ	512
/* size of data file segment */
#define ORIOLEDB_SEGMENT_SIZE	(1024 * 1024 * 1024)

#define GetMaxBackends() MaxBackends

/* Number of orioledb page */
typedef uint32 OInMemoryBlkno;
#define OInvalidInMemoryBlkno		((OInMemoryBlkno) 0xFFFFFFFF)
#define OInMemoryBlknoIsValid(blockNumber) \
	((bool) ((OInMemoryBlkno) (blockNumber) != OInvalidInMemoryBlkno))
#define ORootPageIsValid(desc) (OInMemoryBlknoIsValid((desc)->rootInfo.rootPageBlkno))
#define OMetaPageIsValid(desc) (OInMemoryBlknoIsValid((desc)->rootInfo.metaPageBlkno))

/* Undo log location */
typedef uint64 UndoLocation;
#define	InvalidUndoLocation		UINT64CONST(0x3FFFFFFFFFFFFFFF)
#define UndoLocationIsValid(loc)	((loc) != InvalidUndoLocation)

/* Identifier for orioledb transaction */
typedef uint64 OXid;
#define	InvalidOXid					UINT64CONST(0x7FFFFFFFFFFFFFFF)
#define OXidIsValid(oxid)			((oxid) != InvalidOXid)
#define LXID_NORMAL_FROM			(1)

/* Index number */
typedef uint16 OIndexNumber;

/* Index type */
typedef enum
{
	oIndexInvalid = 0,
	oIndexToast = 1,
	oIndexPrimary = 2,
	oIndexUnique = 3,
	oIndexRegular = 4
} OIndexType;

#define PROC_XID_ARRAY_SIZE	32

typedef struct
{
	OXid		oxid;
	VirtualTransactionId vxid;
} XidVXidMapElement;

typedef struct
{
	pg_atomic_uint64 location;
	pg_atomic_uint64 branchLocation;
	pg_atomic_uint64 subxactLocation;
	pg_atomic_uint64 onCommitLocation;
} UndoStackSharedLocations;

typedef struct
{
	pg_atomic_uint64 reservedUndoLocation;
	pg_atomic_uint64 transactionUndoRetainLocation;
	pg_atomic_uint64 snapshotRetainUndoLocation;
	pg_atomic_uint64 commitInProgressXlogLocation;
	LocalTransactionId lastLXid;
	LWLock		undoStackLocationsFlushLock;
	bool		flushUndoLocations;
	pg_atomic_uint64 xmin;
	UndoStackSharedLocations undoStackLocations[PROC_XID_ARRAY_SIZE];
	XidVXidMapElement vxids[PROC_XID_ARRAY_SIZE];
} ODBProcData;

typedef struct
{
	Oid			datoid;
	Oid			reloid;
	Oid			relnode;
} ORelOids;

#define ORelOidsIsValid(oids) (OidIsValid((oids).datoid) && OidIsValid((oids).reloid) && OidIsValid((oids).relnode))
#define ORelOidsIsEqual(l, r) ((l).datoid == (r).datoid && (l).reloid == (r).reloid && (l).relnode == (r).relnode)

typedef struct
{
	uint64		len:16,
				off:48;
} FileExtent;

#define InvalidFileExtentLen (0)
#define InvalidFileExtentOff (UINT64CONST(0xFFFFFFFFFFFF))
#define FileExtentLenIsValid(len) ((len) != InvalidFileExtentLen)
#define FileExtentOffIsValid(off) ((off) < InvalidFileExtentOff)
#define FileExtentIsValid(extent) (FileExtentLenIsValid((extent).len) && FileExtentOffIsValid((extent).off))
#define CompressedSize(page_size) ((page_size) == ORIOLEDB_BLCKSZ \
										? ORIOLEDB_BLCKSZ \
										: ((page_size) + sizeof(OCompressHeader) + ORIOLEDB_COMP_BLCKSZ - 1))
#define FileExtentLen(page_size) (CompressedSize(page_size) / ORIOLEDB_COMP_BLCKSZ)

typedef struct
{
	ORelOids	oids;
	int			ionum;
	FileExtent	fileExtent;
	uint32		flags:4,
				type:28;
	proclist_head waitersList;
} OrioleDBPageDesc;

/*
 * Should be used as beginning of header in all orioledb shared pages:
 * BTree pages, Meta-pages, SeqBuf pages, etc.
 */
typedef struct
{
	pg_atomic_uint32 state;
	pg_atomic_uint32 usageCount;
	uint32		pageChangeCount;
} OrioleDBPageHeader;

#define O_PAGE_HEADER_SIZE		sizeof(OrioleDBPageHeader)
#define O_PAGE_HEADER(page)	((OrioleDBPageHeader *)(page))

#define O_PAGE_CHANGE_COUNT_MAX		(0x7FFFFFFF)
#define InvalidOPageChangeCount		(O_PAGE_CHANGE_COUNT_MAX)
#define O_PAGE_CHANGE_COUNT_INC(page) \
	if (O_PAGE_HEADER(page)->pageChangeCount >= O_PAGE_CHANGE_COUNT_MAX) \
		O_PAGE_HEADER(page)->pageChangeCount = 0; \
	else \
		O_PAGE_HEADER(page)->pageChangeCount++;
#define O_PAGE_GET_CHANGE_COUNT(p) (O_PAGE_HEADER(p)->pageChangeCount)

/* orioledb.c */
extern Size orioledb_buffers_size;
extern Size orioledb_buffers_count;
extern Size undo_circular_buffer_size;
extern uint32 undo_buffers_count;
extern Size xid_circular_buffer_size;
extern uint32 xid_buffers_count;
extern Pointer o_shared_buffers;
extern ODBProcData *oProcData;
extern int	max_procs;
extern OrioleDBPageDesc *page_descs;
extern bool remove_old_checkpoint_files;
extern bool debug_disable_bgwriter;
extern MemoryContext btree_insert_context;
extern MemoryContext btree_seqscan_context;
extern double o_checkpoint_completion_ratio;
extern int	max_io_concurrency;
extern bool use_mmap;
extern bool use_device;
extern int	device_fd;
extern char *device_filename;
extern Pointer mmap_data;
extern Size device_length;
extern int	default_compress;
extern int	default_primary_compress;
extern int	default_toast_compress;

#define GET_CUR_PROCDATA() \
	(AssertMacro(MyProc->pgprocno >= 0 && \
				 MyProc->pgprocno < max_procs), \
	 &oProcData[MyProc->pgprocno])
#define O_GET_IN_MEMORY_PAGE(blkno) \
	(AssertMacro(OInMemoryBlknoIsValid(blkno)), \
	 (Page)(o_shared_buffers + ((uint64) (blkno)) * ((uint64) ORIOLEDB_BLCKSZ)))
#define O_GET_IN_MEMORY_PAGEDESC(blkno) \
	(AssertMacro(OInMemoryBlknoIsValid(blkno)), page_descs + (blkno))
#define O_GET_IN_MEMORY_PAGE_CHANGE_COUNT(blkno) \
	(O_PAGE_GET_CHANGE_COUNT(O_GET_IN_MEMORY_PAGE(blkno)))

extern void orioledb_check_shmem(void);

typedef int OCompress;
#define O_COMPRESS_DEFAULT (10)
#define InvalidOCompress (-1)
#define OCompressIsValid(compress) ((compress) != InvalidOCompress)

/*
 * We save number of chunks inside downlinks instead of size of compressed data
 * because it helps us to avoid too often setup dirty flag for parent if page
 * is changed.
 *
 * The header of compressed data contains compressed data length.
 */
typedef uint16 OCompressHeader;

extern List *extract_compress_rel_option(List *defs, char *option, int *value);
extern void validate_compress(OCompress compress, char *prefix);
extern void o_invalidate_oids(ORelOids oids);

#define EXPR_ATTNUM (FirstLowInvalidHeapAttributeNumber - 1)

/* orioledb.c */
typedef enum OPagePoolType
{
	OPagePoolMain = 0,
	OPagePoolFreeTree = 1,
	OPagePoolCatalog = 2
} OPagePoolType;
#define OPagePoolTypesCount 3

typedef struct OPagePool OPagePool;
struct BTreeDescr;

extern uint64 orioledb_device_alloc(struct BTreeDescr *descr, uint32 size);
extern OPagePool *get_ppool(OPagePoolType type);
extern OPagePool *get_ppool_by_blkno(OInMemoryBlkno blkno);
extern OInMemoryBlkno get_dirty_pages_count_sum(void);
extern void jsonb_push_key(JsonbParseState **state, char *key);
extern void jsonb_push_null_key(JsonbParseState **state, char *key);
extern void jsonb_push_bool_key(JsonbParseState **state, char *key, bool value);
extern void jsonb_push_int8_key(JsonbParseState **state, char *key, int64 value);
extern void jsonb_push_string_key(JsonbParseState **state, const char *key, const char *value);

extern CheckPoint_hook_type next_CheckPoint_hook;

/* tableam_handler.c */
extern bool is_orioledb_rel(Relation rel);

typedef struct OTableDescr OTableDescr;
typedef struct OIndexDescr OIndexDescr;

/* ddl.c */
extern void orioledb_setup_ddl_hooks(void);
extern UndoLocation saved_undo_location;
void cleanup_saved_undo_locations(void);

#endif							/* __ORIOLEDB_H__ */
