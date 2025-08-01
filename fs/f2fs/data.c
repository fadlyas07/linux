// SPDX-License-Identifier: GPL-2.0
/*
 * fs/f2fs/data.c
 *
 * Copyright (c) 2012 Samsung Electronics Co., Ltd.
 *             http://www.samsung.com/
 */
#include <linux/fs.h>
#include <linux/f2fs_fs.h>
#include <linux/sched/mm.h>
#include <linux/mpage.h>
#include <linux/writeback.h>
#include <linux/pagevec.h>
#include <linux/blkdev.h>
#include <linux/bio.h>
#include <linux/blk-crypto.h>
#include <linux/swap.h>
#include <linux/prefetch.h>
#include <linux/uio.h>
#include <linux/sched/signal.h>
#include <linux/fiemap.h>
#include <linux/iomap.h>

#include "f2fs.h"
#include "node.h"
#include "segment.h"
#include "iostat.h"
#include <trace/events/f2fs.h>

#define NUM_PREALLOC_POST_READ_CTXS	128

static struct kmem_cache *bio_post_read_ctx_cache;
static struct kmem_cache *bio_entry_slab;
static mempool_t *bio_post_read_ctx_pool;
static struct bio_set f2fs_bioset;

#define	F2FS_BIO_POOL_SIZE	NR_CURSEG_TYPE

int __init f2fs_init_bioset(void)
{
	return bioset_init(&f2fs_bioset, F2FS_BIO_POOL_SIZE,
					0, BIOSET_NEED_BVECS);
}

void f2fs_destroy_bioset(void)
{
	bioset_exit(&f2fs_bioset);
}

bool f2fs_is_cp_guaranteed(struct page *page)
{
	struct address_space *mapping = page_folio(page)->mapping;
	struct inode *inode;
	struct f2fs_sb_info *sbi;

	if (fscrypt_is_bounce_page(page))
		return page_private_gcing(fscrypt_pagecache_page(page));

	inode = mapping->host;
	sbi = F2FS_I_SB(inode);

	if (inode->i_ino == F2FS_META_INO(sbi) ||
			inode->i_ino == F2FS_NODE_INO(sbi) ||
			S_ISDIR(inode->i_mode))
		return true;

	if ((S_ISREG(inode->i_mode) && IS_NOQUOTA(inode)) ||
			page_private_gcing(page))
		return true;
	return false;
}

static enum count_type __read_io_type(struct folio *folio)
{
	struct address_space *mapping = folio->mapping;

	if (mapping) {
		struct inode *inode = mapping->host;
		struct f2fs_sb_info *sbi = F2FS_I_SB(inode);

		if (inode->i_ino == F2FS_META_INO(sbi))
			return F2FS_RD_META;

		if (inode->i_ino == F2FS_NODE_INO(sbi))
			return F2FS_RD_NODE;
	}
	return F2FS_RD_DATA;
}

/* postprocessing steps for read bios */
enum bio_post_read_step {
#ifdef CONFIG_FS_ENCRYPTION
	STEP_DECRYPT	= BIT(0),
#else
	STEP_DECRYPT	= 0,	/* compile out the decryption-related code */
#endif
#ifdef CONFIG_F2FS_FS_COMPRESSION
	STEP_DECOMPRESS	= BIT(1),
#else
	STEP_DECOMPRESS	= 0,	/* compile out the decompression-related code */
#endif
#ifdef CONFIG_FS_VERITY
	STEP_VERITY	= BIT(2),
#else
	STEP_VERITY	= 0,	/* compile out the verity-related code */
#endif
};

struct bio_post_read_ctx {
	struct bio *bio;
	struct f2fs_sb_info *sbi;
	struct work_struct work;
	unsigned int enabled_steps;
	/*
	 * decompression_attempted keeps track of whether
	 * f2fs_end_read_compressed_page() has been called on the pages in the
	 * bio that belong to a compressed cluster yet.
	 */
	bool decompression_attempted;
	block_t fs_blkaddr;
};

/*
 * Update and unlock a bio's pages, and free the bio.
 *
 * This marks pages up-to-date only if there was no error in the bio (I/O error,
 * decryption error, or verity error), as indicated by bio->bi_status.
 *
 * "Compressed pages" (pagecache pages backed by a compressed cluster on-disk)
 * aren't marked up-to-date here, as decompression is done on a per-compression-
 * cluster basis rather than a per-bio basis.  Instead, we only must do two
 * things for each compressed page here: call f2fs_end_read_compressed_page()
 * with failed=true if an error occurred before it would have normally gotten
 * called (i.e., I/O error or decryption error, but *not* verity error), and
 * release the bio's reference to the decompress_io_ctx of the page's cluster.
 */
static void f2fs_finish_read_bio(struct bio *bio, bool in_task)
{
	struct folio_iter fi;
	struct bio_post_read_ctx *ctx = bio->bi_private;

	bio_for_each_folio_all(fi, bio) {
		struct folio *folio = fi.folio;

		if (f2fs_is_compressed_page(&folio->page)) {
			if (ctx && !ctx->decompression_attempted)
				f2fs_end_read_compressed_page(&folio->page, true, 0,
							in_task);
			f2fs_put_folio_dic(folio, in_task);
			continue;
		}

		dec_page_count(F2FS_F_SB(folio), __read_io_type(folio));
		folio_end_read(folio, bio->bi_status == BLK_STS_OK);
	}

	if (ctx)
		mempool_free(ctx, bio_post_read_ctx_pool);
	bio_put(bio);
}

static void f2fs_verify_bio(struct work_struct *work)
{
	struct bio_post_read_ctx *ctx =
		container_of(work, struct bio_post_read_ctx, work);
	struct bio *bio = ctx->bio;
	bool may_have_compressed_pages = (ctx->enabled_steps & STEP_DECOMPRESS);

	/*
	 * fsverity_verify_bio() may call readahead() again, and while verity
	 * will be disabled for this, decryption and/or decompression may still
	 * be needed, resulting in another bio_post_read_ctx being allocated.
	 * So to prevent deadlocks we need to release the current ctx to the
	 * mempool first.  This assumes that verity is the last post-read step.
	 */
	mempool_free(ctx, bio_post_read_ctx_pool);
	bio->bi_private = NULL;

	/*
	 * Verify the bio's pages with fs-verity.  Exclude compressed pages,
	 * as those were handled separately by f2fs_end_read_compressed_page().
	 */
	if (may_have_compressed_pages) {
		struct bio_vec *bv;
		struct bvec_iter_all iter_all;

		bio_for_each_segment_all(bv, bio, iter_all) {
			struct page *page = bv->bv_page;

			if (!f2fs_is_compressed_page(page) &&
			    !fsverity_verify_page(page)) {
				bio->bi_status = BLK_STS_IOERR;
				break;
			}
		}
	} else {
		fsverity_verify_bio(bio);
	}

	f2fs_finish_read_bio(bio, true);
}

/*
 * If the bio's data needs to be verified with fs-verity, then enqueue the
 * verity work for the bio.  Otherwise finish the bio now.
 *
 * Note that to avoid deadlocks, the verity work can't be done on the
 * decryption/decompression workqueue.  This is because verifying the data pages
 * can involve reading verity metadata pages from the file, and these verity
 * metadata pages may be encrypted and/or compressed.
 */
static void f2fs_verify_and_finish_bio(struct bio *bio, bool in_task)
{
	struct bio_post_read_ctx *ctx = bio->bi_private;

	if (ctx && (ctx->enabled_steps & STEP_VERITY)) {
		INIT_WORK(&ctx->work, f2fs_verify_bio);
		fsverity_enqueue_verify_work(&ctx->work);
	} else {
		f2fs_finish_read_bio(bio, in_task);
	}
}

/*
 * Handle STEP_DECOMPRESS by decompressing any compressed clusters whose last
 * remaining page was read by @ctx->bio.
 *
 * Note that a bio may span clusters (even a mix of compressed and uncompressed
 * clusters) or be for just part of a cluster.  STEP_DECOMPRESS just indicates
 * that the bio includes at least one compressed page.  The actual decompression
 * is done on a per-cluster basis, not a per-bio basis.
 */
static void f2fs_handle_step_decompress(struct bio_post_read_ctx *ctx,
		bool in_task)
{
	struct bio_vec *bv;
	struct bvec_iter_all iter_all;
	bool all_compressed = true;
	block_t blkaddr = ctx->fs_blkaddr;

	bio_for_each_segment_all(bv, ctx->bio, iter_all) {
		struct page *page = bv->bv_page;

		if (f2fs_is_compressed_page(page))
			f2fs_end_read_compressed_page(page, false, blkaddr,
						      in_task);
		else
			all_compressed = false;

		blkaddr++;
	}

	ctx->decompression_attempted = true;

	/*
	 * Optimization: if all the bio's pages are compressed, then scheduling
	 * the per-bio verity work is unnecessary, as verity will be fully
	 * handled at the compression cluster level.
	 */
	if (all_compressed)
		ctx->enabled_steps &= ~STEP_VERITY;
}

static void f2fs_post_read_work(struct work_struct *work)
{
	struct bio_post_read_ctx *ctx =
		container_of(work, struct bio_post_read_ctx, work);
	struct bio *bio = ctx->bio;

	if ((ctx->enabled_steps & STEP_DECRYPT) && !fscrypt_decrypt_bio(bio)) {
		f2fs_finish_read_bio(bio, true);
		return;
	}

	if (ctx->enabled_steps & STEP_DECOMPRESS)
		f2fs_handle_step_decompress(ctx, true);

	f2fs_verify_and_finish_bio(bio, true);
}

static void f2fs_read_end_io(struct bio *bio)
{
	struct f2fs_sb_info *sbi = F2FS_P_SB(bio_first_page_all(bio));
	struct bio_post_read_ctx *ctx;
	bool intask = in_task();

	iostat_update_and_unbind_ctx(bio);
	ctx = bio->bi_private;

	if (time_to_inject(sbi, FAULT_READ_IO))
		bio->bi_status = BLK_STS_IOERR;

	if (bio->bi_status != BLK_STS_OK) {
		f2fs_finish_read_bio(bio, intask);
		return;
	}

	if (ctx) {
		unsigned int enabled_steps = ctx->enabled_steps &
					(STEP_DECRYPT | STEP_DECOMPRESS);

		/*
		 * If we have only decompression step between decompression and
		 * decrypt, we don't need post processing for this.
		 */
		if (enabled_steps == STEP_DECOMPRESS &&
				!f2fs_low_mem_mode(sbi)) {
			f2fs_handle_step_decompress(ctx, intask);
		} else if (enabled_steps) {
			INIT_WORK(&ctx->work, f2fs_post_read_work);
			queue_work(ctx->sbi->post_read_wq, &ctx->work);
			return;
		}
	}

	f2fs_verify_and_finish_bio(bio, intask);
}

static void f2fs_write_end_io(struct bio *bio)
{
	struct f2fs_sb_info *sbi;
	struct folio_iter fi;

	iostat_update_and_unbind_ctx(bio);
	sbi = bio->bi_private;

	if (time_to_inject(sbi, FAULT_WRITE_IO))
		bio->bi_status = BLK_STS_IOERR;

	bio_for_each_folio_all(fi, bio) {
		struct folio *folio = fi.folio;
		enum count_type type;

		if (fscrypt_is_bounce_folio(folio)) {
			struct folio *io_folio = folio;

			folio = fscrypt_pagecache_folio(io_folio);
			fscrypt_free_bounce_page(&io_folio->page);
		}

#ifdef CONFIG_F2FS_FS_COMPRESSION
		if (f2fs_is_compressed_page(&folio->page)) {
			f2fs_compress_write_end_io(bio, &folio->page);
			continue;
		}
#endif

		type = WB_DATA_TYPE(&folio->page, false);

		if (unlikely(bio->bi_status != BLK_STS_OK)) {
			mapping_set_error(folio->mapping, -EIO);
			if (type == F2FS_WB_CP_DATA)
				f2fs_stop_checkpoint(sbi, true,
						STOP_CP_REASON_WRITE_FAIL);
		}

		f2fs_bug_on(sbi, is_node_folio(folio) &&
				folio->index != nid_of_node(&folio->page));

		dec_page_count(sbi, type);
		if (f2fs_in_warm_node_list(sbi, folio))
			f2fs_del_fsync_node_entry(sbi, folio);
		clear_page_private_gcing(&folio->page);
		folio_end_writeback(folio);
	}
	if (!get_pages(sbi, F2FS_WB_CP_DATA) &&
				wq_has_sleeper(&sbi->cp_wait))
		wake_up(&sbi->cp_wait);

	bio_put(bio);
}

#ifdef CONFIG_BLK_DEV_ZONED
static void f2fs_zone_write_end_io(struct bio *bio)
{
	struct f2fs_bio_info *io = (struct f2fs_bio_info *)bio->bi_private;

	bio->bi_private = io->bi_private;
	complete(&io->zone_wait);
	f2fs_write_end_io(bio);
}
#endif

struct block_device *f2fs_target_device(struct f2fs_sb_info *sbi,
		block_t blk_addr, sector_t *sector)
{
	struct block_device *bdev = sbi->sb->s_bdev;
	int i;

	if (f2fs_is_multi_device(sbi)) {
		for (i = 0; i < sbi->s_ndevs; i++) {
			if (FDEV(i).start_blk <= blk_addr &&
			    FDEV(i).end_blk >= blk_addr) {
				blk_addr -= FDEV(i).start_blk;
				bdev = FDEV(i).bdev;
				break;
			}
		}
	}

	if (sector)
		*sector = SECTOR_FROM_BLOCK(blk_addr);
	return bdev;
}

int f2fs_target_device_index(struct f2fs_sb_info *sbi, block_t blkaddr)
{
	int i;

	if (!f2fs_is_multi_device(sbi))
		return 0;

	for (i = 0; i < sbi->s_ndevs; i++)
		if (FDEV(i).start_blk <= blkaddr && FDEV(i).end_blk >= blkaddr)
			return i;
	return 0;
}

static blk_opf_t f2fs_io_flags(struct f2fs_io_info *fio)
{
	unsigned int temp_mask = GENMASK(NR_TEMP_TYPE - 1, 0);
	struct folio *fio_folio = page_folio(fio->page);
	unsigned int fua_flag, meta_flag, io_flag;
	blk_opf_t op_flags = 0;

	if (fio->op != REQ_OP_WRITE)
		return 0;
	if (fio->type == DATA)
		io_flag = fio->sbi->data_io_flag;
	else if (fio->type == NODE)
		io_flag = fio->sbi->node_io_flag;
	else
		return 0;

	fua_flag = io_flag & temp_mask;
	meta_flag = (io_flag >> NR_TEMP_TYPE) & temp_mask;

	/*
	 * data/node io flag bits per temp:
	 *      REQ_META     |      REQ_FUA      |
	 *    5 |    4 |   3 |    2 |    1 |   0 |
	 * Cold | Warm | Hot | Cold | Warm | Hot |
	 */
	if (BIT(fio->temp) & meta_flag)
		op_flags |= REQ_META;
	if (BIT(fio->temp) & fua_flag)
		op_flags |= REQ_FUA;

	if (fio->type == DATA &&
	    F2FS_I(fio_folio->mapping->host)->ioprio_hint == F2FS_IOPRIO_WRITE)
		op_flags |= REQ_PRIO;

	return op_flags;
}

static struct bio *__bio_alloc(struct f2fs_io_info *fio, int npages)
{
	struct f2fs_sb_info *sbi = fio->sbi;
	struct block_device *bdev;
	sector_t sector;
	struct bio *bio;

	bdev = f2fs_target_device(sbi, fio->new_blkaddr, &sector);
	bio = bio_alloc_bioset(bdev, npages,
				fio->op | fio->op_flags | f2fs_io_flags(fio),
				GFP_NOIO, &f2fs_bioset);
	bio->bi_iter.bi_sector = sector;
	if (is_read_io(fio->op)) {
		bio->bi_end_io = f2fs_read_end_io;
		bio->bi_private = NULL;
	} else {
		bio->bi_end_io = f2fs_write_end_io;
		bio->bi_private = sbi;
		bio->bi_write_hint = f2fs_io_type_to_rw_hint(sbi,
						fio->type, fio->temp);
	}
	iostat_alloc_and_bind_ctx(sbi, bio, NULL);

	if (fio->io_wbc)
		wbc_init_bio(fio->io_wbc, bio);

	return bio;
}

static void f2fs_set_bio_crypt_ctx(struct bio *bio, const struct inode *inode,
				  pgoff_t first_idx,
				  const struct f2fs_io_info *fio,
				  gfp_t gfp_mask)
{
	/*
	 * The f2fs garbage collector sets ->encrypted_page when it wants to
	 * read/write raw data without encryption.
	 */
	if (!fio || !fio->encrypted_page)
		fscrypt_set_bio_crypt_ctx(bio, inode, first_idx, gfp_mask);
}

static bool f2fs_crypt_mergeable_bio(struct bio *bio, const struct inode *inode,
				     pgoff_t next_idx,
				     const struct f2fs_io_info *fio)
{
	/*
	 * The f2fs garbage collector sets ->encrypted_page when it wants to
	 * read/write raw data without encryption.
	 */
	if (fio && fio->encrypted_page)
		return !bio_has_crypt_ctx(bio);

	return fscrypt_mergeable_bio(bio, inode, next_idx);
}

void f2fs_submit_read_bio(struct f2fs_sb_info *sbi, struct bio *bio,
				 enum page_type type)
{
	WARN_ON_ONCE(!is_read_io(bio_op(bio)));
	trace_f2fs_submit_read_bio(sbi->sb, type, bio);

	iostat_update_submit_ctx(bio, type);
	submit_bio(bio);
}

static void f2fs_submit_write_bio(struct f2fs_sb_info *sbi, struct bio *bio,
				  enum page_type type)
{
	WARN_ON_ONCE(is_read_io(bio_op(bio)));
	trace_f2fs_submit_write_bio(sbi->sb, type, bio);
	iostat_update_submit_ctx(bio, type);
	submit_bio(bio);
}

static void __submit_merged_bio(struct f2fs_bio_info *io)
{
	struct f2fs_io_info *fio = &io->fio;

	if (!io->bio)
		return;

	if (is_read_io(fio->op)) {
		trace_f2fs_prepare_read_bio(io->sbi->sb, fio->type, io->bio);
		f2fs_submit_read_bio(io->sbi, io->bio, fio->type);
	} else {
		trace_f2fs_prepare_write_bio(io->sbi->sb, fio->type, io->bio);
		f2fs_submit_write_bio(io->sbi, io->bio, fio->type);
	}
	io->bio = NULL;
}

static bool __has_merged_page(struct bio *bio, struct inode *inode,
						struct page *page, nid_t ino)
{
	struct folio_iter fi;

	if (!bio)
		return false;

	if (!inode && !page && !ino)
		return true;

	bio_for_each_folio_all(fi, bio) {
		struct folio *target = fi.folio;

		if (fscrypt_is_bounce_folio(target)) {
			target = fscrypt_pagecache_folio(target);
			if (IS_ERR(target))
				continue;
		}
		if (f2fs_is_compressed_page(&target->page)) {
			target = f2fs_compress_control_folio(target);
			if (IS_ERR(target))
				continue;
		}

		if (inode && inode == target->mapping->host)
			return true;
		if (page && page == &target->page)
			return true;
		if (ino && ino == ino_of_node(&target->page))
			return true;
	}

	return false;
}

int f2fs_init_write_merge_io(struct f2fs_sb_info *sbi)
{
	int i;

	for (i = 0; i < NR_PAGE_TYPE; i++) {
		int n = (i == META) ? 1 : NR_TEMP_TYPE;
		int j;

		sbi->write_io[i] = f2fs_kmalloc(sbi,
				array_size(n, sizeof(struct f2fs_bio_info)),
				GFP_KERNEL);
		if (!sbi->write_io[i])
			return -ENOMEM;

		for (j = HOT; j < n; j++) {
			struct f2fs_bio_info *io = &sbi->write_io[i][j];

			init_f2fs_rwsem(&io->io_rwsem);
			io->sbi = sbi;
			io->bio = NULL;
			io->last_block_in_bio = 0;
			spin_lock_init(&io->io_lock);
			INIT_LIST_HEAD(&io->io_list);
			INIT_LIST_HEAD(&io->bio_list);
			init_f2fs_rwsem(&io->bio_list_lock);
#ifdef CONFIG_BLK_DEV_ZONED
			init_completion(&io->zone_wait);
			io->zone_pending_bio = NULL;
			io->bi_private = NULL;
#endif
		}
	}

	return 0;
}

static void __f2fs_submit_merged_write(struct f2fs_sb_info *sbi,
				enum page_type type, enum temp_type temp)
{
	enum page_type btype = PAGE_TYPE_OF_BIO(type);
	struct f2fs_bio_info *io = sbi->write_io[btype] + temp;

	f2fs_down_write(&io->io_rwsem);

	if (!io->bio)
		goto unlock_out;

	/* change META to META_FLUSH in the checkpoint procedure */
	if (type >= META_FLUSH) {
		io->fio.type = META_FLUSH;
		io->bio->bi_opf |= REQ_META | REQ_PRIO | REQ_SYNC;
		if (!test_opt(sbi, NOBARRIER))
			io->bio->bi_opf |= REQ_PREFLUSH | REQ_FUA;
	}
	__submit_merged_bio(io);
unlock_out:
	f2fs_up_write(&io->io_rwsem);
}

static void __submit_merged_write_cond(struct f2fs_sb_info *sbi,
				struct inode *inode, struct page *page,
				nid_t ino, enum page_type type, bool force)
{
	enum temp_type temp;
	bool ret = true;

	for (temp = HOT; temp < NR_TEMP_TYPE; temp++) {
		if (!force)	{
			enum page_type btype = PAGE_TYPE_OF_BIO(type);
			struct f2fs_bio_info *io = sbi->write_io[btype] + temp;

			f2fs_down_read(&io->io_rwsem);
			ret = __has_merged_page(io->bio, inode, page, ino);
			f2fs_up_read(&io->io_rwsem);
		}
		if (ret)
			__f2fs_submit_merged_write(sbi, type, temp);

		/* TODO: use HOT temp only for meta pages now. */
		if (type >= META)
			break;
	}
}

void f2fs_submit_merged_write(struct f2fs_sb_info *sbi, enum page_type type)
{
	__submit_merged_write_cond(sbi, NULL, NULL, 0, type, true);
}

void f2fs_submit_merged_write_cond(struct f2fs_sb_info *sbi,
				struct inode *inode, struct page *page,
				nid_t ino, enum page_type type)
{
	__submit_merged_write_cond(sbi, inode, page, ino, type, false);
}

void f2fs_flush_merged_writes(struct f2fs_sb_info *sbi)
{
	f2fs_submit_merged_write(sbi, DATA);
	f2fs_submit_merged_write(sbi, NODE);
	f2fs_submit_merged_write(sbi, META);
}

/*
 * Fill the locked page with data located in the block address.
 * A caller needs to unlock the page on failure.
 */
int f2fs_submit_page_bio(struct f2fs_io_info *fio)
{
	struct bio *bio;
	struct folio *fio_folio = page_folio(fio->page);
	struct folio *data_folio = fio->encrypted_page ?
			page_folio(fio->encrypted_page) : fio_folio;

	if (!f2fs_is_valid_blkaddr(fio->sbi, fio->new_blkaddr,
			fio->is_por ? META_POR : (__is_meta_io(fio) ?
			META_GENERIC : DATA_GENERIC_ENHANCE)))
		return -EFSCORRUPTED;

	trace_f2fs_submit_folio_bio(data_folio, fio);

	/* Allocate a new bio */
	bio = __bio_alloc(fio, 1);

	f2fs_set_bio_crypt_ctx(bio, fio_folio->mapping->host,
			fio_folio->index, fio, GFP_NOIO);
	bio_add_folio_nofail(bio, data_folio, folio_size(data_folio), 0);

	if (fio->io_wbc && !is_read_io(fio->op))
		wbc_account_cgroup_owner(fio->io_wbc, fio_folio, PAGE_SIZE);

	inc_page_count(fio->sbi, is_read_io(fio->op) ?
			__read_io_type(data_folio) : WB_DATA_TYPE(fio->page, false));

	if (is_read_io(bio_op(bio)))
		f2fs_submit_read_bio(fio->sbi, bio, fio->type);
	else
		f2fs_submit_write_bio(fio->sbi, bio, fio->type);
	return 0;
}

static bool page_is_mergeable(struct f2fs_sb_info *sbi, struct bio *bio,
				block_t last_blkaddr, block_t cur_blkaddr)
{
	if (unlikely(sbi->max_io_bytes &&
			bio->bi_iter.bi_size >= sbi->max_io_bytes))
		return false;
	if (last_blkaddr + 1 != cur_blkaddr)
		return false;
	return bio->bi_bdev == f2fs_target_device(sbi, cur_blkaddr, NULL);
}

static bool io_type_is_mergeable(struct f2fs_bio_info *io,
						struct f2fs_io_info *fio)
{
	if (io->fio.op != fio->op)
		return false;
	return io->fio.op_flags == fio->op_flags;
}

static bool io_is_mergeable(struct f2fs_sb_info *sbi, struct bio *bio,
					struct f2fs_bio_info *io,
					struct f2fs_io_info *fio,
					block_t last_blkaddr,
					block_t cur_blkaddr)
{
	if (!page_is_mergeable(sbi, bio, last_blkaddr, cur_blkaddr))
		return false;
	return io_type_is_mergeable(io, fio);
}

static void add_bio_entry(struct f2fs_sb_info *sbi, struct bio *bio,
				struct page *page, enum temp_type temp)
{
	struct f2fs_bio_info *io = sbi->write_io[DATA] + temp;
	struct bio_entry *be;

	be = f2fs_kmem_cache_alloc(bio_entry_slab, GFP_NOFS, true, NULL);
	be->bio = bio;
	bio_get(bio);

	if (bio_add_page(bio, page, PAGE_SIZE, 0) != PAGE_SIZE)
		f2fs_bug_on(sbi, 1);

	f2fs_down_write(&io->bio_list_lock);
	list_add_tail(&be->list, &io->bio_list);
	f2fs_up_write(&io->bio_list_lock);
}

static void del_bio_entry(struct bio_entry *be)
{
	list_del(&be->list);
	kmem_cache_free(bio_entry_slab, be);
}

static int add_ipu_page(struct f2fs_io_info *fio, struct bio **bio,
							struct page *page)
{
	struct folio *fio_folio = page_folio(fio->page);
	struct f2fs_sb_info *sbi = fio->sbi;
	enum temp_type temp;
	bool found = false;
	int ret = -EAGAIN;

	for (temp = HOT; temp < NR_TEMP_TYPE && !found; temp++) {
		struct f2fs_bio_info *io = sbi->write_io[DATA] + temp;
		struct list_head *head = &io->bio_list;
		struct bio_entry *be;

		f2fs_down_write(&io->bio_list_lock);
		list_for_each_entry(be, head, list) {
			if (be->bio != *bio)
				continue;

			found = true;

			f2fs_bug_on(sbi, !page_is_mergeable(sbi, *bio,
							    *fio->last_block,
							    fio->new_blkaddr));
			if (f2fs_crypt_mergeable_bio(*bio,
					fio_folio->mapping->host,
					fio_folio->index, fio) &&
			    bio_add_page(*bio, page, PAGE_SIZE, 0) ==
					PAGE_SIZE) {
				ret = 0;
				break;
			}

			/* page can't be merged into bio; submit the bio */
			del_bio_entry(be);
			f2fs_submit_write_bio(sbi, *bio, DATA);
			break;
		}
		f2fs_up_write(&io->bio_list_lock);
	}

	if (ret) {
		bio_put(*bio);
		*bio = NULL;
	}

	return ret;
}

void f2fs_submit_merged_ipu_write(struct f2fs_sb_info *sbi,
					struct bio **bio, struct folio *folio)
{
	enum temp_type temp;
	bool found = false;
	struct bio *target = bio ? *bio : NULL;

	f2fs_bug_on(sbi, !target && !folio);

	for (temp = HOT; temp < NR_TEMP_TYPE && !found; temp++) {
		struct f2fs_bio_info *io = sbi->write_io[DATA] + temp;
		struct list_head *head = &io->bio_list;
		struct bio_entry *be;

		if (list_empty(head))
			continue;

		f2fs_down_read(&io->bio_list_lock);
		list_for_each_entry(be, head, list) {
			if (target)
				found = (target == be->bio);
			else
				found = __has_merged_page(be->bio, NULL,
							&folio->page, 0);
			if (found)
				break;
		}
		f2fs_up_read(&io->bio_list_lock);

		if (!found)
			continue;

		found = false;

		f2fs_down_write(&io->bio_list_lock);
		list_for_each_entry(be, head, list) {
			if (target)
				found = (target == be->bio);
			else
				found = __has_merged_page(be->bio, NULL,
							&folio->page, 0);
			if (found) {
				target = be->bio;
				del_bio_entry(be);
				break;
			}
		}
		f2fs_up_write(&io->bio_list_lock);
	}

	if (found)
		f2fs_submit_write_bio(sbi, target, DATA);
	if (bio && *bio) {
		bio_put(*bio);
		*bio = NULL;
	}
}

int f2fs_merge_page_bio(struct f2fs_io_info *fio)
{
	struct bio *bio = *fio->bio;
	struct page *page = fio->encrypted_page ?
			fio->encrypted_page : fio->page;
	struct folio *folio = page_folio(fio->page);

	if (!f2fs_is_valid_blkaddr(fio->sbi, fio->new_blkaddr,
			__is_meta_io(fio) ? META_GENERIC : DATA_GENERIC))
		return -EFSCORRUPTED;

	trace_f2fs_submit_folio_bio(page_folio(page), fio);

	if (bio && !page_is_mergeable(fio->sbi, bio, *fio->last_block,
						fio->new_blkaddr))
		f2fs_submit_merged_ipu_write(fio->sbi, &bio, NULL);
alloc_new:
	if (!bio) {
		bio = __bio_alloc(fio, BIO_MAX_VECS);
		f2fs_set_bio_crypt_ctx(bio, folio->mapping->host,
				folio->index, fio, GFP_NOIO);

		add_bio_entry(fio->sbi, bio, page, fio->temp);
	} else {
		if (add_ipu_page(fio, &bio, page))
			goto alloc_new;
	}

	if (fio->io_wbc)
		wbc_account_cgroup_owner(fio->io_wbc, folio, folio_size(folio));

	inc_page_count(fio->sbi, WB_DATA_TYPE(page, false));

	*fio->last_block = fio->new_blkaddr;
	*fio->bio = bio;

	return 0;
}

#ifdef CONFIG_BLK_DEV_ZONED
static bool is_end_zone_blkaddr(struct f2fs_sb_info *sbi, block_t blkaddr)
{
	struct block_device *bdev = sbi->sb->s_bdev;
	int devi = 0;

	if (f2fs_is_multi_device(sbi)) {
		devi = f2fs_target_device_index(sbi, blkaddr);
		if (blkaddr < FDEV(devi).start_blk ||
		    blkaddr > FDEV(devi).end_blk) {
			f2fs_err(sbi, "Invalid block %x", blkaddr);
			return false;
		}
		blkaddr -= FDEV(devi).start_blk;
		bdev = FDEV(devi).bdev;
	}
	return bdev_is_zoned(bdev) &&
		f2fs_blkz_is_seq(sbi, devi, blkaddr) &&
		(blkaddr % sbi->blocks_per_blkz == sbi->blocks_per_blkz - 1);
}
#endif

void f2fs_submit_page_write(struct f2fs_io_info *fio)
{
	struct f2fs_sb_info *sbi = fio->sbi;
	enum page_type btype = PAGE_TYPE_OF_BIO(fio->type);
	struct f2fs_bio_info *io = sbi->write_io[btype] + fio->temp;
	struct page *bio_page;
	enum count_type type;

	f2fs_bug_on(sbi, is_read_io(fio->op));

	f2fs_down_write(&io->io_rwsem);
next:
#ifdef CONFIG_BLK_DEV_ZONED
	if (f2fs_sb_has_blkzoned(sbi) && btype < META && io->zone_pending_bio) {
		wait_for_completion_io(&io->zone_wait);
		bio_put(io->zone_pending_bio);
		io->zone_pending_bio = NULL;
		io->bi_private = NULL;
	}
#endif

	if (fio->in_list) {
		spin_lock(&io->io_lock);
		if (list_empty(&io->io_list)) {
			spin_unlock(&io->io_lock);
			goto out;
		}
		fio = list_first_entry(&io->io_list,
						struct f2fs_io_info, list);
		list_del(&fio->list);
		spin_unlock(&io->io_lock);
	}

	verify_fio_blkaddr(fio);

	if (fio->encrypted_page)
		bio_page = fio->encrypted_page;
	else if (fio->compressed_page)
		bio_page = fio->compressed_page;
	else
		bio_page = fio->page;

	/* set submitted = true as a return value */
	fio->submitted = 1;

	type = WB_DATA_TYPE(bio_page, fio->compressed_page);
	inc_page_count(sbi, type);

	if (io->bio &&
	    (!io_is_mergeable(sbi, io->bio, io, fio, io->last_block_in_bio,
			      fio->new_blkaddr) ||
	     !f2fs_crypt_mergeable_bio(io->bio, fio_inode(fio),
				page_folio(bio_page)->index, fio)))
		__submit_merged_bio(io);
alloc_new:
	if (io->bio == NULL) {
		io->bio = __bio_alloc(fio, BIO_MAX_VECS);
		f2fs_set_bio_crypt_ctx(io->bio, fio_inode(fio),
				page_folio(bio_page)->index, fio, GFP_NOIO);
		io->fio = *fio;
	}

	if (bio_add_page(io->bio, bio_page, PAGE_SIZE, 0) < PAGE_SIZE) {
		__submit_merged_bio(io);
		goto alloc_new;
	}

	if (fio->io_wbc)
		wbc_account_cgroup_owner(fio->io_wbc, page_folio(fio->page),
					 PAGE_SIZE);

	io->last_block_in_bio = fio->new_blkaddr;

	trace_f2fs_submit_folio_write(page_folio(fio->page), fio);
#ifdef CONFIG_BLK_DEV_ZONED
	if (f2fs_sb_has_blkzoned(sbi) && btype < META &&
			is_end_zone_blkaddr(sbi, fio->new_blkaddr)) {
		bio_get(io->bio);
		reinit_completion(&io->zone_wait);
		io->bi_private = io->bio->bi_private;
		io->bio->bi_private = io;
		io->bio->bi_end_io = f2fs_zone_write_end_io;
		io->zone_pending_bio = io->bio;
		__submit_merged_bio(io);
	}
#endif
	if (fio->in_list)
		goto next;
out:
	if (is_sbi_flag_set(sbi, SBI_IS_SHUTDOWN) ||
				!f2fs_is_checkpoint_ready(sbi))
		__submit_merged_bio(io);
	f2fs_up_write(&io->io_rwsem);
}

static struct bio *f2fs_grab_read_bio(struct inode *inode, block_t blkaddr,
				      unsigned nr_pages, blk_opf_t op_flag,
				      pgoff_t first_idx, bool for_write)
{
	struct f2fs_sb_info *sbi = F2FS_I_SB(inode);
	struct bio *bio;
	struct bio_post_read_ctx *ctx = NULL;
	unsigned int post_read_steps = 0;
	sector_t sector;
	struct block_device *bdev = f2fs_target_device(sbi, blkaddr, &sector);

	bio = bio_alloc_bioset(bdev, bio_max_segs(nr_pages),
			       REQ_OP_READ | op_flag,
			       for_write ? GFP_NOIO : GFP_KERNEL, &f2fs_bioset);
	bio->bi_iter.bi_sector = sector;
	f2fs_set_bio_crypt_ctx(bio, inode, first_idx, NULL, GFP_NOFS);
	bio->bi_end_io = f2fs_read_end_io;

	if (fscrypt_inode_uses_fs_layer_crypto(inode))
		post_read_steps |= STEP_DECRYPT;

	if (f2fs_need_verity(inode, first_idx))
		post_read_steps |= STEP_VERITY;

	/*
	 * STEP_DECOMPRESS is handled specially, since a compressed file might
	 * contain both compressed and uncompressed clusters.  We'll allocate a
	 * bio_post_read_ctx if the file is compressed, but the caller is
	 * responsible for enabling STEP_DECOMPRESS if it's actually needed.
	 */

	if (post_read_steps || f2fs_compressed_file(inode)) {
		/* Due to the mempool, this never fails. */
		ctx = mempool_alloc(bio_post_read_ctx_pool, GFP_NOFS);
		ctx->bio = bio;
		ctx->sbi = sbi;
		ctx->enabled_steps = post_read_steps;
		ctx->fs_blkaddr = blkaddr;
		ctx->decompression_attempted = false;
		bio->bi_private = ctx;
	}
	iostat_alloc_and_bind_ctx(sbi, bio, ctx);

	return bio;
}

/* This can handle encryption stuffs */
static int f2fs_submit_page_read(struct inode *inode, struct folio *folio,
				 block_t blkaddr, blk_opf_t op_flags,
				 bool for_write)
{
	struct f2fs_sb_info *sbi = F2FS_I_SB(inode);
	struct bio *bio;

	bio = f2fs_grab_read_bio(inode, blkaddr, 1, op_flags,
					folio->index, for_write);
	if (IS_ERR(bio))
		return PTR_ERR(bio);

	/* wait for GCed page writeback via META_MAPPING */
	f2fs_wait_on_block_writeback(inode, blkaddr);

	if (!bio_add_folio(bio, folio, PAGE_SIZE, 0)) {
		iostat_update_and_unbind_ctx(bio);
		if (bio->bi_private)
			mempool_free(bio->bi_private, bio_post_read_ctx_pool);
		bio_put(bio);
		return -EFAULT;
	}
	inc_page_count(sbi, F2FS_RD_DATA);
	f2fs_update_iostat(sbi, NULL, FS_DATA_READ_IO, F2FS_BLKSIZE);
	f2fs_submit_read_bio(sbi, bio, DATA);
	return 0;
}

static void __set_data_blkaddr(struct dnode_of_data *dn, block_t blkaddr)
{
	__le32 *addr = get_dnode_addr(dn->inode, dn->node_folio);

	dn->data_blkaddr = blkaddr;
	addr[dn->ofs_in_node] = cpu_to_le32(dn->data_blkaddr);
}

/*
 * Lock ordering for the change of data block address:
 * ->data_page
 *  ->node_folio
 *    update block addresses in the node page
 */
void f2fs_set_data_blkaddr(struct dnode_of_data *dn, block_t blkaddr)
{
	f2fs_folio_wait_writeback(dn->node_folio, NODE, true, true);
	__set_data_blkaddr(dn, blkaddr);
	if (folio_mark_dirty(dn->node_folio))
		dn->node_changed = true;
}

void f2fs_update_data_blkaddr(struct dnode_of_data *dn, block_t blkaddr)
{
	f2fs_set_data_blkaddr(dn, blkaddr);
	f2fs_update_read_extent_cache(dn);
}

/* dn->ofs_in_node will be returned with up-to-date last block pointer */
int f2fs_reserve_new_blocks(struct dnode_of_data *dn, blkcnt_t count)
{
	struct f2fs_sb_info *sbi = F2FS_I_SB(dn->inode);
	int err;

	if (!count)
		return 0;

	if (unlikely(is_inode_flag_set(dn->inode, FI_NO_ALLOC)))
		return -EPERM;
	err = inc_valid_block_count(sbi, dn->inode, &count, true);
	if (unlikely(err))
		return err;

	trace_f2fs_reserve_new_blocks(dn->inode, dn->nid,
						dn->ofs_in_node, count);

	f2fs_folio_wait_writeback(dn->node_folio, NODE, true, true);

	for (; count > 0; dn->ofs_in_node++) {
		block_t blkaddr = f2fs_data_blkaddr(dn);

		if (blkaddr == NULL_ADDR) {
			__set_data_blkaddr(dn, NEW_ADDR);
			count--;
		}
	}

	if (folio_mark_dirty(dn->node_folio))
		dn->node_changed = true;
	return 0;
}

/* Should keep dn->ofs_in_node unchanged */
int f2fs_reserve_new_block(struct dnode_of_data *dn)
{
	unsigned int ofs_in_node = dn->ofs_in_node;
	int ret;

	ret = f2fs_reserve_new_blocks(dn, 1);
	dn->ofs_in_node = ofs_in_node;
	return ret;
}

int f2fs_reserve_block(struct dnode_of_data *dn, pgoff_t index)
{
	bool need_put = dn->inode_folio ? false : true;
	int err;

	err = f2fs_get_dnode_of_data(dn, index, ALLOC_NODE);
	if (err)
		return err;

	if (dn->data_blkaddr == NULL_ADDR)
		err = f2fs_reserve_new_block(dn);
	if (err || need_put)
		f2fs_put_dnode(dn);
	return err;
}

struct folio *f2fs_get_read_data_folio(struct inode *inode, pgoff_t index,
		blk_opf_t op_flags, bool for_write, pgoff_t *next_pgofs)
{
	struct address_space *mapping = inode->i_mapping;
	struct dnode_of_data dn;
	struct folio *folio;
	int err;

	folio = f2fs_grab_cache_folio(mapping, index, for_write);
	if (IS_ERR(folio))
		return folio;

	if (f2fs_lookup_read_extent_cache_block(inode, index,
						&dn.data_blkaddr)) {
		if (!f2fs_is_valid_blkaddr(F2FS_I_SB(inode), dn.data_blkaddr,
						DATA_GENERIC_ENHANCE_READ)) {
			err = -EFSCORRUPTED;
			goto put_err;
		}
		goto got_it;
	}

	set_new_dnode(&dn, inode, NULL, NULL, 0);
	err = f2fs_get_dnode_of_data(&dn, index, LOOKUP_NODE);
	if (err) {
		if (err == -ENOENT && next_pgofs)
			*next_pgofs = f2fs_get_next_page_offset(&dn, index);
		goto put_err;
	}
	f2fs_put_dnode(&dn);

	if (unlikely(dn.data_blkaddr == NULL_ADDR)) {
		err = -ENOENT;
		if (next_pgofs)
			*next_pgofs = index + 1;
		goto put_err;
	}
	if (dn.data_blkaddr != NEW_ADDR &&
			!f2fs_is_valid_blkaddr(F2FS_I_SB(inode),
						dn.data_blkaddr,
						DATA_GENERIC_ENHANCE)) {
		err = -EFSCORRUPTED;
		goto put_err;
	}
got_it:
	if (folio_test_uptodate(folio)) {
		folio_unlock(folio);
		return folio;
	}

	/*
	 * A new dentry page is allocated but not able to be written, since its
	 * new inode page couldn't be allocated due to -ENOSPC.
	 * In such the case, its blkaddr can be remained as NEW_ADDR.
	 * see, f2fs_add_link -> f2fs_get_new_data_folio ->
	 * f2fs_init_inode_metadata.
	 */
	if (dn.data_blkaddr == NEW_ADDR) {
		folio_zero_segment(folio, 0, folio_size(folio));
		if (!folio_test_uptodate(folio))
			folio_mark_uptodate(folio);
		folio_unlock(folio);
		return folio;
	}

	err = f2fs_submit_page_read(inode, folio, dn.data_blkaddr,
						op_flags, for_write);
	if (err)
		goto put_err;
	return folio;

put_err:
	f2fs_folio_put(folio, true);
	return ERR_PTR(err);
}

struct folio *f2fs_find_data_folio(struct inode *inode, pgoff_t index,
					pgoff_t *next_pgofs)
{
	struct address_space *mapping = inode->i_mapping;
	struct folio *folio;

	folio = __filemap_get_folio(mapping, index, FGP_ACCESSED, 0);
	if (IS_ERR(folio))
		goto read;
	if (folio_test_uptodate(folio))
		return folio;
	f2fs_folio_put(folio, false);

read:
	folio = f2fs_get_read_data_folio(inode, index, 0, false, next_pgofs);
	if (IS_ERR(folio))
		return folio;

	if (folio_test_uptodate(folio))
		return folio;

	folio_wait_locked(folio);
	if (unlikely(!folio_test_uptodate(folio))) {
		f2fs_folio_put(folio, false);
		return ERR_PTR(-EIO);
	}
	return folio;
}

/*
 * If it tries to access a hole, return an error.
 * Because, the callers, functions in dir.c and GC, should be able to know
 * whether this page exists or not.
 */
struct folio *f2fs_get_lock_data_folio(struct inode *inode, pgoff_t index,
							bool for_write)
{
	struct address_space *mapping = inode->i_mapping;
	struct folio *folio;

	folio = f2fs_get_read_data_folio(inode, index, 0, for_write, NULL);
	if (IS_ERR(folio))
		return folio;

	/* wait for read completion */
	folio_lock(folio);
	if (unlikely(folio->mapping != mapping || !folio_test_uptodate(folio))) {
		f2fs_folio_put(folio, true);
		return ERR_PTR(-EIO);
	}
	return folio;
}

/*
 * Caller ensures that this data page is never allocated.
 * A new zero-filled data page is allocated in the page cache.
 *
 * Also, caller should grab and release a rwsem by calling f2fs_lock_op() and
 * f2fs_unlock_op().
 * Note that, ifolio is set only by make_empty_dir, and if any error occur,
 * ifolio should be released by this function.
 */
struct folio *f2fs_get_new_data_folio(struct inode *inode,
		struct folio *ifolio, pgoff_t index, bool new_i_size)
{
	struct address_space *mapping = inode->i_mapping;
	struct folio *folio;
	struct dnode_of_data dn;
	int err;

	folio = f2fs_grab_cache_folio(mapping, index, true);
	if (IS_ERR(folio)) {
		/*
		 * before exiting, we should make sure ifolio will be released
		 * if any error occur.
		 */
		f2fs_folio_put(ifolio, true);
		return ERR_PTR(-ENOMEM);
	}

	set_new_dnode(&dn, inode, ifolio, NULL, 0);
	err = f2fs_reserve_block(&dn, index);
	if (err) {
		f2fs_folio_put(folio, true);
		return ERR_PTR(err);
	}
	if (!ifolio)
		f2fs_put_dnode(&dn);

	if (folio_test_uptodate(folio))
		goto got_it;

	if (dn.data_blkaddr == NEW_ADDR) {
		folio_zero_segment(folio, 0, folio_size(folio));
		if (!folio_test_uptodate(folio))
			folio_mark_uptodate(folio);
	} else {
		f2fs_folio_put(folio, true);

		/* if ifolio exists, blkaddr should be NEW_ADDR */
		f2fs_bug_on(F2FS_I_SB(inode), ifolio);
		folio = f2fs_get_lock_data_folio(inode, index, true);
		if (IS_ERR(folio))
			return folio;
	}
got_it:
	if (new_i_size && i_size_read(inode) <
				((loff_t)(index + 1) << PAGE_SHIFT))
		f2fs_i_size_write(inode, ((loff_t)(index + 1) << PAGE_SHIFT));
	return folio;
}

static int __allocate_data_block(struct dnode_of_data *dn, int seg_type)
{
	struct f2fs_sb_info *sbi = F2FS_I_SB(dn->inode);
	struct f2fs_summary sum;
	struct node_info ni;
	block_t old_blkaddr;
	blkcnt_t count = 1;
	int err;

	if (unlikely(is_inode_flag_set(dn->inode, FI_NO_ALLOC)))
		return -EPERM;

	err = f2fs_get_node_info(sbi, dn->nid, &ni, false);
	if (err)
		return err;

	dn->data_blkaddr = f2fs_data_blkaddr(dn);
	if (dn->data_blkaddr == NULL_ADDR) {
		err = inc_valid_block_count(sbi, dn->inode, &count, true);
		if (unlikely(err))
			return err;
	}

	set_summary(&sum, dn->nid, dn->ofs_in_node, ni.version);
	old_blkaddr = dn->data_blkaddr;
	err = f2fs_allocate_data_block(sbi, NULL, old_blkaddr,
				&dn->data_blkaddr, &sum, seg_type, NULL);
	if (err)
		return err;

	if (GET_SEGNO(sbi, old_blkaddr) != NULL_SEGNO)
		f2fs_invalidate_internal_cache(sbi, old_blkaddr, 1);

	f2fs_update_data_blkaddr(dn, dn->data_blkaddr);
	return 0;
}

static void f2fs_map_lock(struct f2fs_sb_info *sbi, int flag)
{
	if (flag == F2FS_GET_BLOCK_PRE_AIO)
		f2fs_down_read(&sbi->node_change);
	else
		f2fs_lock_op(sbi);
}

static void f2fs_map_unlock(struct f2fs_sb_info *sbi, int flag)
{
	if (flag == F2FS_GET_BLOCK_PRE_AIO)
		f2fs_up_read(&sbi->node_change);
	else
		f2fs_unlock_op(sbi);
}

int f2fs_get_block_locked(struct dnode_of_data *dn, pgoff_t index)
{
	struct f2fs_sb_info *sbi = F2FS_I_SB(dn->inode);
	int err = 0;

	f2fs_map_lock(sbi, F2FS_GET_BLOCK_PRE_AIO);
	if (!f2fs_lookup_read_extent_cache_block(dn->inode, index,
						&dn->data_blkaddr))
		err = f2fs_reserve_block(dn, index);
	f2fs_map_unlock(sbi, F2FS_GET_BLOCK_PRE_AIO);

	return err;
}

static int f2fs_map_no_dnode(struct inode *inode,
		struct f2fs_map_blocks *map, struct dnode_of_data *dn,
		pgoff_t pgoff)
{
	struct f2fs_sb_info *sbi = F2FS_I_SB(inode);

	/*
	 * There is one exceptional case that read_node_page() may return
	 * -ENOENT due to filesystem has been shutdown or cp_error, return
	 * -EIO in that case.
	 */
	if (map->m_may_create &&
	    (is_sbi_flag_set(sbi, SBI_IS_SHUTDOWN) || f2fs_cp_error(sbi)))
		return -EIO;

	if (map->m_next_pgofs)
		*map->m_next_pgofs = f2fs_get_next_page_offset(dn, pgoff);
	if (map->m_next_extent)
		*map->m_next_extent = f2fs_get_next_page_offset(dn, pgoff);
	return 0;
}

static bool f2fs_map_blocks_cached(struct inode *inode,
		struct f2fs_map_blocks *map, int flag)
{
	struct f2fs_sb_info *sbi = F2FS_I_SB(inode);
	unsigned int maxblocks = map->m_len;
	pgoff_t pgoff = (pgoff_t)map->m_lblk;
	struct extent_info ei = {};

	if (!f2fs_lookup_read_extent_cache(inode, pgoff, &ei))
		return false;

	map->m_pblk = ei.blk + pgoff - ei.fofs;
	map->m_len = min((pgoff_t)maxblocks, ei.fofs + ei.len - pgoff);
	map->m_flags = F2FS_MAP_MAPPED;
	if (map->m_next_extent)
		*map->m_next_extent = pgoff + map->m_len;

	/* for hardware encryption, but to avoid potential issue in future */
	if (flag == F2FS_GET_BLOCK_DIO)
		f2fs_wait_on_block_writeback_range(inode,
					map->m_pblk, map->m_len);

	if (f2fs_allow_multi_device_dio(sbi, flag)) {
		int bidx = f2fs_target_device_index(sbi, map->m_pblk);
		struct f2fs_dev_info *dev = &sbi->devs[bidx];

		map->m_bdev = dev->bdev;
		map->m_pblk -= dev->start_blk;
		map->m_len = min(map->m_len, dev->end_blk + 1 - map->m_pblk);
	} else {
		map->m_bdev = inode->i_sb->s_bdev;
	}
	return true;
}

static bool map_is_mergeable(struct f2fs_sb_info *sbi,
				struct f2fs_map_blocks *map,
				block_t blkaddr, int flag, int bidx,
				int ofs)
{
	if (map->m_multidev_dio && map->m_bdev != FDEV(bidx).bdev)
		return false;
	if (map->m_pblk != NEW_ADDR && blkaddr == (map->m_pblk + ofs))
		return true;
	if (map->m_pblk == NEW_ADDR && blkaddr == NEW_ADDR)
		return true;
	if (flag == F2FS_GET_BLOCK_PRE_DIO)
		return true;
	if (flag == F2FS_GET_BLOCK_DIO &&
		map->m_pblk == NULL_ADDR && blkaddr == NULL_ADDR)
		return true;
	return false;
}

/*
 * f2fs_map_blocks() tries to find or build mapping relationship which
 * maps continuous logical blocks to physical blocks, and return such
 * info via f2fs_map_blocks structure.
 */
int f2fs_map_blocks(struct inode *inode, struct f2fs_map_blocks *map, int flag)
{
	unsigned int maxblocks = map->m_len;
	struct dnode_of_data dn;
	struct f2fs_sb_info *sbi = F2FS_I_SB(inode);
	int mode = map->m_may_create ? ALLOC_NODE : LOOKUP_NODE;
	pgoff_t pgofs, end_offset, end;
	int err = 0, ofs = 1;
	unsigned int ofs_in_node, last_ofs_in_node;
	blkcnt_t prealloc;
	block_t blkaddr;
	unsigned int start_pgofs;
	int bidx = 0;
	bool is_hole;

	if (!maxblocks)
		return 0;

	if (!map->m_may_create && f2fs_map_blocks_cached(inode, map, flag))
		goto out;

	map->m_bdev = inode->i_sb->s_bdev;
	map->m_multidev_dio =
		f2fs_allow_multi_device_dio(F2FS_I_SB(inode), flag);

	map->m_len = 0;
	map->m_flags = 0;

	/* it only supports block size == page size */
	pgofs =	(pgoff_t)map->m_lblk;
	end = pgofs + maxblocks;

next_dnode:
	if (map->m_may_create)
		f2fs_map_lock(sbi, flag);

	/* When reading holes, we need its node page */
	set_new_dnode(&dn, inode, NULL, NULL, 0);
	err = f2fs_get_dnode_of_data(&dn, pgofs, mode);
	if (err) {
		if (flag == F2FS_GET_BLOCK_BMAP)
			map->m_pblk = 0;
		if (err == -ENOENT)
			err = f2fs_map_no_dnode(inode, map, &dn, pgofs);
		goto unlock_out;
	}

	start_pgofs = pgofs;
	prealloc = 0;
	last_ofs_in_node = ofs_in_node = dn.ofs_in_node;
	end_offset = ADDRS_PER_PAGE(&dn.node_folio->page, inode);

next_block:
	blkaddr = f2fs_data_blkaddr(&dn);
	is_hole = !__is_valid_data_blkaddr(blkaddr);
	if (!is_hole &&
	    !f2fs_is_valid_blkaddr(sbi, blkaddr, DATA_GENERIC_ENHANCE)) {
		err = -EFSCORRUPTED;
		goto sync_out;
	}

	/* use out-place-update for direct IO under LFS mode */
	if (map->m_may_create && (is_hole ||
		(flag == F2FS_GET_BLOCK_DIO && f2fs_lfs_mode(sbi) &&
		!f2fs_is_pinned_file(inode)))) {
		if (unlikely(f2fs_cp_error(sbi))) {
			err = -EIO;
			goto sync_out;
		}

		switch (flag) {
		case F2FS_GET_BLOCK_PRE_AIO:
			if (blkaddr == NULL_ADDR) {
				prealloc++;
				last_ofs_in_node = dn.ofs_in_node;
			}
			break;
		case F2FS_GET_BLOCK_PRE_DIO:
		case F2FS_GET_BLOCK_DIO:
			err = __allocate_data_block(&dn, map->m_seg_type);
			if (err)
				goto sync_out;
			if (flag == F2FS_GET_BLOCK_PRE_DIO)
				file_need_truncate(inode);
			set_inode_flag(inode, FI_APPEND_WRITE);
			break;
		default:
			WARN_ON_ONCE(1);
			err = -EIO;
			goto sync_out;
		}

		blkaddr = dn.data_blkaddr;
		if (is_hole)
			map->m_flags |= F2FS_MAP_NEW;
	} else if (is_hole) {
		if (f2fs_compressed_file(inode) &&
		    f2fs_sanity_check_cluster(&dn)) {
			err = -EFSCORRUPTED;
			f2fs_handle_error(sbi,
					ERROR_CORRUPTED_CLUSTER);
			goto sync_out;
		}

		switch (flag) {
		case F2FS_GET_BLOCK_PRECACHE:
			goto sync_out;
		case F2FS_GET_BLOCK_BMAP:
			map->m_pblk = 0;
			goto sync_out;
		case F2FS_GET_BLOCK_FIEMAP:
			if (blkaddr == NULL_ADDR) {
				if (map->m_next_pgofs)
					*map->m_next_pgofs = pgofs + 1;
				goto sync_out;
			}
			break;
		case F2FS_GET_BLOCK_DIO:
			if (map->m_next_pgofs)
				*map->m_next_pgofs = pgofs + 1;
			break;
		default:
			/* for defragment case */
			if (map->m_next_pgofs)
				*map->m_next_pgofs = pgofs + 1;
			goto sync_out;
		}
	}

	if (flag == F2FS_GET_BLOCK_PRE_AIO)
		goto skip;

	if (map->m_multidev_dio)
		bidx = f2fs_target_device_index(sbi, blkaddr);

	if (map->m_len == 0) {
		/* reserved delalloc block should be mapped for fiemap. */
		if (blkaddr == NEW_ADDR)
			map->m_flags |= F2FS_MAP_DELALLOC;
		/* DIO READ and hole case, should not map the blocks. */
		if (!(flag == F2FS_GET_BLOCK_DIO && is_hole && !map->m_may_create))
			map->m_flags |= F2FS_MAP_MAPPED;

		map->m_pblk = blkaddr;
		map->m_len = 1;

		if (map->m_multidev_dio)
			map->m_bdev = FDEV(bidx).bdev;
	} else if (map_is_mergeable(sbi, map, blkaddr, flag, bidx, ofs)) {
		ofs++;
		map->m_len++;
	} else {
		goto sync_out;
	}

skip:
	dn.ofs_in_node++;
	pgofs++;

	/* preallocate blocks in batch for one dnode page */
	if (flag == F2FS_GET_BLOCK_PRE_AIO &&
			(pgofs == end || dn.ofs_in_node == end_offset)) {

		dn.ofs_in_node = ofs_in_node;
		err = f2fs_reserve_new_blocks(&dn, prealloc);
		if (err)
			goto sync_out;

		map->m_len += dn.ofs_in_node - ofs_in_node;
		if (prealloc && dn.ofs_in_node != last_ofs_in_node + 1) {
			err = -ENOSPC;
			goto sync_out;
		}
		dn.ofs_in_node = end_offset;
	}

	if (flag == F2FS_GET_BLOCK_DIO && f2fs_lfs_mode(sbi) &&
	    map->m_may_create) {
		/* the next block to be allocated may not be contiguous. */
		if (GET_SEGOFF_FROM_SEG0(sbi, blkaddr) % BLKS_PER_SEC(sbi) ==
		    CAP_BLKS_PER_SEC(sbi) - 1)
			goto sync_out;
	}

	if (pgofs >= end)
		goto sync_out;
	else if (dn.ofs_in_node < end_offset)
		goto next_block;

	if (flag == F2FS_GET_BLOCK_PRECACHE) {
		if (map->m_flags & F2FS_MAP_MAPPED) {
			unsigned int ofs = start_pgofs - map->m_lblk;

			f2fs_update_read_extent_cache_range(&dn,
				start_pgofs, map->m_pblk + ofs,
				map->m_len - ofs);
		}
	}

	f2fs_put_dnode(&dn);

	if (map->m_may_create) {
		f2fs_map_unlock(sbi, flag);
		f2fs_balance_fs(sbi, dn.node_changed);
	}
	goto next_dnode;

sync_out:

	if (flag == F2FS_GET_BLOCK_DIO && map->m_flags & F2FS_MAP_MAPPED) {
		/*
		 * for hardware encryption, but to avoid potential issue
		 * in future
		 */
		f2fs_wait_on_block_writeback_range(inode,
						map->m_pblk, map->m_len);

		if (map->m_multidev_dio) {
			block_t blk_addr = map->m_pblk;

			bidx = f2fs_target_device_index(sbi, map->m_pblk);

			map->m_bdev = FDEV(bidx).bdev;
			map->m_pblk -= FDEV(bidx).start_blk;

			if (map->m_may_create)
				f2fs_update_device_state(sbi, inode->i_ino,
							blk_addr, map->m_len);

			f2fs_bug_on(sbi, blk_addr + map->m_len >
						FDEV(bidx).end_blk + 1);
		}
	}

	if (flag == F2FS_GET_BLOCK_PRECACHE) {
		if (map->m_flags & F2FS_MAP_MAPPED) {
			unsigned int ofs = start_pgofs - map->m_lblk;

			f2fs_update_read_extent_cache_range(&dn,
				start_pgofs, map->m_pblk + ofs,
				map->m_len - ofs);
		}
		if (map->m_next_extent)
			*map->m_next_extent = pgofs + 1;
	}
	f2fs_put_dnode(&dn);
unlock_out:
	if (map->m_may_create) {
		f2fs_map_unlock(sbi, flag);
		f2fs_balance_fs(sbi, dn.node_changed);
	}
out:
	trace_f2fs_map_blocks(inode, map, flag, err);
	return err;
}

bool f2fs_overwrite_io(struct inode *inode, loff_t pos, size_t len)
{
	struct f2fs_map_blocks map;
	block_t last_lblk;
	int err;

	if (pos + len > i_size_read(inode))
		return false;

	map.m_lblk = F2FS_BYTES_TO_BLK(pos);
	map.m_next_pgofs = NULL;
	map.m_next_extent = NULL;
	map.m_seg_type = NO_CHECK_TYPE;
	map.m_may_create = false;
	last_lblk = F2FS_BLK_ALIGN(pos + len);

	while (map.m_lblk < last_lblk) {
		map.m_len = last_lblk - map.m_lblk;
		err = f2fs_map_blocks(inode, &map, F2FS_GET_BLOCK_DEFAULT);
		if (err || map.m_len == 0)
			return false;
		map.m_lblk += map.m_len;
	}
	return true;
}

static int f2fs_xattr_fiemap(struct inode *inode,
				struct fiemap_extent_info *fieinfo)
{
	struct f2fs_sb_info *sbi = F2FS_I_SB(inode);
	struct node_info ni;
	__u64 phys = 0, len;
	__u32 flags;
	nid_t xnid = F2FS_I(inode)->i_xattr_nid;
	int err = 0;

	if (f2fs_has_inline_xattr(inode)) {
		int offset;
		struct folio *folio = f2fs_grab_cache_folio(NODE_MAPPING(sbi),
				inode->i_ino, false);

		if (IS_ERR(folio))
			return PTR_ERR(folio);

		err = f2fs_get_node_info(sbi, inode->i_ino, &ni, false);
		if (err) {
			f2fs_folio_put(folio, true);
			return err;
		}

		phys = F2FS_BLK_TO_BYTES(ni.blk_addr);
		offset = offsetof(struct f2fs_inode, i_addr) +
					sizeof(__le32) * (DEF_ADDRS_PER_INODE -
					get_inline_xattr_addrs(inode));

		phys += offset;
		len = inline_xattr_size(inode);

		f2fs_folio_put(folio, true);

		flags = FIEMAP_EXTENT_DATA_INLINE | FIEMAP_EXTENT_NOT_ALIGNED;

		if (!xnid)
			flags |= FIEMAP_EXTENT_LAST;

		err = fiemap_fill_next_extent(fieinfo, 0, phys, len, flags);
		trace_f2fs_fiemap(inode, 0, phys, len, flags, err);
		if (err)
			return err;
	}

	if (xnid) {
		struct folio *folio = f2fs_grab_cache_folio(NODE_MAPPING(sbi),
				xnid, false);

		if (IS_ERR(folio))
			return PTR_ERR(folio);

		err = f2fs_get_node_info(sbi, xnid, &ni, false);
		if (err) {
			f2fs_folio_put(folio, true);
			return err;
		}

		phys = F2FS_BLK_TO_BYTES(ni.blk_addr);
		len = inode->i_sb->s_blocksize;

		f2fs_folio_put(folio, true);

		flags = FIEMAP_EXTENT_LAST;
	}

	if (phys) {
		err = fiemap_fill_next_extent(fieinfo, 0, phys, len, flags);
		trace_f2fs_fiemap(inode, 0, phys, len, flags, err);
	}

	return (err < 0 ? err : 0);
}

int f2fs_fiemap(struct inode *inode, struct fiemap_extent_info *fieinfo,
		u64 start, u64 len)
{
	struct f2fs_map_blocks map;
	sector_t start_blk, last_blk, blk_len, max_len;
	pgoff_t next_pgofs;
	u64 logical = 0, phys = 0, size = 0;
	u32 flags = 0;
	int ret = 0;
	bool compr_cluster = false, compr_appended;
	unsigned int cluster_size = F2FS_I(inode)->i_cluster_size;
	unsigned int count_in_cluster = 0;
	loff_t maxbytes;

	if (fieinfo->fi_flags & FIEMAP_FLAG_CACHE) {
		ret = f2fs_precache_extents(inode);
		if (ret)
			return ret;
	}

	ret = fiemap_prep(inode, fieinfo, start, &len, FIEMAP_FLAG_XATTR);
	if (ret)
		return ret;

	inode_lock_shared(inode);

	maxbytes = F2FS_BLK_TO_BYTES(max_file_blocks(inode));
	if (start > maxbytes) {
		ret = -EFBIG;
		goto out;
	}

	if (len > maxbytes || (maxbytes - len) < start)
		len = maxbytes - start;

	if (fieinfo->fi_flags & FIEMAP_FLAG_XATTR) {
		ret = f2fs_xattr_fiemap(inode, fieinfo);
		goto out;
	}

	if (f2fs_has_inline_data(inode) || f2fs_has_inline_dentry(inode)) {
		ret = f2fs_inline_data_fiemap(inode, fieinfo, start, len);
		if (ret != -EAGAIN)
			goto out;
	}

	start_blk = F2FS_BYTES_TO_BLK(start);
	last_blk = F2FS_BYTES_TO_BLK(start + len - 1);
	blk_len = last_blk - start_blk + 1;
	max_len = F2FS_BYTES_TO_BLK(maxbytes) - start_blk;

next:
	memset(&map, 0, sizeof(map));
	map.m_lblk = start_blk;
	map.m_len = blk_len;
	map.m_next_pgofs = &next_pgofs;
	map.m_seg_type = NO_CHECK_TYPE;

	if (compr_cluster) {
		map.m_lblk += 1;
		map.m_len = cluster_size - count_in_cluster;
	}

	ret = f2fs_map_blocks(inode, &map, F2FS_GET_BLOCK_FIEMAP);
	if (ret)
		goto out;

	/* HOLE */
	if (!compr_cluster && !(map.m_flags & F2FS_MAP_FLAGS)) {
		start_blk = next_pgofs;

		if (F2FS_BLK_TO_BYTES(start_blk) < maxbytes)
			goto prep_next;

		flags |= FIEMAP_EXTENT_LAST;
	}

	/*
	 * current extent may cross boundary of inquiry, increase len to
	 * requery.
	 */
	if (!compr_cluster && (map.m_flags & F2FS_MAP_MAPPED) &&
				map.m_lblk + map.m_len - 1 == last_blk &&
				blk_len != max_len) {
		blk_len = max_len;
		goto next;
	}

	compr_appended = false;
	/* In a case of compressed cluster, append this to the last extent */
	if (compr_cluster && ((map.m_flags & F2FS_MAP_DELALLOC) ||
			!(map.m_flags & F2FS_MAP_FLAGS))) {
		compr_appended = true;
		goto skip_fill;
	}

	if (size) {
		flags |= FIEMAP_EXTENT_MERGED;
		if (IS_ENCRYPTED(inode))
			flags |= FIEMAP_EXTENT_DATA_ENCRYPTED;

		ret = fiemap_fill_next_extent(fieinfo, logical,
				phys, size, flags);
		trace_f2fs_fiemap(inode, logical, phys, size, flags, ret);
		if (ret)
			goto out;
		size = 0;
	}

	if (start_blk > last_blk)
		goto out;

skip_fill:
	if (map.m_pblk == COMPRESS_ADDR) {
		compr_cluster = true;
		count_in_cluster = 1;
	} else if (compr_appended) {
		unsigned int appended_blks = cluster_size -
						count_in_cluster + 1;
		size += F2FS_BLK_TO_BYTES(appended_blks);
		start_blk += appended_blks;
		compr_cluster = false;
	} else {
		logical = F2FS_BLK_TO_BYTES(start_blk);
		phys = __is_valid_data_blkaddr(map.m_pblk) ?
			F2FS_BLK_TO_BYTES(map.m_pblk) : 0;
		size = F2FS_BLK_TO_BYTES(map.m_len);
		flags = 0;

		if (compr_cluster) {
			flags = FIEMAP_EXTENT_ENCODED;
			count_in_cluster += map.m_len;
			if (count_in_cluster == cluster_size) {
				compr_cluster = false;
				size += F2FS_BLKSIZE;
			}
		} else if (map.m_flags & F2FS_MAP_DELALLOC) {
			flags = FIEMAP_EXTENT_UNWRITTEN;
		}

		start_blk += F2FS_BYTES_TO_BLK(size);
	}

prep_next:
	cond_resched();
	if (fatal_signal_pending(current))
		ret = -EINTR;
	else
		goto next;
out:
	if (ret == 1)
		ret = 0;

	inode_unlock_shared(inode);
	return ret;
}

static inline loff_t f2fs_readpage_limit(struct inode *inode)
{
	if (IS_ENABLED(CONFIG_FS_VERITY) && IS_VERITY(inode))
		return F2FS_BLK_TO_BYTES(max_file_blocks(inode));

	return i_size_read(inode);
}

static inline blk_opf_t f2fs_ra_op_flags(struct readahead_control *rac)
{
	return rac ? REQ_RAHEAD : 0;
}

static int f2fs_read_single_page(struct inode *inode, struct folio *folio,
					unsigned nr_pages,
					struct f2fs_map_blocks *map,
					struct bio **bio_ret,
					sector_t *last_block_in_bio,
					struct readahead_control *rac)
{
	struct bio *bio = *bio_ret;
	const unsigned int blocksize = F2FS_BLKSIZE;
	sector_t block_in_file;
	sector_t last_block;
	sector_t last_block_in_file;
	sector_t block_nr;
	pgoff_t index = folio->index;
	int ret = 0;

	block_in_file = (sector_t)index;
	last_block = block_in_file + nr_pages;
	last_block_in_file = F2FS_BYTES_TO_BLK(f2fs_readpage_limit(inode) +
							blocksize - 1);
	if (last_block > last_block_in_file)
		last_block = last_block_in_file;

	/* just zeroing out page which is beyond EOF */
	if (block_in_file >= last_block)
		goto zero_out;
	/*
	 * Map blocks using the previous result first.
	 */
	if ((map->m_flags & F2FS_MAP_MAPPED) &&
			block_in_file > map->m_lblk &&
			block_in_file < (map->m_lblk + map->m_len))
		goto got_it;

	/*
	 * Then do more f2fs_map_blocks() calls until we are
	 * done with this page.
	 */
	map->m_lblk = block_in_file;
	map->m_len = last_block - block_in_file;

	ret = f2fs_map_blocks(inode, map, F2FS_GET_BLOCK_DEFAULT);
	if (ret)
		goto out;
got_it:
	if ((map->m_flags & F2FS_MAP_MAPPED)) {
		block_nr = map->m_pblk + block_in_file - map->m_lblk;
		folio_set_mappedtodisk(folio);

		if (!f2fs_is_valid_blkaddr(F2FS_I_SB(inode), block_nr,
						DATA_GENERIC_ENHANCE_READ)) {
			ret = -EFSCORRUPTED;
			goto out;
		}
	} else {
zero_out:
		folio_zero_segment(folio, 0, folio_size(folio));
		if (f2fs_need_verity(inode, index) &&
		    !fsverity_verify_folio(folio)) {
			ret = -EIO;
			goto out;
		}
		if (!folio_test_uptodate(folio))
			folio_mark_uptodate(folio);
		folio_unlock(folio);
		goto out;
	}

	/*
	 * This page will go to BIO.  Do we need to send this
	 * BIO off first?
	 */
	if (bio && (!page_is_mergeable(F2FS_I_SB(inode), bio,
				       *last_block_in_bio, block_nr) ||
		    !f2fs_crypt_mergeable_bio(bio, inode, index, NULL))) {
submit_and_realloc:
		f2fs_submit_read_bio(F2FS_I_SB(inode), bio, DATA);
		bio = NULL;
	}
	if (bio == NULL) {
		bio = f2fs_grab_read_bio(inode, block_nr, nr_pages,
				f2fs_ra_op_flags(rac), index,
				false);
		if (IS_ERR(bio)) {
			ret = PTR_ERR(bio);
			bio = NULL;
			goto out;
		}
	}

	/*
	 * If the page is under writeback, we need to wait for
	 * its completion to see the correct decrypted data.
	 */
	f2fs_wait_on_block_writeback(inode, block_nr);

	if (!bio_add_folio(bio, folio, blocksize, 0))
		goto submit_and_realloc;

	inc_page_count(F2FS_I_SB(inode), F2FS_RD_DATA);
	f2fs_update_iostat(F2FS_I_SB(inode), NULL, FS_DATA_READ_IO,
							F2FS_BLKSIZE);
	*last_block_in_bio = block_nr;
out:
	*bio_ret = bio;
	return ret;
}

#ifdef CONFIG_F2FS_FS_COMPRESSION
int f2fs_read_multi_pages(struct compress_ctx *cc, struct bio **bio_ret,
				unsigned nr_pages, sector_t *last_block_in_bio,
				struct readahead_control *rac, bool for_write)
{
	struct dnode_of_data dn;
	struct inode *inode = cc->inode;
	struct f2fs_sb_info *sbi = F2FS_I_SB(inode);
	struct bio *bio = *bio_ret;
	unsigned int start_idx = cc->cluster_idx << cc->log_cluster_size;
	sector_t last_block_in_file;
	const unsigned int blocksize = F2FS_BLKSIZE;
	struct decompress_io_ctx *dic = NULL;
	struct extent_info ei = {};
	bool from_dnode = true;
	int i;
	int ret = 0;

	if (unlikely(f2fs_cp_error(sbi))) {
		ret = -EIO;
		from_dnode = false;
		goto out_put_dnode;
	}

	f2fs_bug_on(sbi, f2fs_cluster_is_empty(cc));

	last_block_in_file = F2FS_BYTES_TO_BLK(f2fs_readpage_limit(inode) +
							blocksize - 1);

	/* get rid of pages beyond EOF */
	for (i = 0; i < cc->cluster_size; i++) {
		struct page *page = cc->rpages[i];
		struct folio *folio;

		if (!page)
			continue;

		folio = page_folio(page);
		if ((sector_t)folio->index >= last_block_in_file) {
			folio_zero_segment(folio, 0, folio_size(folio));
			if (!folio_test_uptodate(folio))
				folio_mark_uptodate(folio);
		} else if (!folio_test_uptodate(folio)) {
			continue;
		}
		folio_unlock(folio);
		if (for_write)
			folio_put(folio);
		cc->rpages[i] = NULL;
		cc->nr_rpages--;
	}

	/* we are done since all pages are beyond EOF */
	if (f2fs_cluster_is_empty(cc))
		goto out;

	if (f2fs_lookup_read_extent_cache(inode, start_idx, &ei))
		from_dnode = false;

	if (!from_dnode)
		goto skip_reading_dnode;

	set_new_dnode(&dn, inode, NULL, NULL, 0);
	ret = f2fs_get_dnode_of_data(&dn, start_idx, LOOKUP_NODE);
	if (ret)
		goto out;

	f2fs_bug_on(sbi, dn.data_blkaddr != COMPRESS_ADDR);

skip_reading_dnode:
	for (i = 1; i < cc->cluster_size; i++) {
		block_t blkaddr;

		blkaddr = from_dnode ? data_blkaddr(dn.inode, dn.node_folio,
					dn.ofs_in_node + i) :
					ei.blk + i - 1;

		if (!__is_valid_data_blkaddr(blkaddr))
			break;

		if (!f2fs_is_valid_blkaddr(sbi, blkaddr, DATA_GENERIC)) {
			ret = -EFAULT;
			goto out_put_dnode;
		}
		cc->nr_cpages++;

		if (!from_dnode && i >= ei.c_len)
			break;
	}

	/* nothing to decompress */
	if (cc->nr_cpages == 0) {
		ret = 0;
		goto out_put_dnode;
	}

	dic = f2fs_alloc_dic(cc);
	if (IS_ERR(dic)) {
		ret = PTR_ERR(dic);
		goto out_put_dnode;
	}

	for (i = 0; i < cc->nr_cpages; i++) {
		struct folio *folio = page_folio(dic->cpages[i]);
		block_t blkaddr;
		struct bio_post_read_ctx *ctx;

		blkaddr = from_dnode ? data_blkaddr(dn.inode, dn.node_folio,
					dn.ofs_in_node + i + 1) :
					ei.blk + i;

		f2fs_wait_on_block_writeback(inode, blkaddr);

		if (f2fs_load_compressed_folio(sbi, folio, blkaddr)) {
			if (atomic_dec_and_test(&dic->remaining_pages)) {
				f2fs_decompress_cluster(dic, true);
				break;
			}
			continue;
		}

		if (bio && (!page_is_mergeable(sbi, bio,
					*last_block_in_bio, blkaddr) ||
		    !f2fs_crypt_mergeable_bio(bio, inode, folio->index, NULL))) {
submit_and_realloc:
			f2fs_submit_read_bio(sbi, bio, DATA);
			bio = NULL;
		}

		if (!bio) {
			bio = f2fs_grab_read_bio(inode, blkaddr, nr_pages,
					f2fs_ra_op_flags(rac),
					folio->index, for_write);
			if (IS_ERR(bio)) {
				ret = PTR_ERR(bio);
				f2fs_decompress_end_io(dic, ret, true);
				f2fs_put_dnode(&dn);
				*bio_ret = NULL;
				return ret;
			}
		}

		if (!bio_add_folio(bio, folio, blocksize, 0))
			goto submit_and_realloc;

		ctx = get_post_read_ctx(bio);
		ctx->enabled_steps |= STEP_DECOMPRESS;
		refcount_inc(&dic->refcnt);

		inc_page_count(sbi, F2FS_RD_DATA);
		f2fs_update_iostat(sbi, inode, FS_DATA_READ_IO, F2FS_BLKSIZE);
		*last_block_in_bio = blkaddr;
	}

	if (from_dnode)
		f2fs_put_dnode(&dn);

	*bio_ret = bio;
	return 0;

out_put_dnode:
	if (from_dnode)
		f2fs_put_dnode(&dn);
out:
	for (i = 0; i < cc->cluster_size; i++) {
		if (cc->rpages[i]) {
			ClearPageUptodate(cc->rpages[i]);
			unlock_page(cc->rpages[i]);
		}
	}
	*bio_ret = bio;
	return ret;
}
#endif

/*
 * This function was originally taken from fs/mpage.c, and customized for f2fs.
 * Major change was from block_size == page_size in f2fs by default.
 */
static int f2fs_mpage_readpages(struct inode *inode,
		struct readahead_control *rac, struct folio *folio)
{
	struct bio *bio = NULL;
	sector_t last_block_in_bio = 0;
	struct f2fs_map_blocks map;
#ifdef CONFIG_F2FS_FS_COMPRESSION
	struct compress_ctx cc = {
		.inode = inode,
		.log_cluster_size = F2FS_I(inode)->i_log_cluster_size,
		.cluster_size = F2FS_I(inode)->i_cluster_size,
		.cluster_idx = NULL_CLUSTER,
		.rpages = NULL,
		.cpages = NULL,
		.nr_rpages = 0,
		.nr_cpages = 0,
	};
	pgoff_t nc_cluster_idx = NULL_CLUSTER;
	pgoff_t index;
#endif
	unsigned nr_pages = rac ? readahead_count(rac) : 1;
	unsigned max_nr_pages = nr_pages;
	int ret = 0;

	map.m_pblk = 0;
	map.m_lblk = 0;
	map.m_len = 0;
	map.m_flags = 0;
	map.m_next_pgofs = NULL;
	map.m_next_extent = NULL;
	map.m_seg_type = NO_CHECK_TYPE;
	map.m_may_create = false;

	for (; nr_pages; nr_pages--) {
		if (rac) {
			folio = readahead_folio(rac);
			prefetchw(&folio->flags);
		}

#ifdef CONFIG_F2FS_FS_COMPRESSION
		index = folio->index;

		if (!f2fs_compressed_file(inode))
			goto read_single_page;

		/* there are remained compressed pages, submit them */
		if (!f2fs_cluster_can_merge_page(&cc, index)) {
			ret = f2fs_read_multi_pages(&cc, &bio,
						max_nr_pages,
						&last_block_in_bio,
						rac, false);
			f2fs_destroy_compress_ctx(&cc, false);
			if (ret)
				goto set_error_page;
		}
		if (cc.cluster_idx == NULL_CLUSTER) {
			if (nc_cluster_idx == index >> cc.log_cluster_size)
				goto read_single_page;

			ret = f2fs_is_compressed_cluster(inode, index);
			if (ret < 0)
				goto set_error_page;
			else if (!ret) {
				nc_cluster_idx =
					index >> cc.log_cluster_size;
				goto read_single_page;
			}

			nc_cluster_idx = NULL_CLUSTER;
		}
		ret = f2fs_init_compress_ctx(&cc);
		if (ret)
			goto set_error_page;

		f2fs_compress_ctx_add_page(&cc, folio);

		goto next_page;
read_single_page:
#endif

		ret = f2fs_read_single_page(inode, folio, max_nr_pages, &map,
					&bio, &last_block_in_bio, rac);
		if (ret) {
#ifdef CONFIG_F2FS_FS_COMPRESSION
set_error_page:
#endif
			folio_zero_segment(folio, 0, folio_size(folio));
			folio_unlock(folio);
		}
#ifdef CONFIG_F2FS_FS_COMPRESSION
next_page:
#endif

#ifdef CONFIG_F2FS_FS_COMPRESSION
		if (f2fs_compressed_file(inode)) {
			/* last page */
			if (nr_pages == 1 && !f2fs_cluster_is_empty(&cc)) {
				ret = f2fs_read_multi_pages(&cc, &bio,
							max_nr_pages,
							&last_block_in_bio,
							rac, false);
				f2fs_destroy_compress_ctx(&cc, false);
			}
		}
#endif
	}
	if (bio)
		f2fs_submit_read_bio(F2FS_I_SB(inode), bio, DATA);
	return ret;
}

static int f2fs_read_data_folio(struct file *file, struct folio *folio)
{
	struct inode *inode = folio->mapping->host;
	int ret = -EAGAIN;

	trace_f2fs_readpage(folio, DATA);

	if (!f2fs_is_compress_backend_ready(inode)) {
		folio_unlock(folio);
		return -EOPNOTSUPP;
	}

	/* If the file has inline data, try to read it directly */
	if (f2fs_has_inline_data(inode))
		ret = f2fs_read_inline_data(inode, folio);
	if (ret == -EAGAIN)
		ret = f2fs_mpage_readpages(inode, NULL, folio);
	return ret;
}

static void f2fs_readahead(struct readahead_control *rac)
{
	struct inode *inode = rac->mapping->host;

	trace_f2fs_readpages(inode, readahead_index(rac), readahead_count(rac));

	if (!f2fs_is_compress_backend_ready(inode))
		return;

	/* If the file has inline data, skip readahead */
	if (f2fs_has_inline_data(inode))
		return;

	f2fs_mpage_readpages(inode, rac, NULL);
}

int f2fs_encrypt_one_page(struct f2fs_io_info *fio)
{
	struct inode *inode = fio_inode(fio);
	struct folio *mfolio;
	struct page *page;
	gfp_t gfp_flags = GFP_NOFS;

	if (!f2fs_encrypted_file(inode))
		return 0;

	page = fio->compressed_page ? fio->compressed_page : fio->page;

	if (fscrypt_inode_uses_inline_crypto(inode))
		return 0;

retry_encrypt:
	fio->encrypted_page = fscrypt_encrypt_pagecache_blocks(page_folio(page),
					PAGE_SIZE, 0, gfp_flags);
	if (IS_ERR(fio->encrypted_page)) {
		/* flush pending IOs and wait for a while in the ENOMEM case */
		if (PTR_ERR(fio->encrypted_page) == -ENOMEM) {
			f2fs_flush_merged_writes(fio->sbi);
			memalloc_retry_wait(GFP_NOFS);
			gfp_flags |= __GFP_NOFAIL;
			goto retry_encrypt;
		}
		return PTR_ERR(fio->encrypted_page);
	}

	mfolio = filemap_lock_folio(META_MAPPING(fio->sbi), fio->old_blkaddr);
	if (!IS_ERR(mfolio)) {
		if (folio_test_uptodate(mfolio))
			memcpy(folio_address(mfolio),
				page_address(fio->encrypted_page), PAGE_SIZE);
		f2fs_folio_put(mfolio, true);
	}
	return 0;
}

static inline bool check_inplace_update_policy(struct inode *inode,
				struct f2fs_io_info *fio)
{
	struct f2fs_sb_info *sbi = F2FS_I_SB(inode);

	if (IS_F2FS_IPU_HONOR_OPU_WRITE(sbi) &&
	    is_inode_flag_set(inode, FI_OPU_WRITE))
		return false;
	if (IS_F2FS_IPU_FORCE(sbi))
		return true;
	if (IS_F2FS_IPU_SSR(sbi) && f2fs_need_SSR(sbi))
		return true;
	if (IS_F2FS_IPU_UTIL(sbi) && utilization(sbi) > SM_I(sbi)->min_ipu_util)
		return true;
	if (IS_F2FS_IPU_SSR_UTIL(sbi) && f2fs_need_SSR(sbi) &&
	    utilization(sbi) > SM_I(sbi)->min_ipu_util)
		return true;

	/*
	 * IPU for rewrite async pages
	 */
	if (IS_F2FS_IPU_ASYNC(sbi) && fio && fio->op == REQ_OP_WRITE &&
	    !(fio->op_flags & REQ_SYNC) && !IS_ENCRYPTED(inode))
		return true;

	/* this is only set during fdatasync */
	if (IS_F2FS_IPU_FSYNC(sbi) && is_inode_flag_set(inode, FI_NEED_IPU))
		return true;

	if (unlikely(fio && is_sbi_flag_set(sbi, SBI_CP_DISABLED) &&
			!f2fs_is_checkpointed_data(sbi, fio->old_blkaddr)))
		return true;

	return false;
}

bool f2fs_should_update_inplace(struct inode *inode, struct f2fs_io_info *fio)
{
	/* swap file is migrating in aligned write mode */
	if (is_inode_flag_set(inode, FI_ALIGNED_WRITE))
		return false;

	if (f2fs_is_pinned_file(inode))
		return true;

	/* if this is cold file, we should overwrite to avoid fragmentation */
	if (file_is_cold(inode) && !is_inode_flag_set(inode, FI_OPU_WRITE))
		return true;

	return check_inplace_update_policy(inode, fio);
}

bool f2fs_should_update_outplace(struct inode *inode, struct f2fs_io_info *fio)
{
	struct f2fs_sb_info *sbi = F2FS_I_SB(inode);

	/* The below cases were checked when setting it. */
	if (f2fs_is_pinned_file(inode))
		return false;
	if (fio && is_sbi_flag_set(sbi, SBI_NEED_FSCK))
		return true;
	if (f2fs_lfs_mode(sbi))
		return true;
	if (S_ISDIR(inode->i_mode))
		return true;
	if (IS_NOQUOTA(inode))
		return true;
	if (f2fs_used_in_atomic_write(inode))
		return true;
	/* rewrite low ratio compress data w/ OPU mode to avoid fragmentation */
	if (f2fs_compressed_file(inode) &&
		F2FS_OPTION(sbi).compress_mode == COMPR_MODE_USER &&
		is_inode_flag_set(inode, FI_ENABLE_COMPRESS))
		return true;

	/* swap file is migrating in aligned write mode */
	if (is_inode_flag_set(inode, FI_ALIGNED_WRITE))
		return true;

	if (is_inode_flag_set(inode, FI_OPU_WRITE))
		return true;

	if (fio) {
		if (page_private_gcing(fio->page))
			return true;
		if (unlikely(is_sbi_flag_set(sbi, SBI_CP_DISABLED) &&
			f2fs_is_checkpointed_data(sbi, fio->old_blkaddr)))
			return true;
	}
	return false;
}

static inline bool need_inplace_update(struct f2fs_io_info *fio)
{
	struct inode *inode = fio_inode(fio);

	if (f2fs_should_update_outplace(inode, fio))
		return false;

	return f2fs_should_update_inplace(inode, fio);
}

int f2fs_do_write_data_page(struct f2fs_io_info *fio)
{
	struct folio *folio = page_folio(fio->page);
	struct inode *inode = folio->mapping->host;
	struct dnode_of_data dn;
	struct node_info ni;
	bool ipu_force = false;
	bool atomic_commit;
	int err = 0;

	/* Use COW inode to make dnode_of_data for atomic write */
	atomic_commit = f2fs_is_atomic_file(inode) &&
				page_private_atomic(folio_page(folio, 0));
	if (atomic_commit)
		set_new_dnode(&dn, F2FS_I(inode)->cow_inode, NULL, NULL, 0);
	else
		set_new_dnode(&dn, inode, NULL, NULL, 0);

	if (need_inplace_update(fio) &&
	    f2fs_lookup_read_extent_cache_block(inode, folio->index,
						&fio->old_blkaddr)) {
		if (!f2fs_is_valid_blkaddr(fio->sbi, fio->old_blkaddr,
						DATA_GENERIC_ENHANCE))
			return -EFSCORRUPTED;

		ipu_force = true;
		fio->need_lock = LOCK_DONE;
		goto got_it;
	}

	/* Deadlock due to between page->lock and f2fs_lock_op */
	if (fio->need_lock == LOCK_REQ && !f2fs_trylock_op(fio->sbi))
		return -EAGAIN;

	err = f2fs_get_dnode_of_data(&dn, folio->index, LOOKUP_NODE);
	if (err)
		goto out;

	fio->old_blkaddr = dn.data_blkaddr;

	/* This page is already truncated */
	if (fio->old_blkaddr == NULL_ADDR) {
		folio_clear_uptodate(folio);
		clear_page_private_gcing(folio_page(folio, 0));
		goto out_writepage;
	}
got_it:
	if (__is_valid_data_blkaddr(fio->old_blkaddr) &&
		!f2fs_is_valid_blkaddr(fio->sbi, fio->old_blkaddr,
						DATA_GENERIC_ENHANCE)) {
		err = -EFSCORRUPTED;
		goto out_writepage;
	}

	/* wait for GCed page writeback via META_MAPPING */
	if (fio->meta_gc)
		f2fs_wait_on_block_writeback(inode, fio->old_blkaddr);

	/*
	 * If current allocation needs SSR,
	 * it had better in-place writes for updated data.
	 */
	if (ipu_force ||
		(__is_valid_data_blkaddr(fio->old_blkaddr) &&
					need_inplace_update(fio))) {
		err = f2fs_encrypt_one_page(fio);
		if (err)
			goto out_writepage;

		folio_start_writeback(folio);
		f2fs_put_dnode(&dn);
		if (fio->need_lock == LOCK_REQ)
			f2fs_unlock_op(fio->sbi);
		err = f2fs_inplace_write_data(fio);
		if (err) {
			if (fscrypt_inode_uses_fs_layer_crypto(inode))
				fscrypt_finalize_bounce_page(&fio->encrypted_page);
			folio_end_writeback(folio);
		} else {
			set_inode_flag(inode, FI_UPDATE_WRITE);
		}
		trace_f2fs_do_write_data_page(folio, IPU);
		return err;
	}

	if (fio->need_lock == LOCK_RETRY) {
		if (!f2fs_trylock_op(fio->sbi)) {
			err = -EAGAIN;
			goto out_writepage;
		}
		fio->need_lock = LOCK_REQ;
	}

	err = f2fs_get_node_info(fio->sbi, dn.nid, &ni, false);
	if (err)
		goto out_writepage;

	fio->version = ni.version;

	err = f2fs_encrypt_one_page(fio);
	if (err)
		goto out_writepage;

	folio_start_writeback(folio);

	if (fio->compr_blocks && fio->old_blkaddr == COMPRESS_ADDR)
		f2fs_i_compr_blocks_update(inode, fio->compr_blocks - 1, false);

	/* LFS mode write path */
	f2fs_outplace_write_data(&dn, fio);
	trace_f2fs_do_write_data_page(folio, OPU);
	set_inode_flag(inode, FI_APPEND_WRITE);
	if (atomic_commit)
		clear_page_private_atomic(folio_page(folio, 0));
out_writepage:
	f2fs_put_dnode(&dn);
out:
	if (fio->need_lock == LOCK_REQ)
		f2fs_unlock_op(fio->sbi);
	return err;
}

int f2fs_write_single_data_page(struct folio *folio, int *submitted,
				struct bio **bio,
				sector_t *last_block,
				struct writeback_control *wbc,
				enum iostat_type io_type,
				int compr_blocks,
				bool allow_balance)
{
	struct inode *inode = folio->mapping->host;
	struct page *page = folio_page(folio, 0);
	struct f2fs_sb_info *sbi = F2FS_I_SB(inode);
	loff_t i_size = i_size_read(inode);
	const pgoff_t end_index = ((unsigned long long)i_size)
							>> PAGE_SHIFT;
	loff_t psize = (loff_t)(folio->index + 1) << PAGE_SHIFT;
	unsigned offset = 0;
	bool need_balance_fs = false;
	bool quota_inode = IS_NOQUOTA(inode);
	int err = 0;
	struct f2fs_io_info fio = {
		.sbi = sbi,
		.ino = inode->i_ino,
		.type = DATA,
		.op = REQ_OP_WRITE,
		.op_flags = wbc_to_write_flags(wbc),
		.old_blkaddr = NULL_ADDR,
		.page = page,
		.encrypted_page = NULL,
		.submitted = 0,
		.compr_blocks = compr_blocks,
		.need_lock = compr_blocks ? LOCK_DONE : LOCK_RETRY,
		.meta_gc = f2fs_meta_inode_gc_required(inode) ? 1 : 0,
		.io_type = io_type,
		.io_wbc = wbc,
		.bio = bio,
		.last_block = last_block,
	};

	trace_f2fs_writepage(folio, DATA);

	/* we should bypass data pages to proceed the kworker jobs */
	if (unlikely(f2fs_cp_error(sbi))) {
		mapping_set_error(folio->mapping, -EIO);
		/*
		 * don't drop any dirty dentry pages for keeping lastest
		 * directory structure.
		 */
		if (S_ISDIR(inode->i_mode) &&
				!is_sbi_flag_set(sbi, SBI_IS_CLOSE))
			goto redirty_out;

		/* keep data pages in remount-ro mode */
		if (F2FS_OPTION(sbi).errors == MOUNT_ERRORS_READONLY)
			goto redirty_out;
		goto out;
	}

	if (unlikely(is_sbi_flag_set(sbi, SBI_POR_DOING)))
		goto redirty_out;

	if (folio->index < end_index ||
			f2fs_verity_in_progress(inode) ||
			compr_blocks)
		goto write;

	/*
	 * If the offset is out-of-range of file size,
	 * this page does not have to be written to disk.
	 */
	offset = i_size & (PAGE_SIZE - 1);
	if ((folio->index >= end_index + 1) || !offset)
		goto out;

	folio_zero_segment(folio, offset, folio_size(folio));
write:
	/* Dentry/quota blocks are controlled by checkpoint */
	if (S_ISDIR(inode->i_mode) || quota_inode) {
		/*
		 * We need to wait for node_write to avoid block allocation during
		 * checkpoint. This can only happen to quota writes which can cause
		 * the below discard race condition.
		 */
		if (quota_inode)
			f2fs_down_read(&sbi->node_write);

		fio.need_lock = LOCK_DONE;
		err = f2fs_do_write_data_page(&fio);

		if (quota_inode)
			f2fs_up_read(&sbi->node_write);

		goto done;
	}

	need_balance_fs = true;
	err = -EAGAIN;
	if (f2fs_has_inline_data(inode)) {
		err = f2fs_write_inline_data(inode, folio);
		if (!err)
			goto out;
	}

	if (err == -EAGAIN) {
		err = f2fs_do_write_data_page(&fio);
		if (err == -EAGAIN) {
			f2fs_bug_on(sbi, compr_blocks);
			fio.need_lock = LOCK_REQ;
			err = f2fs_do_write_data_page(&fio);
		}
	}

	if (err) {
		file_set_keep_isize(inode);
	} else {
		spin_lock(&F2FS_I(inode)->i_size_lock);
		if (F2FS_I(inode)->last_disk_size < psize)
			F2FS_I(inode)->last_disk_size = psize;
		spin_unlock(&F2FS_I(inode)->i_size_lock);
	}

done:
	if (err && err != -ENOENT)
		goto redirty_out;

out:
	inode_dec_dirty_pages(inode);
	if (err) {
		folio_clear_uptodate(folio);
		clear_page_private_gcing(page);
	}
	folio_unlock(folio);
	if (!S_ISDIR(inode->i_mode) && !IS_NOQUOTA(inode) &&
			!F2FS_I(inode)->wb_task && allow_balance)
		f2fs_balance_fs(sbi, need_balance_fs);

	if (unlikely(f2fs_cp_error(sbi))) {
		f2fs_submit_merged_write(sbi, DATA);
		if (bio && *bio)
			f2fs_submit_merged_ipu_write(sbi, bio, NULL);
		submitted = NULL;
	}

	if (submitted)
		*submitted = fio.submitted;

	return 0;

redirty_out:
	folio_redirty_for_writepage(wbc, folio);
	/*
	 * pageout() in MM translates EAGAIN, so calls handle_write_error()
	 * -> mapping_set_error() -> set_bit(AS_EIO, ...).
	 * file_write_and_wait_range() will see EIO error, which is critical
	 * to return value of fsync() followed by atomic_write failure to user.
	 */
	folio_unlock(folio);
	if (!err)
		return 1;
	return err;
}

/*
 * This function was copied from write_cache_pages from mm/page-writeback.c.
 * The major change is making write step of cold data page separately from
 * warm/hot data page.
 */
static int f2fs_write_cache_pages(struct address_space *mapping,
					struct writeback_control *wbc,
					enum iostat_type io_type)
{
	int ret = 0;
	int done = 0, retry = 0;
	struct page *pages_local[F2FS_ONSTACK_PAGES];
	struct page **pages = pages_local;
	struct folio_batch fbatch;
	struct f2fs_sb_info *sbi = F2FS_M_SB(mapping);
	struct bio *bio = NULL;
	sector_t last_block;
#ifdef CONFIG_F2FS_FS_COMPRESSION
	struct inode *inode = mapping->host;
	struct compress_ctx cc = {
		.inode = inode,
		.log_cluster_size = F2FS_I(inode)->i_log_cluster_size,
		.cluster_size = F2FS_I(inode)->i_cluster_size,
		.cluster_idx = NULL_CLUSTER,
		.rpages = NULL,
		.nr_rpages = 0,
		.cpages = NULL,
		.valid_nr_cpages = 0,
		.rbuf = NULL,
		.cbuf = NULL,
		.rlen = PAGE_SIZE * F2FS_I(inode)->i_cluster_size,
		.private = NULL,
	};
#endif
	int nr_folios, p, idx;
	int nr_pages;
	unsigned int max_pages = F2FS_ONSTACK_PAGES;
	pgoff_t index;
	pgoff_t end;		/* Inclusive */
	pgoff_t done_index;
	int range_whole = 0;
	xa_mark_t tag;
	int nwritten = 0;
	int submitted = 0;
	int i;

#ifdef CONFIG_F2FS_FS_COMPRESSION
	if (f2fs_compressed_file(inode) &&
		1 << cc.log_cluster_size > F2FS_ONSTACK_PAGES) {
		pages = f2fs_kzalloc(sbi, sizeof(struct page *) <<
				cc.log_cluster_size, GFP_NOFS | __GFP_NOFAIL);
		max_pages = 1 << cc.log_cluster_size;
	}
#endif

	folio_batch_init(&fbatch);

	if (get_dirty_pages(mapping->host) <=
				SM_I(F2FS_M_SB(mapping))->min_hot_blocks)
		set_inode_flag(mapping->host, FI_HOT_DATA);
	else
		clear_inode_flag(mapping->host, FI_HOT_DATA);

	if (wbc->range_cyclic) {
		index = mapping->writeback_index; /* prev offset */
		end = -1;
	} else {
		index = wbc->range_start >> PAGE_SHIFT;
		end = wbc->range_end >> PAGE_SHIFT;
		if (wbc->range_start == 0 && wbc->range_end == LLONG_MAX)
			range_whole = 1;
	}
	if (wbc->sync_mode == WB_SYNC_ALL || wbc->tagged_writepages)
		tag = PAGECACHE_TAG_TOWRITE;
	else
		tag = PAGECACHE_TAG_DIRTY;
retry:
	retry = 0;
	if (wbc->sync_mode == WB_SYNC_ALL || wbc->tagged_writepages)
		tag_pages_for_writeback(mapping, index, end);
	done_index = index;
	while (!done && !retry && (index <= end)) {
		nr_pages = 0;
again:
		nr_folios = filemap_get_folios_tag(mapping, &index, end,
				tag, &fbatch);
		if (nr_folios == 0) {
			if (nr_pages)
				goto write;
			break;
		}

		for (i = 0; i < nr_folios; i++) {
			struct folio *folio = fbatch.folios[i];

			idx = 0;
			p = folio_nr_pages(folio);
add_more:
			pages[nr_pages] = folio_page(folio, idx);
			folio_get(folio);
			if (++nr_pages == max_pages) {
				index = folio->index + idx + 1;
				folio_batch_release(&fbatch);
				goto write;
			}
			if (++idx < p)
				goto add_more;
		}
		folio_batch_release(&fbatch);
		goto again;
write:
		for (i = 0; i < nr_pages; i++) {
			struct page *page = pages[i];
			struct folio *folio = page_folio(page);
			bool need_readd;
readd:
			need_readd = false;
#ifdef CONFIG_F2FS_FS_COMPRESSION
			if (f2fs_compressed_file(inode)) {
				void *fsdata = NULL;
				struct page *pagep;
				int ret2;

				ret = f2fs_init_compress_ctx(&cc);
				if (ret) {
					done = 1;
					break;
				}

				if (!f2fs_cluster_can_merge_page(&cc,
								folio->index)) {
					ret = f2fs_write_multi_pages(&cc,
						&submitted, wbc, io_type);
					if (!ret)
						need_readd = true;
					goto result;
				}

				if (unlikely(f2fs_cp_error(sbi)))
					goto lock_folio;

				if (!f2fs_cluster_is_empty(&cc))
					goto lock_folio;

				if (f2fs_all_cluster_page_ready(&cc,
					pages, i, nr_pages, true))
					goto lock_folio;

				ret2 = f2fs_prepare_compress_overwrite(
							inode, &pagep,
							folio->index, &fsdata);
				if (ret2 < 0) {
					ret = ret2;
					done = 1;
					break;
				} else if (ret2 &&
					(!f2fs_compress_write_end(inode,
						fsdata, folio->index, 1) ||
					 !f2fs_all_cluster_page_ready(&cc,
						pages, i, nr_pages,
						false))) {
					retry = 1;
					break;
				}
			}
#endif
			/* give a priority to WB_SYNC threads */
			if (atomic_read(&sbi->wb_sync_req[DATA]) &&
					wbc->sync_mode == WB_SYNC_NONE) {
				done = 1;
				break;
			}
#ifdef CONFIG_F2FS_FS_COMPRESSION
lock_folio:
#endif
			done_index = folio->index;
retry_write:
			folio_lock(folio);

			if (unlikely(folio->mapping != mapping)) {
continue_unlock:
				folio_unlock(folio);
				continue;
			}

			if (!folio_test_dirty(folio)) {
				/* someone wrote it for us */
				goto continue_unlock;
			}

			if (folio_test_writeback(folio)) {
				if (wbc->sync_mode == WB_SYNC_NONE)
					goto continue_unlock;
				f2fs_folio_wait_writeback(folio, DATA, true, true);
			}

			if (!folio_clear_dirty_for_io(folio))
				goto continue_unlock;

#ifdef CONFIG_F2FS_FS_COMPRESSION
			if (f2fs_compressed_file(inode)) {
				folio_get(folio);
				f2fs_compress_ctx_add_page(&cc, folio);
				continue;
			}
#endif
			submitted = 0;
			ret = f2fs_write_single_data_page(folio,
					&submitted, &bio, &last_block,
					wbc, io_type, 0, true);
#ifdef CONFIG_F2FS_FS_COMPRESSION
result:
#endif
			nwritten += submitted;
			wbc->nr_to_write -= submitted;

			if (unlikely(ret)) {
				/*
				 * keep nr_to_write, since vfs uses this to
				 * get # of written pages.
				 */
				if (ret == 1) {
					ret = 0;
					goto next;
				} else if (ret == -EAGAIN) {
					ret = 0;
					if (wbc->sync_mode == WB_SYNC_ALL) {
						f2fs_io_schedule_timeout(
							DEFAULT_IO_TIMEOUT);
						goto retry_write;
					}
					goto next;
				}
				done_index = folio_next_index(folio);
				done = 1;
				break;
			}

			if (wbc->nr_to_write <= 0 &&
					wbc->sync_mode == WB_SYNC_NONE) {
				done = 1;
				break;
			}
next:
			if (need_readd)
				goto readd;
		}
		release_pages(pages, nr_pages);
		cond_resched();
	}
#ifdef CONFIG_F2FS_FS_COMPRESSION
	/* flush remained pages in compress cluster */
	if (f2fs_compressed_file(inode) && !f2fs_cluster_is_empty(&cc)) {
		ret = f2fs_write_multi_pages(&cc, &submitted, wbc, io_type);
		nwritten += submitted;
		wbc->nr_to_write -= submitted;
		if (ret) {
			done = 1;
			retry = 0;
		}
	}
	if (f2fs_compressed_file(inode))
		f2fs_destroy_compress_ctx(&cc, false);
#endif
	if (retry) {
		index = 0;
		end = -1;
		goto retry;
	}
	if (wbc->range_cyclic && !done)
		done_index = 0;
	if (wbc->range_cyclic || (range_whole && wbc->nr_to_write > 0))
		mapping->writeback_index = done_index;

	if (nwritten)
		f2fs_submit_merged_write_cond(F2FS_M_SB(mapping), mapping->host,
								NULL, 0, DATA);
	/* submit cached bio of IPU write */
	if (bio)
		f2fs_submit_merged_ipu_write(sbi, &bio, NULL);

#ifdef CONFIG_F2FS_FS_COMPRESSION
	if (pages != pages_local)
		kfree(pages);
#endif

	return ret;
}

static inline bool __should_serialize_io(struct inode *inode,
					struct writeback_control *wbc)
{
	/* to avoid deadlock in path of data flush */
	if (F2FS_I(inode)->wb_task)
		return false;

	if (!S_ISREG(inode->i_mode))
		return false;
	if (IS_NOQUOTA(inode))
		return false;

	if (f2fs_need_compress_data(inode))
		return true;
	if (wbc->sync_mode != WB_SYNC_ALL)
		return true;
	if (get_dirty_pages(inode) >= SM_I(F2FS_I_SB(inode))->min_seq_blocks)
		return true;
	return false;
}

static int __f2fs_write_data_pages(struct address_space *mapping,
						struct writeback_control *wbc,
						enum iostat_type io_type)
{
	struct inode *inode = mapping->host;
	struct f2fs_sb_info *sbi = F2FS_I_SB(inode);
	struct blk_plug plug;
	int ret;
	bool locked = false;

	/* skip writing if there is no dirty page in this inode */
	if (!get_dirty_pages(inode) && wbc->sync_mode == WB_SYNC_NONE)
		return 0;

	/* during POR, we don't need to trigger writepage at all. */
	if (unlikely(is_sbi_flag_set(sbi, SBI_POR_DOING)))
		goto skip_write;

	if ((S_ISDIR(inode->i_mode) || IS_NOQUOTA(inode)) &&
			wbc->sync_mode == WB_SYNC_NONE &&
			get_dirty_pages(inode) < nr_pages_to_skip(sbi, DATA) &&
			f2fs_available_free_memory(sbi, DIRTY_DENTS))
		goto skip_write;

	/* skip writing in file defragment preparing stage */
	if (is_inode_flag_set(inode, FI_SKIP_WRITES))
		goto skip_write;

	trace_f2fs_writepages(mapping->host, wbc, DATA);

	/* to avoid spliting IOs due to mixed WB_SYNC_ALL and WB_SYNC_NONE */
	if (wbc->sync_mode == WB_SYNC_ALL)
		atomic_inc(&sbi->wb_sync_req[DATA]);
	else if (atomic_read(&sbi->wb_sync_req[DATA])) {
		/* to avoid potential deadlock */
		if (current->plug)
			blk_finish_plug(current->plug);
		goto skip_write;
	}

	if (__should_serialize_io(inode, wbc)) {
		mutex_lock(&sbi->writepages);
		locked = true;
	}

	blk_start_plug(&plug);
	ret = f2fs_write_cache_pages(mapping, wbc, io_type);
	blk_finish_plug(&plug);

	if (locked)
		mutex_unlock(&sbi->writepages);

	if (wbc->sync_mode == WB_SYNC_ALL)
		atomic_dec(&sbi->wb_sync_req[DATA]);
	/*
	 * if some pages were truncated, we cannot guarantee its mapping->host
	 * to detect pending bios.
	 */

	f2fs_remove_dirty_inode(inode);
	return ret;

skip_write:
	wbc->pages_skipped += get_dirty_pages(inode);
	trace_f2fs_writepages(mapping->host, wbc, DATA);
	return 0;
}

static int f2fs_write_data_pages(struct address_space *mapping,
			    struct writeback_control *wbc)
{
	struct inode *inode = mapping->host;

	return __f2fs_write_data_pages(mapping, wbc,
			F2FS_I(inode)->cp_task == current ?
			FS_CP_DATA_IO : FS_DATA_IO);
}

void f2fs_write_failed(struct inode *inode, loff_t to)
{
	loff_t i_size = i_size_read(inode);

	if (IS_NOQUOTA(inode))
		return;

	/* In the fs-verity case, f2fs_end_enable_verity() does the truncate */
	if (to > i_size && !f2fs_verity_in_progress(inode)) {
		f2fs_down_write(&F2FS_I(inode)->i_gc_rwsem[WRITE]);
		filemap_invalidate_lock(inode->i_mapping);

		truncate_pagecache(inode, i_size);
		f2fs_truncate_blocks(inode, i_size, true);

		filemap_invalidate_unlock(inode->i_mapping);
		f2fs_up_write(&F2FS_I(inode)->i_gc_rwsem[WRITE]);
	}
}

static int prepare_write_begin(struct f2fs_sb_info *sbi,
			struct folio *folio, loff_t pos, unsigned int len,
			block_t *blk_addr, bool *node_changed)
{
	struct inode *inode = folio->mapping->host;
	pgoff_t index = folio->index;
	struct dnode_of_data dn;
	struct folio *ifolio;
	bool locked = false;
	int flag = F2FS_GET_BLOCK_PRE_AIO;
	int err = 0;

	/*
	 * If a whole page is being written and we already preallocated all the
	 * blocks, then there is no need to get a block address now.
	 */
	if (len == PAGE_SIZE && is_inode_flag_set(inode, FI_PREALLOCATED_ALL))
		return 0;

	/* f2fs_lock_op avoids race between write CP and convert_inline_page */
	if (f2fs_has_inline_data(inode)) {
		if (pos + len > MAX_INLINE_DATA(inode))
			flag = F2FS_GET_BLOCK_DEFAULT;
		f2fs_map_lock(sbi, flag);
		locked = true;
	} else if ((pos & PAGE_MASK) >= i_size_read(inode)) {
		f2fs_map_lock(sbi, flag);
		locked = true;
	}

restart:
	/* check inline_data */
	ifolio = f2fs_get_inode_folio(sbi, inode->i_ino);
	if (IS_ERR(ifolio)) {
		err = PTR_ERR(ifolio);
		goto unlock_out;
	}

	set_new_dnode(&dn, inode, ifolio, ifolio, 0);

	if (f2fs_has_inline_data(inode)) {
		if (pos + len <= MAX_INLINE_DATA(inode)) {
			f2fs_do_read_inline_data(folio, ifolio);
			set_inode_flag(inode, FI_DATA_EXIST);
			if (inode->i_nlink)
				set_page_private_inline(&ifolio->page);
			goto out;
		}
		err = f2fs_convert_inline_folio(&dn, folio);
		if (err || dn.data_blkaddr != NULL_ADDR)
			goto out;
	}

	if (!f2fs_lookup_read_extent_cache_block(inode, index,
						 &dn.data_blkaddr)) {
		if (IS_DEVICE_ALIASING(inode)) {
			err = -ENODATA;
			goto out;
		}

		if (locked) {
			err = f2fs_reserve_block(&dn, index);
			goto out;
		}

		/* hole case */
		err = f2fs_get_dnode_of_data(&dn, index, LOOKUP_NODE);
		if (!err && dn.data_blkaddr != NULL_ADDR)
			goto out;
		f2fs_put_dnode(&dn);
		f2fs_map_lock(sbi, F2FS_GET_BLOCK_PRE_AIO);
		WARN_ON(flag != F2FS_GET_BLOCK_PRE_AIO);
		locked = true;
		goto restart;
	}
out:
	if (!err) {
		/* convert_inline_page can make node_changed */
		*blk_addr = dn.data_blkaddr;
		*node_changed = dn.node_changed;
	}
	f2fs_put_dnode(&dn);
unlock_out:
	if (locked)
		f2fs_map_unlock(sbi, flag);
	return err;
}

static int __find_data_block(struct inode *inode, pgoff_t index,
				block_t *blk_addr)
{
	struct dnode_of_data dn;
	struct folio *ifolio;
	int err = 0;

	ifolio = f2fs_get_inode_folio(F2FS_I_SB(inode), inode->i_ino);
	if (IS_ERR(ifolio))
		return PTR_ERR(ifolio);

	set_new_dnode(&dn, inode, ifolio, ifolio, 0);

	if (!f2fs_lookup_read_extent_cache_block(inode, index,
						 &dn.data_blkaddr)) {
		/* hole case */
		err = f2fs_get_dnode_of_data(&dn, index, LOOKUP_NODE);
		if (err) {
			dn.data_blkaddr = NULL_ADDR;
			err = 0;
		}
	}
	*blk_addr = dn.data_blkaddr;
	f2fs_put_dnode(&dn);
	return err;
}

static int __reserve_data_block(struct inode *inode, pgoff_t index,
				block_t *blk_addr, bool *node_changed)
{
	struct f2fs_sb_info *sbi = F2FS_I_SB(inode);
	struct dnode_of_data dn;
	struct folio *ifolio;
	int err = 0;

	f2fs_map_lock(sbi, F2FS_GET_BLOCK_PRE_AIO);

	ifolio = f2fs_get_inode_folio(sbi, inode->i_ino);
	if (IS_ERR(ifolio)) {
		err = PTR_ERR(ifolio);
		goto unlock_out;
	}
	set_new_dnode(&dn, inode, ifolio, ifolio, 0);

	if (!f2fs_lookup_read_extent_cache_block(dn.inode, index,
						&dn.data_blkaddr))
		err = f2fs_reserve_block(&dn, index);

	*blk_addr = dn.data_blkaddr;
	*node_changed = dn.node_changed;
	f2fs_put_dnode(&dn);

unlock_out:
	f2fs_map_unlock(sbi, F2FS_GET_BLOCK_PRE_AIO);
	return err;
}

static int prepare_atomic_write_begin(struct f2fs_sb_info *sbi,
			struct folio *folio, loff_t pos, unsigned int len,
			block_t *blk_addr, bool *node_changed, bool *use_cow)
{
	struct inode *inode = folio->mapping->host;
	struct inode *cow_inode = F2FS_I(inode)->cow_inode;
	pgoff_t index = folio->index;
	int err = 0;
	block_t ori_blk_addr = NULL_ADDR;

	/* If pos is beyond the end of file, reserve a new block in COW inode */
	if ((pos & PAGE_MASK) >= i_size_read(inode))
		goto reserve_block;

	/* Look for the block in COW inode first */
	err = __find_data_block(cow_inode, index, blk_addr);
	if (err) {
		return err;
	} else if (*blk_addr != NULL_ADDR) {
		*use_cow = true;
		return 0;
	}

	if (is_inode_flag_set(inode, FI_ATOMIC_REPLACE))
		goto reserve_block;

	/* Look for the block in the original inode */
	err = __find_data_block(inode, index, &ori_blk_addr);
	if (err)
		return err;

reserve_block:
	/* Finally, we should reserve a new block in COW inode for the update */
	err = __reserve_data_block(cow_inode, index, blk_addr, node_changed);
	if (err)
		return err;
	inc_atomic_write_cnt(inode);

	if (ori_blk_addr != NULL_ADDR)
		*blk_addr = ori_blk_addr;
	return 0;
}

static int f2fs_write_begin(const struct kiocb *iocb,
			    struct address_space *mapping,
			    loff_t pos, unsigned len, struct folio **foliop,
			    void **fsdata)
{
	struct inode *inode = mapping->host;
	struct f2fs_sb_info *sbi = F2FS_I_SB(inode);
	struct folio *folio;
	pgoff_t index = pos >> PAGE_SHIFT;
	bool need_balance = false;
	bool use_cow = false;
	block_t blkaddr = NULL_ADDR;
	int err = 0;

	trace_f2fs_write_begin(inode, pos, len);

	if (!f2fs_is_checkpoint_ready(sbi)) {
		err = -ENOSPC;
		goto fail;
	}

	/*
	 * We should check this at this moment to avoid deadlock on inode page
	 * and #0 page. The locking rule for inline_data conversion should be:
	 * folio_lock(folio #0) -> folio_lock(inode_page)
	 */
	if (index != 0) {
		err = f2fs_convert_inline_inode(inode);
		if (err)
			goto fail;
	}

#ifdef CONFIG_F2FS_FS_COMPRESSION
	if (f2fs_compressed_file(inode)) {
		int ret;
		struct page *page;

		*fsdata = NULL;

		if (len == PAGE_SIZE && !(f2fs_is_atomic_file(inode)))
			goto repeat;

		ret = f2fs_prepare_compress_overwrite(inode, &page,
							index, fsdata);
		if (ret < 0) {
			err = ret;
			goto fail;
		} else if (ret) {
			*foliop = page_folio(page);
			return 0;
		}
	}
#endif

repeat:
	/*
	 * Do not use FGP_STABLE to avoid deadlock.
	 * Will wait that below with our IO control.
	 */
	folio = __filemap_get_folio(mapping, index,
				FGP_LOCK | FGP_WRITE | FGP_CREAT, GFP_NOFS);
	if (IS_ERR(folio)) {
		err = PTR_ERR(folio);
		goto fail;
	}

	/* TODO: cluster can be compressed due to race with .writepage */

	*foliop = folio;

	if (f2fs_is_atomic_file(inode))
		err = prepare_atomic_write_begin(sbi, folio, pos, len,
					&blkaddr, &need_balance, &use_cow);
	else
		err = prepare_write_begin(sbi, folio, pos, len,
					&blkaddr, &need_balance);
	if (err)
		goto put_folio;

	if (need_balance && !IS_NOQUOTA(inode) &&
			has_not_enough_free_secs(sbi, 0, 0)) {
		folio_unlock(folio);
		f2fs_balance_fs(sbi, true);
		folio_lock(folio);
		if (folio->mapping != mapping) {
			/* The folio got truncated from under us */
			folio_unlock(folio);
			folio_put(folio);
			goto repeat;
		}
	}

	f2fs_folio_wait_writeback(folio, DATA, false, true);

	if (len == folio_size(folio) || folio_test_uptodate(folio))
		return 0;

	if (!(pos & (PAGE_SIZE - 1)) && (pos + len) >= i_size_read(inode) &&
	    !f2fs_verity_in_progress(inode)) {
		folio_zero_segment(folio, len, folio_size(folio));
		return 0;
	}

	if (blkaddr == NEW_ADDR) {
		folio_zero_segment(folio, 0, folio_size(folio));
		folio_mark_uptodate(folio);
	} else {
		if (!f2fs_is_valid_blkaddr(sbi, blkaddr,
				DATA_GENERIC_ENHANCE_READ)) {
			err = -EFSCORRUPTED;
			goto put_folio;
		}
		err = f2fs_submit_page_read(use_cow ?
				F2FS_I(inode)->cow_inode : inode,
				folio, blkaddr, 0, true);
		if (err)
			goto put_folio;

		folio_lock(folio);
		if (unlikely(folio->mapping != mapping)) {
			folio_unlock(folio);
			folio_put(folio);
			goto repeat;
		}
		if (unlikely(!folio_test_uptodate(folio))) {
			err = -EIO;
			goto put_folio;
		}
	}
	return 0;

put_folio:
	folio_unlock(folio);
	folio_put(folio);
fail:
	f2fs_write_failed(inode, pos + len);
	return err;
}

static int f2fs_write_end(const struct kiocb *iocb,
			struct address_space *mapping,
			loff_t pos, unsigned len, unsigned copied,
			struct folio *folio, void *fsdata)
{
	struct inode *inode = folio->mapping->host;

	trace_f2fs_write_end(inode, pos, len, copied);

	/*
	 * This should be come from len == PAGE_SIZE, and we expect copied
	 * should be PAGE_SIZE. Otherwise, we treat it with zero copied and
	 * let generic_perform_write() try to copy data again through copied=0.
	 */
	if (!folio_test_uptodate(folio)) {
		if (unlikely(copied != len))
			copied = 0;
		else
			folio_mark_uptodate(folio);
	}

#ifdef CONFIG_F2FS_FS_COMPRESSION
	/* overwrite compressed file */
	if (f2fs_compressed_file(inode) && fsdata) {
		f2fs_compress_write_end(inode, fsdata, folio->index, copied);
		f2fs_update_time(F2FS_I_SB(inode), REQ_TIME);

		if (pos + copied > i_size_read(inode) &&
				!f2fs_verity_in_progress(inode))
			f2fs_i_size_write(inode, pos + copied);
		return copied;
	}
#endif

	if (!copied)
		goto unlock_out;

	folio_mark_dirty(folio);

	if (f2fs_is_atomic_file(inode))
		set_page_private_atomic(folio_page(folio, 0));

	if (pos + copied > i_size_read(inode) &&
	    !f2fs_verity_in_progress(inode)) {
		f2fs_i_size_write(inode, pos + copied);
		if (f2fs_is_atomic_file(inode))
			f2fs_i_size_write(F2FS_I(inode)->cow_inode,
					pos + copied);
	}
unlock_out:
	folio_unlock(folio);
	folio_put(folio);
	f2fs_update_time(F2FS_I_SB(inode), REQ_TIME);
	return copied;
}

void f2fs_invalidate_folio(struct folio *folio, size_t offset, size_t length)
{
	struct inode *inode = folio->mapping->host;
	struct f2fs_sb_info *sbi = F2FS_I_SB(inode);

	if (inode->i_ino >= F2FS_ROOT_INO(sbi) &&
				(offset || length != folio_size(folio)))
		return;

	if (folio_test_dirty(folio)) {
		if (inode->i_ino == F2FS_META_INO(sbi)) {
			dec_page_count(sbi, F2FS_DIRTY_META);
		} else if (inode->i_ino == F2FS_NODE_INO(sbi)) {
			dec_page_count(sbi, F2FS_DIRTY_NODES);
		} else {
			inode_dec_dirty_pages(inode);
			f2fs_remove_dirty_inode(inode);
		}
	}
	clear_page_private_all(&folio->page);
}

bool f2fs_release_folio(struct folio *folio, gfp_t wait)
{
	/* If this is dirty folio, keep private data */
	if (folio_test_dirty(folio))
		return false;

	clear_page_private_all(&folio->page);
	return true;
}

static bool f2fs_dirty_data_folio(struct address_space *mapping,
		struct folio *folio)
{
	struct inode *inode = mapping->host;

	trace_f2fs_set_page_dirty(folio, DATA);

	if (!folio_test_uptodate(folio))
		folio_mark_uptodate(folio);
	BUG_ON(folio_test_swapcache(folio));

	if (filemap_dirty_folio(mapping, folio)) {
		f2fs_update_dirty_folio(inode, folio);
		return true;
	}
	return false;
}


static sector_t f2fs_bmap_compress(struct inode *inode, sector_t block)
{
#ifdef CONFIG_F2FS_FS_COMPRESSION
	struct dnode_of_data dn;
	sector_t start_idx, blknr = 0;
	int ret;

	start_idx = round_down(block, F2FS_I(inode)->i_cluster_size);

	set_new_dnode(&dn, inode, NULL, NULL, 0);
	ret = f2fs_get_dnode_of_data(&dn, start_idx, LOOKUP_NODE);
	if (ret)
		return 0;

	if (dn.data_blkaddr != COMPRESS_ADDR) {
		dn.ofs_in_node += block - start_idx;
		blknr = f2fs_data_blkaddr(&dn);
		if (!__is_valid_data_blkaddr(blknr))
			blknr = 0;
	}

	f2fs_put_dnode(&dn);
	return blknr;
#else
	return 0;
#endif
}


static sector_t f2fs_bmap(struct address_space *mapping, sector_t block)
{
	struct inode *inode = mapping->host;
	sector_t blknr = 0;

	if (f2fs_has_inline_data(inode))
		goto out;

	/* make sure allocating whole blocks */
	if (mapping_tagged(mapping, PAGECACHE_TAG_DIRTY))
		filemap_write_and_wait(mapping);

	/* Block number less than F2FS MAX BLOCKS */
	if (unlikely(block >= max_file_blocks(inode)))
		goto out;

	if (f2fs_compressed_file(inode)) {
		blknr = f2fs_bmap_compress(inode, block);
	} else {
		struct f2fs_map_blocks map;

		memset(&map, 0, sizeof(map));
		map.m_lblk = block;
		map.m_len = 1;
		map.m_next_pgofs = NULL;
		map.m_seg_type = NO_CHECK_TYPE;

		if (!f2fs_map_blocks(inode, &map, F2FS_GET_BLOCK_BMAP))
			blknr = map.m_pblk;
	}
out:
	trace_f2fs_bmap(inode, block, blknr);
	return blknr;
}

#ifdef CONFIG_SWAP
static int f2fs_migrate_blocks(struct inode *inode, block_t start_blk,
							unsigned int blkcnt)
{
	struct f2fs_sb_info *sbi = F2FS_I_SB(inode);
	unsigned int blkofs;
	unsigned int blk_per_sec = BLKS_PER_SEC(sbi);
	unsigned int end_blk = start_blk + blkcnt - 1;
	unsigned int secidx = start_blk / blk_per_sec;
	unsigned int end_sec;
	int ret = 0;

	if (!blkcnt)
		return 0;
	end_sec = end_blk / blk_per_sec;

	f2fs_down_write(&F2FS_I(inode)->i_gc_rwsem[WRITE]);
	filemap_invalidate_lock(inode->i_mapping);

	set_inode_flag(inode, FI_ALIGNED_WRITE);
	set_inode_flag(inode, FI_OPU_WRITE);

	for (; secidx <= end_sec; secidx++) {
		unsigned int blkofs_end = secidx == end_sec ?
				end_blk % blk_per_sec : blk_per_sec - 1;

		f2fs_down_write(&sbi->pin_sem);

		ret = f2fs_allocate_pinning_section(sbi);
		if (ret) {
			f2fs_up_write(&sbi->pin_sem);
			break;
		}

		set_inode_flag(inode, FI_SKIP_WRITES);

		for (blkofs = 0; blkofs <= blkofs_end; blkofs++) {
			struct folio *folio;
			unsigned int blkidx = secidx * blk_per_sec + blkofs;

			folio = f2fs_get_lock_data_folio(inode, blkidx, true);
			if (IS_ERR(folio)) {
				f2fs_up_write(&sbi->pin_sem);
				ret = PTR_ERR(folio);
				goto done;
			}

			folio_mark_dirty(folio);
			f2fs_folio_put(folio, true);
		}

		clear_inode_flag(inode, FI_SKIP_WRITES);

		ret = filemap_fdatawrite(inode->i_mapping);

		f2fs_up_write(&sbi->pin_sem);

		if (ret)
			break;
	}

done:
	clear_inode_flag(inode, FI_SKIP_WRITES);
	clear_inode_flag(inode, FI_OPU_WRITE);
	clear_inode_flag(inode, FI_ALIGNED_WRITE);

	filemap_invalidate_unlock(inode->i_mapping);
	f2fs_up_write(&F2FS_I(inode)->i_gc_rwsem[WRITE]);

	return ret;
}

static int check_swap_activate(struct swap_info_struct *sis,
				struct file *swap_file, sector_t *span)
{
	struct address_space *mapping = swap_file->f_mapping;
	struct inode *inode = mapping->host;
	struct f2fs_sb_info *sbi = F2FS_I_SB(inode);
	block_t cur_lblock;
	block_t last_lblock;
	block_t pblock;
	block_t lowest_pblock = -1;
	block_t highest_pblock = 0;
	int nr_extents = 0;
	unsigned int nr_pblocks;
	unsigned int blks_per_sec = BLKS_PER_SEC(sbi);
	unsigned int not_aligned = 0;
	int ret = 0;

	/*
	 * Map all the blocks into the extent list.  This code doesn't try
	 * to be very smart.
	 */
	cur_lblock = 0;
	last_lblock = F2FS_BYTES_TO_BLK(i_size_read(inode));

	while (cur_lblock < last_lblock && cur_lblock < sis->max) {
		struct f2fs_map_blocks map;
retry:
		cond_resched();

		memset(&map, 0, sizeof(map));
		map.m_lblk = cur_lblock;
		map.m_len = last_lblock - cur_lblock;
		map.m_next_pgofs = NULL;
		map.m_next_extent = NULL;
		map.m_seg_type = NO_CHECK_TYPE;
		map.m_may_create = false;

		ret = f2fs_map_blocks(inode, &map, F2FS_GET_BLOCK_FIEMAP);
		if (ret)
			goto out;

		/* hole */
		if (!(map.m_flags & F2FS_MAP_FLAGS)) {
			f2fs_err(sbi, "Swapfile has holes");
			ret = -EINVAL;
			goto out;
		}

		pblock = map.m_pblk;
		nr_pblocks = map.m_len;

		if ((pblock - SM_I(sbi)->main_blkaddr) % blks_per_sec ||
				nr_pblocks % blks_per_sec ||
				f2fs_is_sequential_zone_area(sbi, pblock)) {
			bool last_extent = false;

			not_aligned++;

			nr_pblocks = roundup(nr_pblocks, blks_per_sec);
			if (cur_lblock + nr_pblocks > sis->max)
				nr_pblocks -= blks_per_sec;

			/* this extent is last one */
			if (!nr_pblocks) {
				nr_pblocks = last_lblock - cur_lblock;
				last_extent = true;
			}

			ret = f2fs_migrate_blocks(inode, cur_lblock,
							nr_pblocks);
			if (ret) {
				if (ret == -ENOENT)
					ret = -EINVAL;
				goto out;
			}

			if (!last_extent)
				goto retry;
		}

		if (cur_lblock + nr_pblocks >= sis->max)
			nr_pblocks = sis->max - cur_lblock;

		if (cur_lblock) {	/* exclude the header page */
			if (pblock < lowest_pblock)
				lowest_pblock = pblock;
			if (pblock + nr_pblocks - 1 > highest_pblock)
				highest_pblock = pblock + nr_pblocks - 1;
		}

		/*
		 * We found a PAGE_SIZE-length, PAGE_SIZE-aligned run of blocks
		 */
		ret = add_swap_extent(sis, cur_lblock, nr_pblocks, pblock);
		if (ret < 0)
			goto out;
		nr_extents += ret;
		cur_lblock += nr_pblocks;
	}
	ret = nr_extents;
	*span = 1 + highest_pblock - lowest_pblock;
	if (cur_lblock == 0)
		cur_lblock = 1;	/* force Empty message */
	sis->max = cur_lblock;
	sis->pages = cur_lblock - 1;
out:
	if (not_aligned)
		f2fs_warn(sbi, "Swapfile (%u) is not align to section: 1) creat(), 2) ioctl(F2FS_IOC_SET_PIN_FILE), 3) fallocate(%lu * N)",
			  not_aligned, blks_per_sec * F2FS_BLKSIZE);
	return ret;
}

static int f2fs_swap_activate(struct swap_info_struct *sis, struct file *file,
				sector_t *span)
{
	struct inode *inode = file_inode(file);
	struct f2fs_sb_info *sbi = F2FS_I_SB(inode);
	int ret;

	if (!S_ISREG(inode->i_mode))
		return -EINVAL;

	if (f2fs_readonly(sbi->sb))
		return -EROFS;

	if (f2fs_lfs_mode(sbi) && !f2fs_sb_has_blkzoned(sbi)) {
		f2fs_err(sbi, "Swapfile not supported in LFS mode");
		return -EINVAL;
	}

	ret = f2fs_convert_inline_inode(inode);
	if (ret)
		return ret;

	if (!f2fs_disable_compressed_file(inode))
		return -EINVAL;

	ret = filemap_fdatawrite(inode->i_mapping);
	if (ret < 0)
		return ret;

	f2fs_precache_extents(inode);

	ret = check_swap_activate(sis, file, span);
	if (ret < 0)
		return ret;

	stat_inc_swapfile_inode(inode);
	set_inode_flag(inode, FI_PIN_FILE);
	f2fs_update_time(sbi, REQ_TIME);
	return ret;
}

static void f2fs_swap_deactivate(struct file *file)
{
	struct inode *inode = file_inode(file);

	stat_dec_swapfile_inode(inode);
	clear_inode_flag(inode, FI_PIN_FILE);
}
#else
static int f2fs_swap_activate(struct swap_info_struct *sis, struct file *file,
				sector_t *span)
{
	return -EOPNOTSUPP;
}

static void f2fs_swap_deactivate(struct file *file)
{
}
#endif

const struct address_space_operations f2fs_dblock_aops = {
	.read_folio	= f2fs_read_data_folio,
	.readahead	= f2fs_readahead,
	.writepages	= f2fs_write_data_pages,
	.write_begin	= f2fs_write_begin,
	.write_end	= f2fs_write_end,
	.dirty_folio	= f2fs_dirty_data_folio,
	.migrate_folio	= filemap_migrate_folio,
	.invalidate_folio = f2fs_invalidate_folio,
	.release_folio	= f2fs_release_folio,
	.bmap		= f2fs_bmap,
	.swap_activate  = f2fs_swap_activate,
	.swap_deactivate = f2fs_swap_deactivate,
};

void f2fs_clear_page_cache_dirty_tag(struct folio *folio)
{
	struct address_space *mapping = folio->mapping;
	unsigned long flags;

	xa_lock_irqsave(&mapping->i_pages, flags);
	__xa_clear_mark(&mapping->i_pages, folio->index,
						PAGECACHE_TAG_DIRTY);
	xa_unlock_irqrestore(&mapping->i_pages, flags);
}

int __init f2fs_init_post_read_processing(void)
{
	bio_post_read_ctx_cache =
		kmem_cache_create("f2fs_bio_post_read_ctx",
				  sizeof(struct bio_post_read_ctx), 0, 0, NULL);
	if (!bio_post_read_ctx_cache)
		goto fail;
	bio_post_read_ctx_pool =
		mempool_create_slab_pool(NUM_PREALLOC_POST_READ_CTXS,
					 bio_post_read_ctx_cache);
	if (!bio_post_read_ctx_pool)
		goto fail_free_cache;
	return 0;

fail_free_cache:
	kmem_cache_destroy(bio_post_read_ctx_cache);
fail:
	return -ENOMEM;
}

void f2fs_destroy_post_read_processing(void)
{
	mempool_destroy(bio_post_read_ctx_pool);
	kmem_cache_destroy(bio_post_read_ctx_cache);
}

int f2fs_init_post_read_wq(struct f2fs_sb_info *sbi)
{
	if (!f2fs_sb_has_encrypt(sbi) &&
		!f2fs_sb_has_verity(sbi) &&
		!f2fs_sb_has_compression(sbi))
		return 0;

	sbi->post_read_wq = alloc_workqueue("f2fs_post_read_wq",
						 WQ_UNBOUND | WQ_HIGHPRI,
						 num_online_cpus());
	return sbi->post_read_wq ? 0 : -ENOMEM;
}

void f2fs_destroy_post_read_wq(struct f2fs_sb_info *sbi)
{
	if (sbi->post_read_wq)
		destroy_workqueue(sbi->post_read_wq);
}

int __init f2fs_init_bio_entry_cache(void)
{
	bio_entry_slab = f2fs_kmem_cache_create("f2fs_bio_entry_slab",
			sizeof(struct bio_entry));
	return bio_entry_slab ? 0 : -ENOMEM;
}

void f2fs_destroy_bio_entry_cache(void)
{
	kmem_cache_destroy(bio_entry_slab);
}

static int f2fs_iomap_begin(struct inode *inode, loff_t offset, loff_t length,
			    unsigned int flags, struct iomap *iomap,
			    struct iomap *srcmap)
{
	struct f2fs_map_blocks map = {};
	pgoff_t next_pgofs = 0;
	int err;

	map.m_lblk = F2FS_BYTES_TO_BLK(offset);
	map.m_len = F2FS_BYTES_TO_BLK(offset + length - 1) - map.m_lblk + 1;
	map.m_next_pgofs = &next_pgofs;
	map.m_seg_type = f2fs_rw_hint_to_seg_type(F2FS_I_SB(inode),
						inode->i_write_hint);

	/*
	 * If the blocks being overwritten are already allocated,
	 * f2fs_map_lock and f2fs_balance_fs are not necessary.
	 */
	if ((flags & IOMAP_WRITE) &&
		!f2fs_overwrite_io(inode, offset, length))
		map.m_may_create = true;

	err = f2fs_map_blocks(inode, &map, F2FS_GET_BLOCK_DIO);
	if (err)
		return err;

	iomap->offset = F2FS_BLK_TO_BYTES(map.m_lblk);

	/*
	 * When inline encryption is enabled, sometimes I/O to an encrypted file
	 * has to be broken up to guarantee DUN contiguity.  Handle this by
	 * limiting the length of the mapping returned.
	 */
	map.m_len = fscrypt_limit_io_blocks(inode, map.m_lblk, map.m_len);

	/*
	 * We should never see delalloc or compressed extents here based on
	 * prior flushing and checks.
	 */
	if (WARN_ON_ONCE(map.m_pblk == COMPRESS_ADDR))
		return -EINVAL;

	if (map.m_flags & F2FS_MAP_MAPPED) {
		if (WARN_ON_ONCE(map.m_pblk == NEW_ADDR))
			return -EINVAL;

		iomap->length = F2FS_BLK_TO_BYTES(map.m_len);
		iomap->type = IOMAP_MAPPED;
		iomap->flags |= IOMAP_F_MERGED;
		iomap->bdev = map.m_bdev;
		iomap->addr = F2FS_BLK_TO_BYTES(map.m_pblk);
	} else {
		if (flags & IOMAP_WRITE)
			return -ENOTBLK;

		if (map.m_pblk == NULL_ADDR) {
			iomap->length = F2FS_BLK_TO_BYTES(next_pgofs) -
							iomap->offset;
			iomap->type = IOMAP_HOLE;
		} else if (map.m_pblk == NEW_ADDR) {
			iomap->length = F2FS_BLK_TO_BYTES(map.m_len);
			iomap->type = IOMAP_UNWRITTEN;
		} else {
			f2fs_bug_on(F2FS_I_SB(inode), 1);
		}
		iomap->addr = IOMAP_NULL_ADDR;
	}

	if (map.m_flags & F2FS_MAP_NEW)
		iomap->flags |= IOMAP_F_NEW;
	if ((inode->i_state & I_DIRTY_DATASYNC) ||
	    offset + length > i_size_read(inode))
		iomap->flags |= IOMAP_F_DIRTY;

	return 0;
}

const struct iomap_ops f2fs_iomap_ops = {
	.iomap_begin	= f2fs_iomap_begin,
};
