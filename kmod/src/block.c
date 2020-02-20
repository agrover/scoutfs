/*
 * Copyright (C) 2019 Versity Software, Inc.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License v2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 */
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/crc32c.h>
#include <linux/pagemap.h>
#include <linux/vmalloc.h>
#include <linux/sched.h>
#include <linux/bio.h>
#include <linux/blkdev.h>
#include <linux/rbtree.h>

#include "format.h"
#include "super.h"
#include "block.h"
#include "counters.h"
#include "msg.h"
#include "scoutfs_trace.h"

/*
 * The scoutfs block cache manages metadata blocks that can be larger
 * than the page size.  Callers can have their own contexts for tracking
 * dirty blocks that are written together.  We pin dirty blocks in
 * memory and only checksum them all as they're all written.
 *
 * An LRU is maintained so the VM can reclaim the oldest presumably
 * unlikely to be used blocks.  But we don't maintain a perfect record
 * of access order.  We only move accessed blocks to the tail of the rcu
 * if they weren't in the most recently moved fraction of the total
 * population.  This means that reclaim will walk through waves of that
 * fraction of the population.  It's close enough and removes lru
 * maintenance locking from the fast path.
 */

struct block_info {
	struct super_block *sb;
	spinlock_t lock;
	struct rb_root root;
	struct list_head lru_list;
	u64 lru_nr;
	u64 lru_move_counter;
	wait_queue_head_t waitq;
	struct shrinker shrinker;
	struct work_struct free_work;
	struct llist_head free_llist;
};

#define DECLARE_BLOCK_INFO(sb, name) \
	struct block_info *name = SCOUTFS_SB(sb)->block_info

enum {
	BLOCK_BIT_UPTODATE = 0,	/* contents consistent with media */
	BLOCK_BIT_NEW,		/* newly allocated, contents undefined */
	BLOCK_BIT_DIRTY,	/* dirty, writer will write */
	BLOCK_BIT_ERROR,	/* saw IO error */
	BLOCK_BIT_DELETED,	/* has been deleted from rbtree */
	BLOCK_BIT_PAGE_ALLOC,	/* page (possibly high order) allocation */
	BLOCK_BIT_VIRT,		/* mapped virt allocation */
	BLOCK_BIT_CRC_VALID,	/* crc has been verified */
	BLOCK_BIT_VISITED,	/* used by callers to track blocks */
};

struct block_private {
	struct scoutfs_block bl;
	struct rb_node node;
	struct super_block *sb;
	atomic_t refcount;
	union {
		struct list_head lru_entry;
		struct llist_node free_node;
	};
	u64 lru_moved;
	struct list_head dirty_entry;
	unsigned long bits;
	atomic_t io_count;
	union {
		struct page *page;
		void *virt;
	};
};

#define TRACE_BLOCK(which, bp)						\
do {									\
	__typeof__(bp) _bp = (bp);					\
	trace_scoutfs_block_##which(_bp->sb, _bp, _bp->bl.blkno,	\
				   atomic_read(&_bp->refcount),		\
				   atomic_read(&_bp->io_count),		\
				   _bp->bits, _bp->lru_moved);		\
} while (0)

#define BLOCK_PRIVATE(_bl) \
	container_of((_bl), struct block_private, bl)

/*
 * These _block_header helpers are from a previous generation and may
 * be refactored away.
 */

__le32 scoutfs_block_calc_crc(struct scoutfs_block_header *hdr)
{
	int off = offsetof(struct scoutfs_block_header, crc) +
		  FIELD_SIZEOF(struct scoutfs_block_header, crc);
	u32 calc = crc32c(~0, (char *)hdr + off, SCOUTFS_BLOCK_SIZE - off);

	return cpu_to_le32(calc);
}

bool scoutfs_block_valid_crc(struct scoutfs_block_header *hdr)
{
	return hdr->crc == scoutfs_block_calc_crc(hdr);
}

bool scoutfs_block_valid_ref(struct super_block *sb,
			     struct scoutfs_block_header *hdr,
			     __le64 seq, __le64 blkno)
{
	struct scoutfs_super_block *super = &SCOUTFS_SB(sb)->super;

	return hdr->fsid == super->hdr.fsid && hdr->seq == seq &&
	       hdr->blkno == blkno;
}

bool scoutfs_block_tas_visited(struct super_block *sb,
			       struct scoutfs_block *bl)
{
	struct block_private *bp = BLOCK_PRIVATE(bl);

	return test_bit(BLOCK_BIT_VISITED, &bp->bits) != 0;
}

void scoutfs_block_clear_visited(struct super_block *sb,
				 struct scoutfs_block *bl)
{
	struct block_private *bp = BLOCK_PRIVATE(bl);

	clear_bit(BLOCK_BIT_VISITED, &bp->bits);
}

static struct block_private *block_alloc(struct super_block *sb, u64 blkno)
{
	struct block_private *bp;

	/*
	 * If we had multiple blocks per page we'd need to be a little
	 * more careful with a partial page allocator when allocating
	 * blocks and would make the lru per-page instead of per-block.
	 */
	BUILD_BUG_ON(PAGE_SIZE > SCOUTFS_BLOCK_SIZE);

	bp = kzalloc(sizeof(struct block_private), GFP_NOFS);
	if (!bp)
		goto out;

	bp->page = alloc_pages(GFP_NOFS, SCOUTFS_BLOCK_PAGE_ORDER);
	if (bp->page) {
		scoutfs_inc_counter(sb, block_cache_alloc_page_order);
		set_bit(BLOCK_BIT_PAGE_ALLOC, &bp->bits);
		bp->bl.data = page_address(bp->page);
	} else {
		bp->virt = __vmalloc(SCOUTFS_BLOCK_SIZE,
				     GFP_NOFS | __GFP_HIGHMEM, PAGE_KERNEL);
		if (!bp->virt) {
			kfree(bp);
			bp = NULL;
			goto out;
		}

		scoutfs_inc_counter(sb, block_cache_alloc_virt);
		set_bit(BLOCK_BIT_VIRT, &bp->bits);
		bp->bl.data = bp->virt;
	}

	bp->bl.blkno = blkno;
	RB_CLEAR_NODE(&bp->node);
	bp->sb = sb;
	atomic_set(&bp->refcount, 1);
	INIT_LIST_HEAD(&bp->lru_entry);
	INIT_LIST_HEAD(&bp->dirty_entry);
	set_bit(BLOCK_BIT_NEW, &bp->bits);
	atomic_set(&bp->io_count, 0);

	TRACE_BLOCK(allocate, bp);

out:
	if (!bp)
		scoutfs_inc_counter(sb, block_cache_alloc_failure);
	return bp;
}

static void block_free(struct super_block *sb, struct block_private *bp)
{
	scoutfs_inc_counter(sb, block_cache_free);

	TRACE_BLOCK(free, bp);

	if (test_bit(BLOCK_BIT_PAGE_ALLOC, &bp->bits))
		__free_pages(bp->page, SCOUTFS_BLOCK_PAGE_ORDER);
	else if (test_bit(BLOCK_BIT_VIRT, &bp->bits))
		vfree(bp->virt);
	else
		BUG();

	/* lru_entry could have been clobbered by union member free_node */
	WARN_ON_ONCE(!list_empty(&bp->dirty_entry));
	WARN_ON_ONCE(atomic_read(&bp->refcount));
	WARN_ON_ONCE(atomic_read(&bp->io_count));
	kfree(bp);
}

/*
 * We free blocks in task context so we can free kernel virtual mappings.
 */
static void block_free_work(struct work_struct *work)
{
	struct block_info *binf = container_of(work, struct block_info,
					       free_work);
	struct super_block *sb = binf->sb;
	struct block_private *bp;
	struct llist_node *deleted;

	deleted = llist_del_all(&binf->free_llist);

	llist_for_each_entry(bp, deleted, free_node) {
		block_free(sb, bp);
	}
}

/*
 * After we've dropped the final ref kick off the final free in task
 * context.  This happens in the relatively rare cases of IO errors,
 * stale cached data, memory pressure, and unmount.
 */
static void block_put(struct super_block *sb, struct block_private *bp)
{
	DECLARE_BLOCK_INFO(sb, binf);

	if (!IS_ERR_OR_NULL(bp) && atomic_dec_and_test(&bp->refcount)) {
		WARN_ON_ONCE(!list_empty(&bp->lru_entry));
		llist_add(&bp->free_node, &binf->free_llist);
		schedule_work(&binf->free_work);
	}
}

static struct block_private *walk_block_rbtree(struct rb_root *root,
					       u64 blkno,
					       struct block_private *ins)
{
	struct rb_node **node = &root->rb_node;
	struct rb_node *parent = NULL;
	struct block_private *bp;
	int cmp;

	while (*node) {
		parent = *node;
		bp = container_of(*node, struct block_private, node);

		cmp = scoutfs_cmp_u64s(bp->bl.blkno, blkno);
		if (cmp == 0)
			return bp;
		else if (cmp < 0)
			node = &(*node)->rb_left;
		else
			node = &(*node)->rb_right;
	}

	if (ins) {
		rb_link_node(&ins->node, parent, node);
		rb_insert_color(&ins->node, root);
		return ins;
	}

	return NULL;
}

/*
 * Add a new block into the cache.  The caller holds the lock.
 */
static void block_insert(struct super_block *sb, struct block_private *bp,
			 u64 blkno)
{
	DECLARE_BLOCK_INFO(sb, binf);

	assert_spin_locked(&binf->lock);
	BUG_ON(!list_empty(&bp->lru_entry));
	BUG_ON(!RB_EMPTY_NODE(&bp->node));

	atomic_inc(&bp->refcount);
	walk_block_rbtree(&binf->root, blkno, bp);
	list_add_tail(&bp->lru_entry, &binf->lru_list);
	bp->lru_moved = ++binf->lru_move_counter;
	binf->lru_nr++;

	TRACE_BLOCK(insert, bp);
}

/*
 * Only move the block to the tail of the LRU if it's outside of the
 * small fraction of the lru population that has been most recently
 * used.  This gives us a reasonable number of most recently accessed
 * blocks which will be reclaimed after the rest of the least recently
 * used blocks while reducing per-access locking overhead of maintaining
 * the LRU.  We don't care about unlikely non-atomic u64 accesses racing
 * and messing up LRU position.
 *
 * This can race with blocks being removed from the cache (shrinking,
 * stale, errors) so we're careful to only move the entry if it's still
 * on the list after we acquire the lock.  We still hold a reference so it's
 * lru_entry hasn't transitioned to being used as the free_node.
 */
static void block_accessed(struct super_block *sb, struct block_private *bp)
{
	DECLARE_BLOCK_INFO(sb, binf);
	u64 recent = binf->lru_nr >> 3;

	scoutfs_inc_counter(sb, block_cache_access);

	if (bp->lru_moved < (binf->lru_move_counter - recent)) {
		spin_lock(&binf->lock);
		if (!list_empty(&bp->lru_entry)) {
			list_move_tail(&bp->lru_entry, &binf->lru_list);
			bp->lru_moved = ++binf->lru_move_counter;
			scoutfs_inc_counter(sb, block_cache_lru_move);
		}
		spin_unlock(&binf->lock);
	}
}

/*
 * Remove a block from the cache and drop its reference.  We only remove
 * the block once as the deleted bit is first set.
 */
static void block_remove(struct super_block *sb, struct block_private *bp)
{
	DECLARE_BLOCK_INFO(sb, binf);

	if (!test_and_set_bit(BLOCK_BIT_DELETED, &bp->bits)) {
		BUG_ON(list_empty(&bp->lru_entry));
		rb_erase(&bp->node, &binf->root);
		RB_CLEAR_NODE(&bp->node);
		list_del_init(&bp->lru_entry);
		binf->lru_nr--;
		block_put(sb, bp);
	}
}

/*
 * Called during shutdown with no other users.
 */
static void block_remove_all(struct super_block *sb)
{
	DECLARE_BLOCK_INFO(sb, binf);
	struct block_private *bp;
	struct rb_node *node;

	for (node = rb_first(&binf->root); node; ) {
		bp = container_of(node, struct block_private, node);
		node = rb_next(node);
		wait_event(binf->waitq, atomic_read(&bp->io_count) == 0);
		block_remove(sb, bp);
	}

	WARN_ON_ONCE(!list_empty(&binf->lru_list));
	WARN_ON_ONCE(binf->lru_nr != 0);
	WARN_ON_ONCE(!RB_EMPTY_ROOT(&binf->root));
}

/*
 * XXX The io_count and sb fields in the block_private are only used
 * during IO.  We don't need to have them sitting around for the entire
 * lifetime of each cached block.
 *
 * This is happening in interrupt context so we do as little work as
 * possible.  Final freeing, verifying checksums, and unlinking errored
 * blocks are all done by future users of the blocks.
 */
static void block_end_io(struct super_block *sb, int rw,
			 struct block_private *bp, int err)
{
	DECLARE_BLOCK_INFO(sb, binf);
	bool is_read = !(rw & WRITE);

	if (err) {
		scoutfs_inc_counter(sb, block_cache_end_io_error);
		set_bit(BLOCK_BIT_ERROR, &bp->bits);
	}

	/* update bits before waiters see io_count == 0 */
	if (atomic_read(&bp->io_count) == 1) {
		if (is_read && !test_bit(BLOCK_BIT_ERROR, &bp->bits))
			set_bit(BLOCK_BIT_UPTODATE, &bp->bits);
	}

	/* make sure bits are visible to woken */
	smp_mb__after_atomic();

	/* then wake */
	if (atomic_dec_and_test(&bp->io_count))
		wake_up(&binf->waitq);
}

static void block_bio_end_io(struct bio *bio, int err)
{
	struct block_private *bp = bio->bi_private;
	struct super_block *sb = bp->sb;


	block_end_io(sb, bio->bi_rw, bp, err);
	bio_put(bio);
	TRACE_BLOCK(end_io, bp);
	block_put(sb, bp);
}

/*
 * Kick off IO for a single block.
 */
static int block_submit_bio(struct super_block *sb, struct block_private *bp,
			    int rw)
{
	struct bio *bio = NULL;
	struct blk_plug plug;
	struct page *page;
	unsigned long off;
	sector_t sector;
	int ret = 0;

	sector = bp->bl.blkno << (SCOUTFS_BLOCK_SHIFT - 9);

	WARN_ON_ONCE(bp->bl.blkno == U64_MAX);
	WARN_ON_ONCE(sector == U64_MAX || sector == 0);

	/* don't let racing end_io during submission think block is complete */
	atomic_inc(&bp->io_count);

	blk_start_plug(&plug);

	for (off = 0; off < SCOUTFS_BLOCK_SIZE; off += PAGE_SIZE) {
		if (!bio) {
			bio = bio_alloc(GFP_NOFS, SCOUTFS_PAGES_PER_BLOCK);
			if (!bio) {
				ret = -ENOMEM;
				break;
			}

			bio->bi_sector = sector + (off >> 9);
			bio->bi_bdev = sb->s_bdev;
			bio->bi_end_io = block_bio_end_io;
			bio->bi_private = bp;

			atomic_inc(&bp->refcount);
			atomic_inc(&bp->io_count);

			TRACE_BLOCK(submit, bp);
		}

		if (test_bit(BLOCK_BIT_PAGE_ALLOC, &bp->bits))
			page = virt_to_page((char *)bp->bl.data + off);
		else if (test_bit(BLOCK_BIT_VIRT, &bp->bits))
			page = vmalloc_to_page((char *)bp->bl.data + off);
		else
			BUG();

		if (!bio_add_page(bio, page, PAGE_SIZE, 0)) {
			submit_bio(rw, bio);
			bio = NULL;
		}
	}

	if (bio)
		submit_bio(rw, bio);

	blk_finish_plug(&plug);

	/* let racing end_io know we're done */
	block_end_io(sb, rw, bp, ret);

	return ret;
}

/*
 * Return a reference to a cached block in the system, allocating a new
 * block if one isn't found in the rbtree.  Its contents are undefined
 * if it's newly allocated.
 */
static struct block_private *block_get(struct super_block *sb, u64 blkno)
{
	DECLARE_BLOCK_INFO(sb, binf);
	struct block_private *found;
	struct block_private *bp;
	int ret;

	spin_lock(&binf->lock);
	bp = walk_block_rbtree(&binf->root, blkno, NULL);
	if (bp)
		atomic_inc(&bp->refcount);
	spin_unlock(&binf->lock);

	/* drop failed reads that interrupted waiters abandoned */
	if (bp && (test_bit(BLOCK_BIT_ERROR, &bp->bits) &&
	           !test_bit(BLOCK_BIT_DIRTY, &bp->bits))) {
		spin_lock(&binf->lock);
		block_remove(sb, bp);
		spin_unlock(&binf->lock);
		block_put(sb, bp);
		bp = NULL;
	}

	if (!bp) {
		bp = block_alloc(sb, blkno);
		if (bp == NULL) {
			ret = -ENOMEM;
			goto out;
		}

		/* could refactor to insert in one walk */
		spin_lock(&binf->lock);
		found = walk_block_rbtree(&binf->root, blkno, NULL);
		if (found) {
			atomic_inc(&found->refcount);
		} else {
			block_insert(sb, bp, blkno);
		}
		spin_unlock(&binf->lock);

		if (found) {
			block_put(sb, bp);
			bp = found;
		}
	}

	block_accessed(sb, bp);
	ret = 0;

out:
	if (ret < 0) {
		block_put(sb, bp);
		return ERR_PTR(ret);
	}

	return bp;
}

/*
 * Return a cached block or a newly allocated block whose contents are
 * undefined.  The caller is going to initialize the block contents.
 */
struct scoutfs_block *scoutfs_block_create(struct super_block *sb, u64 blkno)
{
	struct block_private *bp;

	bp = block_get(sb, blkno);
	if (IS_ERR(bp))
		return ERR_CAST(bp);

	set_bit(BLOCK_BIT_UPTODATE, &bp->bits);
	set_bit(BLOCK_BIT_CRC_VALID, &bp->bits);

	return &bp->bl;
}

static bool uptodate_or_error(struct block_private *bp)
{
	smp_rmb(); /* test after adding to wait queue */
	return test_bit(BLOCK_BIT_UPTODATE, &bp->bits) ||
	       test_bit(BLOCK_BIT_ERROR, &bp->bits);
}

struct scoutfs_block *scoutfs_block_read(struct super_block *sb, u64 blkno)
{
	DECLARE_BLOCK_INFO(sb, binf);
	struct block_private *bp = NULL;
	int ret;

	bp = block_get(sb, blkno);
	if (IS_ERR(bp)) {
		ret = PTR_ERR(bp);
		goto out;
	}

	if (!test_bit(BLOCK_BIT_UPTODATE, &bp->bits) &&
	     test_and_clear_bit(BLOCK_BIT_NEW, &bp->bits)) {
		ret = block_submit_bio(sb, bp, READ);
		if (ret < 0)
			goto out;
	}

	ret = wait_event_interruptible(binf->waitq, uptodate_or_error(bp));
	if (ret == 0 && test_bit(BLOCK_BIT_ERROR, &bp->bits))
		ret = -EIO;

out:
	if (ret < 0) {
		block_put(sb, bp);
		return ERR_PTR(ret);
	}

	return &bp->bl;
}

/*
 * Drop a stale cached read block from the cache.  A future read will
 * re-read the block from the device.  This doesn't drop the caller's reference,
 * they still have to call _put.
 */
void scoutfs_block_invalidate(struct super_block *sb, struct scoutfs_block *bl)
{
	DECLARE_BLOCK_INFO(sb, binf);
	struct block_private *bp = BLOCK_PRIVATE(bl);

	if (!WARN_ON_ONCE(test_bit(BLOCK_BIT_DIRTY, &bp->bits))) {
		scoutfs_inc_counter(sb, block_cache_invalidate);
		spin_lock(&binf->lock);
		block_remove(sb, bp);
		spin_unlock(&binf->lock);
		TRACE_BLOCK(invalidate, bp);
	}
}

bool scoutfs_block_consistent_ref(struct super_block *sb,
				  struct scoutfs_block *bl,
				  __le64 seq, __le64 blkno, u32 magic)
{
	struct scoutfs_super_block *super = &SCOUTFS_SB(sb)->super;
	struct block_private *bp = BLOCK_PRIVATE(bl);
	struct scoutfs_block_header *hdr = bl->data;

	if (!test_bit(BLOCK_BIT_CRC_VALID, &bp->bits)) {
		if (hdr->crc != scoutfs_block_calc_crc(hdr))
			return false;
		set_bit(BLOCK_BIT_CRC_VALID, &bp->bits);
	}

	return hdr->magic == cpu_to_le32(magic) &&
	       hdr->fsid == super->hdr.fsid &&
	       hdr->seq == seq &&
	       hdr->blkno == blkno;
}

void scoutfs_block_put(struct super_block *sb, struct scoutfs_block *bl)
{
	if (!IS_ERR_OR_NULL(bl))
		block_put(sb, BLOCK_PRIVATE(bl));
}

void scoutfs_block_writer_init(struct super_block *sb,
			       struct scoutfs_block_writer *wri)
{
	spin_lock_init(&wri->lock);
	INIT_LIST_HEAD(&wri->dirty_list);
	wri->nr_dirty_blocks = 0;
}

/*
 * Mark a given block dirty.  The caller serializes all dirtying calls
 * with writer write calls.  As it happens we dirty in allocation order
 * and allocate with an advancing cursor so we always dirty in block
 * offset order and can walk our list to submit nice ordered IO.
 */
void scoutfs_block_writer_mark_dirty(struct super_block *sb,
				     struct scoutfs_block_writer *wri,
				     struct scoutfs_block *bl)
{
	struct block_private *bp = BLOCK_PRIVATE(bl);

	if (!test_and_set_bit(BLOCK_BIT_DIRTY, &bp->bits)) {
		BUG_ON(!list_empty(&bp->dirty_entry));
		atomic_inc(&bp->refcount);
		spin_lock(&wri->lock);
		list_add_tail(&bp->dirty_entry, &wri->dirty_list);
		wri->nr_dirty_blocks++;
		spin_unlock(&wri->lock);

		TRACE_BLOCK(mark_dirty, bp);
	}
}

bool scoutfs_block_writer_is_dirty(struct super_block *sb,
				   struct scoutfs_block *bl)
{
	struct block_private *bp = BLOCK_PRIVATE(bl);

	return test_bit(BLOCK_BIT_DIRTY, &bp->bits) != 0;
}

/*
 * Submit writes for all the dirty blocks in the writer's dirty list and
 * wait for them to complete.  The caller must serialize this with
 * attempts to dirty blocks in the writer.  If we return an error then
 * all the blocks will still be considered dirty.  This can be called
 * again to attempt to write all the blocks again.
 */
int scoutfs_block_writer_write(struct super_block *sb,
			       struct scoutfs_block_writer *wri)
{
	DECLARE_BLOCK_INFO(sb, binf);
	struct scoutfs_block_header *hdr;
	struct block_private *bp;
	struct blk_plug plug;
	int ret = 0;

	if (wri->nr_dirty_blocks == 0)
		return 0;

	/* checksum everything to reduce time between io submission merging */
	list_for_each_entry(bp, &wri->dirty_list, dirty_entry) {
		hdr = bp->bl.data;
		hdr->crc = scoutfs_block_calc_crc(hdr);
	}

        blk_start_plug(&plug);

	list_for_each_entry(bp, &wri->dirty_list, dirty_entry) {
		/* retry previous write errors */
		clear_bit(BLOCK_BIT_ERROR, &bp->bits);

		ret = block_submit_bio(sb, bp, WRITE);
		if (ret < 0)
			break;
	}

	blk_finish_plug(&plug);

	list_for_each_entry(bp, &wri->dirty_list, dirty_entry) {
		/* XXX should this be interruptible? */
		wait_event(binf->waitq, atomic_read(&bp->io_count) == 0);
		if (ret == 0 && test_bit(BLOCK_BIT_ERROR, &bp->bits)) {
			clear_bit(BLOCK_BIT_ERROR, &bp->bits);
			ret = -EIO;
		}
	}

	if (ret == 0)
		scoutfs_block_writer_forget_all(sb, wri);

	return ret;
}

static void block_forget(struct super_block *sb,
			 struct scoutfs_block_writer *wri,
			 struct block_private *bp)
{
	assert_spin_locked(&wri->lock);

	clear_bit(BLOCK_BIT_DIRTY, &bp->bits);
	list_del_init(&bp->dirty_entry);
	wri->nr_dirty_blocks--;
	TRACE_BLOCK(forget, bp);
	block_put(sb, bp);
}

/*
 * Clear the dirty status of all the blocks in the writer.  The blocks
 * remain clean in cache but can be freed by reclaim and then re-read
 * from disk, losing whatever modifications made them dirty.
 */
void scoutfs_block_writer_forget_all(struct super_block *sb,
				     struct scoutfs_block_writer *wri)
{
	struct block_private *tmp;
	struct block_private *bp;

	spin_lock(&wri->lock);

	list_for_each_entry_safe(bp, tmp, &wri->dirty_list, dirty_entry)
		block_forget(sb, wri, bp);

	spin_unlock(&wri->lock);
}

/*
 * Forget that the given block was dirty.  It won't be written in the
 * future.  Its contents remain in the cache.  This is typically used
 * as a block is freed.  If it is allocated and re-used then its contents
 * will be re-initialized.
 *
 * The caller should ensure that we don't try and mark and forget the
 * same block, but this is racing with marking and forgetting other
 * blocks.
 */
void scoutfs_block_writer_forget(struct super_block *sb,
			         struct scoutfs_block_writer *wri,
				 struct scoutfs_block *bl)
{
	struct block_private *bp = BLOCK_PRIVATE(bl);

	if (test_bit(BLOCK_BIT_DIRTY, &bp->bits)) {
		scoutfs_inc_counter(sb, block_cache_forget);
		spin_lock(&wri->lock);
		if (test_bit(BLOCK_BIT_DIRTY, &bp->bits))
			block_forget(sb, wri, bp);
		spin_unlock(&wri->lock);
	}
}

/*
 * The caller has ensured that no more dirtying will take place.  This
 * helps the caller avoid doing a bunch of work before calling into the
 * writer to write dirty blocks that didn't exist.
 */
bool scoutfs_block_writer_has_dirty(struct super_block *sb,
				    struct scoutfs_block_writer *wri)
{
	return wri->nr_dirty_blocks != 0;
}

/*
 * This is a best-effort guess.  It's only used for heuristics so it's OK
 * if it goes a little bonkers sometimes.
 */
u64 scoutfs_block_writer_dirty_bytes(struct super_block *sb,
				     struct scoutfs_block_writer *wri)
{
	return wri->nr_dirty_blocks * SCOUTFS_BLOCK_SIZE;
}

/*
 * Remove a number of least recently accessed blocks and free them.  We
 * don't take locking hit of removing blocks from the lru as they're
 * used so this is racing with accesses holding an elevated refcount.
 * We check the refcount to attempt to not free a block that snuck in
 * and is being accessed while the block is still at the head of the
 * LRU.
 *
 * Dirty blocks will always have an elevated refcount (and will be
 * likely be towards the tail of the LRU).  Even if we do remove them
 * from the LRU their dirty refcount will keep them live until IO
 * completes and their dirty refcount is dropped.
 */
static int block_shrink(struct shrinker *shrink, struct shrink_control *sc)
{
	struct block_info *binf = container_of(shrink, struct block_info,
					       shrinker);
	struct super_block *sb = binf->sb;
	struct block_private *tmp;
	struct block_private *bp;
	unsigned long nr;
	LIST_HEAD(list);

	nr = sc->nr_to_scan;
	if (!nr)
		goto out;

	spin_lock(&binf->lock);

	list_for_each_entry_safe(bp, tmp, &binf->lru_list, lru_entry) {

		if (atomic_read(&bp->refcount) > 1)
			continue;

		if (nr-- == 0)
			break;

		TRACE_BLOCK(shrink, bp);

		scoutfs_inc_counter(sb, block_cache_shrink);
		block_remove(sb, bp);

	}

	spin_unlock(&binf->lock);

out:
	return min_t(u64, binf->lru_nr * SCOUTFS_PAGES_PER_BLOCK, INT_MAX);
}

int scoutfs_block_setup(struct super_block *sb)
{
	struct scoutfs_sb_info *sbi = SCOUTFS_SB(sb);
	struct block_info *binf;
	int ret;

	binf = kzalloc(sizeof(struct block_info), GFP_KERNEL);
	if (!binf) {
		ret = -ENOMEM;
		goto out;
	}

	binf->sb = sb;
	spin_lock_init(&binf->lock);
	binf->root = RB_ROOT;
	INIT_LIST_HEAD(&binf->lru_list);
	init_waitqueue_head(&binf->waitq);
	binf->shrinker.shrink = block_shrink;
	binf->shrinker.seeks = DEFAULT_SEEKS;
	register_shrinker(&binf->shrinker);
	INIT_WORK(&binf->free_work, block_free_work);
	init_llist_head(&binf->free_llist);

	sbi->block_info = binf;

	ret = 0;
out:
	if (ret)
		scoutfs_block_destroy(sb);

	return 0;
}

void scoutfs_block_destroy(struct super_block *sb)
{
	struct scoutfs_sb_info *sbi = SCOUTFS_SB(sb);
	struct block_info *binf = SCOUTFS_SB(sb)->block_info;

	if (binf) {
		unregister_shrinker(&binf->shrinker);
		block_remove_all(sb);
		flush_work(&binf->free_work);

		WARN_ON_ONCE(!llist_empty(&binf->free_llist));
		kfree(binf);

		sbi->block_info = NULL;
	}
}
