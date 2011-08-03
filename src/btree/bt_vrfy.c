/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

/*
 * There's a bunch of stuff we pass around during verification, group it
 * together to make the code prettier.
 */
typedef struct {
	uint32_t  frags;			/* Total frags */
	bitstr_t *fragbits;			/* Frag tracking bit list */

	uint64_t record_total;			/* Total record count */

	WT_BUF  *max_key;			/* Largest key */
	uint32_t max_addr;			/* Largest key page */

	uint64_t fcnt;				/* Progress counter */

	int	 dumpfile;			/* Dump file stream */
} WT_VSTUFF;

static int __verify_addfrag(WT_SESSION_IMPL *, uint32_t, uint32_t, WT_VSTUFF *);
static int __verify_checkfrag(WT_SESSION_IMPL *, WT_VSTUFF *);
static int __verify_freelist(WT_SESSION_IMPL *, WT_VSTUFF *);
static int __verify_int(WT_SESSION_IMPL *, int);
static int __verify_overflow(WT_SESSION_IMPL *, uint32_t, uint32_t, WT_VSTUFF *);
static int __verify_overflow_cell(WT_SESSION_IMPL *, WT_PAGE *, WT_VSTUFF *);
static int __verify_row_int_key_order(
	WT_SESSION_IMPL *, WT_PAGE *, WT_ROW_REF *, uint32_t, WT_VSTUFF *);
static int __verify_row_leaf_key_order(
	WT_SESSION_IMPL *, WT_PAGE *, WT_VSTUFF *);
static int __verify_tree(WT_SESSION_IMPL *, WT_REF *, uint64_t, WT_VSTUFF *);

/*
 * __wt_verify --
 *	Verify a file.
 */
int
__wt_verify(WT_SESSION_IMPL *session, const char *config)
{
	WT_UNUSED(config);			/* XXX: unused for now */

	return (__verify_int(session, 0));
}

/*
 * __wt_dumpfile --
 *	Dump a file in debugging mode.
 */
int
__wt_dumpfile(WT_SESSION_IMPL *session, const char *config)
{
	WT_UNUSED(config);			/* XXX: unused for now */

#ifdef HAVE_DIAGNOSTIC
	/*
	 * We use the verification code to do debugging dumps because if we're
	 * dumping in debugging mode, we want to confirm the page is OK before
	 * walking it.
	 */
	return (__verify_int(session, 1));
#else
	__wt_errx(session,
	    "the WiredTiger library was not built in diagnostic mode");
	return (EOPNOTSUPP);
#endif
}

/*
 * __verify_int --
 *	Internal version of verify: verify a Btree, optionally dumping each
 * page in debugging mode.
 */
static int
__verify_int(WT_SESSION_IMPL *session, int dumpfile)
{
	WT_BTREE *btree;
	WT_VSTUFF *vs, _vstuff;
	int ret;

	btree = session->btree;
	ret = 0;

	WT_CLEAR(_vstuff);
	vs = &_vstuff;

	vs->dumpfile = dumpfile;
	WT_ERR(__wt_scr_alloc(session, 0, &vs->max_key));
	vs->max_addr = WT_ADDR_INVALID;

	/*
	 * If the file has no data pages, we're done.
	 * The file size should be a multiple of the allocsize, offset by the
	 * size of the descriptor sector, the first 512B of the file.
	 */
	if (btree->fh->file_size <= WT_BTREE_DESC_SECTOR) {
		__wt_errx(session,
		    "the file contains no data pages and cannot be verified");
		goto err;
	}
	if ((btree->fh->file_size -
	    WT_BTREE_DESC_SECTOR) % btree->allocsize != 0) {
		__wt_errx(session,
		    "the file size is not valid for the allocation size");
		    goto err;
	}

	/*
	 * Allocate a bit array, where each bit represents a single allocation
	 * size piece of the file (this is how we track the parts of the file
	 * we've verified, and check for multiply referenced or unreferenced
	 * blocks).  Storing this on the heap seems reasonable; verifying a 1TB
	 * file with an allocation size of 512B would require a 256MB bit array:
	 *
	 *	(((1 * 2^40) / 512) / 8) / 2^20 = 256
	 *
	 * To verify larger files than we can handle in this way, we'd have to
	 * write parts of the bit array into a disk file.
	 *
	 * !!!
	 * There's one portability issue -- the bitstring package uses "ints",
	 * not unsigned ints, or any fixed size.   If an "int" can't hold a
	 * big enough value, we could lose.   There's a check here to make we
	 * don't overflow.   I don't ever expect to see this error message, but
	 * better safe than sorry.
	 */
	vs->frags = WT_OFF_TO_ADDR(btree, btree->fh->file_size);
	if (vs->frags > INT_MAX) {
		__wt_errx(session, "file is too large to verify");
		goto err;
	}
	WT_ERR(bit_alloc(session, vs->frags, &vs->fragbits));

	/* Verify the tree, starting at the root. */
	WT_ERR(__verify_tree(session, &btree->root_page, (uint64_t)1, vs));

	/* Verify the free-list. */
	WT_ERR(__verify_freelist(session, vs));

	/* Verify we read every file block. */
	WT_ERR(__verify_checkfrag(session, vs));

	if (0) {
err:		if (ret == 0)
			ret = WT_ERROR;
	}

	/* Discard the root page from the tree. */
	if (btree->root_page.page != NULL)
		WT_TRET(__wt_page_reconcile(session,
		    btree->root_page.page, WT_REC_EVICT | WT_REC_LOCKED));

	if (vs != NULL) {
		/* Wrap up reporting. */
		__wt_progress(session, NULL, vs->fcnt);

		/* Free allocated memory. */
		if (vs->fragbits != NULL)
			__wt_free(session, vs->fragbits);
		if (vs->max_key != NULL)
			__wt_scr_release(&vs->max_key);
	}

	return (ret);
}

/*
 * __verify_tree --
 *	Verify a tree, recursively descending through it in depth-first fashion.
 * The page argument was physically verified (so we know it's correctly formed),
 * and the in-memory version built.  Our job is to check logical relationships
 * in the page and in the tree.
 */
static int
__verify_tree(
    WT_SESSION_IMPL *session, WT_REF *ref, uint64_t parent_recno, WT_VSTUFF *vs)
{
	WT_CELL *cell;
	WT_CELL_UNPACK *unpack, _unpack;
	WT_COL *cip;
	WT_COL_REF *cref;
	WT_PAGE *page;
	WT_ROW_REF *rref;
	uint64_t recno;
	uint32_t entry, i;
	int ret;

	ret = 0;
	page = ref->page;
	unpack = &_unpack;

	/*
	 * The page's physical structure was verified when it was read into
	 * memory by the read server thread, and then the in-memory version
	 * of the page was built.   Now we make sure the page and tree are
	 * logically consistent.
	 *
	 * !!!
	 * The problem: (1) the read server has to build the in-memory version
	 * of the page because the read server is the thread that flags when
	 * any thread can access the page in the tree; (2) we can't build the
	 * in-memory version of the page until the physical structure is known
	 * to be OK, so the read server has to verify at least the physical
	 * structure of the page; (3) doing complete page verification requires
	 * reading additional pages (for example, overflow keys imply reading
	 * overflow pages in order to test the key's order in the page); (4)
	 * the read server cannot read additional pages because it will hang
	 * waiting on itself.  For this reason, we split page verification
	 * into a physical verification, which allows the in-memory version
	 * of the page to be built, and then a subsequent logical verification
	 * which happens here.
	 *
	 * Report progress every 10 pages.
	 */
	if (++vs->fcnt % 10 == 0)
		__wt_progress(session, NULL, vs->fcnt);

	/*
	 * Update frags list.
	 *
	 * XXX
	 * Verify currently walks the in-memory tree, which means we can see
	 * pages that have not yet been written to disk.  That's not going to
	 * work because in-flight pages don't map correctly to on-disk pages.
	 * Verify will only work correctly on a clean tree -- make sure that
	 * is what we're seeing.  This test can go away when verify takes a
	 * file argument instead of an already opened tree (or a tree that's
	 * known to be clean, assuming the upper-level is doing the open for
	 * us.)
	 */
	WT_ASSERT(session, ref->addr != WT_ADDR_INVALID);
	WT_RET(__verify_addfrag(session, ref->addr, ref->size, vs));

#ifdef HAVE_DIAGNOSTIC
	/* Optionally dump the page in debugging mode. */
	if (vs->dumpfile)
		WT_RET(__wt_debug_page(session, page, NULL));
#endif

	/*
	 * Column-store key order checks: check the starting record number,
	 * then update the total record count.
	 */
	switch (page->type) {
	case WT_PAGE_COL_INT:
		recno = page->u.col_int.recno;
		goto recno_chk;
	case WT_PAGE_COL_FIX:
	case WT_PAGE_COL_VAR:
		recno = page->u.col_leaf.recno;
recno_chk:	if (parent_recno != recno) {
			__wt_errx(session, "page at addr %" PRIu32
			    " has a starting record of %" PRIu64
			    " where the expected starting record was %" PRIu64,
			    WT_PADDR(page), recno, parent_recno);
			return (WT_ERROR);
		}
		break;
	}
	switch (page->type) {
	case WT_PAGE_COL_FIX:
		vs->record_total += page->entries;
		break;
	case WT_PAGE_COL_VAR:
		recno = 0;
		WT_COL_FOREACH(page, cip, i)
			if ((cell = WT_COL_PTR(page, cip)) == NULL)
				++recno;
			else {
				__wt_cell_unpack(cell, unpack);
				recno += unpack->rle;
			}
		vs->record_total += recno;
		break;
	}

	/*
	 * Row-store leaf page key order check: it's a depth-first traversal,
	 * the first key on this page should be larger than any key previously
	 * seen.
	 */
	switch (page->type) {
	case WT_PAGE_ROW_LEAF:
		WT_RET(__verify_row_leaf_key_order(session, page, vs));
		break;
	}

	/*
	 * Check overflow pages.  We check overflow cells separately from other
	 * tests that walk the page as it's simpler, and I don't care much how
	 * fast table verify runs.
	 */
	switch (page->type) {
	case WT_PAGE_COL_VAR:
	case WT_PAGE_ROW_INT:
	case WT_PAGE_ROW_LEAF:
		WT_RET(__verify_overflow_cell(session, page, vs));
		break;
	}

	/* Check tree connections and recursively descend the tree. */
	switch (page->type) {
	case WT_PAGE_COL_INT:
		/* For each entry in an internal page, verify the subtree. */
		WT_COL_REF_FOREACH(page, cref, i) {
			/*
			 * It's a depth-first traversal: this entry's starting
			 * record number should be 1 more than the total records
			 * reviewed to this point.
			 */
			if (cref->recno != vs->record_total + 1) {
				__wt_errx(session, "page at addr %" PRIu32
				    " has a starting record of %" PRIu64
				    " where the expected starting record was "
				    "%" PRIu64,
				    WT_COL_REF_ADDR(cref),
				    cref->recno, vs->record_total + 1);
				return (WT_ERROR);
			}

			/* cref references the subtree containing the record */
			ref = &cref->ref;
			WT_RET(__wt_page_in(session, page, ref, 1));
			ret = __verify_tree(session, ref, cref->recno, vs);
			__wt_hazard_clear(session, ref->page);
			WT_TRET(__wt_page_reconcile(session,
			    ref->page, WT_REC_EVICT | WT_REC_LOCKED));
			WT_RET_TEST(ret, ret);
		}
		break;
	case WT_PAGE_ROW_INT:
		/* For each entry in an internal page, verify the subtree. */
		entry = 0;
		WT_ROW_REF_FOREACH(page, rref, i) {
			/*
			 * It's a depth-first traversal: this entry's starting
			 * key should be larger than the largest key previously
			 * reviewed.
			 *
			 * The 0th key of any internal page is magic, and we
			 * can't test against it.
			 */
			if (entry != 0)
				WT_RET(__verify_row_int_key_order(
				    session, page, rref, entry, vs));
			++entry;

			/* rref references the subtree containing the record */
			ref = &rref->ref;
			WT_RET(__wt_page_in(session, page, ref, 1));
			ret = __verify_tree(session, ref, (uint64_t)0, vs);
			__wt_hazard_clear(session, ref->page);
			WT_TRET(__wt_page_reconcile(session,
			    ref->page, WT_REC_EVICT | WT_REC_LOCKED));
			WT_RET_TEST(ret, ret);
		}
		break;
	}
	return (0);
}

/*
 * __verify_row_int_key_order --
 *	Compare a key on an internal page to the largest key we've seen so
 * far; update the largest key we've seen so far to that key.
 */
static int
__verify_row_int_key_order(WT_SESSION_IMPL *session,
    WT_PAGE *page, WT_ROW_REF *rref, uint32_t entry, WT_VSTUFF *vs)
{
	WT_BTREE *btree;
	WT_IKEY *ikey;
	WT_ITEM item;
	int (*func)(WT_BTREE *, const WT_ITEM *, const WT_ITEM *);

	btree = session->btree;
	func = btree->btree_compare;

	/* The maximum key is set, we updated it from a leaf page first. */
	WT_ASSERT(session, vs->max_addr != WT_ADDR_INVALID);

	/* Set up the key structure. */
	ikey = rref->key;
	item.data = WT_IKEY_DATA(ikey);
	item.size = ikey->size;

	/* Compare the key against the largest key we've seen so far. */
	if (func(btree, &item, (WT_ITEM *)vs->max_key) <= 0) {
		__wt_errx(session,
		    "the internal key in entry %" PRIu32
		    " on the page at addr %" PRIu32
		    " sorts before the last key appearing on page %" PRIu32,
		    entry, WT_PADDR(page), vs->max_addr);
		return (WT_ERROR);
	}

	/* Update the largest key we've seen to the key just checked. */
	WT_RET(__wt_buf_set(session, vs->max_key, item.data, item.size));
	vs->max_addr = WT_PADDR(page);

	return (0);
}

/*
 * __verify_row_leaf_key_order --
 *	Compare the first key on a leaf page to the largest key we've seen so
 * far; update the largest key we've seen so far to the last key on the page.
 */
static int
__verify_row_leaf_key_order(
    WT_SESSION_IMPL *session, WT_PAGE *page, WT_VSTUFF *vs)
{
	WT_BTREE *btree;
	WT_BUF *key;
	int ret, (*func)(WT_BTREE *, const WT_ITEM *, const WT_ITEM *);

	btree = session->btree;
	key = NULL;
	func = btree->btree_compare;
	ret = 0;

	/*
	 * We visit our first leaf page before setting the maximum key (the 0th
	 * keys on the internal pages leading to the smallest leaf in the tree
	 * are all empty entries).
	 */
	if (vs->max_addr == WT_ADDR_INVALID) {
		WT_RET(__wt_scr_alloc(session, 0, &key));
		WT_RET(__wt_row_key(session, page, page->u.row_leaf.d, key));

		/*
		 * Compare the key against the largest key we've seen so far.
		 *
		 * If we're comparing against a key taken from an internal page,
		 * we can compare equal (which is an expected path, the internal
		 * page key is often a copy of the leaf page's first key).  But,
		 * in the case of the 0th slot on an internal page, the last key
		 * we've seen was a key from a previous leaf page, and it's not
		 * OK to compare equally in that case.
		 */
		if (func(btree, (WT_ITEM *)key, (WT_ITEM *)vs->max_key) < 0) {
			__wt_errx(session,
			    "the first key on the page at addr %" PRIu32
			    " sorts equal or less than a key"
			    " appearing on page %" PRIu32,
			    WT_PADDR(page), vs->max_addr);
			ret = WT_ERROR;
		}

		__wt_scr_release(&key);
		if (ret != 0)
			return (ret);
	}

	/* Update the largest key we've seen to the last key on this page. */
	vs->max_addr = WT_PADDR(page);
	return (__wt_row_key(session,
	    page, page->u.row_leaf.d + (page->entries - 1), vs->max_key));
}

/*
 * __verify_overflow_cell --
 *	Verify any overflow cells on the page.
 */
static int
__verify_overflow_cell(
    WT_SESSION_IMPL *session, WT_PAGE *page, WT_VSTUFF *vs)
{
	WT_CELL *cell;
	WT_CELL_UNPACK *unpack, _unpack;
	WT_PAGE_DISK *dsk;
	uint32_t i;

	unpack = &_unpack;

	/*
	 * Row-store internal page disk images are discarded when there's no
	 * overflow items on the page.   If there's no disk image, we're done.
	 */
	if ((dsk = page->dsk) == NULL) {
		WT_ASSERT(session, page->type == WT_PAGE_ROW_INT);
		return (0);
	}

	/* Walk the disk page, verifying pages referenced by overflow cells. */
	WT_CELL_FOREACH(dsk, cell, unpack, i) {
		__wt_cell_unpack(cell, unpack);
		switch (unpack->type) {
		case WT_CELL_KEY_OVFL:
		case WT_CELL_VALUE_OVFL:
			WT_RET(__verify_overflow(
			    session, unpack->off.addr, unpack->off.size, vs));
			break;
		}
	}
	return (0);
}

/*
 * __verify_overflow --
 *	Read in an overflow page and check it.
 */
static int
__verify_overflow(
    WT_SESSION_IMPL *session, uint32_t addr, uint32_t size, WT_VSTUFF *vs)
{
	WT_BUF *tmp;
	WT_PAGE_DISK *dsk;
	int ret;

	tmp = NULL;
	ret = 0;

	/* Allocate enough memory to hold the overflow pages. */
	WT_RET(__wt_scr_alloc(session, size, &tmp));

	/* Read the page. */
	dsk = tmp->mem;
	WT_ERR(__wt_disk_read(session, dsk, addr, size));

	/*
	 * Verify the disk image -- this function would normally be called
	 * from the asynchronous read server, but overflow pages are read
	 * synchronously. Regardless, we break the overflow verification code
	 * into two parts, on-disk format checking and internal checking,
	 * just so it looks like all of the other page type checking.
	 */
	WT_ERR(
	    __wt_verify_dsk_chunk(session, dsk, addr, dsk->u.datalen, size, 0));

	/* Add the fragments. */
	WT_ERR(__verify_addfrag(session, addr, size, vs));

err:	__wt_scr_release(&tmp);

	return (ret);
}

/*
 * __verify_freelist --
 *	Add the freelist fragments to the list of verified fragments.
 */
static int
__verify_freelist(WT_SESSION_IMPL *session, WT_VSTUFF *vs)
{
	WT_BTREE *btree;
	WT_FREE_ENTRY *fe;
	int ret;

	btree = session->btree;
	ret = 0;

	TAILQ_FOREACH(fe, &btree->freeqa, qa) {
		if (WT_ADDR_TO_OFF(btree, fe->addr) +
		    fe->size > btree->fh->file_size) {
			__wt_errx(session,
			    "free-list entry addr %" PRIu32 "references "
			    "non-existent file pages",
			    fe->addr);
			return (WT_ERROR);
		}
		WT_TRET(__verify_addfrag(session, fe->addr, fe->size, vs));
	}

	return (ret);
}

/*
 * __verify_addfrag --
 *	Add the WT_PAGE's fragments to the list, and complain if we've already
 *	verified this chunk of the file.
 */
static int
__verify_addfrag(
    WT_SESSION_IMPL *session, uint32_t addr, uint32_t size, WT_VSTUFF *vs)
{
	WT_BTREE *btree;
	uint32_t frags, i;

	btree = session->btree;

	frags = size / btree->allocsize;
	for (i = 0; i < frags; ++i)
		if (bit_test(vs->fragbits, addr + i)) {
			__wt_errx(session,
			    "file fragment at addr %" PRIu32
			    " already verified", addr);
			return (WT_ERROR);
		}
	if (frags > 0)
		bit_nset(vs->fragbits, addr, addr + (frags - 1));
	return (0);
}

/*
 * __verify_checkfrag --
 *	Verify we've checked all the fragments in the file.
 */
static int
__verify_checkfrag(WT_SESSION_IMPL *session, WT_VSTUFF *vs)
{
	int ffc, ffc_start, ffc_end, frags, ret;

	frags = (int)vs->frags;		/* XXX: bitstring.h wants "ints" */
	ret = 0;

	/* Check for file fragments we haven't verified. */
	for (ffc_start = ffc_end = -1;;) {
		bit_ffc(vs->fragbits, frags, &ffc);
		if (ffc != -1) {
			bit_set(vs->fragbits, ffc);
			if (ffc_start == -1) {
				ffc_start = ffc_end = ffc;
				continue;
			}
			if (ffc_end == ffc - 1) {
				ffc_end = ffc;
				continue;
			}
		}
		if (ffc_start != -1) {
			if (ffc_start == ffc_end)
				__wt_errx(session,
				    "file fragment %d was never verified",
				    ffc_start);
			else
				__wt_errx(session,
				    "file fragments %d-%d were never verified",
				    ffc_start, ffc_end);
			ret = WT_ERROR;
		}
		ffc_start = ffc_end = ffc;
		if (ffc == -1)
			break;
	}
	return (ret);
}
