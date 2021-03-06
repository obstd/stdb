/*!
 * \file eval.h
 */
#ifndef STDB_QUERY_QUERY_PROCESSING_EVAL_H_
#define STDB_QUERY_QUERY_PROCESSING_EVAL_H_

#include <memory>
#include <string>

#include "../queryprocessor_framework.h"

namespace stdb {
namespace qp {

struct MuparserEvalImpl;

struct ExprEval : Node {
  std::shared_ptr<MuparserEvalImpl> impl_;

  ExprEval(const ExprEval&) = delete;
  ExprEval& operator = (const ExprEval&) = delete;

  //! Bild eval node using 'expr' field of the ptree object.
  ExprEval(const boost::property_tree::ptree& expr,
           const ReshapeRequest&              req,
           std::shared_ptr<Node>              next);

  virtual void complete();

  virtual bool put(MutableSample& sample);

  virtual void set_error(common::Status status);

  virtual int get_requirements() const;
};

}  // namespace qp
}  // namespace stdb

#endif  // STDB_QUERY_QUERY_PROCESSING_EVAL_H_
