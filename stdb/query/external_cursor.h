/*!
 * \file external_cursor.h
 */
#ifndef STDB_QUERY_EXTERNAL_CURSOR_H_
#define STDB_QUERY_EXTERNAL_CURSOR_H_

#include "stdb/common/basic.h"

namespace stdb {

/** Data retreival interface that can be used by
 *  code that reads data from FastSTDB
 */
struct ExternalCursor {

  /** New read interface for variably sized samples.
   * @param buffer is an array of Sample structs
   * @param item_size defines size of each struct 0 - size = sizeof(Sample)
   * @param buffer_size defines size of the buffer in bytes (should be a multiple of item_size)
   * @return number of overwritten bytes in `buffer`
   */
  virtual u32 read(void* buffer, u32 buffer_size) = 0;

  //! Check is everything done
  virtual bool is_done() const = 0;

  //! Check if error occured and (optionally) get the error code
  virtual bool is_error(common::Status* out_error_code_or_null = nullptr) const = 0;

  //! Check if error occured and get the zero-terminated error message and error code
  virtual bool is_error(const char** buffer, common::Status* error_code) const = 0;

  //! Finalizer
  virtual void close() = 0;

  virtual ~ExternalCursor() = default;
};

}  // namespace stdb

#endif  // STDB_QUERY_EXTERNAL_CURSOR_H_
