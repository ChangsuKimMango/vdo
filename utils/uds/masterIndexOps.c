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
 * $Id: //eng/uds-releases/krusty/src/uds/masterIndexOps.c#14 $
 */
#include "masterIndexOps.h"

#include "compiler.h"
#include "errors.h"
#include "geometry.h"
#include "indexComponent.h"
#include "logger.h"
#include "masterIndex005.h"
#include "masterIndex006.h"
#include "memoryAlloc.h"
#include "permassert.h"
#include "uds.h"
#include "zone.h"

/**********************************************************************/
static INLINE bool uses_sparse(const struct configuration *config)
{
	return is_sparse(config->geometry);
}

/**********************************************************************/
void get_master_index_combined_stats(const struct master_index *master_index,
				     struct master_index_stats *stats)
{
	struct master_index_stats dense, sparse;
	get_master_index_stats(master_index, &dense, &sparse);
	stats->memory_allocated =
		dense.memory_allocated + sparse.memory_allocated;
	stats->rebalance_time = dense.rebalance_time + sparse.rebalance_time;
	stats->rebalance_count =
		dense.rebalance_count + sparse.rebalance_count;
	stats->record_count = dense.record_count + sparse.record_count;
	stats->collision_count =
		dense.collision_count + sparse.collision_count;
	stats->discard_count = dense.discard_count + sparse.discard_count;
	stats->overflow_count = dense.overflow_count + sparse.overflow_count;
	stats->num_lists = dense.num_lists + sparse.num_lists;
	stats->early_flushes = dense.early_flushes + sparse.early_flushes;
}

/**********************************************************************/
int make_master_index(const struct configuration *config,
		      unsigned int num_zones,
		      uint64_t volume_nonce,
		      struct master_index **master_index)
{
	if (uses_sparse(config)) {
		return make_master_index006(config, num_zones, volume_nonce,
					    master_index);
	} else {
		return make_master_index005(config, num_zones, volume_nonce,
					    master_index);
	}
}

/**********************************************************************/
int compute_master_index_save_blocks(const struct configuration *config,
				     size_t block_size,
				     uint64_t *block_count)
{
	size_t num_bytes;
	int result = (uses_sparse(config) ?
			      compute_master_index_save_bytes006(config,
								 &num_bytes) :
			      compute_master_index_save_bytes005(config,
								 &num_bytes));
	if (result != UDS_SUCCESS) {
		return result;
	}
	num_bytes += sizeof(struct delta_list_save_info);
	*block_count = (num_bytes + block_size - 1) / block_size + MAX_ZONES;
	return UDS_SUCCESS;
}

/**********************************************************************/
static int read_master_index(struct read_portal *portal)
{
	struct master_index *master_index =
		index_component_context(portal->component);
	unsigned int num_zones = portal->zones;
	if (num_zones > MAX_ZONES) {
		return logErrorWithStringError(UDS_BAD_STATE,
					       "zone count %u must not exceed MAX_ZONES",
					       num_zones);
	}

	struct buffered_reader *readers[MAX_ZONES];
	unsigned int z;
	for (z = 0; z < num_zones; ++z) {
		int result =
			get_buffered_reader_for_portal(portal, z, &readers[z]);
		if (result != UDS_SUCCESS) {
			return logErrorWithStringError(result,
						       "cannot read component for zone %u",
						       z);
		}
	}
	return restore_master_index(readers, num_zones, master_index);
}

/**********************************************************************/
static int write_master_index(struct index_component *component,
			      struct buffered_writer *writer,
			      unsigned int zone,
			      enum incremental_writer_command command,
			      bool *completed)
{
	struct master_index *master_index = index_component_context(component);
	bool is_complete = false;

	int result = UDS_SUCCESS;

	switch (command) {
	case IWC_START:
		result = start_saving_master_index(master_index, zone, writer);
		is_complete = result != UDS_SUCCESS;
		break;
	case IWC_CONTINUE:
		is_complete = is_saving_master_index_done(master_index, zone);
		break;
	case IWC_FINISH:
		result = finish_saving_master_index(master_index, zone);
		if (result == UDS_SUCCESS) {
			result = write_guard_delta_list(writer);
		}
		is_complete = true;
		break;
	case IWC_ABORT:
		result = abort_saving_master_index(master_index, zone);
		is_complete = true;
		break;
	default:
		result = logWarningWithStringError(UDS_INVALID_ARGUMENT,
						   "Invalid writer command");
		break;
	}
	if (completed != NULL) {
		*completed = is_complete;
	}
	return result;
}

/**********************************************************************/

static const struct index_component_info MASTER_INDEX_INFO_DATA = {
	.kind = RL_KIND_MASTER_INDEX,
	.name = "master index",
	.save_only = false,
	.chapter_sync = false,
	.multi_zone = true,
	.io_storage = true,
	.loader = read_master_index,
	.saver = NULL,
	.incremental = write_master_index,
};
const struct index_component_info *const MASTER_INDEX_INFO =
	&MASTER_INDEX_INFO_DATA;

/**********************************************************************/
static int restore_master_index_body(struct buffered_reader **buffered_readers,
				     unsigned int num_readers,
				     struct master_index *master_index,
				     byte dl_data[DELTA_LIST_MAX_BYTE_COUNT])
{
	// Start by reading the "header" section of the stream
	int result = start_restoring_master_index(master_index,
						  buffered_readers,
						  num_readers);
	if (result != UDS_SUCCESS) {
		return result;
	}
	// Loop to read the delta lists, stopping when they have all been
	// processed.
	unsigned int z;
	for (z = 0; z < num_readers; z++) {
		for (;;) {
			struct delta_list_save_info dlsi;
			result = read_saved_delta_list(&dlsi, dl_data,
						       buffered_readers[z]);
			if (result == UDS_END_OF_FILE) {
				break;
			} else if (result != UDS_SUCCESS) {
				abort_restoring_master_index(master_index);
				return result;
			}
			result = restore_delta_list_to_master_index(master_index,
								    &dlsi,
								    dl_data);
			if (result != UDS_SUCCESS) {
				abort_restoring_master_index(master_index);
				return result;
			}
		}
	}
	if (!is_restoring_master_index_done(master_index)) {
		abort_restoring_master_index(master_index);
		return logWarningWithStringError(UDS_CORRUPT_COMPONENT,
						 "incomplete delta list data");
	}
	return UDS_SUCCESS;
}

/**********************************************************************/
int restore_master_index(struct buffered_reader **buffered_readers,
			 unsigned int num_readers,
			 struct master_index *master_index)
{
	byte *dl_data;
	int result =
		ALLOCATE(DELTA_LIST_MAX_BYTE_COUNT, byte, __func__, &dl_data);
	if (result != UDS_SUCCESS) {
		return result;
	}
	result = restore_master_index_body(buffered_readers, num_readers,
					   master_index, dl_data);
	FREE(dl_data);
	return result;
}
