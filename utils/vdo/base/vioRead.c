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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/vioRead.c#11 $
 */

#include "vioRead.h"

#include "logger.h"

#include "blockMap.h"
#include "dataVIO.h"
#include "vdoInternal.h"
#include "vioWrite.h"

/**
 * Do the modify-write part of a read-modify-write cycle. This callback is
 * registered in read_block().
 *
 * @param completion  The data_vio which has just finished its read
 **/
static void modify_for_partial_write(struct vdo_completion *completion)
{
	struct data_vio *data_vio = asDataVIO(completion);
	assertInLogicalZone(data_vio);

	if (completion->result != VDO_SUCCESS) {
		completeDataVIO(completion);
		return;
	}

	applyPartialWrite(data_vio);
	struct vio *vio = dataVIOAsVIO(data_vio);
	vio->operation = VIO_WRITE | (vio->operation & ~VIO_READ_WRITE_MASK);
	data_vio->isPartialWrite = true;
	launch_write_data_vio(data_vio);
}

/**
 * Read a block asynchronously. This is the callback registered in
 * read_block_mapping().
 *
 * @param completion  The data_vio to read
 **/
static void read_block(struct vdo_completion *completion)
{
	if (completion->result != VDO_SUCCESS) {
		completeDataVIO(completion);
		return;
	}

	struct data_vio *data_vio = asDataVIO(completion);
	struct vio *vio = asVIO(completion);
	completion->callback =
		(isReadVIO(vio) ? completeDataVIO : modify_for_partial_write);

	if (data_vio->mapped.pbn == ZERO_BLOCK) {
		zeroDataVIO(data_vio);
		invoke_callback(completion);
		return;
	}

	vio->physical = data_vio->mapped.pbn;
	data_vio->lastAsyncOperation = READ_DATA;
	readDataVIO(data_vio);
}

/**
 * Read the data_vio's mapping from the block map. This callback is registered
 * in launch_read_data_vio().
 *
 * @param completion  The data_vio to be read
 **/
static void read_block_mapping(struct vdo_completion *completion)
{
	if (completion->result != VDO_SUCCESS) {
		completeDataVIO(completion);
		return;
	}

	struct data_vio *data_vio = asDataVIO(completion);
	assertInLogicalZone(data_vio);
	setLogicalCallback(data_vio, read_block,
			   THIS_LOCATION("$F;cb=read_block"));
	data_vio->lastAsyncOperation = GET_MAPPED_BLOCK;
	get_mapped_block_async(data_vio);
}

/**********************************************************************/
void launch_read_data_vio(struct data_vio *data_vio)
{
	assertInLogicalZone(data_vio);
	data_vio->lastAsyncOperation = FIND_BLOCK_MAP_SLOT;
	// Go find the block map slot for the LBN mapping.
	find_block_map_slot_async(data_vio,
				  read_block_mapping,
				  get_logical_zone_thread_id(data_vio->logical.zone));
}

/**
 * Release the logical block lock which a read data_vio obtained now that it
 * is done.
 *
 * @param completion  The data_vio
 **/
static void release_logical_lock(struct vdo_completion *completion)
{
	struct data_vio *data_vio = asDataVIO(completion);
	assertInLogicalZone(data_vio);
	releaseLogicalBlockLock(data_vio);
	vioDoneCallback(completion);
}

/**
 * Clean up a data_vio which has finished processing a read.
 *
 * @param data_vio  The data_vio to clean up
 **/
void cleanup_read_data_vio(struct data_vio *data_vio)
{
	launchLogicalCallback(data_vio, release_logical_lock,
			      THIS_LOCATION("$F;cb=releaseLL"));
}
