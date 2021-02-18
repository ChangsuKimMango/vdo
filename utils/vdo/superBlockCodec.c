/*
 * Copyright Red Hat
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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/superBlockCodec.c#8 $
 */

#include "superBlockCodec.h"

#include "buffer.h"
#include "logger.h"
#include "memoryAlloc.h"
#include "permassert.h"

#include "checksum.h"
#include "constants.h"
#include "header.h"
#include "physicalLayer.h"
#include "statusCodes.h"

enum {
	SUPER_BLOCK_FIXED_SIZE = ENCODED_HEADER_SIZE + CHECKSUM_SIZE,
	MAX_COMPONENT_DATA_SIZE = VDO_SECTOR_SIZE - SUPER_BLOCK_FIXED_SIZE,
};

static const struct header SUPER_BLOCK_HEADER_12_0 = {
	.id = SUPER_BLOCK,
	.version =
		{
			.major_version = 12,
			.minor_version = 0,
		},

	// This is the minimum size, if the super block contains no components.
	.size = SUPER_BLOCK_FIXED_SIZE - ENCODED_HEADER_SIZE,
};

/**********************************************************************/
int initialize_super_block_codec(struct super_block_codec *codec)
{
	int result = make_buffer(MAX_COMPONENT_DATA_SIZE,
				 &codec->component_buffer);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = ALLOCATE(VDO_BLOCK_SIZE, char, "encoded super block",
			  (char **) &codec->encoded_super_block);
	if (result != UDS_SUCCESS) {
		return result;
	}

	// Even though the buffer is a full block, to avoid the potential
	// corruption from a torn write, the entire encoding must fit in the
	// first sector.
	return wrap_buffer(codec->encoded_super_block,
			   VDO_SECTOR_SIZE,
			   0,
			   &codec->block_buffer);
}

/**********************************************************************/
void destroy_super_block_codec(struct super_block_codec *codec)
{
	free_buffer(&codec->block_buffer);
	free_buffer(&codec->component_buffer);
	FREE(codec->encoded_super_block);
}

/**********************************************************************/
int encode_super_block(struct super_block_codec *codec)
{
	size_t component_data_size;
	struct header header = SUPER_BLOCK_HEADER_12_0;
	crc32_checksum_t checksum;
	struct buffer *buffer = codec->block_buffer;
	int result = reset_buffer_end(buffer, 0);
	if (result != VDO_SUCCESS) {
		return result;
	}

	component_data_size = content_length(codec->component_buffer);

	// Encode the header.
	header.size += component_data_size;
	result = encode_header(&header, buffer);
	if (result != UDS_SUCCESS) {
		return result;
	}

	// Copy the already-encoded component data.
	result = put_bytes(buffer, component_data_size,
			  get_buffer_contents(codec->component_buffer));
	if (result != UDS_SUCCESS) {
		return result;
	}

	// Compute and encode the checksum.
	checksum = update_crc32(INITIAL_CHECKSUM, codec->encoded_super_block,
			        content_length(buffer));
	result = put_uint32_le_into_buffer(buffer, checksum);
	if (result != UDS_SUCCESS) {
		return result;
	}

	return UDS_SUCCESS;
}

/**********************************************************************/
int decode_super_block(struct super_block_codec *codec)
{
	struct header header;
	int result;
	size_t component_data_size;
	crc32_checksum_t checksum, saved_checksum;

	// Reset the block buffer to start decoding the entire first sector.
	struct buffer *buffer = codec->block_buffer;
	clear_buffer(buffer);

	// Decode and validate the header.
	result = decode_header(buffer, &header);
	if (result != VDO_SUCCESS) {
		return result;
	}

	result = validate_header(&SUPER_BLOCK_HEADER_12_0, &header, false,
				 __func__);
	if (result != VDO_SUCCESS) {
		return result;
	}

	if (header.size > content_length(buffer)) {
		// We can't check release version or checksum until we know the
		// content size, so we have to assume a version mismatch on
		// unexpected values.
		return log_error_strerror(VDO_UNSUPPORTED_VERSION,
					  "super block contents too large: %zu",
					  header.size);
	}

	// Restrict the buffer to the actual payload bytes that remain.
	result =
		reset_buffer_end(buffer, uncompacted_amount(buffer) + header.size);
	if (result != VDO_SUCCESS) {
		return result;
	}

	// The component data is all the rest, except for the checksum.
	component_data_size =
		content_length(buffer) - sizeof(crc32_checksum_t);
	result = put_buffer(codec->component_buffer, buffer,
			    component_data_size);
	if (result != VDO_SUCCESS) {
		return result;
	}

	// Checksum everything up to but not including the saved checksum
	// itself.
	checksum = update_crc32(INITIAL_CHECKSUM, codec->encoded_super_block,
			        uncompacted_amount(buffer));

	// Decode and verify the saved checksum.
	result = get_uint32_le_from_buffer(buffer, &saved_checksum);
	if (result != VDO_SUCCESS) {
		return result;
	}

	result = ASSERT(content_length(buffer) == 0,
			"must have decoded entire superblock payload");
	if (result != VDO_SUCCESS) {
		return result;
	}

	return ((checksum != saved_checksum) ? VDO_CHECKSUM_MISMATCH
		: VDO_SUCCESS);
}

/**********************************************************************/
size_t get_fixed_super_block_size(void)
{
	return SUPER_BLOCK_FIXED_SIZE;
}
