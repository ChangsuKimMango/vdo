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
 * $Id: //eng/uds-releases/krusty/src/uds/bufferedWriter.h#5 $
 */

#ifndef BUFFERED_WRITER_H
#define BUFFERED_WRITER_H 1

#include "common.h"

#ifdef __KERNEL__
struct dm_bufio_client;
struct io_factory;
#else
struct ioRegion;
#endif

typedef struct bufferedWriter BufferedWriter;

#ifdef __KERNEL__
/**
 * Make a new buffered writer.
 *
 * @param factory       The IO factory creating the buffered writer
 * @param client        The dm_bufio_client to write to.
 * @param block_limit   The number of blocks that may be written to.
 * @param writer_ptr    The new buffered writer goes here.
 *
 * @return UDS_SUCCESS or an error code.
 **/
int __must_check make_buffered_writer(struct io_factory *factory,
				      struct dm_bufio_client *client,
				      sector_t block_limit,
				      BufferedWriter **writer_ptr);
#else
/**
 * Make a new buffered writer.
 *
 * @param region        The IOregion to write to.
 * @param writer_ptr    The new buffered writer goes here.
 *
 * @return UDS_SUCCESS or an error code.
 **/
int __must_check
make_buffered_writer(struct ioRegion *region, BufferedWriter **writer_ptr);
#endif

/**
 * Free a buffered writer, without flushing.
 *
 * @param [in] buffer   The buffered writer object.
 **/
void free_buffered_writer(BufferedWriter *buffer);

/**
 * Append data to buffer, writing as needed.
 *
 * @param buffer        The buffered writer object.
 * @param data          The data to write.
 * @param len           The length of the data written.
 *
 * @return              UDS_SUCCESS or an error code.
 *                      The error may reflect previous attempts to write
 *                      or flush the buffer.  Once a write or flush error
 *                      occurs it is sticky.
 **/
int __must_check
write_to_buffered_writer(BufferedWriter *buffer, const void *data, size_t len);

/**
 * Zero data in the buffer, writing as needed.
 *
 * @param bw            The buffered writer object.
 * @param len           The number of zero bytes to write.
 *
 * @return              UDS_SUCCESS or an error code.
 *                      The error may reflect previous attempts to write
 *                      or flush the buffer.  Once a write or flush error
 *                      occurs it is sticky.
 **/
int __must_check write_zeros_to_buffered_writer(BufferedWriter *bw, size_t len);

/**
 * Flush any partial data from the buffer.
 *
 * @param buffer        The buffered writer object.
 *
 * @return              UDS_SUCCESS or an error code.
 *                      The error may reflect previous attempts to write
 *                      or flush the buffer.  Once a write or flush error
 *                      occurs it is sticky.
 **/
int __must_check flush_buffered_writer(BufferedWriter *buffer);

/**
 * Return the size of the remaining space in the buffer (for testing)
 *
 * @param [in] buffer   The buffered writer object.
 *
 * @return              The number of available bytes in the buffer.
 **/
size_t __must_check space_remaining_in_write_buffer(BufferedWriter *buffer);

/**
 * Return whether the buffer was ever written to.
 *
 * @param buffer        The buffered writer object.
 *
 * @return              True if at least one call to write_to_buffered_writer
 *                      was made.
 **/
bool __must_check was_buffered_writer_used(const BufferedWriter *buffer);

/**
 * Note the buffer has been used.
 *
 * @param buffer        The buffered writer object.
 **/
void note_buffered_writer_used(BufferedWriter *buffer);

#endif // BUFFERED_WRITER_H
