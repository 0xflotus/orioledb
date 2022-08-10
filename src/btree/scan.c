/*-------------------------------------------------------------------------
 *
 * scan.c
 *		Routines for sequential scan of orioledb B-tree
 *
 * Copyright (c) 2021-2022, Oriole DB Inc.
 *
 * IDENTIFICATION
 *	  contrib/orioledb/src/btree/scan.c
 *
 * ALGORITHM
 *
 *		The big picture algorithm of sequential scan is following.
 *		1. Scan all the internal pages with level == 1. The total amount of
 *		   internal pages are expected to be small. So, it should be OK to
 *		   scan them in logical order.
 *		   1.1. Immediately scan children's leaves and return their contents.
 *		   1.2. Edge cases are handled using iterators. They are expected to
 *		   be very rare.
 *		   1.3. Collect on-disk downlinks into an array together with CSN at
 *		   the moment of the corresponding internal page read.
 *		2. Ascending sort array of downlinks providing as sequential access
 *		   pattern as possible.
 *		3. Scan sorted downlink and apply the corresponding CSN.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "orioledb.h"

#include "btree/btree.h"
#include "btree/find.h"
#include "btree/io.h"
#include "btree/iterator.h"
#include "btree/page_chunks.h"
#include "btree/scan.h"
#include "btree/undo.h"
#include "tuple/slot.h"
#include "utils/sampling.h"
#include "utils/stopevent.h"
#include "tableam/handler.h" /* include OScanDesc and ParallelOscanDesc. XXX Consider optimizing order of includes later */

#include "miscadmin.h"

typedef enum
{
	BTreeSeqScanInMemory,
	BTreeSeqScanDisk,
	BTreeSeqScanFinished
} BTreeSeqScanStatus;

typedef struct
{
	uint64		downlink;
	CommitSeqNo csn;
} BTreeSeqScanDiskDownlink;

struct BTreeSeqScan
{
	BTreeDescr *desc;

	char		leafImg[ORIOLEDB_BLCKSZ];
	char		histImg[ORIOLEDB_BLCKSZ];

	CommitSeqNo snapshotCsn;
	OBTreeFindPageContext context;
	OFixedKey	prevHikey;
	OFixedKey	curHikey;
	BTreeLocationHint hint;

	BTreePageItemLocator intLoc;

	BTreePageItemLocator leafLoc;

	bool		haveHistImg;
	BTreePageItemLocator histLoc;

	BTreeSeqScanStatus status;
	MemoryContext mctx;

	BTreeSeqScanDiskDownlink *diskDownlinks;
	int64		downlinksCount;
	int64		downlinkIndex;
	int64		allocatedDownlinks;

	BTreeIterator *iter;
	OTuple		iterEnd;

	/*
	 * Number of the last completed checkpoint when scan was started.  We need
	 * on-disk pages of this checkpoint to be not overriden until scan
	 * finishes.  This means we shouldn't start using free blocks of later
	 * checkpoints before this scan is finished.
	 */
	uint32		checkpointNumber;

	BTreeMetaPage *metaPageBlkno;
	dlist_node	listNode;

	bool		firstNextKey;
	OFixedKey	nextKey;

	bool		needSampling;
	BlockSampler sampler;
	BlockNumber samplingNumber;
	BlockNumber samplingNext;

	BTreeSeqScanCallbacks *cb;
	void	   *arg;

	/* Private parallel worker info in a backend */
	bool	 is_leader;
	int		 worker_number;
};

static dlist_head listOfScans = DLIST_STATIC_INIT(listOfScans);

static void
load_first_historical_page(BTreeSeqScan *scan)
{
	BTreePageHeader *header = (BTreePageHeader *) scan->leafImg;
	Pointer		key = NULL;
	BTreeKeyType kind = BTreeKeyNone;
	OFixedKey	lokey,
			   *lokeyPtr = &lokey;
	OFixedKey	hikey;

//	elog(WARNING, "worker %d, load_first_historical_page", scan->worker_number);
	scan->haveHistImg = false;
	if (!COMMITSEQNO_IS_NORMAL(scan->snapshotCsn))
		return;

	if (!O_PAGE_IS(scan->leafImg, RIGHTMOST))
		copy_fixed_hikey(scan->desc, &hikey, scan->leafImg);
	else
		O_TUPLE_SET_NULL(hikey.tuple);
	O_TUPLE_SET_NULL(lokey.tuple);

	while (COMMITSEQNO_IS_NORMAL(header->csn) &&
		   header->csn >= scan->snapshotCsn)
	{
		if (!UNDO_REC_EXISTS(header->undoLocation))
		{
			ereport(ERROR,
					(errcode(ERRCODE_SNAPSHOT_TOO_OLD),
					 errmsg("snapshot too old")));
		}

		(void) get_page_from_undo(scan->desc, header->undoLocation, key, kind,
								  scan->histImg, NULL, NULL, NULL,
								  lokeyPtr, &hikey.tuple);

		if (!O_PAGE_IS(scan->histImg, RIGHTMOST))
			copy_fixed_hikey(scan->desc, &hikey, scan->histImg);
		else
			O_TUPLE_SET_NULL(hikey.tuple);

		scan->haveHistImg = true;
		header = (BTreePageHeader *) scan->histImg;
		if (!O_TUPLE_IS_NULL(lokey.tuple))
		{
			key = (Pointer) &lokey.tuple;
			kind = BTreeKeyNonLeafKey;
			lokeyPtr = NULL;
		}
	}

	if (!scan->haveHistImg)
		return;

	if (!O_TUPLE_IS_NULL(lokey.tuple))
	{
		(void) btree_page_search(scan->desc, scan->histImg,
								 (Pointer) &lokey.tuple,
								 BTreeKeyNonLeafKey, NULL,
								 &scan->histLoc);
		(void) page_locator_find_real_item(scan->histImg, NULL, &scan->histLoc);
	}
	else
	{
		BTREE_PAGE_LOCATOR_FIRST(scan->histImg, &scan->histLoc);
	}

}

static void
load_next_historical_page(BTreeSeqScan *scan)
{
	BTreePageHeader *header = (BTreePageHeader *) scan->leafImg;
	OFixedKey	prevHikey;

	copy_fixed_hikey(scan->desc, &prevHikey, scan->histImg);

	while (COMMITSEQNO_IS_NORMAL(header->csn) &&
		   header->csn >= scan->snapshotCsn)
	{
		if (!UNDO_REC_EXISTS(header->undoLocation))
		{
			ereport(ERROR,
					(errcode(ERRCODE_SNAPSHOT_TOO_OLD),
					 errmsg("snapshot too old")));
		}
		(void) get_page_from_undo(scan->desc, header->undoLocation,
								  (Pointer) &prevHikey.tuple, BTreeKeyNonLeafKey,
								  scan->histImg, NULL, NULL, NULL,
								  NULL, NULL);
		header = (BTreePageHeader *) scan->histImg;
	}
	BTREE_PAGE_LOCATOR_FIRST(scan->histImg, &scan->histLoc);
}

/*
 * Returns first free shared internal page slot form parallel descriptor.
 * Returns zero if all sots busy.
 * Should be called under the lock.
 */
static inline int
get_free_shared_int_page_slot(ParallelOScanDesc poscan)
{
	if(poscan->int_page[0].is_empty)
			return 1;
	else if(poscan->int_page[1].is_empty)
			return 2;
	else return 0;
}

static int
load_next_internal_page(BTreeSeqScan *scan, ParallelOScanDesc poscan)
{
	int		loaded = 0;

	scan->context.flags &= ~BTREE_PAGE_FIND_DOWNLINK_LOCATION;
	if (!O_TUPLE_IS_NULL(scan->curHikey.tuple))
	{
		copy_fixed_key(scan->desc, &scan->prevHikey, scan->curHikey.tuple);
		find_page(&scan->context, &scan->curHikey.tuple,
				  BTreeKeyNonLeafKey, 1);
	}
	else
		find_page(&scan->context, NULL, BTreeKeyNone, 1);

	if (!O_PAGE_IS(scan->context.img, RIGHTMOST))
		copy_fixed_hikey(scan->desc, &scan->curHikey, scan->context.img);
	else
		clear_fixed_key(&scan->curHikey);

	if (PAGE_GET_LEVEL(scan->context.img) == 1)
	{
		BTREE_PAGE_LOCATOR_FIRST(scan->context.img, &scan->intLoc);

		if (poscan)
		{
			/* Load parallel data for this internal page to a free slot in
			 * ParallelOScanDescData
			 */
			int free_slot;

			SpinLockAcquire(&poscan->mutex);
			free_slot = get_free_shared_int_page_slot(poscan);

			if (free_slot)
			{
				BTreeIntPageParallel 	shared_int_page = &poscan->int_page[free_slot - 1];

				shared_int_page->is_empty = false;
				memcpy(&shared_int_page->img, &scan->context.img, ORIOLEDB_BLCKSZ);
				shared_int_page->offset = BTREE_PAGE_LOCATOR_GET_OFFSET(scan->context.img,
											&scan->intLoc);
				loaded = 1;
				elog(WARNING, "worker %d loaded #%d int page", scan->worker_number, free_slot);
			}
			SpinLockRelease(&poscan->mutex);

			/* Try to load second page into shared state */
			if (loaded == 1 && free_slot == 1)
			{
				elog(WARNING, "worker %d try to load second int page", scan->worker_number);
				loaded += load_next_internal_page(scan, poscan);
			}
		}
		else
			loaded = 1;
	}
	else
	{
		Assert(PAGE_GET_LEVEL(scan->context.img) == 0); /* leaf page */
		memcpy(scan->leafImg, scan->context.img, ORIOLEDB_BLCKSZ);
		BTREE_PAGE_LOCATOR_FIRST(scan->leafImg, &scan->leafLoc);
		scan->hint.blkno = scan->context.items[0].blkno;
		scan->hint.pageChangeCount = scan->context.items[0].pageChangeCount;
		BTREE_PAGE_LOCATOR_SET_INVALID(&scan->intLoc);
		scan->firstNextKey = true;
		O_TUPLE_SET_NULL(scan->nextKey.tuple);
		load_first_historical_page(scan);
	}
	elog(WARNING, "worker %d, loaded %d int pages", scan->worker_number, loaded);
	return loaded;
}

static void
add_on_disk_downlink(BTreeSeqScan *scan, uint64 downlink, CommitSeqNo csn)
{
	if (scan->downlinksCount >= scan->allocatedDownlinks)
	{
		scan->allocatedDownlinks *= 2;
		scan->diskDownlinks = (BTreeSeqScanDiskDownlink *) repalloc_huge(scan->diskDownlinks,
																		 sizeof(scan->diskDownlinks[0]) * scan->allocatedDownlinks);
	}
	scan->diskDownlinks[scan->downlinksCount].downlink = downlink;
	scan->diskDownlinks[scan->downlinksCount].csn = csn;
	scan->downlinksCount++;
}

static int
cmp_downlinks(const void *p1, const void *p2)
{
	uint64		d1 = ((BTreeSeqScanDiskDownlink *) p1)->downlink;
	uint64		d2 = ((BTreeSeqScanDiskDownlink *) p2)->downlink;

	if (d1 < d2)
		return -1;
	else if (d1 == d2)
		return 0;
	else
		return 1;
}

static void
switch_to_disk_scan(BTreeSeqScan *scan)
{
	scan->status = BTreeSeqScanDisk;
	BTREE_PAGE_LOCATOR_SET_INVALID(&scan->leafLoc);
	qsort(scan->diskDownlinks,
		  scan->downlinksCount,
		  sizeof(scan->diskDownlinks[0]),
		  cmp_downlinks);
}

/*
 * Make an interator to read the key range from `startKey` to the next
 * downlink or hikey of internal page hikey if we're considering the last
 * downlink.
 */
static void
scan_make_iterator(BTreeSeqScan *scan, OTuple startKey, ParallelOScanDesc poscan)
{
	MemoryContext mctx;

	mctx = MemoryContextSwitchTo(scan->mctx);
	if (!O_TUPLE_IS_NULL(startKey))
		scan->iter = o_btree_iterator_create(scan->desc, &startKey, BTreeKeyNonLeafKey,
											 scan->snapshotCsn,
											 ForwardScanDirection);
	else
		scan->iter = o_btree_iterator_create(scan->desc, NULL, BTreeKeyNone,
											 scan->snapshotCsn,
											 ForwardScanDirection);
	MemoryContextSwitchTo(mctx);

	BTREE_PAGE_LOCATOR_SET_INVALID(&scan->leafLoc);
	scan->haveHistImg = false;

	BTREE_PAGE_LOCATOR_NEXT(scan->context.img, &scan->intLoc);
	if (BTREE_PAGE_LOCATOR_IS_VALID(scan->context.img, &scan->intLoc))
		BTREE_PAGE_READ_INTERNAL_TUPLE(scan->iterEnd, scan->context.img, &scan->intLoc);
	else if (!O_PAGE_IS(scan->context.img, RIGHTMOST))
		BTREE_PAGE_GET_HIKEY(scan->iterEnd, scan->context.img);
	else
		O_TUPLE_SET_NULL(scan->iterEnd);
}

static void
refind_downlink(BTreeSeqScan *scan, ParallelOScanDesc poscan)
{
	OFixedKey	refindKey;
	OTuple		downlinkKey;
	int			cmp;

	scan->context.flags |= BTREE_PAGE_FIND_DOWNLINK_LOCATION;
	if (BTREE_PAGE_LOCATOR_GET_OFFSET(scan->context.img, &scan->intLoc) != 0)
		copy_fixed_page_key(scan->desc, &refindKey, scan->context.img, &scan->intLoc);
	else
		copy_fixed_key(scan->desc, &refindKey, scan->context.lokey.tuple);

	if (!O_TUPLE_IS_NULL(refindKey.tuple))
		find_page(&scan->context, &refindKey.tuple, BTreeKeyNonLeafKey, 1);
	else
		find_page(&scan->context, NULL, BTreeKeyNone, 1);

	if (!O_PAGE_IS(scan->context.img, RIGHTMOST))
		copy_fixed_hikey(scan->desc, &scan->curHikey, scan->context.img);
	else
		clear_fixed_key(&scan->curHikey);

	scan->intLoc = scan->context.items[scan->context.index].locator;
	if (O_TUPLE_IS_NULL(scan->context.lokey.tuple))
		return;

	BTREE_PAGE_READ_INTERNAL_TUPLE(downlinkKey, scan->context.img, &scan->intLoc);
	cmp = o_btree_cmp(scan->desc,
					  &downlinkKey, BTreeKeyNonLeafKey,
					  &refindKey.tuple, BTreeKeyNonLeafKey);
	if (cmp != 0)
	{
		Assert(cmp < 0);
		scan_make_iterator(scan, downlinkKey, poscan);
	}
}

/*
 * Checks if loaded leaf page matches downlink of internal page.  Makes iterator
 * to read the considered key range if check failed.
 *
 * Hikey of leaf page should match to next downlink or internal page hikey if
 * we're considering the last downlink.
 */
static void
check_in_memory_leaf_page(BTreeSeqScan *scan, ParallelOScanDesc poscan)
{
	OTuple		nextKey,
				leafHikey;
	BTreePageItemLocator next = scan->intLoc;
	bool		result = false;

	BTREE_PAGE_LOCATOR_NEXT(scan->context.img, &next);
	if (BTREE_PAGE_LOCATOR_IS_VALID(scan->context.img, &next))
		BTREE_PAGE_READ_INTERNAL_TUPLE(nextKey, scan->context.img, &next);
	else if (!O_PAGE_IS(scan->context.img, RIGHTMOST))
		BTREE_PAGE_GET_HIKEY(nextKey, scan->context.img);
	else
		O_TUPLE_SET_NULL(nextKey);

	if (!O_PAGE_IS(scan->leafImg, RIGHTMOST))
		BTREE_PAGE_GET_HIKEY(leafHikey, scan->leafImg);
	else
		O_TUPLE_SET_NULL(leafHikey);

	if (O_TUPLE_IS_NULL(nextKey) && O_TUPLE_IS_NULL(leafHikey))
		return;

	if (O_TUPLE_IS_NULL(nextKey) || O_TUPLE_IS_NULL(leafHikey))
	{
		result = true;
	}
	else
	{
		if (o_btree_cmp(scan->desc,
						&nextKey, BTreeKeyNonLeafKey,
						&leafHikey, BTreeKeyNonLeafKey) != 0)
			result = true;
	}

	if (result)
	{
		OTuple		startKey;

		if (BTREE_PAGE_LOCATOR_GET_OFFSET(scan->context.img, &scan->intLoc) != 0)
			BTREE_PAGE_READ_INTERNAL_TUPLE(startKey, scan->context.img, &scan->intLoc);
		else if (!O_PAGE_IS(scan->context.img, LEFTMOST))
			startKey = scan->context.lokey.tuple;
		else
			O_TUPLE_SET_NULL(startKey);
		scan_make_iterator(scan, startKey, poscan);
	}
}

/*
 * Output current item locator from parallel state for usage in local backend as next.
 * Then increase item locator in parallel state to be taken on a next call of internal_locator_next()
 */
static bool
internal_locator_next(BTreeSeqScan *scan, ParallelOScanDesc poscan,
					  BTreePageItemLocator *intLoc)
{
	if (poscan)
	{
		BTreeIntPageParallel 	parallel_int_page;
		BTreePageItemLocator 	tmpLoc;

		SpinLockAcquire(&poscan->mutex);
		parallel_int_page = &poscan->int_page[poscan->cur_int_page];
		/* Fetch next item locator from parallel state to local backend memory */

		if (parallel_int_page->is_empty)
			elog(ERROR, "		is_empty from parallel locator"); /* should not be */

		BTREE_PAGE_OFFSET_GET_LOCATOR(scan->context.img, parallel_int_page->offset,
									  intLoc);

		elog(WARNING, "		worker %d, internal_locator_next (%d,%d)", scan->worker_number, scan->intLoc.chunkOffset, scan->intLoc.itemOffset);

		/*
		 * Increase next item locator in parallel state. It will affect the other
		 * workers, but the local worker only after the next call of
		 * internal_locator_next()
		 */
	    tmpLoc = scan->intLoc;
		BTREE_PAGE_LOCATOR_NEXT(scan->context.img, &tmpLoc);
		parallel_int_page->offset = BTREE_PAGE_LOCATOR_GET_OFFSET (scan->context.img, &tmpLoc);

		if (!BTREE_PAGE_LOCATOR_IS_VALID(scan->context.img, &tmpLoc))
		{
			parallel_int_page->is_empty = true;
			elog(WARNING, "		worker %d, end _next_ page", scan->worker_number);
		}
		SpinLockRelease(&poscan->mutex);
	}
	else
		BTREE_PAGE_LOCATOR_NEXT(scan->context.img, intLoc);

	/* NB: validity check remain just in case, as of now the result is not evaluated by
	 * callers.
	/* End of internal page */
	if (!BTREE_PAGE_LOCATOR_IS_VALID(scan->context.img, intLoc))
	{
		elog(WARNING, "		worker %d, internal_locator_next (%d,%d) invalid locator from %s",
			 scan->worker_number, scan->intLoc.chunkOffset, scan->intLoc.itemOffset, poscan ? "parallel" : "end page");

		return false;
	}

	return true;
}

/*
 * Interates the internal page till we either:
 *  - Successfully read the next in-memory leaf page;
 *  - Made an iterator to read key range, which belongs to current downlink;
 *  - Reached the end of internal page.
 */
static bool
iterate_internal_page(BTreeSeqScan *scan, ParallelOScanDesc poscan)
{
	if (!internal_locator_next(scan, poscan, &scan->intLoc))
		return false;

	while (BTREE_PAGE_LOCATOR_IS_VALID(scan->context.img, &scan->intLoc))
	{
		BTreeNonLeafTuphdr *tuphdr;
		OTuple		tuple;
		uint64		downlink;
		bool		valid_downlink = true;

		STOPEVENT(STOPEVENT_STEP_DOWN,
				  btree_downlink_stopevent_params(scan->desc,
												  scan->context.img,
												  &scan->intLoc));

		BTREE_PAGE_READ_INTERNAL_ITEM(tuphdr, tuple, scan->context.img, &scan->intLoc);
		downlink = tuphdr->downlink;

		/* Special cases */
		if (scan->cb && scan->cb->isRangeValid)
		{
			BTreePageHeader *header = (BTreePageHeader *) scan->context.img;
			BTreePageItemLocator end_locator = scan->intLoc;
			OTuple		start_tuple = {0};
			OTuple		end_tuple = {0};
			bool		has_next_downlink = true;

			if ((scan->intLoc.chunkOffset == 0 &&
				 scan->intLoc.itemOffset == 0))
			{
				if (!O_TUPLE_IS_NULL(scan->prevHikey.tuple))
					start_tuple = scan->prevHikey.tuple;
			}
			else
				start_tuple = tuple;

			elog(WARNING, "cb");
			internal_locator_next(scan, poscan, &end_locator);
			if (end_locator.chunkOffset == header->chunksCount - 1)
			{
				if (end_locator.itemOffset ==
					end_locator.chunkItemsCount)
				{
					if (!O_PAGE_IS(scan->context.img, RIGHTMOST))
						end_tuple = scan->curHikey.tuple;
					has_next_downlink = false;
				}
			}

			if (has_next_downlink)
			{
				BTreeNonLeafTuphdr *tuphdr pg_attribute_unused();

				BTREE_PAGE_READ_INTERNAL_ITEM(tuphdr, end_tuple,
											  scan->context.img, &end_locator);
			}
			valid_downlink = scan->cb->isRangeValid(start_tuple, end_tuple,
													scan->arg);
		}
		else if (scan->needSampling)
		{
			if (scan->samplingNumber < scan->samplingNext)
			{
				valid_downlink = false;
			}
			else
			{
				if (BlockSampler_HasMore(scan->sampler))
					scan->samplingNext = BlockSampler_Next(scan->sampler);
				else
					scan->samplingNext = InvalidBlockNumber;
			}
			scan->samplingNumber++;
			elog (WARNING, "sampler");
		}

		/* General case */
		if (valid_downlink)
		{
			if (DOWNLINK_IS_ON_DISK(downlink))
			{
				add_on_disk_downlink(scan, downlink, scan->context.imgReadCsn);
			}
			else if (DOWNLINK_IS_IN_MEMORY(downlink))
			{
				ReadPageResult result;

				result = o_btree_try_read_page(
											   scan->desc,
											   DOWNLINK_GET_IN_MEMORY_BLKNO(downlink),
											   DOWNLINK_GET_IN_MEMORY_CHANGECOUNT(downlink),
											   scan->leafImg,
											   scan->context.imgReadCsn,
											   NULL,
											   BTreeKeyNone,
											   NULL,
											   NULL);

				if (result == ReadPageResultOK)
				{
					//elog(WARNING, "worker %d, check_in_memory_leaf_page", scan->worker_number);
					check_in_memory_leaf_page(scan, poscan);
					if (scan->iter)
						return true;

					scan->hint.blkno = DOWNLINK_GET_IN_MEMORY_BLKNO(downlink);
					scan->hint.pageChangeCount = DOWNLINK_GET_IN_MEMORY_CHANGECOUNT(downlink);
					BTREE_PAGE_LOCATOR_FIRST(scan->leafImg, &scan->leafLoc);
					internal_locator_next(scan, poscan, &scan->intLoc);

					scan->firstNextKey = true;
					O_TUPLE_SET_NULL(scan->nextKey.tuple);
					load_first_historical_page(scan);
					return true;
				}
				else
				{
					refind_downlink(scan, poscan);
					if (scan->iter)
						return true;
					continue;
				}
			}
			else if (DOWNLINK_IS_IN_IO(downlink))
			{
				/*
				 * Downlink has currently IO in-progress.  Wait for IO
				 * completion and refind this downlink.
				 */
				int			ionum = DOWNLINK_GET_IO_LOCKNUM(downlink);

				wait_for_io_completion(ionum);

				refind_downlink(scan, poscan);
				if (scan->iter)
					return true;
				continue;
			}
		}
		internal_locator_next(scan, poscan, &scan->intLoc);
	}
	return false;
}

static bool
load_next_in_memory_leaf_page(BTreeSeqScan *scan, ParallelOScanDesc poscan)
{
	while (true)
	{
		if (poscan)
		{
			/*
			 * Fetch current internal page data and current item locator from
			 * parallel state to local backend memory
			 */
			SpinLockAcquire(&poscan->mutex);
			Assert(!poscan->int_page[poscan->cur_int_page].is_empty);
			memcpy(&scan->context.img, &poscan->int_page[poscan->cur_int_page].img, ORIOLEDB_BLCKSZ);
			BTREE_PAGE_OFFSET_GET_LOCATOR(scan->context.img,
										  poscan->int_page[poscan->cur_int_page].offset,
										  &scan->intLoc);
			elog(WARNING, "worker %d, load_next_in_memory_leaf_page, cur_int_page = %d", scan->worker_number, poscan->cur_int_page);
			SpinLockRelease(&poscan->mutex);
		}

		if (iterate_internal_page(scan, poscan))
			break;

		/* XXX What if intLoc or cur_int_page changed by concurrent process? */

//		if (poscan)
//		{
			/* Update current item locator in the parallel state unless a concurrent process have already went ahead */
//			SpinLockAcquire(&poscan->mutex);
//			if (poscan->int_page[poscan->cur_int_page].intLoc.chunkOffset <= scan->intLoc.chunkOffset &&
//					poscan->int_page[poscan->cur_int_page].intLoc.itemOffset < scan->intLoc.itemOffset)
//			{
//				poscan->int_page[poscan->cur_int_page].intLoc = scan->intLoc;
//				elog(WARNING, "worker %d, load_next_in_memory_leaf_page, update cur_int_page %d (%d,%d)", scan->worker_number, poscan->cur_int_page, scan->intLoc.chunkOffset, scan->intLoc.itemOffset );
//			}
//			else
//				elog(WARNING, "worker %d, load_next_in_memory_leaf_page, another worker (%d,%d) ahead of current (%d,%d)", scan->worker_number,
//						poscan->int_page[poscan->cur_int_page].intLoc.chunkOffset, poscan->int_page[poscan->cur_int_page].intLoc.itemOffset,
//						scan->intLoc.itemOffset, scan->intLoc.chunkOffset);
//
//			SpinLockRelease(&poscan->mutex);
//		}

		if (O_TUPLE_IS_NULL(scan->curHikey.tuple))
		{
			return false;
		}
		else
		{
			bool		result PG_USED_FOR_ASSERTS_ONLY;

			if (poscan)
			{
				/* Internal page finished */
				SpinLockAcquire(&poscan->mutex);
				poscan->int_page[poscan->cur_int_page].is_empty = true;
				SpinLockRelease(&poscan->mutex);
			}

			elog(WARNING, "worker %d, load_next_internal_page (2)", scan->worker_number);
			result = load_next_internal_page(scan, poscan);
			Assert(result);
		}
	}
	return true;
}

static bool
load_next_disk_leaf_page(BTreeSeqScan *scan)
{
	FileExtent	extent;
	bool		success;
	BTreePageHeader *header;
	BTreeSeqScanDiskDownlink downlink;

	if (scan->downlinkIndex >= scan->downlinksCount)
		return false;

	downlink = scan->diskDownlinks[scan->downlinkIndex];
	success = read_page_from_disk(scan->desc,
								  scan->leafImg,
								  downlink.downlink,
								  &extent);
	header = (BTreePageHeader *) scan->leafImg;
	if (header->csn >= downlink.csn)
		read_page_from_undo(scan->desc, scan->leafImg, header->undoLocation,
							downlink.csn, NULL, BTreeKeyNone, NULL);

	STOPEVENT(STOPEVENT_SCAN_DISK_PAGE,
			  btree_page_stopevent_params(scan->desc,
										  scan->leafImg));

	if (!success)
		elog(ERROR, "can not read leaf page from disk");

	BTREE_PAGE_LOCATOR_FIRST(scan->leafImg, &scan->leafLoc);
	scan->downlinkIndex++;
	scan->hint.blkno = OInvalidInMemoryBlkno;
	scan->hint.pageChangeCount = InvalidOPageChangeCount;
	scan->firstNextKey = true;
	O_TUPLE_SET_NULL(scan->nextKey.tuple);
	load_first_historical_page(scan);
	return true;
}

static BTreeSeqScan *
make_btree_seq_scan_internal(BTreeDescr *desc, CommitSeqNo csn,
							 BTreeSeqScanCallbacks *cb, void *arg,
							 BlockSampler sampler, ParallelOScanDesc poscan)
{
	BTreeSeqScan *scan = (BTreeSeqScan *) MemoryContextAlloc(TopMemoryContext, sizeof(BTreeSeqScan));
	uint32		checkpointNumberBefore,
				checkpointNumberAfter;
	bool		checkpointConcurrent;
	BTreeMetaPage *metaPageBlkno = BTREE_GET_META(desc);
	int 		i = 0;

	if(poscan)
	{

		SpinLockAcquire(&poscan->mutex);
		for (scan->worker_number = 0; poscan->worker_active[scan->worker_number] == true; scan->worker_number++) {}

//		elog(WARNING, "make_btree_seq_scan_internal, scan=%x, poscan=%x, i = %d", scan, poscan, i);
		poscan->worker_active[scan->worker_number] = true;

		/* leader */
		if (scan->worker_number == 0)
		{
//			elog(WARNING, "leader started, scan=%x, poscan=%x", scan, poscan);
			Assert(!poscan->leader_started);
			poscan->leader_started = true;
			scan->is_leader = true;
		}

		SpinLockRelease(&poscan->mutex);
	}

	elog(WARNING, "make_btree_seq_scan_internal. %s worker %d, is_leader = %s", poscan ? "Parallel" : "Single", scan->worker_number, scan->is_leader ? "Y" : "N");

	scan->desc = desc;
	scan->snapshotCsn = csn;
	scan->status = BTreeSeqScanInMemory;
	scan->allocatedDownlinks = 16;
	scan->downlinksCount = 0;
	scan->downlinkIndex = 0;
	scan->diskDownlinks = (BTreeSeqScanDiskDownlink *) palloc(sizeof(scan->diskDownlinks[0]) * scan->allocatedDownlinks);
	scan->mctx = CurrentMemoryContext;
	scan->iter = NULL;
	scan->cb = cb;
	scan->arg = arg;
	scan->firstNextKey = true;

	scan->samplingNumber = 0;
	scan->sampler = sampler;
	if (sampler)
	{
		scan->needSampling = true;
		if (BlockSampler_HasMore(scan->sampler))
			scan->samplingNext = BlockSampler_Next(scan->sampler);
		else
			scan->samplingNext = InvalidBlockNumber;
	}
	else
	{
		scan->needSampling = false;
		scan->samplingNext = InvalidBlockNumber;
	}

	O_TUPLE_SET_NULL(scan->nextKey.tuple);

	START_CRIT_SECTION();
	dlist_push_tail(&listOfScans, &scan->listNode);

	/*
	 * Get the checkpoint number for the scan.  There is race condition with
	 * concurrent switching tree to the next checkpoint.  So, we have to
	 * workaround this with recheck-retry loop,
	 */
	checkpointNumberBefore = get_cur_checkpoint_number(&desc->oids,
													   desc->type,
													   &checkpointConcurrent);
	while (true)
	{
		(void) pg_atomic_fetch_add_u32(&metaPageBlkno->numSeqScans[checkpointNumberBefore % NUM_SEQ_SCANS_ARRAY_SIZE], 1);
		checkpointNumberAfter = get_cur_checkpoint_number(&desc->oids,
														  desc->type,
														  &checkpointConcurrent);
		if (checkpointNumberAfter == checkpointNumberBefore)
		{
			scan->checkpointNumber = checkpointNumberBefore;
			break;
		}
		(void) pg_atomic_fetch_sub_u32(&metaPageBlkno->numSeqScans[checkpointNumberBefore % NUM_SEQ_SCANS_ARRAY_SIZE], 1);
		checkpointNumberBefore = checkpointNumberAfter;
	}
	END_CRIT_SECTION();

	init_page_find_context(&scan->context, desc, csn, BTREE_PAGE_FIND_IMAGE |
						   BTREE_PAGE_FIND_KEEP_LOKEY |
						   BTREE_PAGE_FIND_READ_CSN);
	clear_fixed_key(&scan->prevHikey);
	clear_fixed_key(&scan->curHikey);

	if (load_next_internal_page(scan, poscan) ||
			(poscan && ( !poscan->int_page[0].is_empty || !poscan->int_page[1].is_empty)))
	{
		if (!load_next_in_memory_leaf_page(scan, poscan))
		{
			switch_to_disk_scan(scan);
			if (!load_next_disk_leaf_page(scan))
				scan->status = BTreeSeqScanFinished;
		}
	}
	return scan;
}

BTreeSeqScan *
make_btree_seq_scan(BTreeDescr *desc, CommitSeqNo csn, void *poscan)
{
	return make_btree_seq_scan_internal(desc, csn, NULL, NULL, NULL, poscan);
}

BTreeSeqScan *
make_btree_seq_scan_cb(BTreeDescr *desc, CommitSeqNo csn,
					   BTreeSeqScanCallbacks *cb, void *arg)
{
	return make_btree_seq_scan_internal(desc, csn, cb, arg, NULL, NULL);
}

BTreeSeqScan *
make_btree_sampling_scan(BTreeDescr *desc, BlockSampler sampler)
{
	return make_btree_seq_scan_internal(desc, COMMITSEQNO_INPROGRESS,
										NULL, NULL, sampler, NULL);
}

static OTuple
btree_seq_scan_get_tuple_from_iterator(BTreeSeqScan *scan,
									   CommitSeqNo *tupleCsn,
									   BTreeLocationHint *hint)
{
	OTuple		result;

	if (!O_TUPLE_IS_NULL(scan->iterEnd))
		result = o_btree_iterator_fetch(scan->iter, tupleCsn,
										&scan->iterEnd, BTreeKeyNonLeafKey,
										false, hint);
	else
		result = o_btree_iterator_fetch(scan->iter, tupleCsn,
										NULL, BTreeKeyNone,
										false, hint);

	if (O_TUPLE_IS_NULL(result))
	{
		btree_iterator_free(scan->iter);
		scan->iter = NULL;
	}
	return result;
}

static bool
adjust_location_with_next_key(BTreeSeqScan *scan,
							  Page p, BTreePageItemLocator *loc)
{
	BTreeDescr *desc = scan->desc;
	BTreePageHeader *header = (BTreePageHeader *) p;
	int			cmp;
	OTuple		key;

	if (!BTREE_PAGE_LOCATOR_IS_VALID(p, loc))
		return false;

	BTREE_PAGE_READ_LEAF_TUPLE(key, p, loc);

	cmp = o_btree_cmp(desc, &key, BTreeKeyLeafTuple,
					  &scan->nextKey.tuple, BTreeKeyNonLeafKey);
	if (cmp == 0)
		return true;
	if (cmp > 0)
		return false;

	while (true)
	{
		if (loc->chunkOffset == (header->chunksCount - 1))
			break;

		key.formatFlags = header->chunkDesc[loc->chunkOffset].hikeyFlags;
		key.data = (Pointer) p + SHORT_GET_LOCATION(header->chunkDesc[loc->chunkOffset].hikeyShortLocation);
		cmp = o_btree_cmp(desc, &key, BTreeKeyNonLeafKey,
						  &scan->nextKey.tuple, BTreeKeyNonLeafKey);
		if (cmp > 0)
			break;
		loc->itemOffset = loc->chunkItemsCount;
		if (!page_locator_next_chunk(p, loc))
		{
			BTREE_PAGE_LOCATOR_SET_INVALID(loc);
			return false;
		}
	}

	while (BTREE_PAGE_LOCATOR_IS_VALID(p, loc))
	{
		BTREE_PAGE_READ_LEAF_TUPLE(key, p, loc);
		cmp = o_btree_cmp(desc,
						  &key, BTreeKeyLeafTuple,
						  &scan->nextKey.tuple, BTreeKeyNonLeafKey);
		if (cmp == 0)
			return true;
		if (cmp > 0)
			break;
		BTREE_PAGE_LOCATOR_NEXT(p, loc);
	}

	return false;
}

static void
apply_next_key(BTreeSeqScan *scan)
{
	BTreeDescr *desc = scan->desc;

	Assert(BTREE_PAGE_LOCATOR_IS_VALID(scan->leafImg, &scan->leafLoc) ||
		   (scan->haveHistImg && BTREE_PAGE_LOCATOR_IS_VALID(scan->histImg, &scan->histLoc)));

	while (true)
	{
		OTuple		key;
		bool		leafResult,
					histResult;

		if (BTREE_PAGE_LOCATOR_IS_VALID(scan->leafImg, &scan->leafLoc))
			BTREE_PAGE_READ_LEAF_TUPLE(key, scan->leafImg, &scan->leafLoc);
		else
			O_TUPLE_SET_NULL(key);

		if (scan->haveHistImg &&
			BTREE_PAGE_LOCATOR_IS_VALID(scan->histImg, &scan->histLoc))
		{
			if (O_TUPLE_IS_NULL(key))
			{
				BTREE_PAGE_READ_LEAF_TUPLE(key, scan->histImg, &scan->histLoc);
			}
			else
			{
				OTuple		histKey;

				BTREE_PAGE_READ_LEAF_TUPLE(histKey, scan->histImg, &scan->histLoc);
				if (o_btree_cmp(desc,
								&key, BTreeKeyLeafTuple,
								&histKey, BTreeKeyNonLeafKey) > 0)
					key = histKey;
			}
		}

		scan->nextKey.tuple = key;
		if (O_TUPLE_IS_NULL(key) ||
			!scan->cb->getNextKey(&scan->nextKey, true, scan->arg))
		{
			BTREE_PAGE_LOCATOR_SET_INVALID(&scan->leafLoc);
			return;
		}

		leafResult = adjust_location_with_next_key(scan,
												   scan->leafImg,
												   &scan->leafLoc);
		if (scan->haveHistImg)
		{
			histResult = adjust_location_with_next_key(scan,
													   scan->histImg,
													   &scan->histLoc);
			if (leafResult || histResult)
				return;
		}
		else if (leafResult)
			return;

		if (!BTREE_PAGE_LOCATOR_IS_VALID(scan->leafImg, &scan->leafLoc) &&
			!(scan->haveHistImg &&
			  BTREE_PAGE_LOCATOR_IS_VALID(scan->histImg, &scan->histLoc)))
			return;
	}
}

static OTuple
btree_seq_scan_getnext_internal(BTreeSeqScan *scan, MemoryContext mctx,
								CommitSeqNo *tupleCsn, BTreeLocationHint *hint)
{
	OTuple		tuple;

	if (scan->iter)
	{
		tuple = btree_seq_scan_get_tuple_from_iterator(scan, tupleCsn, hint);
		if (!O_TUPLE_IS_NULL(tuple))
			return tuple;
	}

	while (true)
	{
		while (scan->haveHistImg)
		{
			OTuple		histTuple;

			while (!BTREE_PAGE_LOCATOR_IS_VALID(scan->histImg, &scan->histLoc))
			{
				if (O_PAGE_IS(scan->histImg, RIGHTMOST))
				{
					scan->haveHistImg = false;
					break;
				}
				if (!O_PAGE_IS(scan->leafImg, RIGHTMOST))
				{
					OTuple		leafHikey,
								histHikey;

					BTREE_PAGE_GET_HIKEY(leafHikey, scan->leafImg);
					BTREE_PAGE_GET_HIKEY(histHikey, scan->histImg);
					if (o_btree_cmp(scan->desc,
									&histHikey, BTreeKeyNonLeafKey,
									&leafHikey, BTreeKeyNonLeafKey) >= 0)
					{
						scan->haveHistImg = false;
						break;
					}
				}
				load_next_historical_page(scan);
			}

			if (!scan->haveHistImg)
				break;

			if (scan->cb && scan->cb->getNextKey)
				apply_next_key(scan);
			if (!BTREE_PAGE_LOCATOR_IS_VALID(scan->histImg, &scan->histLoc))
				continue;

			BTREE_PAGE_READ_LEAF_TUPLE(histTuple, scan->histImg,
									   &scan->histLoc);
			if (!BTREE_PAGE_LOCATOR_IS_VALID(scan->leafImg, &scan->leafLoc))
			{
				OTuple		leafHikey;

				if (!O_PAGE_IS(scan->leafImg, RIGHTMOST))
				{
					BTREE_PAGE_GET_HIKEY(leafHikey, scan->leafImg);
					if (o_btree_cmp(scan->desc,
									&histTuple, BTreeKeyLeafTuple,
									&leafHikey, BTreeKeyNonLeafKey) >= 0)
					{
						scan->haveHistImg = false;
						break;
					}
				}
			}
			else
			{
				BTreeLeafTuphdr *tuphdr;
				OTuple		leafTuple;
				int			cmp;

				BTREE_PAGE_READ_LEAF_ITEM(tuphdr, leafTuple,
										  scan->leafImg, &scan->leafLoc);

				cmp = o_btree_cmp(scan->desc,
								  &histTuple, BTreeKeyLeafTuple,
								  &leafTuple, BTreeKeyLeafTuple);
				if (cmp > 0)
					break;

				if (cmp == 0)
				{
					if (XACT_INFO_OXID_IS_CURRENT(tuphdr->xactInfo))
					{
						BTREE_PAGE_LOCATOR_NEXT(scan->histImg, &scan->histLoc);
						break;
					}
					else
					{
						BTREE_PAGE_LOCATOR_NEXT(scan->leafImg, &scan->leafLoc);
					}
				}
			}

			tuple = o_find_tuple_version(scan->desc,
										 scan->histImg,
										 &scan->histLoc,
										 scan->snapshotCsn,
										 tupleCsn,
										 mctx,
										 NULL,
										 NULL);
			BTREE_PAGE_LOCATOR_NEXT(scan->histImg, &scan->histLoc);
			if (!O_TUPLE_IS_NULL(tuple))
			{
				if (hint)
					*hint = scan->hint;
				return tuple;
			}
		}

		if (scan->cb && scan->cb->getNextKey &&
			BTREE_PAGE_LOCATOR_IS_VALID(scan->leafImg, &scan->leafLoc))
			apply_next_key(scan);

		if (!BTREE_PAGE_LOCATOR_IS_VALID(scan->leafImg, &scan->leafLoc))
		{
			if (scan->status == BTreeSeqScanInMemory)
			{
				if (load_next_in_memory_leaf_page(scan, NULL))
				{
					if (scan->iter)
					{
						tuple = btree_seq_scan_get_tuple_from_iterator(scan,
																	   tupleCsn,
																	   hint);
						if (!O_TUPLE_IS_NULL(tuple))
							return tuple;
					}
				}
				else
				{
					switch_to_disk_scan(scan);
				}
			}
			if (scan->status == BTreeSeqScanDisk)
			{
				if (!load_next_disk_leaf_page(scan))
				{
					scan->status = BTreeSeqScanFinished;
					O_TUPLE_SET_NULL(tuple);
					return tuple;
				}
			}
			continue;
		}

		tuple = o_find_tuple_version(scan->desc,
									 scan->leafImg,
									 &scan->leafLoc,
									 scan->snapshotCsn,
									 tupleCsn,
									 mctx,
									 NULL,
									 NULL);
		BTREE_PAGE_LOCATOR_NEXT(scan->leafImg, &scan->leafLoc);
		if (!O_TUPLE_IS_NULL(tuple))
		{
			if (hint)
				*hint = scan->hint;
			return tuple;
		}
	}

	/* keep compiler quiet */
	O_TUPLE_SET_NULL(tuple);
	return tuple;
}

OTuple
btree_seq_scan_getnext(BTreeSeqScan *scan, MemoryContext mctx,
					   CommitSeqNo *tupleCsn, BTreeLocationHint *hint)
{
	OTuple		tuple;

	if (scan->status == BTreeSeqScanInMemory ||
		scan->status == BTreeSeqScanDisk)
	{
		tuple = btree_seq_scan_getnext_internal(scan, mctx, tupleCsn, hint);

		if (!O_TUPLE_IS_NULL(tuple))
			return tuple;
	}
	Assert(scan->status == BTreeSeqScanFinished);

	O_TUPLE_SET_NULL(tuple);
	return tuple;
}

static OTuple
btree_seq_scan_get_tuple_from_iterator_raw(BTreeSeqScan *scan,
										   bool *end,
										   BTreeLocationHint *hint)
{
	OTuple		result;

	if (!O_TUPLE_IS_NULL(scan->iterEnd))
		result = btree_iterate_raw(scan->iter, &scan->iterEnd, BTreeKeyNonLeafKey,
								   false, end, hint);
	else
		result = btree_iterate_raw(scan->iter, NULL, BTreeKeyNone,
								   false, end, hint);

	if (*end)
	{
		btree_iterator_free(scan->iter);
		scan->iter = NULL;
	}
	return result;
}

static OTuple
btree_seq_scan_getnext_raw_internal(BTreeSeqScan *scan, MemoryContext mctx,
									BTreeLocationHint *hint)
{
	BTreeLeafTuphdr *tupHdr;
	OTuple		tuple;

	if (scan->iter)
	{
		bool		end;

		tuple = btree_seq_scan_get_tuple_from_iterator_raw(scan, &end, hint);
		if (!end)
			return tuple;
	}

	while (!BTREE_PAGE_LOCATOR_IS_VALID(scan->leafImg, &scan->leafLoc))
	{
		if (scan->status == BTreeSeqScanInMemory)
		{
			if (load_next_in_memory_leaf_page(scan, NULL))
			{
				if (scan->iter)
				{
					bool		end;

					tuple = btree_seq_scan_get_tuple_from_iterator_raw(scan, &end, hint);
					if (!end)
						return tuple;
				}
			}
			else
			{
				switch_to_disk_scan(scan);
			}
		}
		if (scan->status == BTreeSeqScanDisk)
		{
			if (!load_next_disk_leaf_page(scan))
			{
				scan->status = BTreeSeqScanFinished;
				O_TUPLE_SET_NULL(tuple);
				return tuple;
			}
		}
	}

	BTREE_PAGE_READ_LEAF_ITEM(tupHdr, tuple, scan->leafImg, &scan->leafLoc);
	BTREE_PAGE_LOCATOR_NEXT(scan->leafImg, &scan->leafLoc);

	if (!tupHdr->deleted)
	{
		if (hint)
			*hint = scan->hint;

		return tuple;
	}
	else
	{
		O_TUPLE_SET_NULL(tuple);
		return tuple;
	}
}

OTuple
btree_seq_scan_getnext_raw(BTreeSeqScan *scan, MemoryContext mctx,
						   bool *end, BTreeLocationHint *hint)
{
	OTuple		tuple;

	if (scan->status == BTreeSeqScanInMemory ||
		scan->status == BTreeSeqScanDisk)
	{
		tuple = btree_seq_scan_getnext_raw_internal(scan, mctx, hint);
		if (scan->status == BTreeSeqScanInMemory ||
			scan->status == BTreeSeqScanDisk)
		{
			*end = false;
			return tuple;
		}
	}
	Assert(scan->status == BTreeSeqScanFinished);

	O_TUPLE_SET_NULL(tuple);
	*end = true;
	return tuple;
}

void
free_btree_seq_scan(BTreeSeqScan *scan)
{
	BTreeMetaPage *metaPageBlkno = BTREE_GET_META(scan->desc);

	START_CRIT_SECTION();
	dlist_delete(&scan->listNode);
	(void) pg_atomic_fetch_sub_u32(&metaPageBlkno->numSeqScans[scan->checkpointNumber % NUM_SEQ_SCANS_ARRAY_SIZE], 1);
	END_CRIT_SECTION();

	pfree(scan->diskDownlinks);
	pfree(scan);
}

/*
 * Error cleanup for sequential scans.  No scans survives the error, but they
 * are't cleaned up individually.  Thus, we have to walk trough all the scans
 * and revert changes made to the metaPageBlkno->numSeqScans.
 */
void
seq_scans_cleanup(void)
{
	START_CRIT_SECTION();
	while (!dlist_is_empty(&listOfScans))
	{
		BTreeSeqScan *scan = dlist_head_element(BTreeSeqScan, listNode, &listOfScans);
		BTreeMetaPage *metaPageBlkno = BTREE_GET_META(scan->desc);

		(void) pg_atomic_fetch_sub_u32(&metaPageBlkno->numSeqScans[scan->checkpointNumber % NUM_SEQ_SCANS_ARRAY_SIZE], 1);

		dlist_delete(&scan->listNode);
		pfree(scan);
	}
	dlist_init(&listOfScans);
	END_CRIT_SECTION();
}
