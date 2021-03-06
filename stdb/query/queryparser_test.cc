/*!
 * \file queryparser_test.cc
 */
#include "stdb/query/queryparser.h"

#include "gtest/gtest.h"

#include <sstream>

#include "stdb/common/datetime.h"
#include "stdb/index/seriesparser.h"

namespace stdb {
namespace qp {

static SeriesMatcher global_series_matcher;

void init_series_matcher() {
  static bool inited = false;
  if (inited) return;
  {
    const char* series = "test tag1=1";
    global_series_matcher.add(series , series + strlen(series));
  }
  {
    const char* series = "test tag1=2";
    global_series_matcher.add(series, series + strlen(series));
  }
  {
    const char* series = "test tag1=3";
    global_series_matcher.add(series, series + strlen(series));
  }
  inited = true;
}

static std::string make_suggest_query() {
  std::stringstream str;
  str << "{ \"select\": \"tag-names\",";
  str << "\"metric\": \"test\",";
  str << "\"starts-with\": \"tag1=2\"";
  str << "}";
  return str.str();
}

TEST(TestQueryParser, Test_suggest_query) {
  init_series_matcher();

  std::string suggest_json = make_suggest_query();
  // LOG(INFO) << "suggest_json=" << suggest_json;
  common::Status status;
  boost::property_tree::ptree ptree;
  ErrorMsg error_msg;
  std::tie(status, ptree, error_msg) = QueryParser::parse_json(suggest_json.c_str());
  EXPECT_TRUE(status.IsOk());

  QueryKind query_kind;
  std::tie(status, query_kind, error_msg) = QueryParser::get_query_kind(ptree);
  EXPECT_TRUE(status.IsOk());
  EXPECT_EQ(qp::QueryKind::SELECT, query_kind);

  std::vector<ParamId> ids;
  std::shared_ptr<PlainSeriesMatcher> subtites;
  std::tie(status, subtites, ids, error_msg) = QueryParser::parse_suggest_query(ptree, global_series_matcher);
  EXPECT_EQ(0, ids.size());
}

static std::string make_search_query() {
  std::stringstream str;
  str << "{ \"select\": \"test\",";
  str << "  \"where\": " << "[ { \"tag1\" : \"1\" }, { \"tag1\": \"2\" } ]";
  str << "}";
  return str.str();
}

TEST(TestQueryParser, Test_search_query) {
  init_series_matcher();
  
  std::string query_json = make_search_query();
  common::Status status;
  boost::property_tree::ptree ptree;
  ErrorMsg error_msg;
  std::tie(status, ptree, error_msg) = QueryParser::parse_json(query_json.c_str());
  EXPECT_TRUE(status.IsOk());

  QueryKind query_kind;
  std::tie(status, query_kind, error_msg) = QueryParser::get_query_kind(ptree);
  EXPECT_TRUE(status.IsOk());
  EXPECT_EQ(qp::QueryKind::SELECT, query_kind);

  std::vector<ParamId> ids;
  std::tie(status, ids, error_msg) = QueryParser::parse_search_query(ptree, global_series_matcher);
  EXPECT_TRUE(status.IsOk());
  EXPECT_EQ(2, ids.size());
  EXPECT_EQ(1024, ids[0]);
  EXPECT_EQ(1025, ids[1]);
}

static std::string make_scan_query(Timestamp begin, Timestamp end, OrderBy order) {
  std::stringstream str;
  str << "{ \"select\": \"test\", \"range\": { \"from\": " << "\"" << DateTimeUtil::to_iso_string(begin) << "\"";
  str << ", \"to\": " << "\"" << DateTimeUtil::to_iso_string(end) << "\"" << "},";
  str << "  \"order-by\": " << (order == OrderBy::SERIES ? "\"series\"," : "\"time\",");
  str << "  \"where\": " << "[ { \"tag1\" : \"1\" }, { \"tag1\": \"2\" } ],";
  str << "  \"filter\": " << " { \"test\": { \"gt\": 100 } }";
  str << "}";
  return str.str();
}

TEST(TestQueryParser, Test_scan_query) {
  init_series_matcher();

  std::string query_json = make_scan_query(1136214245999999999ul, 1136215245999999999ul, OrderBy::TIME); 
  common::Status status;
  boost::property_tree::ptree ptree;
  ErrorMsg error_msg;
  std::tie(status, ptree, error_msg) = QueryParser::parse_json(query_json.c_str());
  EXPECT_TRUE(status.IsOk());

  QueryKind query_kind;
  std::tie(status, query_kind, error_msg) = QueryParser::get_query_kind(ptree);
  EXPECT_TRUE(status.IsOk());
  EXPECT_EQ(qp::QueryKind::SELECT, query_kind);

  ReshapeRequest req;
  std::tie(status, req, error_msg) = QueryParser::parse_select_query(ptree, global_series_matcher);
  EXPECT_TRUE(status.IsOk());

  EXPECT_EQ(1, req.select.columns.size());
  EXPECT_EQ(2, req.select.columns[0].ids.size());
  EXPECT_EQ(1024, req.select.columns[0].ids[0]);
  EXPECT_EQ(1025, req.select.columns[0].ids[1]);

  EXPECT_EQ(1136214245999999999ul, req.select.begin);
  EXPECT_EQ(1136215245999999999ul, req.select.end);
  EXPECT_FALSE(req.select.events);
  EXPECT_EQ(0, req.select.event_body_regex.length());
  EXPECT_EQ(1, req.select.filters.size());
  EXPECT_TRUE(req.select.filters[0].enabled);
  EXPECT_EQ(Filter::GT, req.select.filters[0].flags);
  EXPECT_EQ(100, req.select.filters[0].gt);

  switch (req.order_by) {
    case OrderBy::SERIES: {
      EXPECT_TRUE(false);
    } break;
    case OrderBy::TIME: {
      EXPECT_TRUE(true);
    } break;
  }

  switch (req.select.filter_rule) {
    case FilterCombinationRule::ALL: {
      EXPECT_TRUE(true);
      break;
    }
    case FilterCombinationRule::ANY: {
      EXPECT_TRUE(false);
      break;
    }
  }

  std::vector<std::shared_ptr<Node>> nodes;
  InternalCursor* cur = nullptr;
  std::tie(status, nodes, error_msg) = QueryParser::parse_processing_topology(ptree, cur, req);
  EXPECT_EQ(1, nodes.size());
}

static std::string make_select_meta_query() {
  std::stringstream ss;
  ss << "{ \"select\": \"meta:namestest\",";
  ss << "   \"where\":" << "[ { \"tag1\" : \"1\" }, { \"tag1\": \"2\" } ]";
  ss << "}";
  return ss.str();
}

TEST(TestQueryParser, Test_select_meta_query) {
  init_series_matcher();

  std::string query_json = make_select_meta_query();
  common::Status status;
  boost::property_tree::ptree ptree;
  ErrorMsg error_msg;
  std::tie(status, ptree, error_msg) = QueryParser::parse_json(query_json.c_str());
  EXPECT_TRUE(status.IsOk());

  QueryKind query_kind;
  std::tie(status, query_kind, error_msg) = QueryParser::get_query_kind(ptree);
  EXPECT_TRUE(status.IsOk());
  EXPECT_EQ(qp::QueryKind::SELECT_META, query_kind);

  std::vector<ParamId> ids;
  std::tie(status, ids, error_msg) = QueryParser::parse_select_meta_query(ptree, global_series_matcher);

  EXPECT_EQ(common::Status::NotFound(), status);
}

static std::string make_select_events_query(Timestamp begin, Timestamp end, OrderBy order) {
  std::stringstream ss;
  ss << "{ \"select-events\": \"!test\",";
  ss << "  \"range\": { \"from\": " << "\"" << DateTimeUtil::to_iso_string(begin) << "\"";
  ss << ", \"to\": " << "\"" << DateTimeUtil::to_iso_string(end) << "\"" << "},";
  ss << "  \"order-by\": " << (order == OrderBy::SERIES ? "\"series\"," : "\"time\",");
  ss << "  \"where\": " << "[ { \"tag1\" : \"1\" }, { \"tag1\": \"2\" } ],";
  ss << "  \"filter\": " << " { \"test\": { \"gt\": 100 } }";
  ss << "}";
  return ss.str();
}

TEST(TestQueryParser, Test_select_events_query) {
  init_series_matcher();

  std::string query_json = make_select_events_query(1136214245999999999ul, 1136215245999999999ul, OrderBy::TIME);
  common::Status status;
  boost::property_tree::ptree ptree;
  ErrorMsg error_msg;
  std::tie(status, ptree, error_msg) = QueryParser::parse_json(query_json.c_str());
  EXPECT_TRUE(status.IsOk());

  QueryKind query_kind;
  std::tie(status, query_kind, error_msg) = QueryParser::get_query_kind(ptree);
  EXPECT_TRUE(status.IsOk());
  EXPECT_EQ(qp::QueryKind::SELECT_EVENTS, query_kind);

  ReshapeRequest req;
  std::tie(status, req, error_msg) = QueryParser::parse_select_events_query(ptree, global_series_matcher);
  EXPECT_EQ(common::Status::NotFound(), status);
}

}  // namespace qp
}  // namespace stdb
