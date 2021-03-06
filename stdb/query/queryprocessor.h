/**
 * \file query_processing.h
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
#ifndef STDB_STORAGE_QUERY_PROCESSING_H_
#define STDB_STORAGE_QUERY_PROCESSING_H_

#include <memory>

#include "stdb/common/basic.h"
#include "stdb/query/queryprocessor_framework.h"

namespace stdb {
namespace qp {


/** Numeric data query processor. Can be used to return raw data
 * from HDD or derivatives (Depending on the list of processing nodes).
 */
struct ScanQueryProcessor : IStreamProcessor {

  //! Root of the processing topology
  std::shared_ptr<Node> root_node_;
  //! Final of the processing topology
  std::shared_ptr<Node> last_node_;

  /** Create new query processor.
   * @param root is a root of the processing topology
   */
  ScanQueryProcessor(std::vector<std::shared_ptr<Node>> nodes, bool group_by_time);

  bool start();
  //! Process value
  bool put(const Sample& sample);
  //! Should be called when processing completed
  void stop();
  //! Set execution error
  void set_error(common::Status error);
};


struct MetadataQueryProcessor : IStreamProcessor {

  std::shared_ptr<Node> root_;
  std::vector<ParamId> ids_;

  MetadataQueryProcessor(std::shared_ptr<Node> node, std::vector<ParamId>&& ids);

  bool start();
  bool put(const Sample& sample);
  void stop();
  void set_error(common::Status error);
};

}  // namespace qp
}  // namespace stdb

#endif  // STDB_STORAGE_QUERY_PROCESSING_H_
