/*!
 * \file absolute.h
 */
#ifndef STDB_QUERY_QUERY_PROCESSING_ABSOLUTE_H_
#define STDB_QUERY_QUERY_PROCESSING_ABSOLUTE_H_

#include <memory>

#include "stdb/query/queryprocessor_framework.h"

namespace stdb {
namespace qp {

/** Returns absolute value
*/
struct Absolute : Node {

  std::vector<double> weights_;
  std::shared_ptr<Node> next_;

  Absolute(std::shared_ptr<Node> next);

  Absolute(const boost::property_tree::ptree&, const ReshapeRequest &, std::shared_ptr<Node> next);

  virtual void complete();

  virtual bool put(MutableSample& sample);

  virtual void set_error(common::Status status);

  virtual int get_requirements() const;
};

}  // namespace qp
}  // namespace stdb

#endif  // STDB_QUERY_QUERY_PROCESSING_ABSOLUTE_H_
