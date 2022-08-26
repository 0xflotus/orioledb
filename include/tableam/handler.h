/*-------------------------------------------------------------------------
 *
 * handler.h
 *		Declarations of table access method handler
 *
 * Copyright (c) 2021-2022, Oriole DB Inc.
 *
 * IDENTIFICATION
 *	  contrib/orioledb/include/tableam/handler.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef __TABLEAM_HANDLER_H__
#define __TABLEAM_HANDLER_H__

#include "btree/btree.h"
#include "catalog/o_tables.h"

#include "access/tableam.h"
#include "nodes/execnodes.h"
#include "nodes/pathnodes.h"
#include "rewrite/rewriteHandler.h"

extern bool is_orioledb_rel(Relation rel);
extern OIndexNumber find_tree_in_descr(OTableDescr *descr, ORelOids oids);

/* EXPLAIN ANALYZE functions call counter */
typedef struct
{
	uint32		read;			/* o_btree_read_page() */
	uint32		write;			/* write_page() */
	uint32		load;			/* load_page() */
	uint32		lock;			/* lock_page() */
	uint32		evict;			/* evict_page() */
} OEACallsCounter;

#define EA_COUNTERS_NUM (5)		/* number of EXPLAIN ANALYZE counters */

/*
 * EXPLAIN ANALYZE counters for different trees involved in single executor
 * node.
 */
typedef struct
{
	/* Identifiers of table being analyzed */
	ORelOids	oids;
	/* Table descriptor */
	OTableDescr *descr;
	/* Counters for primary and secondary indices */
	int			nindices;
	OEACallsCounter *indices;
	/* Counters for TOAST */
	OEACallsCounter toast;
	/* Counters for indices of other tables */
	OEACallsCounter others;
} OEACallsCounters;

/*
 * EXPLAIN ANALYZE function call counters.
 * will be init and free in tableam_scan.c
 */
extern OEACallsCounters *ea_counters;

/* returns AnalyzeCallsCounter for specified index number */
static inline OEACallsCounter *
get_ea_counters(OrioleDBPageDesc *desc)
{
	OIndexNumber ix_num = find_tree_in_descr(ea_counters->descr, desc->oids);

	if (ix_num == InvalidIndexNumber)
		return &ea_counters->others;
	if (ix_num == TOASTIndexNumber)
		return &ea_counters->toast;
	return &ea_counters->indices[ix_num];
}

/* increases EXPLAIN_ANALYZE counter for o_btree_read_page() call */
#define EA_READ_INC(blkno)  \
	if (ea_counters != NULL)	\
	{	\
		OrioleDBPageDesc *desc = O_GET_IN_MEMORY_PAGEDESC(blkno);	\
		OEACallsCounter *ix_counter = get_ea_counters(desc); \
		if (ix_counter != NULL) \
			ix_counter->read++; \
	}

/* increases EXPLAIN_ANALYZE counter for write_read() call */
#define EA_WRITE_INC(blkno)  \
	if (ea_counters != NULL)	\
	{	\
		OrioleDBPageDesc *desc = O_GET_IN_MEMORY_PAGEDESC(blkno);	\
		OEACallsCounter *ix_counter = get_ea_counters(desc); \
		if (ix_counter != NULL) \
			ix_counter->write++; \
	}

/* increases EXPLAIN_ANALYZE counter for load_page() call */
#define EA_LOAD_INC(blkno)  \
	if (ea_counters != NULL)	\
	{	\
		OrioleDBPageDesc *desc = O_GET_IN_MEMORY_PAGEDESC(blkno);	\
		OEACallsCounter *ix_counter = get_ea_counters(desc); \
		if (ix_counter != NULL) \
			ix_counter->load++; \
	}

/* increases EXPLAIN_ANALYZE counter for lock_page() call */
#define EA_LOCK_INC(blkno)  \
	if (ea_counters != NULL)	\
	{	\
		OrioleDBPageDesc *desc = O_GET_IN_MEMORY_PAGEDESC(blkno);	\
		OEACallsCounter *ix_counter = get_ea_counters(desc); \
		if (ix_counter != NULL) \
			ix_counter->lock++; \
	}

/* increases EXPLAIN_ANALYZE counter for evict_page() call */
#define EA_EVICT_INC(blkno)  \
	if (ea_counters != NULL)	\
	{	\
		OrioleDBPageDesc *desc = O_GET_IN_MEMORY_PAGEDESC(blkno);	\
		OEACallsCounter *ix_counter = get_ea_counters(desc); \
		if (ix_counter != NULL) \
			ix_counter->evict++; \
	}

extern void cleanup_btree(Oid datoid, Oid relnode);
extern bool o_drop_shared_root_info(Oid datoid, Oid relnode);
extern void o_tableam_descr_init(void);
extern void o_invalidate_descrs(Oid datoid, Oid reloid, Oid relfilenode);
extern void init_print_options(BTreePrintOptions *printOptions, VarChar *optionsArg);
extern void o_check_constraints(ResultRelInfo *rinfo, TupleTableSlot *slot, EState *estate);
extern void orioledb_free_rd_amcache(Relation rel);
extern OTableDescr *relation_get_descr(Relation rel);
extern void table_descr_inc_refcnt(OTableDescr *descr);
extern void table_descr_dec_refcnt(OTableDescr *descr);

extern Size orioledb_parallelscan_estimate(Relation rel);
extern Size orioledb_parallelscan_initialize(Relation rel, ParallelTableScanDesc pscan);
extern void orioledb_parallelscan_reinitialize(Relation rel, ParallelTableScanDesc pscan);

/*
 * Oriole-specific shared state for parallel table scan.
 *
 * Each backend participating in a parallel table scan has its own
 * OScanDesc in backend-private memory, and those objects all contain a
 * pointer to this structure.  The information here must be sufficient to
 * properly initialize each new OScanDesc as workers join the scan, and it
 * must act as a information what to scan for those workers.
 */

typedef struct BTreeIntPageParallelData
{
       char                            img[ORIOLEDB_BLCKSZ];
       bool                            loaded;
} BTreeIntPageParallelData;

typedef BTreeIntPageParallelData *BTreeIntPageParallel;

typedef struct ParallelOScanDescData
{
       ParallelTableScanDescData        phs_base;         /* Shared AM-independent state for parallel table scan */
       BlockNumber                      nblocks; //maybe not needed
       slock_t                          intpage_access;   /* for current position on level 1 internal page */
       slock_t                          worker_start;	  /* for sequential workers joining */
	   slock_t							intpage_loading;  /* for sequential internal page loading */
       int                              offset;			  /* current offset on internal level1 page to restore locator
	   														 in parallel workers */
       OFixedShmemKey                   prevHikey;        /* low key of current level1 page loaded to shared state */
	   bool 							single_leaf_page_rel;
       BTreeIntPageParallelData         int_page[1];
       bool                            	leader_started;
       bool                             worker_active[10]; /* array of started workers for debug usage */
	   bool								int_finish;		   /* internal pages scan finish flag to avoid copying CurHikey into shared state */
	   bool 							first_page_loaded;
	   int								cur_int_pageno; 	//debug
       // TODO implement shared downlinks storage
       //int64                                    downlinkIndex;
       // BTreeSeqScanDiskDownlink         diskDownlinks[FLEXIBLE_ARRAY_MEMBER];
} ParallelOScanDescData;

typedef ParallelOScanDescData *ParallelOScanDesc;
#endif
