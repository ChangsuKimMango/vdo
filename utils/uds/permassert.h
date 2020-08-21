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
 * $Id: //eng/uds-releases/krusty/src/uds/permassert.h#4 $
 */

#ifndef PERMASSERT_H
#define PERMASSERT_H

#include "compiler.h"
#include "errors.h"
#include "uds-error.h"

#define STRINGIFY(X) #X
#define STRINGIFY_VALUE(X) STRINGIFY(X)

/*
 * A hack to apply the "warn if unused" attribute to an integral expression.
 *
 * Since GCC doesn't propagate the warn_unused_result attribute to
 * conditional expressions incorporating calls to functions with that
 * attribute, this function can be used to wrap such an expression.
 * With optimization enabled, this function contributes no additional
 * instructions, but the warn_unused_result attribute still applies to
 * the code calling it.
 *
 * @param value  The value to return
 *
 * @return       The supplied value
 */
static INLINE int __must_check must_use(int value)
{
	return value;
}

/*
 * A replacement for assert() from assert.h.
 *
 * @param expr      The boolean expression being asserted
 * @param code      The error code to return on non-fatal assertion
 *                  failure
 * @param format    A printf() style format for the message to log on
 *                  assertion failure
 * @param arguments Any additional arguments required by the format
 *
 * @return UDS_SUCCESS If expr is true, code if expr is false and
 *         exit_on_assertion_failure is false. When exit_on_assertion_failure
 *         is true and expr is false, the program will exit from within
 *         this macro.
 */
#define ASSERT_WITH_ERROR_CODE(expr, code, ...)            \
	must_use(__builtin_expect(!!(expr), 1) ?           \
			 UDS_SUCCESS :                     \
			 assertion_failed(STRINGIFY(expr), \
					  code,            \
					  __FILE__,        \
					  __LINE__,        \
					  __VA_ARGS__))

/*
 * A replacement for assert() from assert.h.
 *
 * @param expr      The boolean expression being asserted
 * @param format    A printf() style format for the message to log on
 *                  assertion failure
 * @param arguments Any additional arguments required by the format
 *
 * @return UDS_SUCCESS If expr is true, UDS_ASSERTION_FAILED if expr is
 *         false and exit_on_assertion_failure is false. When
 *         exit_on_assertion_failure is true and expr is false, the
 *         program will exit from within this macro.
 */
#define ASSERT(expr, ...) \
	ASSERT_WITH_ERROR_CODE(expr, UDS_ASSERTION_FAILED, __VA_ARGS__)

/*
 * A replacement for assert() which logs on failure, but does not return an
 * error code. This should be used sparingly. If the expression is false and
 * exit_on_assertion_failure is true, the program will exit from within this
 * macro.
 *
 * @param expr      The boolean expression being asserted
 * @param format    A printf() syle format for the message to log on
 *                  assertion failure
 * @param arguments Any additional arguments required by the format
 */
#define ASSERT_LOG_ONLY(expr, ...)          \
	(__builtin_expect(!!(expr), 1) ?    \
		 UDS_SUCCESS :              \
		 assertion_failed_log_only( \
			 STRINGIFY(expr), __FILE__, __LINE__, __VA_ARGS__))

/*
 * This macro is a convenient wrapper for ASSERT(false, ...).
 */
#define ASSERT_FALSE(...) ASSERT(false, __VA_ARGS__)

#define STATIC_ASSERT(expr)  \
	do {                 \
		switch (0) { \
		case 0:      \
		case expr:;  \
		default:;    \
		}            \
	} while (0)

#define STATIC_ASSERT_SIZEOF(type, expected_size) \
	STATIC_ASSERT(sizeof(type) == (expected_size))

/**
 * Set whether or not to exit on an assertion failure.
 *
 * @param should_exit If <code>true</code> assertion failures will cause
 *                    the program to exit
 *
 * @return The previous setting
 **/
bool set_exit_on_assertion_failure(bool should_exit);

/**
 * Log an assertion failure.
 *
 * @param expression_string The assertion
 * @param error_code        The error code to return
 * @param file_name         The file in which the assertion appears
 * @param line_number       The line number on which the assertion
 *                          appears
 * @param format            A printf() style format describing the
 *                          assertion
 *
 * @return The supplied error_code unless exit_on_assertion_failure is
 *         true, in which case the process will be aborted
 **/
int __must_check assertion_failed(const char *expression_string,
				  int error_code,
				  const char *file_name,
				  int line_number,
				  const char *format,
				  ...) __attribute__((format(printf, 5, 6)));

/**
 * Log an assertion failure. This function is different from
 * assertion_failed() in that its return value may be ignored, and so should
 * only be used in cases where the return value will be ignored.
 *
 * @param expression_string The assertion
 * @param file_name         The file in which the assertion appears
 * @param line_number       The line number on which the assertion
 *                          appears
 * @param format            A printf() style format describing the
 *                          assertion
 *
 * @return UDS_ASSERTION_FAILED unless exit_on_assertion_failure is
 *         true, in which case the process will be aborted
 **/
int assertion_failed_log_only(const char *expression_string,
			      const char *file_name,
			      int line_number,
			      const char *format,
			      ...) __attribute__((format(printf, 4, 5)));

#endif /* PERMASSERT_H */
