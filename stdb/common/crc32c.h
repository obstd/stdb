/*!
 * \file crc32.h
 */
#ifndef STDB_COMMON_CRC32_H_
#define STDB_COMMON_CRC32_H_

#include <stdint.h>
#include <stddef.h>

namespace stdb {
namespace common {

typedef uint32_t (*crc32c_impl_t)(uint32_t crc, const void *buf, size_t len);

enum class CRC32C_hint {
  DETECT,
  FORCE_SW,
  FORCE_HW,
};

//! Return crc32c implementation.
crc32c_impl_t chose_crc32c_implementation(CRC32C_hint hint=CRC32C_hint::DETECT);

}  // namespace common
}  // namespace stdb

#endif  // STDB_COMMON_CRC32_H_
