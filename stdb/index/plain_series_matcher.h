/**
 * \file plain_series_matcher.h
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#ifndef STDB_INDEX_PLAIN_SERIES_MATCHER_H_
#define STDB_INDEX_PLAIN_SERIES_MATCHER_H_

#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <stdint.h>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "stdb/index/rtree.h"
#include "stdb/index/series_matcher_base.h"

namespace stdb {

/** Series matcher. Table that maps series names to series
  * ids.
  * Implements simple forward index. Can be used to map ids
  * to names and names to ids. Can search index using regular
  * expressions.
  */
struct PlainSeriesMatcher : public SeriesMatcherBase {
  typedef StringTools::TableT TableT;
  typedef StringTools::InvT   InvT;

  // Variables
  rtree::RTree<LocationType, RTREE_NDIMS, RTREE_BLOCK_SIZE> rtree_index;
  LegacyStringPool         pool;       //! String pool that stores time-series
  TableT                   table;      //! Series table (name to id mapping)
  InvT                     inv_table;  //! Ids table (id to name mapping)
  i64                      series_id;  //! Series ID counter
  std::vector<SeriesNameT> names;      //! List of recently added names
  std::vector<Location> locations;     //! List of recently added locations
  mutable std::mutex       mutex;      //! Mutex for shared data

  PlainSeriesMatcher(i64 starting_id = STDB_STARTING_SERIES_ID);

  i64 add(const char* begin, const char* end) override;
  i64 add(const char* begin, const char* end, const Location& location) override;

  void _add(const std::string& series, i64 id) override;
  void _add(const std::string& series, const Location& location, i64 id) override;

  void _add(const char* begin, const char* end, i64 id) override;
  void _add(const char* begin, const char* end, const Location& location, i64 id) override;

  /** Match string and return it's id. If string is new return 0.
  */
  i64 match(const char* begin, const char* end) const override;

  //! Convert id to string
  StringT id2str(i64 tokenid) const override;

  /** Push all new elements to the buffer.
   * @param buffer is an output parameter that will receive new elements
   */
  void pull_new_series(std::vector<SeriesNameT>* buffer) override;

  /**
   * pull all new names and locations to the buffer
   */
  void pull_new_series(std::vector<SeriesNameT>* name_buffer, std::vector<Location>* location_buffer) override;

  /**
   * get all the ids
   */
  std::vector<i64> get_all_ids() const override;

  std::vector<SeriesNameT> regex_match(const char* rexp) const;

  std::vector<SeriesNameT> regex_match(const char* rexp, StringPoolOffset* offset, size_t* prevsize) const;

 protected:
  i64 add_impl(const char* begin, const char* end);
};

}  // namespace stdb

#endif  // STDB_INDEX_PLAIN_SERIES_MATCHER_H_
