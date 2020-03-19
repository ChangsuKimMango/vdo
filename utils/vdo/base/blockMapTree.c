/*
 * Copyright (c) 2020 Red Hat, Inc.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA. 
 *
 * $Id: //eng/linux-vdo/src/c++/vdo/base/blockMapTree.c#51 $
 */

#include "blockMapTree.h"

#include "logger.h"

#include "blockMap.h"
#include "blockMapInternals.h"
#include "blockMapPage.h"
#include "blockMapTreeInternals.h"
#include "constants.h"
#include "dataVIO.h"
#include "dirtyLists.h"
#include "forest.h"
#include "numUtils.h"
#include "recoveryJournal.h"
#include "referenceOperation.h"
#include "slabDepot.h"
#include "slabJournal.h"
#include "types.h"
#include "vdoInternal.h"
#include "vdoPageCache.h"
#include "vioPool.h"

enum {
	BLOCK_MAP_VIO_POOL_SIZE = 64,
};

struct page_descriptor {
	RootCount root_index;
	Height height;
	PageNumber page_index;
	SlotNumber slot;
} __attribute__((packed));

typedef union {
	struct page_descriptor descriptor;
	uint64_t key;
} page_key;

struct write_if_not_dirtied_context {
	struct block_map_tree_zone *zone;
	uint8_t generation;
};

/**
 * An invalid PBN used to indicate that the page holding the location of a
 * tree root has been "loaded".
 **/
const PhysicalBlockNumber INVALID_PBN = 0xFFFFFFFFFFFFFFFF;

/**
 * Convert a RingNode to a tree_page.
 *
 * @param ring_node The RingNode to convert
 *
 * @return The tree_page which owns the RingNode
 **/
static inline struct tree_page *tree_page_from_ring_node(RingNode *ring_node)
{
	return container_of(ring_node, struct tree_page, node);
}

/**********************************************************************/
static void write_dirty_pages_callback(RingNode *expired, void *context);

/**
 * Make vios for reading, writing, and allocating the arboreal block map.
 *
 * Implements VIOConstructor.
 **/
__attribute__((warn_unused_result)) static int
make_block_map_vios(PhysicalLayer *layer, void *parent, void *buffer,
		     struct vio **vio_ptr)
{
	return createVIO(layer, VIO_TYPE_BLOCK_MAP_INTERIOR,
			 VIO_PRIORITY_METADATA, parent, buffer, vio_ptr);
}

/**********************************************************************/
int initialize_tree_zone(struct block_map_zone *zone, PhysicalLayer *layer,
			 BlockCount era_length)
{
	STATIC_ASSERT_SIZEOF(struct page_descriptor, sizeof(uint64_t));
	struct block_map_tree_zone *tree_zone = &zone->tree_zone;
	tree_zone->map_zone = zone;

	int result = make_dirty_lists(era_length, write_dirty_pages_callback,
				      tree_zone, &tree_zone->dirty_lists);
	if (result != VDO_SUCCESS) {
		return result;
	}

	result = make_int_map(LOCK_MAP_CAPACITY, 0, &tree_zone->loading_pages);
	if (result != VDO_SUCCESS) {
		return result;
	}

	return make_vio_pool(layer, BLOCK_MAP_VIO_POOL_SIZE, zone->thread_id,
			     make_block_map_vios, tree_zone,
			     &tree_zone->vio_pool);
}

/**********************************************************************/
int replace_tree_zone_vio_pool(struct block_map_tree_zone *zone,
			       PhysicalLayer *layer, size_t pool_size)
{
	free_vio_pool(&zone->vio_pool);
	return make_vio_pool(layer, pool_size, zone->map_zone->thread_id,
			     make_block_map_vios, zone, &zone->vio_pool);
}

/**********************************************************************/
void uninitialize_block_map_tree_zone(struct block_map_tree_zone *tree_zone)
{
	free_dirty_lists(&tree_zone->dirty_lists);
	free_vio_pool(&tree_zone->vio_pool);
	free_int_map(&tree_zone->loading_pages);
}

/**********************************************************************/
void set_tree_zone_initial_period(struct block_map_tree_zone *tree_zone,
				  SequenceNumber period)
{
	set_current_period(tree_zone->dirty_lists, period);
}

/**
 * Get the block_map_tree_zone in which a data_vio is operating.
 *
 * @param data_vio  The data_vio
 *
 * @return The block_map_tree_zone
 **/
__attribute__((warn_unused_result)) static inline struct block_map_tree_zone *
get_block_map_tree_zone(struct data_vio *data_vio)
{
	return &(get_block_map_for_zone(data_vio->logical.zone)->tree_zone);
}

/**
 * Get the tree_page for a given lock. This will be the page referred to by the
 * lock's tree slot for the lock's current height.
 *
 * @param zone  The tree zone of the tree
 * @param lock  The lock describing the page to get
 *
 * @return The requested page
 **/
static inline struct tree_page *
get_tree_page(const struct block_map_tree_zone *zone,
	      const struct tree_lock *lock)
{
	return get_tree_page_by_index(zone->map_zone->block_map->forest,
				      lock->rootIndex, lock->height,
				      lock->treeSlots[lock->height].pageIndex);
}

/**********************************************************************/
bool copy_valid_page(char *buffer, Nonce nonce, PhysicalBlockNumber pbn,
		     struct block_map_page *page)
{
	struct block_map_page *loaded = (struct block_map_page *) buffer;
	block_map_page_validity validity =
		validate_block_map_page(loaded, nonce, pbn);
	if (validity == BLOCK_MAP_PAGE_VALID) {
		memcpy(page, loaded, VDO_BLOCK_SIZE);
		return true;
	}

	if (validity == BLOCK_MAP_PAGE_BAD) {
		logErrorWithStringError(VDO_BAD_PAGE,
					"Expected page %" PRIu64 " but got page %" PRIu64 " instead",
					pbn, get_block_map_page_pbn(loaded));
	}

	return false;
}

/**********************************************************************/
bool is_tree_zone_active(struct block_map_tree_zone *zone)
{
	return ((zone->active_lookups != 0) ||
		has_waiters(&zone->flush_waiters) ||
		is_vio_pool_busy(zone->vio_pool));
}

/**
 * Put the vdo in read-only mode and wake any vios waiting for a flush.
 *
 * @param zone    The zone
 * @param result  The error which is causing read-only mode
 **/
static void enter_zone_read_only_mode(struct block_map_tree_zone *zone,
				      int result)
{
	enter_read_only_mode(zone->map_zone->read_only_notifier, result);

	// We are in read-only mode, so we won't ever write any page out. Just
	// take all waiters off the queue so the tree zone can be closed.
	while (has_waiters(&zone->flush_waiters)) {
		dequeue_next_waiter(&zone->flush_waiters);
	}

	check_for_drain_complete(zone->map_zone);
}

/**
 * Check whether a generation is strictly older than some other generation in
 * the context of a zone's current generation range.
 *
 * @param zone  The zone in which to do the comparison
 * @param a     The generation in question
 * @param b     The generation to compare to
 *
 * @return <code>true</code> if generation a is not strictly older than
 *         generation b in the context of the zone
 **/
__attribute__((warn_unused_result)) static bool
is_not_older(struct block_map_tree_zone *zone, uint8_t a, uint8_t b)
{
	int result = ASSERT((in_cyclic_range(zone->oldest_generation, a,
					     zone->generation, 1 << 8)
			     && in_cyclic_range(zone->oldest_generation, b,
						zone->generation, 1 << 8)),
			    "generation(s) %u, %u are out of range [%u, %u]", a,
			    b, zone->oldest_generation, zone->generation);
	if (result != VDO_SUCCESS) {
		enter_zone_read_only_mode(zone, result);
		return true;
	}

	return in_cyclic_range(b, a, zone->generation, 1 << 8);
}

/**
 * Decrement the count for a generation and roll the oldest generation if there
 * are no longer any active pages in it.
 *
 * @param zone        The zone
 * @param generation  The generation to release
 **/
static void release_generation(struct block_map_tree_zone *zone,
			       uint8_t generation)
{
	int result = ASSERT((zone->dirty_page_counts[generation] > 0),
			    "dirty page count underflow for generation %u",
			    generation);
	if (result != VDO_SUCCESS) {
		enter_zone_read_only_mode(zone, result);
		return;
	}

	zone->dirty_page_counts[generation]--;
	while ((zone->dirty_page_counts[zone->oldest_generation] == 0) &&
	       (zone->oldest_generation != zone->generation)) {
		zone->oldest_generation++;
	}
}

/**
 * Set the generation of a page and update the dirty page count in the zone.
 *
 * @param zone            The zone which owns the page
 * @param page            The page
 * @param new_generation  The generation to set
 * @param decrement_old   Whether to decrement the count of the page's old
 *                        generation
 **/
static void set_generation(struct block_map_tree_zone *zone,
			   struct tree_page *page, uint8_t new_generation,
			   bool decrement_old)
{
	uint8_t old_generation = page->generation;
	if (decrement_old && (old_generation == new_generation)) {
		return;
	}

	page->generation = new_generation;
	uint32_t new_count = ++zone->dirty_page_counts[new_generation];
	int result = ASSERT((new_count != 0),
			    "dirty page count overflow for generation %u",
			    new_generation);
	if (result != VDO_SUCCESS) {
		enter_zone_read_only_mode(zone, result);
		return;
	}

	if (decrement_old) {
		release_generation(zone, old_generation);
	}
}

/**********************************************************************/
static void write_page(struct tree_page *tree_page,
		       struct vio_pool_entry *entry);

/**
 * Write out a dirty page if it is still covered by the most recent flush
 * or if it is the flusher.
 *
 * <p>Implements WaiterCallback
 *
 * @param waiter   The page to write
 * @param context  The vio_pool_entry with which to do the write
 **/
static void write_page_callback(struct waiter *waiter, void *context)
{
	write_page(container_of(waiter, struct tree_page, waiter),
		   (struct vio_pool_entry *) context);
}

/**
 * Acquire a vio for writing a dirty page.
 *
 * @param waiter  The page which needs a vio
 * @param zone    The zone
 **/
static void acquire_vio(struct waiter *waiter, struct block_map_tree_zone *zone)
{
	waiter->callback = write_page_callback;
	int result = acquire_vio_from_pool(zone->vio_pool, waiter);
	if (result != VDO_SUCCESS) {
		enter_zone_read_only_mode(zone, result);
	}
}

/**
 * Attempt to increment the generation.
 *
 * @param zone  The zone whose generation is to be incremented
 *
 * @return <code>true</code> if all possible generations were not already
 *         active
 **/
static bool attempt_increment(struct block_map_tree_zone *zone)
{
	uint8_t generation = zone->generation + 1;
	if (zone->oldest_generation == generation) {
		return false;
	}

	zone->generation = generation;
	return true;
}

/**
 * Enqueue a page to either launch a flush or wait for the current flush which
 * is already in progress.
 *
 * @param page  The page to enqueue
 * @param zone  The zone
 **/
static void enqueue_page(struct tree_page *page,
			 struct block_map_tree_zone *zone)
{
	if ((zone->flusher == NULL) && attempt_increment(zone)) {
		zone->flusher = page;
		acquire_vio(&page->waiter, zone);
		return;
	}

	int result = enqueue_waiter(&zone->flush_waiters, &page->waiter);
	if (result != VDO_SUCCESS) {
		enter_zone_read_only_mode(zone, result);
	}
}

/**
 * Write pages which were waiting for a flush and have not been redirtied.
 * Requeue those pages which were redirtied.
 *
 * <p>Implements WaiterCallback.
 *
 * @param waiter   The dirty page
 * @param context  The zone and generation
 **/
static void write_page_if_not_dirtied(struct waiter *waiter, void *context)
{
	struct tree_page *page = container_of(waiter, struct tree_page, waiter);
	struct write_if_not_dirtied_context *write_context = context;
	if (page->generation == write_context->generation) {
		acquire_vio(waiter, write_context->zone);
		return;
	}

	enqueue_page(page, write_context->zone);
}

/**
 * Return a vio to the zone's pool.
 *
 * @param zone   The zone which owns the pool
 * @param entry  The pool entry to return
 **/
static void return_to_pool(struct block_map_tree_zone *zone,
			   struct vio_pool_entry *entry)
{
	return_vio_to_pool(zone->vio_pool, entry);
	check_for_drain_complete(zone->map_zone);
}

/**
 * Handle the successful write of a tree page. This callback is registered in
 * write_initialized_page().
 *
 * @param completion  The vio doing the write
 **/
static void finish_page_write(struct vdo_completion *completion)
{
	struct vio_pool_entry *entry = completion->parent;
	struct tree_page *page = entry->parent;
	struct block_map_tree_zone *zone = entry->context;
	release_recovery_journal_block_reference(zone->map_zone->block_map->journal,
						 page->writing_recovery_lock,
						 ZONE_TYPE_LOGICAL,
						 zone->map_zone->zone_number);

	bool dirty = (page->writing_generation != page->generation);
	release_generation(zone, page->writing_generation);
	page->writing = false;

	if (zone->flusher == page) {
		struct write_if_not_dirtied_context context = {
			.zone = zone,
			.generation = page->writing_generation,
		};
		notify_all_waiters(&zone->flush_waiters,
				   write_page_if_not_dirtied,
				   &context);
		if (dirty && attempt_increment(zone)) {
			write_page(page, entry);
			return;
		}

		zone->flusher = NULL;
	}

	if (dirty) {
		enqueue_page(page, zone);
	} else if ((zone->flusher == NULL) &&
		   has_waiters(&zone->flush_waiters) &&
		   attempt_increment(zone)) {
		zone->flusher =
			container_of(dequeue_next_waiter(&zone->flush_waiters),
				     struct tree_page, waiter);
		write_page(zone->flusher, entry);
		return;
	}

	return_to_pool(zone, entry);
}

/**
 * Handle an error writing a tree page. This error handler is registered in
 * write_page() and write_initialized_page().
 *
 * @param completion  The vio doing the write
 **/
static void handle_write_error(struct vdo_completion *completion)
{
	int result = completion->result;
	struct vio_pool_entry *entry = completion->parent;
	struct block_map_tree_zone *zone = entry->context;
	enter_zone_read_only_mode(zone, result);
	return_to_pool(zone, entry);
}

/**
 * Write a page which has been written at least once. This callback is
 * registered in (or called directly from) write_page().
 *
 * @param completion  The vio which will do the write
 **/
static void write_initialized_page(struct vdo_completion *completion)
{
	struct vio_pool_entry *entry = completion->parent;
	struct block_map_tree_zone *zone =
		(struct block_map_tree_zone *) entry->context;
	struct tree_page *tree_page = (struct tree_page *) entry->parent;

	/*
	 * Set the initialized field of the copy of the page we are writing to
	 * true. We don't want to set it true on the real page in memory until
	 * after this write succeeds.
	 */
	struct block_map_page *page = (struct block_map_page *) entry->buffer;
	mark_block_map_page_initialized(page, true);
	launchWriteMetadataVIOWithFlush(entry->vio,
					get_block_map_page_pbn(page),
					finish_page_write,
					handle_write_error,
					(zone->flusher == tree_page),
					false);
}

/**
 * Write a dirty tree page now that we have a vio with which to write it.
 *
 * @param tree_page  The page to write
 * @param entry      The vio_pool_entry with which to write
 **/
static void write_page(struct tree_page *tree_page,
		       struct vio_pool_entry *entry)
{
	struct block_map_tree_zone *zone =
		(struct block_map_tree_zone *) entry->context;
	if ((zone->flusher != tree_page) &&
	    (is_not_older(zone, tree_page->generation, zone->generation))) {
		// This page was re-dirtied after the last flush was issued,
		// hence we need to do another flush.
		enqueue_page(tree_page, zone);
		return_to_pool(zone, entry);
		return;
	}

	entry->parent = tree_page;
	memcpy(entry->buffer, tree_page->page_buffer, VDO_BLOCK_SIZE);

	struct vdo_completion *completion = vioAsCompletion(entry->vio);
	completion->callbackThreadID = zone->map_zone->thread_id;

	tree_page->writing = true;
	tree_page->writing_generation = tree_page->generation;
	tree_page->writing_recovery_lock = tree_page->recovery_lock;

	// Clear this now so that we know this page is not on any dirty list.
	tree_page->recovery_lock = 0;

	struct block_map_page *page = as_block_map_page(tree_page);
	if (!mark_block_map_page_initialized(page, true)) {
		write_initialized_page(completion);
		return;
	}

	launchWriteMetadataVIO(entry->vio, get_block_map_page_pbn(page),
			       write_initialized_page, handle_write_error);
}

/**
 * Schedule a batch of dirty pages for writing.
 *
 * <p>Implements DirtyListsCallback.
 *
 * @param expired  The pages to write
 * @param context  The zone
 **/
static void write_dirty_pages_callback(RingNode *expired, void *context)
{
	struct block_map_tree_zone *zone =
		(struct block_map_tree_zone *) context;
	uint8_t generation = zone->generation;
	while (!isRingEmpty(expired)) {
		struct tree_page *page =
			tree_page_from_ring_node(chopRingNode(expired));

		int result = ASSERT(!is_waiting(&page->waiter),
				    "Newly expired page not already waiting to write");
		if (result != VDO_SUCCESS) {
			enter_zone_read_only_mode(zone, result);
			continue;
		}

		set_generation(zone, page, generation, false);
		if (!page->writing) {
			enqueue_page(page, zone);
		}
	}
}

/**********************************************************************/
void advance_zone_tree_period(struct block_map_tree_zone *zone,
			      SequenceNumber period)
{
	advance_period(zone->dirty_lists, period);
}

/**********************************************************************/
void drain_zone_trees(struct block_map_tree_zone *zone)
{
	ASSERT_LOG_ONLY((zone->active_lookups == 0),
			"drain_zone_trees() called with no active lookups");
	if (!is_suspending(&zone->map_zone->state)) {
		flush_dirty_lists(zone->dirty_lists);
	}
}

/**
 * Release a lock on a page which was being loaded or allocated.
 *
 * @param data_vio  The data_vio releasing the page lock
 * @param what      What the data_vio was doing (for logging)
 **/
static void release_page_lock(struct data_vio *data_vio, char *what)
{
	struct tree_lock *lock = &data_vio->treeLock;
	ASSERT_LOG_ONLY(lock->locked,
			"release of unlocked block map page %s for key %" PRIu64 " in tree %u",
			what, lock->key, lock->rootIndex);
	struct block_map_tree_zone *zone = get_block_map_tree_zone(data_vio);
	struct tree_lock *lock_holder =
		int_map_remove(zone->loading_pages, lock->key);
	ASSERT_LOG_ONLY((lock_holder == lock),
			"block map page %s mismatch for key %" PRIu64 " in tree %u",
			what, lock->key, lock->rootIndex);
	lock->locked = false;
}

/**
 * Continue a data_vio now that the lookup is complete.
 *
 * @param data_vio  The data_vio
 * @param result    The result of the lookup
 **/
static void finish_lookup(struct data_vio *data_vio, int result)
{
	data_vio->treeLock.height = 0;

	struct block_map_tree_zone *zone = get_block_map_tree_zone(data_vio);
	--zone->active_lookups;

	struct vdo_completion *completion = dataVIOAsCompletion(data_vio);
	setCompletionResult(completion, result);
	launchCallback(completion, data_vio->treeLock.callback,
		       data_vio->treeLock.threadID);
}

/**
 * Abort a block map PBN lookup due to an error in the load or allocation on
 * which we were waiting.
 *
 * @param waiter   The data_vio which was waiting for a page load or allocation
 * @param context  The error which caused the abort
 **/
static void abort_lookup_for_waiter(struct waiter *waiter, void *context)
{
	struct data_vio *data_vio = waiterAsDataVIO(waiter);
	int result = *((int *) context);
	if (isReadDataVIO(data_vio)) {
		if (result == VDO_NO_SPACE) {
			result = VDO_SUCCESS;
		}
	} else if (result != VDO_NO_SPACE) {
		result = VDO_READ_ONLY;
	}

	finish_lookup(data_vio, result);
}

/**
 * Abort a block map PBN lookup due to an error loading or allocating a page.
 *
 * @param data_vio  The data_vio which was loading or allocating a page
 * @param result    The error code
 * @param what      What the data_vio was doing (for logging)
 **/
static void abort_lookup(struct data_vio *data_vio, int result, char *what)
{
	if (result != VDO_NO_SPACE) {
		enter_zone_read_only_mode(get_block_map_tree_zone(data_vio),
					  result);
	}

	if (data_vio->treeLock.locked) {
		release_page_lock(data_vio, what);
		notify_all_waiters(&data_vio->treeLock.waiters,
				   abort_lookup_for_waiter, &result);
	}

	finish_lookup(data_vio, result);
}

/**
 * Abort a block map PBN lookup due to an error loading a page.
 *
 * @param data_vio  The data_vio doing the page load
 * @param result    The error code
 **/
static void abort_load(struct data_vio *data_vio, int result)
{
	abort_lookup(data_vio, result, "load");
}

/**
 * Determine if a location represents a valid mapping for a tree page.
 *
 * @param vdo      The vdo
 * @param mapping  The data_location to check
 * @param height   The height of the entry in the tree
 *
 * @return <code>true</code> if the entry represents a invalid page mapping
 **/
__attribute__((warn_unused_result)) static bool
is_invalid_tree_entry(const struct vdo *vdo,
		      const struct data_location *mapping,
		      Height height)
{
	if (!is_valid_location(mapping) || is_compressed(mapping->state) ||
	    (is_mapped_location(mapping) && (mapping->pbn == ZERO_BLOCK))) {
		return true;
	}

	// Roots aren't physical data blocks, so we can't check their PBNs.
	if (height == BLOCK_MAP_TREE_HEIGHT) {
		return false;
	}

	return !is_physical_data_block(vdo->depot, mapping->pbn);
}

/**********************************************************************/
static void load_block_map_page(struct block_map_tree_zone *zone,
				struct data_vio *data_vio);

static void allocate_block_map_page(struct block_map_tree_zone *zone,
				    struct data_vio *data_vio);

/**
 * Continue a block map PBN lookup now that a page has been loaded by
 * descending one level in the tree.
 *
 * @param data_vio  The data_vio doing the lookup
 * @param page      The page which was just loaded
 **/
static void continue_with_loaded_page(struct data_vio *data_vio,
				      struct block_map_page *page)
{
	struct tree_lock *lock = &data_vio->treeLock;
	struct block_map_tree_slot slot = lock->treeSlots[lock->height];
	struct data_location mapping =
		unpack_block_map_entry(&page->entries[slot.blockMapSlot.slot]);
	if (is_invalid_tree_entry(getVDOFromDataVIO(data_vio), &mapping,
			       lock->height)) {
		logErrorWithStringError(VDO_BAD_MAPPING,
					"Invalid block map tree PBN: %" PRIu64 " with state %u for page index %u at height %u",
					mapping.pbn, mapping.state,
					lock->treeSlots[lock->height - 1].pageIndex,
					lock->height - 1);
		abort_load(data_vio, VDO_BAD_MAPPING);
		return;
	}

	if (!is_mapped_location(&mapping)) {
		// The page we need is unallocated
		allocate_block_map_page(get_block_map_tree_zone(data_vio),
					data_vio);
		return;
	}

	lock->treeSlots[lock->height - 1].blockMapSlot.pbn = mapping.pbn;
	if (lock->height == 1) {
		finish_lookup(data_vio, VDO_SUCCESS);
		return;
	}

	// We know what page we need to load next
	load_block_map_page(get_block_map_tree_zone(data_vio), data_vio);
}

/**
 * Continue a block map PBN lookup now that the page load we were waiting on
 * has finished.
 *
 * @param waiter   The data_vio waiting for a page to be loaded
 * @param context  The page which was just loaded
 **/
static void continue_load_for_waiter(struct waiter *waiter, void *context)
{
	struct data_vio *data_vio = waiterAsDataVIO(waiter);
	data_vio->treeLock.height--;
	continue_with_loaded_page(data_vio, (struct block_map_page *) context);
}

/**
 * Finish loading a page now that it has been read in from disk. This callback
 * is registered in load_page().
 *
 * @param completion  The vio doing the page read
 **/
static void finish_block_map_page_load(struct vdo_completion *completion)
{
	struct vio_pool_entry *entry = completion->parent;
	struct data_vio *data_vio = entry->parent;
	struct block_map_tree_zone *zone =
		(struct block_map_tree_zone *) entry->context;
	struct tree_lock *tree_lock = &data_vio->treeLock;

	tree_lock->height--;
	PhysicalBlockNumber pbn =
		tree_lock->treeSlots[tree_lock->height].blockMapSlot.pbn;
	struct tree_page *tree_page = get_tree_page(zone, tree_lock);
	struct block_map_page *page =
		(struct block_map_page *) tree_page->page_buffer;
	Nonce nonce = zone->map_zone->block_map->nonce;
	if (!copy_valid_page(entry->buffer, nonce, pbn, page)) {
		format_block_map_page(page, nonce, pbn, false);
	}
	return_vio_to_pool(zone->vio_pool, entry);

	// Release our claim to the load and wake any waiters
	release_page_lock(data_vio, "load");
	notify_all_waiters(&tree_lock->waiters, continue_load_for_waiter, page);
	continue_with_loaded_page(data_vio, page);
}

/**
 * Handle an error loading a tree page.
 *
 * @param completion  The vio doing the page read
 **/
static void handle_io_error(struct vdo_completion *completion)
{
	int result = completion->result;
	struct vio_pool_entry *entry = completion->parent;
	struct data_vio *data_vio = entry->parent;
	struct block_map_tree_zone *zone =
		(struct block_map_tree_zone *) entry->context;
	return_vio_to_pool(zone->vio_pool, entry);
	abort_load(data_vio, result);
}

/**
 * Read a tree page from disk now that we've gotten a vio with which to do the
 * read. This WaiterCallback is registered in load_block_map_page().
 *
 * @param waiter   The data_vio which requires a page load
 * @param context  The vio pool entry with which to do the read
 **/
static void load_page(struct waiter *waiter, void *context)
{
	struct vio_pool_entry *entry = context;
	struct data_vio *data_vio = waiterAsDataVIO(waiter);

	entry->parent = data_vio;
	entry->vio->completion.callbackThreadID =
		get_block_map_for_zone(data_vio->logical.zone)->thread_id;

	struct tree_lock *lock = &data_vio->treeLock;
	launchReadMetadataVIO(entry->vio,
			      lock->treeSlots[lock->height - 1].blockMapSlot.pbn,
			      finish_block_map_page_load,
			      handle_io_error);
}

/**
 * Attempt to acquire a lock on a page in the block map tree. If the page is
 * already locked, queue up to wait for the lock to be released. If the lock is
 * acquired, the data_vio's treeLock.locked field will be set to true.
 *
 * @param zone      The block_map_tree_zone in which the data_vio operates
 * @param data_vio  The data_vio which desires a page lock
 *
 * @return VDO_SUCCESS or an error
 **/
static int attempt_page_lock(struct block_map_tree_zone *zone,
			     struct data_vio *data_vio)
{
	struct tree_lock *lock = &data_vio->treeLock;
	Height height = lock->height;
	struct block_map_tree_slot tree_slot = lock->treeSlots[height];
	page_key key;
	key.descriptor = (struct page_descriptor) {
		.root_index = lock->rootIndex,
		.height = height,
		.page_index = tree_slot.pageIndex,
		.slot = tree_slot.blockMapSlot.slot,
	};
	lock->key = key.key;

	struct tree_lock *lock_holder;
	int result = int_map_put(zone->loading_pages, lock->key, lock, false,
				 (void **) &lock_holder);
	if (result != VDO_SUCCESS) {
		return result;
	}

	if (lock_holder == NULL) {
		// We got the lock
		data_vio->treeLock.locked = true;
		return VDO_SUCCESS;
	}

	// Someone else is loading or allocating the page we need
	return enqueueDataVIO(&lock_holder->waiters, data_vio,
			      THIS_LOCATION("$F;cb=blockMapTreePage"));
}

/**
 * Load a block map tree page from disk.
 *
 * @param zone      The block_map_tree_zone in which the data_vio operates
 * @param data_vio  The data_vio which requires a page to be loaded
 **/
static void load_block_map_page(struct block_map_tree_zone *zone,
				struct data_vio *data_vio)
{
	int result = attempt_page_lock(zone, data_vio);
	if (result != VDO_SUCCESS) {
		abort_load(data_vio, result);
		return;
	}

	if (data_vio->treeLock.locked) {
		struct waiter *waiter = dataVIOAsWaiter(data_vio);
		waiter->callback = load_page;
		result = acquire_vio_from_pool(zone->vio_pool, waiter);
		if (result != VDO_SUCCESS) {
			abort_load(data_vio, result);
		}
	}
}

/**
 * Set the callback of a data_vio after it has allocated a block map page.
 *
 * @param data_vio  The data_vio
 **/
static void set_post_allocation_callback(struct data_vio *data_vio)
{
	setCallback(dataVIOAsCompletion(data_vio), data_vio->treeLock.callback,
		    data_vio->treeLock.threadID);
}

/**
 * Abort a block map PBN lookup due to an error allocating a page.
 *
 * @param data_vio  The data_vio doing the page allocation
 * @param result    The error code
 **/
static void abort_allocation(struct data_vio *data_vio, int result)
{
	set_post_allocation_callback(data_vio);
	abort_lookup(data_vio, result, "allocation");
}

/**
 * Callback to handle an error while attempting to allocate a page. This
 * callback is used to transfer back to the logical zone along the block map
 * page allocation path.
 *
 * @param completion  The data_vio doing the allocation
 **/
static void allocation_failure(struct vdo_completion *completion)
{
	struct data_vio *data_vio = asDataVIO(completion);
	assertInLogicalZone(data_vio);
	abort_allocation(data_vio, completion->result);
}

/**
 * Continue with page allocations now that a parent page has been allocated.
 *
 * @param waiter   The data_vio which was waiting for a page to be allocated
 * @param context  The physical block number of the page which was just
 *                 allocated
 **/
static void continue_allocation_for_waiter(struct waiter *waiter, void *context)
{
	struct data_vio *data_vio = waiterAsDataVIO(waiter);
	struct tree_lock *tree_lock = &data_vio->treeLock;
	PhysicalBlockNumber pbn = *((PhysicalBlockNumber *) context);

	tree_lock->height--;
	data_vio->treeLock.treeSlots[tree_lock->height].blockMapSlot.pbn = pbn;

	if (tree_lock->height == 0) {
		finish_lookup(data_vio, VDO_SUCCESS);
		return;
	}

	allocate_block_map_page(get_block_map_tree_zone(data_vio), data_vio);
}

/**
 * Finish the page allocation process by recording the allocation in the tree
 * and waking any waiters now that the write lock has been released. This
 * callback is registered in release_block_map_write_lock().
 *
 * @param completion  The data_vio doing the allocation
 **/
static void finish_block_map_allocation(struct vdo_completion *completion)
{
	struct data_vio *data_vio = asDataVIO(completion);
	assertInLogicalZone(data_vio);
	if (completion->result != VDO_SUCCESS) {
		allocation_failure(completion);
		return;
	}

	struct block_map_tree_zone *zone = get_block_map_tree_zone(data_vio);
	struct tree_lock *tree_lock = &data_vio->treeLock;
	struct tree_page *tree_page = get_tree_page(zone, tree_lock);
	Height height = tree_lock->height;

	PhysicalBlockNumber pbn =
		tree_lock->treeSlots[height - 1].blockMapSlot.pbn;

	// Record the allocation.
	struct block_map_page *page =
		(struct block_map_page *) tree_page->page_buffer;
	SequenceNumber old_lock = tree_page->recovery_lock;
	update_block_map_page(page, data_vio, pbn, MAPPING_STATE_UNCOMPRESSED,
			      &tree_page->recovery_lock);

	if (is_waiting(&tree_page->waiter)) {
		// This page is waiting to be written out.
		if (zone->flusher != tree_page) {
			// The outstanding flush won't cover the update we just
			// made, so mark the page as needing another flush.
			set_generation(zone, tree_page, zone->generation, true);
		}
	} else {
		// Put the page on a dirty list
		if (old_lock == 0) {
			initializeRing(&tree_page->node);
		}
		add_to_dirty_lists(zone->dirty_lists,
				   &tree_page->node,
				   old_lock,
				   tree_page->recovery_lock);
	}

	tree_lock->height--;
	if (height > 1) {
		// Format the interior node we just allocated (in memory).
		tree_page = get_tree_page(zone, tree_lock);
		format_block_map_page(tree_page->page_buffer,
				      zone->map_zone->block_map->nonce, pbn,
				      false);
	}

	// Release our claim to the allocation and wake any waiters
	release_page_lock(data_vio, "allocation");
	notify_all_waiters(&tree_lock->waiters, continue_allocation_for_waiter,
			   &pbn);
	if (tree_lock->height == 0) {
		finish_lookup(data_vio, VDO_SUCCESS);
		return;
	}

	allocate_block_map_page(zone, data_vio);
}

/**
 * Release the write lock on a newly allocated block map page now that we
 * have made its journal entries and reference count updates. This callback
 * is registered in set_block_map_page_reference_count().
 *
 * @param completion  The data_vio doing the allocation
 **/
static void release_block_map_write_lock(struct vdo_completion *completion)
{
	struct data_vio *data_vio = asDataVIO(completion);
	struct allocating_vio *allocating_vio =
		dataVIOAsAllocatingVIO(data_vio);
	assertInAllocatedZone(data_vio);
	if (completion->result != VDO_SUCCESS) {
		launchLogicalCallback(data_vio, allocation_failure,
				      THIS_LOCATION(NULL));
		return;
	}

	release_allocation_lock(allocating_vio);
	reset_allocation(allocating_vio);
	launchLogicalCallback(data_vio, finish_block_map_allocation,
			      THIS_LOCATION("$F;cb=finish_block_map_allocation"));
}

/**
 * Set the reference count of a newly allocated block map page to
 * MAXIMUM_REFERENCES now that we have made a recovery journal entry for it.
 * MAXIMUM_REFERENCES is used to prevent deduplication against the block after
 * we release the write lock on it, but before we write out the page.
 *
 * @param completion  The data_vio doing the allocation
 **/
static void
set_block_map_page_reference_count(struct vdo_completion *completion)
{
	struct data_vio *data_vio = asDataVIO(completion);
	assertInAllocatedZone(data_vio);
	if (completion->result != VDO_SUCCESS) {
		launchLogicalCallback(data_vio, allocation_failure,
				      THIS_LOCATION(NULL));
		return;
	}

	struct tree_lock *lock = &data_vio->treeLock;
	PhysicalBlockNumber pbn =
		lock->treeSlots[lock->height - 1].blockMapSlot.pbn;
	completion->callback = release_block_map_write_lock;
	add_slab_journal_entry(get_slab_journal(getVDOFromDataVIO(data_vio)->depot,
						pbn),
			       data_vio);
}

/**
 * Make a recovery journal entry for a newly allocated block map page.
 * This callback is registered in continue_block_map_page_allocation().
 *
 * @param completion  The data_vio doing the allocation
 **/
static void journal_block_map_allocation(struct vdo_completion *completion)
{
	struct data_vio *data_vio = asDataVIO(completion);
	assertInJournalZone(data_vio);
	if (completion->result != VDO_SUCCESS) {
		launchLogicalCallback(data_vio, allocation_failure,
				      THIS_LOCATION(NULL));
		return;
	}

	setAllocatedZoneCallback(data_vio, set_block_map_page_reference_count,
				 THIS_LOCATION(NULL));
	add_recovery_journal_entry(getVDOFromDataVIO(data_vio)->recoveryJournal,
				   data_vio);
}

/**
 * Continue the process of allocating a block map page now that the
 * BlockAllocator has given us a block. This method is supplied as the callback
 * to allocate_data_block() by allocate_block_map_page().
 *
 * @param allocating_vio  The data_vio which is doing the allocation
 **/
static void
continue_block_map_page_allocation(struct allocating_vio *allocating_vio)
{
	struct data_vio *data_vio = allocatingVIOAsDataVIO(allocating_vio);
	if (!hasAllocation(data_vio)) {
		setLogicalCallback(data_vio, allocation_failure,
				   THIS_LOCATION(NULL));
		continueDataVIO(data_vio, VDO_NO_SPACE);
		return;
	}

	PhysicalBlockNumber pbn = allocating_vio->allocation;
	struct tree_lock *lock = &data_vio->treeLock;
	lock->treeSlots[lock->height - 1].blockMapSlot.pbn = pbn;
	set_up_reference_operation_with_lock(BLOCK_MAP_INCREMENT,
					     pbn,
					     MAPPING_STATE_UNCOMPRESSED,
					     allocating_vio->allocation_lock,
					     &data_vio->operation);
	launchJournalCallback(data_vio, journal_block_map_allocation,
			      THIS_LOCATION("$F;cb=journal_block_map_allocation"));
}

/**
 * Allocate a block map page.
 *
 * @param zone      The zone in which the data_vio is operating
 * @param data_vio  The data_vio which needs to allocate a page
 **/
static void allocate_block_map_page(struct block_map_tree_zone *zone,
				    struct data_vio *data_vio)
{
	if (!isWriteDataVIO(data_vio) || isTrimDataVIO(data_vio)) {
		// This is a pure read, the read phase of a read-modify-write,
		// or a trim, so there's nothing left to do here.
		finish_lookup(data_vio, VDO_SUCCESS);
		return;
	}

	int result = attempt_page_lock(zone, data_vio);
	if (result != VDO_SUCCESS) {
		abort_allocation(data_vio, result);
		return;
	}

	if (!data_vio->treeLock.locked) {
		return;
	}

	allocate_data_block(dataVIOAsAllocatingVIO(data_vio),
			    get_allocation_selector(data_vio->logical.zone),
			    VIO_BLOCK_MAP_WRITE_LOCK,
			    continue_block_map_page_allocation);
}

/**********************************************************************/
void lookup_block_map_pbn(struct data_vio *data_vio)
{
	struct block_map_tree_zone *zone = get_block_map_tree_zone(data_vio);
	zone->active_lookups++;
	if (is_draining(&zone->map_zone->state)) {
		finish_lookup(data_vio, VDO_SHUTTING_DOWN);
		return;
	}

	struct tree_lock *lock = &data_vio->treeLock;
	PageNumber page_index = ((lock->treeSlots[0].pageIndex -
				  zone->map_zone->block_map->flat_page_count) /
				 zone->map_zone->block_map->root_count);
	struct block_map_tree_slot tree_slot = {
		.pageIndex = page_index / BLOCK_MAP_ENTRIES_PER_PAGE,
		.blockMapSlot = {
			.pbn = 0,
			.slot = page_index % BLOCK_MAP_ENTRIES_PER_PAGE,
		},
	};

	struct block_map_page *page = NULL;
	for (lock->height = 1; lock->height <= BLOCK_MAP_TREE_HEIGHT;
	     lock->height++) {
		lock->treeSlots[lock->height] = tree_slot;
		page = (struct block_map_page *) (get_tree_page(zone, lock)->page_buffer);
		PhysicalBlockNumber pbn = get_block_map_page_pbn(page);
		if (pbn != ZERO_BLOCK) {
			lock->treeSlots[lock->height].blockMapSlot.pbn = pbn;
			break;
		}

		// Calculate the index and slot for the next level.
		tree_slot.blockMapSlot.slot =
			tree_slot.pageIndex % BLOCK_MAP_ENTRIES_PER_PAGE;
		tree_slot.pageIndex =
			tree_slot.pageIndex / BLOCK_MAP_ENTRIES_PER_PAGE;
	}

	// The page at this height has been allocated and loaded.
	struct data_location mapping =
		unpack_block_map_entry(&page->entries[tree_slot.blockMapSlot.slot]);
	if (is_invalid_tree_entry(getVDOFromDataVIO(data_vio), &mapping,
				  lock->height)) {
		logErrorWithStringError(VDO_BAD_MAPPING,
					"Invalid block map tree PBN: %" PRIu64 " with state %u for page index %u at height %u",
					mapping.pbn, mapping.state,
					lock->treeSlots[lock->height - 1].pageIndex,
					lock->height - 1);
		abort_load(data_vio, VDO_BAD_MAPPING);
		return;
	}

	if (!is_mapped_location(&mapping)) {
		// The page we want one level down has not been allocated, so
		// allocate it.
		allocate_block_map_page(zone, data_vio);
		return;
	}

	lock->treeSlots[lock->height - 1].blockMapSlot.pbn = mapping.pbn;
	if (lock->height == 1) {
		// This is the ultimate block map page, so we're done
		finish_lookup(data_vio, VDO_SUCCESS);
		return;
	}

	// We know what page we need to load.
	load_block_map_page(zone, data_vio);
}

/**********************************************************************/
PhysicalBlockNumber find_block_map_page_pbn(struct block_map *map,
					    PageNumber page_number)
{
	if (page_number < map->flat_page_count) {
		return (BLOCK_MAP_FLAT_PAGE_ORIGIN + page_number);
	}

	RootCount root_index = page_number % map->root_count;
	PageNumber page_index =
		((page_number - map->flat_page_count) / map->root_count);
	SlotNumber slot = page_index % BLOCK_MAP_ENTRIES_PER_PAGE;
	page_index /= BLOCK_MAP_ENTRIES_PER_PAGE;

	struct tree_page *tree_page =
		get_tree_page_by_index(map->forest, root_index, 1, page_index);
	struct block_map_page *page =
		(struct block_map_page *) tree_page->page_buffer;
	if (!is_block_map_page_initialized(page)) {
		return ZERO_BLOCK;
	}

	struct data_location mapping =
		unpack_block_map_entry(&page->entries[slot]);
	if (!is_valid_location(&mapping) || is_compressed(mapping.state)) {
		return ZERO_BLOCK;
	}
	return mapping.pbn;
}

/**********************************************************************/
void write_tree_page(struct tree_page *page, struct block_map_tree_zone *zone)
{
	bool waiting = is_waiting(&page->waiter);
	if (waiting && (zone->flusher == page)) {
		return;
	}

	set_generation(zone, page, zone->generation, waiting);
	if (waiting || page->writing) {
		return;
	}

	enqueue_page(page, zone);
}
