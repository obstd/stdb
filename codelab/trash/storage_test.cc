/*!
 * \file storage.cc
 */
#include <iostream>

#include <vector>

#include "stdb/core/metadatastorage.h"
#include "stdb/core/storage.h"
#include "stdb/query/queryparser.h"
#include "stdb/query/queryprocessor_framework.h"

// To initialize apr and sqlite properly
#include <apr.h>
#include <sqlite3.h>

#include "gtest/gtest.h"

namespace stdb {

using namespace storage;
using namespace qp;

struct Initializer {
  Initializer() {
    sqlite3_initialize();
    apr_initialize();

    apr_pool_t *pool = nullptr;
    auto status = apr_pool_create(&pool, NULL);
    if (status != APR_SUCCESS) {
      LOG(FATAL) << "Can't create memory pool";
    }
    apr_dbd_init(pool);
  }
};

static Initializer initializer;

std::shared_ptr<MetadataStorage> create_metadatastorage() {
  // Create in-memory sqlite database.
  std::shared_ptr<MetadataStorage> meta;
  meta.reset(new MetadataStorage(":memory:"));
  return meta;
}

std::shared_ptr<ColumnStore> create_cstore() {
  std::shared_ptr<BlockStore> bstore = BlockStoreBuilder::create_memstore();
  std::shared_ptr<ColumnStore> cstore;
  cstore.reset(new ColumnStore(bstore));
  return cstore;
}

std::shared_ptr<Storage> create_storage(bool start_worker = false) {
  auto meta = create_metadatastorage();
  auto bstore = BlockStoreBuilder::create_memstore();
  auto cstore = create_cstore();
  auto store = std::make_shared<Storage>(meta, bstore, cstore, start_worker);
  return store;
}

TEST(TestMetadataStorage, Test_metadata_storage_volumes_config) {
  MetadataStorage db(":memory:");
  std::vector<MetadataStorage::VolumeDesc> volumes = {
    { 0, "first", 1, 2, 3, 4 },
    { 1, "second", 5, 6, 7, 8 },
    { 2, "third", 9, 10, 11, 12 },
  };
  db.init_volumes(volumes);
  auto actual = db.get_volumes();
  for (size_t i = 0; i < 3; i++) {
    EXPECT_EQ(volumes.at(i).id, actual.at(i).id);
    EXPECT_EQ(volumes.at(i).path, actual.at(i).path);
    EXPECT_EQ(volumes.at(i).capacity, actual.at(i).capacity);
    EXPECT_EQ(volumes.at(i).generation, actual.at(i).generation);
    EXPECT_EQ(volumes.at(i).nblocks, actual.at(i).nblocks);
    EXPECT_EQ(volumes.at(i).version, actual.at(i).version);
  }
}

TEST(TestMetadataStorage, Test_metadata_storage_numeric_config) {
  MetadataStorage db(":memory:");
  const char* creation_datetime = "2015-02-03 00:00:00";  // Formatting not required
  const char* bstore_type = "FixedSizeFileStorage";
  const char* db_name = "db_test";
  db.init_config(db_name, creation_datetime, bstore_type);

  std::string actual_dt;
  bool success = db.get_config_param("creation_datetime", &actual_dt);
  EXPECT_TRUE(success);
  EXPECT_EQ(creation_datetime, actual_dt);
  std::string actual_bstore_type;
  success = db.get_config_param("blockstore_type", &actual_bstore_type);
  EXPECT_TRUE(success);
  EXPECT_EQ(bstore_type, actual_bstore_type);
  std::string actual_db_name;
  success = db.get_config_param("db_name", &actual_db_name);
  EXPECT_TRUE(success);
  EXPECT_STREQ(db_name, actual_db_name.c_str());
  std::string actual_version;
  success = db.get_config_param("storage_version", &actual_version);
  EXPECT_TRUE(success);
  EXPECT_EQ(actual_version, std::to_string(STDB_VERSION));
}

TEST(TestStorage, Test_storage_add_series_1) {
  common::Status status;
  const char* sname = "hello world=1";
  const char* end = sname + strlen(sname);

  auto store = create_storage();
  auto sessiona = store->create_write_session();
  auto sessionb = store->create_write_session();

  Sample samplea;
  status = sessiona->init_series_id(sname, end, &samplea);
  EXPECT_EQ(status, common::Status::Ok());

  Sample sampleb;
  // Should initialize from global data
  status = sessionb->init_series_id(sname, end, &sampleb);
  EXPECT_EQ(status, common::Status::Ok());

  EXPECT_EQ(samplea.paramid, sampleb.paramid);

  // Should read local data
  status = sessionb->init_series_id(sname, end, &sampleb);
  EXPECT_EQ(status, common::Status::Ok());

  EXPECT_EQ(samplea.paramid, sampleb.paramid);
}

TEST(TestStorage, Test_storage_add_values_1) {
  common::Status status;
  const char* sname = "hello world=1";
  const char* end = sname + strlen(sname);

  auto store = create_storage();
  auto sessiona = store->create_write_session();
  auto sessionb = store->create_write_session();

  Sample samplea;
  samplea.payload.type = PAYLOAD_FLOAT;
  samplea.timestamp = 111;
  samplea.payload.float64 = 111;
  status = sessiona->init_series_id(sname, end, &samplea);
  EXPECT_EQ(status, common::Status::Ok());
  status = sessiona->write(samplea);
  EXPECT_EQ(status, common::Status::Ok());

  Sample sampleb;
  sampleb.payload.type = PAYLOAD_FLOAT;
  sampleb.timestamp = 222;
  sampleb.payload.float64 = 222;
  // Should initialize from global data
  status = sessionb->init_series_id(sname, end, &sampleb);
  EXPECT_EQ(status, common::Status::Ok());
  status = sessionb->write(sampleb);
  EXPECT_EQ(status, common::Status::Ok());

  EXPECT_EQ(samplea.paramid, sampleb.paramid);

  // Should read local data
  sampleb.timestamp = 333;
  sampleb.payload.float64 = 333;
  status = sessiona->init_series_id(sname, end, &sampleb);
  EXPECT_EQ(status, common::Status::Ok());
  status = sessiona->write(sampleb);
  EXPECT_EQ(status, common::Status::Ok());
}

TEST(TestStorage, Test_storage_add_values_2) {
  common::Status status;
  const char* sname = "hello world=1";
  const char* end = sname + strlen(sname);

  auto store = create_storage();
  auto sessiona = store->create_write_session();
  {
    auto sessionb = store->create_write_session();

    Sample sample;
    sample.payload.type = PAYLOAD_FLOAT;
    sample.timestamp = 111;
    sample.payload.float64 = 111;
    status = sessionb->init_series_id(sname, end, &sample);
    EXPECT_EQ(status, common::Status::Ok());
    status = sessionb->write(sample);
    EXPECT_EQ(status, common::Status::Ok());

    // disatcher should be freed and registry entry should be returned
  }
  Sample sample;
  sample.payload.type = PAYLOAD_FLOAT;
  sample.timestamp = 222;
  sample.payload.float64 = 222;

  status = sessiona->init_series_id(sname, end, &sample);
  EXPECT_EQ(status, common::Status::Ok());
  status = sessiona->write(sample);
  EXPECT_EQ(status, common::Status::Ok());

  EXPECT_EQ(sample.paramid, sample.paramid);
}

// Test read queries
void fill_data(std::shared_ptr<StorageSession> session,
               Timestamp begin,
               Timestamp end,
               std::vector<std::string> const& names) {
  for (Timestamp ts = begin; ts < end; ts++) {
    for (auto it: names) {
      Sample sample;
      sample.timestamp = ts;
      sample.payload.type = PAYLOAD_FLOAT;
      sample.payload.float64 = double(ts) / 10.0;
      auto status = session->init_series_id(it.data(), it.data() + it.size(), &sample);
      EXPECT_EQ(status, common::Status::Ok());
      status = session->write(sample);
      EXPECT_EQ(status, common::Status::Ok());
    }
  }
}

void fill_data(std::shared_ptr<StorageSession> session,
               std::vector<std::string> const& names,
               std::vector<Timestamp> const& tss,
               std::vector<double> const& xss) {
  assert(tss.size() == xss.size());
  for (u32 ix = 0; ix < tss.size(); ix++) {
    auto ts = tss.at(ix);
    auto xs = xss.at(ix);
    for (auto it: names) {
      Sample sample;
      sample.payload.type = PAYLOAD_FLOAT;
      sample.timestamp = ts;
      sample.payload.float64 = xs;
      // if (xs > 0 && xs < 1.0E-200) {
      //  LOG(ERROR) << "bug";
      // }
      auto status = session->init_series_id(it.data(), it.data() + it.size(), &sample);
      EXPECT_EQ(status, common::Status::Ok());

      status = session->write(sample);
      EXPECT_EQ(status, common::Status::Ok());
    }
  }
}

struct CursorMock : InternalCursor {
  bool done;
  std::vector<Sample> samples;
  std::vector<std::vector<double>> tuples;
  common::Status error;
  std::string error_msg;

  CursorMock() {
    done = false;
    error = common::Status::Ok();
  }

  virtual bool put(const Sample &val) override {
    if (done) {
      LOG(FATAL) << "Cursor invariant broken";
    }
    samples.push_back(val);
    if (val.payload.type & PData::TUPLE_BIT) {
      std::vector<double> outtup;
      u64 bits;
      memcpy(&bits, &val.payload.float64, sizeof(u64));
      double const* tuple = reinterpret_cast<double const*>(val.payload.data);
      int nelements = bits >> 58;
      int tup_ix = 0;
      for (int ix = 0; ix < nelements; ix++) {
        if (bits & (1 << ix)) {
          outtup.push_back(tuple[tup_ix++]);
        } else {
          // Empty tuple value. Use NaN to represent the hole.
          outtup.push_back(NAN);
        }
      }
      tuples.push_back(std::move(outtup));
    }
    return true;
  }

  virtual void complete() override {
    if (done) {
      LOG(FATAL) << "Cursor invariant broken";
    }
    done = true;
  }

  virtual void set_error( common::Status error_code) override {
    if (done) {
      LOG(FATAL) << "Cursor invariant broken";
    }
    done = true;
    error = error_code;
  }

  virtual void set_error(common::Status error_code, const char* error_message) override {
    if (done) {
      LOG(FATAL) << "Cursor invariant broken";
    }
    done = true;
    error = error_code;
    error_msg = error_message;
  }
};

std::string make_scan_query(Timestamp begin, Timestamp end, OrderBy order) {
  std::stringstream str;
  str << "{ \"select\": \"test\", \"range\": { \"from\": " << begin << ", \"to\": " << end << "},";
  str << "  \"order-by\": " << (order == OrderBy::SERIES ? "\"series\"" : "\"time\"");
  str << "}";
  return str.str();
}

void check_timestamps(CursorMock const& mock,
                      std::vector<Timestamp> expected,
                      OrderBy order,
                      std::vector<std::string> names) {
  size_t tsix = 0;
  if (order == OrderBy::SERIES) {
    for (auto s: names) {
      for (auto expts: expected) {
        EXPECT_EQ(expts, mock.samples.at(tsix).timestamp);
        tsix++;
      }
    }
    EXPECT_EQ(tsix, mock.samples.size());
  } else {
    for (auto expts: expected) {
      for (auto s: names) {
        EXPECT_EQ(expts, mock.samples.at(tsix).timestamp);
        tsix++;
      }
    }
    EXPECT_EQ(tsix, mock.samples.size());
  }
}

static void check_paramids(StorageSession& session,
                           CursorMock const& cursor,
                           OrderBy order,
                           std::vector<std::string> expected_series_names,
                           size_t nelem,
                           bool reverse_dir) {
  if (order == OrderBy::SERIES) {
    auto elperseries = nelem / expected_series_names.size();
    assert(nelem % expected_series_names.size() == 0);
    size_t iter = 0;
    for (auto expected: expected_series_names) {
      for (size_t i = 0; i < elperseries; i++) {
        const size_t buffer_size = 1024;
        char buffer[buffer_size];
        auto id = cursor.samples.at(iter++).paramid;
        auto len = session.get_series_name(id, buffer, buffer_size);
        EXPECT_GT(len, 0);
        std::string actual(buffer, buffer + len);
        EXPECT_EQ(actual, expected);
      }
    }
    EXPECT_EQ(cursor.samples.size(), iter);
  } else {
    if (reverse_dir) {
      std::reverse(expected_series_names.begin(), expected_series_names.end());
    }
    auto elperseries = nelem / expected_series_names.size();
    assert(nelem % expected_series_names.size() == 0);
    size_t iter = 0;
    for (size_t i = 0; i < elperseries; i++) {
      for (auto expected: expected_series_names) {
        const size_t buffer_size = 1024;
        char buffer[buffer_size];
        auto id = cursor.samples.at(iter++).paramid;
        auto len = session.get_series_name(id, buffer, buffer_size);
        EXPECT_GT(len, 0);
        std::string actual(buffer, buffer + len);
        EXPECT_EQ(actual, expected);
      }
    }
    EXPECT_EQ(cursor.samples.size(), iter);
  }
}

void test_storage_read_query(Timestamp begin, Timestamp end, OrderBy order) {
  std::vector<std::string> series_names = {
    "test key=0",
    "test key=1",
    "test key=2",
    "test key=3",
    "test key=4",
    "test key=5",
    "test key=6",
    "test key=7",
    "test key=8",
    "test key=9",
  };
  auto storage = create_storage();
  auto session = storage->create_write_session();
  fill_data(session, std::min(begin, end), std::max(begin, end), series_names);
  CursorMock cursor;
  auto query = make_scan_query(begin, end, order);
  session->query(&cursor, query.c_str());
  EXPECT_TRUE(cursor.done);
  EXPECT_EQ(cursor.error, common::Status::Ok());
  size_t expected_size;
  if (begin < end) {
    expected_size = (end - begin) * series_names.size();
  } else {
    // because we will read data in (end, begin] range but
    // fill data in [end, begin) range
    expected_size = (begin - end - 1) * series_names.size();
  }
  EXPECT_EQ(cursor.samples.size(), expected_size);
  std::vector<Timestamp> expected;
  if (begin < end) {
    for (Timestamp ts = begin; ts < end; ts++) {
      expected.push_back(ts);
    }
  } else {
    for (Timestamp ts = begin - 1; ts > end; ts--) {
      expected.push_back(ts);
    }
  }
  check_timestamps(cursor, expected, order, series_names);
  check_paramids(*session, cursor, order, series_names, expected_size, begin > end);
}

TEST(TestStorage, Test_storage_query) {
  std::vector<std::tuple<Timestamp, Timestamp, OrderBy>> input = {
    std::make_tuple( 100ul,  200ul, OrderBy::TIME),
    std::make_tuple( 200ul,  100ul, OrderBy::TIME),
    std::make_tuple(1000ul, 2000ul, OrderBy::TIME),
    std::make_tuple(2000ul, 1000ul, OrderBy::TIME),
    std::make_tuple( 100ul,  200ul, OrderBy::SERIES),
    std::make_tuple( 200ul,  100ul, OrderBy::SERIES),
    std::make_tuple(1000ul, 2000ul, OrderBy::SERIES),
    std::make_tuple(2000ul, 1000ul, OrderBy::SERIES),
  };
  for (auto tup: input) {
    OrderBy order;
    Timestamp begin;
    Timestamp end;
    std::tie(begin, end, order) = tup;
    test_storage_read_query(begin, end, order);
  }
}

// Test metadata query
static void test_metadata_query() {
  auto query = "{\"select\": \"meta:names\"}";
  auto storage = create_storage();
  auto session = storage->create_write_session();
  std::vector<std::string> series_names = {
    "test key=0",
    "test key=1",
    "test key=2",
    "test key=3",
    "test key=4",
    "test key=5",
    "test key=6",
    "test key=7",
    "test key=8",
    "test key=9",
  };
  for (auto name: series_names) {
    Sample s;
    auto status = session->init_series_id(name.data(), name.data() + name.size(), &s);
    EXPECT_EQ(status, common::Status::Ok());
    s.timestamp = 111;
    s.payload.type = PAYLOAD_FLOAT;
    s.payload.float64 = 0.;
    status = session->write(s);
    EXPECT_EQ(status, common::Status::Ok());
  }
  CursorMock cursor;
  session->query(&cursor, query);
  EXPECT_EQ(cursor.error, common::Status::Ok());
  EXPECT_EQ(cursor.samples.size(), series_names.size());
  for (auto sample: cursor.samples) {
    const int buffer_size = LIMITS_MAX_SNAME;
    char buffer[buffer_size];
    auto len = session->get_series_name(sample.paramid, buffer, buffer_size);
    EXPECT_TRUE(len > 0);
    std::string name(buffer, buffer + len);
    auto cnt = std::count(series_names.begin(), series_names.end(), name);
    EXPECT_EQ(cnt, 1);
  }
}

TEST(TestStorage, Test_storage_metadata_query) {
  test_metadata_query();
}

// Test suggest
static void test_suggest_metric_name() {
  auto query = "{\"select\": \"metric-names\", \"starts-with\": \"test\" }";
  auto storage = create_storage();
  auto session = storage->create_write_session();
  std::set<std::string> expected_metric_names = {
    "test.aaa",
    "test.bbb",
    "test.ccc",
    "test.ddd",
    "test.eee",
  };
  std::vector<std::string> series_names = {
    "test.aaa key=0",
    "test.aaa key=1",
    "test.bbb key=2",
    "test.bbb key=3",
    "test.ccc key=4",
    "test.ccc key=5",
    "test.ddd key=6",
    "test.ddd key=7",
    "test.eee key=8",
    "test.eee key=9",
    "fff.test key=0",
  };
  for (auto name: series_names) {
    Sample s;
    auto status = session->init_series_id(name.data(), name.data() + name.size(), &s);
    EXPECT_EQ(status, common::Status::Ok());
    s.timestamp = 111;
    s.payload.type = PAYLOAD_FLOAT;
    s.payload.float64 = 0.;
    status = session->write(s);
    EXPECT_EQ(status, common::Status::Ok());
  }
  CursorMock cursor;
  session->suggest(&cursor, query);
  EXPECT_EQ(cursor.error, common::Status::Ok());
  EXPECT_EQ(cursor.samples.size(), expected_metric_names.size());
  for (auto sample: cursor.samples) {
    const int buffer_size = LIMITS_MAX_SNAME;
    char buffer[buffer_size];
    auto len = session->get_series_name(sample.paramid, buffer, buffer_size);
    EXPECT_TRUE(len > 0);
    std::string name(buffer, buffer + len);
    auto cnt = expected_metric_names.count(name);
    EXPECT_EQ(cnt, 1);
    // Ensure no duplicates
    expected_metric_names.erase(expected_metric_names.find(name));
  }
}

TEST(TestStorage, Test_storage_suggest_query_1) {
  test_suggest_metric_name();
}

static void test_suggest_tag_name() {
  auto query = "{\"select\": \"tag-names\", \"metric\": \"test\", \"starts-with\": \"ba\" }";
  auto storage = create_storage();
  auto session = storage->create_write_session();
  std::set<std::string> expected_tag_names = {
    "bar",
    "baar",
    "babr",
    "badr",
    "baer",
  };
  std::vector<std::string> series_names = {
    "test foo=0 bar=0",
    "test foo=1 bar=1",
    "test foo=0 bar=0 baar=0",
    "test foo=1 bar=1 babr=1",
    "tost foo=0 bar=0 bacr=0",
    "test foo=1 bar=1 badr=1",
    "test foo=0 bar=0 baer=0",
    "test foo=1 bar=1 baer=0",
    "test foo=1 bar=1",
    "test foo=0 bar=0",
    "test foo=1 bar=1",
  };
  for (auto name: series_names) {
    Sample s;
    auto status = session->init_series_id(name.data(), name.data() + name.size(), &s);
    EXPECT_EQ(status, common::Status::Ok());
    s.timestamp = 111;
    s.payload.type = PAYLOAD_FLOAT;
    s.payload.float64 = 0.;
    status = session->write(s);
    EXPECT_EQ(status, common::Status::Ok());
  }
  CursorMock cursor;
  session->suggest(&cursor, query);
  EXPECT_EQ(cursor.error, common::Status::Ok());
  EXPECT_EQ(cursor.samples.size(), expected_tag_names.size());
  for (auto sample: cursor.samples) {
    const int buffer_size = LIMITS_MAX_SNAME;
    char buffer[buffer_size];
    auto len = session->get_series_name(sample.paramid, buffer, buffer_size);
    EXPECT_TRUE(len > 0);
    std::string name(buffer, buffer + len);
    auto cnt = expected_tag_names.count(name);
    EXPECT_EQ(cnt, 1);
    // Ensure no duplicates
    expected_tag_names.erase(expected_tag_names.find(name));
  }
}

TEST(TestStorage, Test_storage_suggest_query_2) {
  test_suggest_tag_name();
}

static void test_suggest_tag_values() {
  auto query = "{\"select\": \"tag-values\", \"metric\": \"test\", \"tag\":\"foo\", \"starts-with\": \"ba\" }";
  auto storage = create_storage();
  auto session = storage->create_write_session();
  std::set<std::string> expected_tag_values = {
    "bar",
    "baar",
    "bacr",
    "baer",
    "ba",
  };
  std::vector<std::string> series_names = {
    "test key=00000 foo=bar",
    "test key=00000 foo=buz",
    "test key=00000 foo=baar",
    "tost key=00000 foo=babr",
    "test key=00000 foo=bacr",
    "test key=00000 fuz=badr",
    "test key=00000 foo=baer",
    "test key=00000 foo=bin",
    "test key=00000 foo=foo",
    "test key=00000 foo=ba",
    "test key=00001 foo=bar",
  };
  for (auto name: series_names) {
    Sample s;
    auto status = session->init_series_id(name.data(), name.data() + name.size(), &s);
    EXPECT_EQ(status, common::Status::Ok());
    s.timestamp = 111;
    s.payload.type = PAYLOAD_FLOAT;
    s.payload.float64 = 0.;
    status = session->write(s);
    EXPECT_EQ(status, common::Status::Ok());
  }
  CursorMock cursor;
  session->suggest(&cursor, query);
  EXPECT_EQ(cursor.error, common::Status::Ok());
  EXPECT_EQ(cursor.samples.size(), expected_tag_values.size());
  for (auto sample: cursor.samples) {
    const int buffer_size = LIMITS_MAX_SNAME;
    char buffer[buffer_size];
    auto len = session->get_series_name(sample.paramid, buffer, buffer_size);
    EXPECT_TRUE(len > 0);
    std::string name(buffer, buffer + len);
    auto cnt = expected_tag_values.count(name);
    EXPECT_EQ(cnt, 1);
    // Ensure no duplicates
    expected_tag_values.erase(expected_tag_values.find(name));
  }
}

TEST(TestStorage, Test_storage_suggest_query_3) {
  test_suggest_tag_values();
}

// Group-by query
static const Timestamp gb_begin = 100;
static const Timestamp gb_end   = 200;

static std::string make_group_by_query(std::string tag, OrderBy order) {
  std::stringstream str;
  str << "{ \"select\": \"test\",";
  str << "  \"range\": { \"from\": " << gb_begin << ", \"to\": " << gb_end << "},";
  str << "  \"order-by\": " << (order == OrderBy::SERIES ? "\"series\"": "\"time\"") << ",";
  str << "  \"group-by\": [\"" << tag << "\"]";
  str << "}";
  return str.str();
}

static void test_storage_group_by_query(OrderBy order) {
  std::vector<std::string> series_names = {
    "test key=0 group=0",
    "test key=1 group=0",
    "test key=2 group=0",
    "test key=3 group=1",
    "test key=4 group=1",
    "test key=5 group=1",
    "test key=6 group=1",
    "test key=7 group=1",
    "test key=8 group=0",
    "test key=9 group=0",
  };
  // Series names after group-by
  std::vector<std::string> expected_series_names = {
    "test group=0",
    "test group=0",
    "test group=0",
    "test group=0",
    "test group=0",
    "test group=1",
    "test group=1",
    "test group=1",
    "test group=1",
    "test group=1",
  };
  std::vector<std::string> unique_expected_series_names = {
    "test group=0",
    "test group=1",
  };
  auto storage = create_storage();
  auto session = storage->create_write_session();
  fill_data(session, gb_begin, gb_end, series_names);
  CursorMock cursor;
  auto query = make_group_by_query("group", order);
  session->query(&cursor, query.c_str());
  EXPECT_TRUE(cursor.done);
  EXPECT_EQ(cursor.error, common::Status::Ok());
  size_t expected_size;
  expected_size = (gb_end - gb_begin)*series_names.size();
  EXPECT_EQ(cursor.samples.size(), expected_size);
  std::vector<Timestamp> expected_timestamps;
  for (Timestamp ts = gb_begin; ts < gb_end; ts++) {
    for (int i = 0; i < 5; i++) {
      expected_timestamps.push_back(ts);
    }
  }
  check_timestamps(cursor, expected_timestamps, order, unique_expected_series_names);
  check_paramids(*session, cursor, order, expected_series_names, (gb_end - gb_begin)*series_names.size(), false);
}

TEST(TestStorage, Test_storage_groupby_query_0) {
  test_storage_group_by_query(OrderBy::SERIES);
}

TEST(TestStorage, Test_storage_groupby_query_1) {
  test_storage_group_by_query(OrderBy::TIME);
}

// Test aggregate query
TEST(TestStorage, Test_storage_aggregate_query) {
  std::vector<std::string> series_names_1 = {
    "cpu.user key=0 group=1",
    "cpu.user key=1 group=1",
    "cpu.user key=2 group=1",
    "cpu.user key=3 group=1",
    "cpu.syst key=0 group=1",
    "cpu.syst key=1 group=1",
    "cpu.syst key=2 group=1",
    "cpu.syst key=3 group=1",
  };
  std::vector<std::string> series_names_0 = {
    "cpu.user key=4 group=0",
    "cpu.user key=5 group=0",
    "cpu.user key=6 group=0",
    "cpu.user key=7 group=0",
    "cpu.syst key=4 group=0",
    "cpu.syst key=5 group=0",
    "cpu.syst key=6 group=0",
    "cpu.syst key=7 group=0",
  };
  std::vector<double> xss_1, xss_0;
  std::vector<Timestamp> tss_all;
  const Timestamp BASE_TS = 100000, STEP_TS = 1000;
  const double BASE_X0 = 1.0E3, STEP_X0 = 10.0;
  const double BASE_X1 = -10., STEP_X1 = -10.0;
  for (int i = 0; i < 10000; i++) {
    tss_all.push_back(BASE_TS + i*STEP_TS);
    xss_0.push_back(BASE_X0 + i*STEP_X0);
    xss_1.push_back(BASE_X1 + i*STEP_X1);
  }
  auto storage = create_storage();
  auto session = storage->create_write_session();
  fill_data(session, series_names_0, tss_all, xss_0);
  fill_data(session, series_names_1, tss_all, xss_1);

  {
    // Construct aggregate query
    const char* query = R"==(
    {
      "aggregate": {
        "cpu.user": "min",
            "cpu.syst": "max"
      }
    })==";

    CursorMock cursor;
    session->query(&cursor, query);
    EXPECT_TRUE(cursor.done);
    EXPECT_EQ(cursor.error, common::Status::Ok());

    EXPECT_EQ(cursor.samples.size(), series_names_0.size() + series_names_1.size());

    std::vector<std::pair<std::string, double>> expected = {
      std::make_pair("cpu.user:min group=0 key=4", 1000),
      std::make_pair("cpu.user:min group=0 key=5", 1000),
      std::make_pair("cpu.user:min group=0 key=6", 1000),
      std::make_pair("cpu.user:min group=0 key=7", 1000),
      std::make_pair("cpu.user:min group=1 key=0", -100000),
      std::make_pair("cpu.user:min group=1 key=1", -100000),
      std::make_pair("cpu.user:min group=1 key=2", -100000),
      std::make_pair("cpu.user:min group=1 key=3", -100000),
      std::make_pair("cpu.syst:max group=0 key=4", 100990),
      std::make_pair("cpu.syst:max group=0 key=5", 100990),
      std::make_pair("cpu.syst:max group=0 key=6", 100990),
      std::make_pair("cpu.syst:max group=0 key=7", 100990),
      std::make_pair("cpu.syst:max group=1 key=0", -10),
      std::make_pair("cpu.syst:max group=1 key=1", -10),
      std::make_pair("cpu.syst:max group=1 key=2", -10),
      std::make_pair("cpu.syst:max group=1 key=3", -10),
    };

    int i = 0;
    for (const auto& sample: cursor.samples) {
      char buffer[100];
      int len = session->get_series_name(sample.paramid, buffer, 100);
      std::string sname(buffer, buffer + len);
      EXPECT_EQ(expected.at(i).first, sname);
      EXPECT_EQ(expected.at(i).second, sample.payload.float64);
      i++;
    }
  }

  {
    // Construct aggregate - group-by query
    const char* query = R"==(
    {
      "aggregate": {
        "cpu.user": "min",
            "cpu.syst": "max"
      },
          "group-by": [ "group" ]
    })==";

    CursorMock cursor;
    session->query(&cursor, query);
    EXPECT_TRUE(cursor.done);
    EXPECT_EQ(cursor.error, common::Status::Ok());

    EXPECT_EQ(cursor.samples.size(), 4);

    std::vector<std::pair<std::string, double>> expected = {
      std::make_pair("cpu.user:min group=0", 1000),
      std::make_pair("cpu.user:min group=1", -100000),
      std::make_pair("cpu.syst:max group=0", 100990),
      std::make_pair("cpu.syst:max group=1", -10),
    };

    int i = 0;
    for (const auto& sample: cursor.samples) {
      char buffer[100];
      int len = session->get_series_name(sample.paramid, buffer, 100);
      std::string sname(buffer, buffer + len);
      EXPECT_EQ(expected.at(i).first, sname);
      EXPECT_EQ(expected.at(i).second, sample.payload.float64);
      i++;
    }
  }
}

// Test group aggregate query
TEST(TestStorage, Test_storage_group_aggregate_query_0) {
  std::vector<std::string> series_names = {
    "cpu.syst key=0 group=0",
    "cpu.syst key=1 group=0",
    "cpu.syst key=2 group=1",
    "cpu.syst key=3 group=1",
    "cpu.user key=0 group=0",
    "cpu.user key=1 group=0",
    "cpu.user key=2 group=1",
    "cpu.user key=3 group=1",
  };
  std::vector<double> xss;
  std::vector<Timestamp> tss;
  const Timestamp BASE_TS = 100000, STEP_TS = 1000;
  const double BASE_X = 1.0E3, STEP_X = 10.0;
  for (int i = 0; i < 10000; i++) {
    tss.push_back(BASE_TS + i*STEP_TS);
    xss.push_back(BASE_X + i*STEP_X);
  }
  auto storage = create_storage();
  auto session = storage->create_write_session();
  fill_data(session, series_names, tss, xss);

  {
    // Construct group-aggregate query
    const char* query = R"==(
    {
      "group-aggregate": {
        "metric": "cpu.user",
            "step"  : 4000000,
            "func"  : "min"
      },
          "range": {
            "from"  : 100000,
            "to"    : 10100000
          }
    })==";

    CursorMock cursor;
    session->query(&cursor, query);
    EXPECT_TRUE(cursor.done);
    EXPECT_EQ(cursor.error, common::Status::Ok());

    std::vector<std::tuple<std::string, Timestamp, double>> expected = {
      std::make_tuple("cpu.user:min group=0 key=0", 100000,  1000),
      std::make_tuple("cpu.user:min group=0 key=1", 100000,  1000),
      std::make_tuple("cpu.user:min group=1 key=2", 100000,  1000),
      std::make_tuple("cpu.user:min group=1 key=3", 100000,  1000),
      std::make_tuple("cpu.user:min group=0 key=0", 4100000, 41000),
      std::make_tuple("cpu.user:min group=0 key=1", 4100000, 41000),
      std::make_tuple("cpu.user:min group=1 key=2", 4100000, 41000),
      std::make_tuple("cpu.user:min group=1 key=3", 4100000, 41000),
      std::make_tuple("cpu.user:min group=0 key=0", 8100000, 81000),
      std::make_tuple("cpu.user:min group=0 key=1", 8100000, 81000),
      std::make_tuple("cpu.user:min group=1 key=2", 8100000, 81000),
      std::make_tuple("cpu.user:min group=1 key=3", 8100000, 81000),
    };

    EXPECT_EQ(cursor.samples.size(), expected.size());
    EXPECT_EQ(cursor.tuples.size(), expected.size());

    int i = 0;
    for (const auto& sample: cursor.samples) {
      EXPECT_TRUE((sample.payload.type & PData::TUPLE_BIT) != 0);
      char buffer[100];
      int len = session->get_series_name(sample.paramid, buffer, 100);
      std::string sname(buffer, buffer + len);
      u64 bits;
      memcpy(&bits, &sample.payload.float64, sizeof(u64));
      EXPECT_TRUE((bits >> 58) == 1);
      EXPECT_TRUE((bits & 1) == 1);
      double pmin = cursor.tuples[i][0];
      EXPECT_EQ(std::get<0>(expected.at(i)), sname);
      EXPECT_EQ(std::get<1>(expected.at(i)), sample.timestamp);
      EXPECT_EQ(std::get<2>(expected.at(i)), pmin);
      i++;
    }
  }

  {
    // Construct group-aggregate query
    const char* query = R"==(
    {
      "group-aggregate": {
        "metric": ["cpu.user", "cpu.syst"],
            "step"  : 4000000,
            "func"  : "min"
      },
          "group-by-tag": [ "key" ],
          "order-by": "series",
          "range": {
            "from"  : 100000,
            "to"    : 10100000
          }
    })==";

    CursorMock cursor;
    session->query(&cursor, query);
    EXPECT_TRUE(cursor.done);
    EXPECT_EQ(cursor.error, common::Status::Ok());

    std::vector<std::tuple<std::string, Timestamp, double>> expected = {
      std::make_tuple("cpu.user:min group=0", 100000, 1000),
      std::make_tuple("cpu.user:min group=0", 4100000, 41000),
      std::make_tuple("cpu.user:min group=0", 8100000, 81000),
      std::make_tuple("cpu.user:min group=1", 100000, 1000),
      std::make_tuple("cpu.user:min group=1", 4100000, 41000),
      std::make_tuple("cpu.user:min group=1", 8100000, 81000),
      std::make_tuple("cpu.syst:min group=0", 100000, 1000),
      std::make_tuple("cpu.syst:min group=0", 4100000, 41000),
      std::make_tuple("cpu.syst:min group=0", 8100000, 81000),
      std::make_tuple("cpu.syst:min group=1", 100000, 1000),
      std::make_tuple("cpu.syst:min group=1", 4100000, 41000),
      std::make_tuple("cpu.syst:min group=1", 8100000, 81000),
    };

    EXPECT_EQ(cursor.samples.size(), expected.size());
    EXPECT_EQ(cursor.tuples.size(), expected.size());

    int i = 0;
    for (const auto& sample: cursor.samples) {
      char buffer[100];
      int len = session->get_series_name(sample.paramid, buffer, 100);
      std::string sname(buffer, buffer + len);
      u64 bits;
      memcpy(&bits, &sample.payload.float64, sizeof(u64));
      EXPECT_TRUE((bits >> 58) == 1);
      EXPECT_TRUE((bits & 1) == 1);
      double pmin = cursor.tuples[i][0];
      EXPECT_EQ(std::get<0>(expected.at(i)), sname);
      EXPECT_EQ(std::get<1>(expected.at(i)), sample.timestamp);
      EXPECT_EQ(std::get<2>(expected.at(i)), pmin);
      i++;
    }
  }

  {
    // Construct group-aggregate query
    const char* query = R"==(
    {
      "group-aggregate": {
        "metric": "cpu.user",
            "step"  : 4000000,
            "func"  : "min"
      },
          "pivot-by-tag": [ "group" ],
          "order-by": "time",
          "range": {
            "from"  : 100000,
            "to"    : 10100000
          }
    })==";

    CursorMock cursor;
    session->query(&cursor, query);
    EXPECT_TRUE(cursor.done);
    EXPECT_EQ(cursor.error, common::Status::Ok());

    std::vector<std::tuple<std::string, Timestamp, double>> expected = {
      std::make_tuple("cpu.user:min group=0", 100000, 1000),
      std::make_tuple("cpu.user:min group=1", 100000, 1000),
      std::make_tuple("cpu.user:min group=0", 4100000, 41000),
      std::make_tuple("cpu.user:min group=1", 4100000, 41000),
      std::make_tuple("cpu.user:min group=0", 8100000, 81000),
      std::make_tuple("cpu.user:min group=1", 8100000, 81000),
    };

    EXPECT_EQ(cursor.samples.size(), expected.size());
    EXPECT_EQ(cursor.tuples.size(), expected.size());

    int i = 0;
    for (const auto& sample: cursor.samples) {
      char buffer[100];
      int len = session->get_series_name(sample.paramid, buffer, 100);
      std::string sname(buffer, buffer + len);
      u64 bits;
      memcpy(&bits, &sample.payload.float64, sizeof(u64));
      EXPECT_TRUE((bits >> 58) == 1);
      EXPECT_TRUE((bits & 1) == 1);
      double pmin = cursor.tuples[i][0];
      EXPECT_EQ(std::get<0>(expected.at(i)), sname);
      EXPECT_EQ(std::get<1>(expected.at(i)), sample.timestamp);
      EXPECT_EQ(std::get<2>(expected.at(i)), pmin);
      i++;
    }
  }
}

TEST(TestStorage, Test_storage_group_aggregate_query_1) {
  std::vector<std::string> series_names = {
    "cpu.syst key=0 group=0",
    "cpu.syst key=1 group=0",
    "cpu.syst key=2 group=1",
    "cpu.syst key=3 group=1",
    "cpu.user key=0 group=0",
    "cpu.user key=1 group=0",
    "cpu.user key=2 group=1",
    "cpu.user key=3 group=1",
  };
  std::vector<double> xss;
  std::vector<Timestamp> tss;
  const Timestamp BASE_TS = 100000, STEP_TS = 1000;
  const double BASE_X = 1.0E3, STEP_X = 10.0;
  for (int i = 0; i < 10000; i++) {
    tss.push_back(BASE_TS + i*STEP_TS);
    xss.push_back(BASE_X + i*STEP_X);
  }
  auto storage = create_storage();
  auto session = storage->create_write_session();
  fill_data(session, series_names, tss, xss);

  {
    // Construct group-aggregate query
    const char* query = R"==(
    {
      "group-aggregate": {
        "metric": ["cpu.user", "cpu.syst"],
            "step"  : 4000000,
            "func"  : ["min", "max"]
      },
          "range": {
            "from"  : 100000,
            "to"    : 10100000
          }
    })==";

    CursorMock cursor;
    session->query(&cursor, query);
    EXPECT_TRUE(cursor.done);
    EXPECT_EQ(cursor.error, common::Status::Ok());

    std::vector<std::tuple<std::string, Timestamp, double, double>> expected = {
      std::make_tuple("cpu.syst:min|cpu.syst:max group=0 key=0", 100000, 1000, 40990),
      std::make_tuple("cpu.syst:min|cpu.syst:max group=0 key=1", 100000, 1000, 40990),
      std::make_tuple("cpu.syst:min|cpu.syst:max group=1 key=2", 100000, 1000, 40990),
      std::make_tuple("cpu.syst:min|cpu.syst:max group=1 key=3", 100000, 1000, 40990),
      std::make_tuple("cpu.user:min|cpu.user:max group=0 key=0", 100000, 1000, 40990),
      std::make_tuple("cpu.user:min|cpu.user:max group=0 key=1", 100000, 1000, 40990),
      std::make_tuple("cpu.user:min|cpu.user:max group=1 key=2", 100000, 1000, 40990),
      std::make_tuple("cpu.user:min|cpu.user:max group=1 key=3", 100000, 1000, 40990),
      std::make_tuple("cpu.syst:min|cpu.syst:max group=0 key=0", 4100000, 41000, 80990),
      std::make_tuple("cpu.syst:min|cpu.syst:max group=0 key=1", 4100000, 41000, 80990),
      std::make_tuple("cpu.syst:min|cpu.syst:max group=1 key=2", 4100000, 41000, 80990),
      std::make_tuple("cpu.syst:min|cpu.syst:max group=1 key=3", 4100000, 41000, 80990),
      std::make_tuple("cpu.user:min|cpu.user:max group=0 key=0", 4100000, 41000, 80990),
      std::make_tuple("cpu.user:min|cpu.user:max group=0 key=1", 4100000, 41000, 80990),
      std::make_tuple("cpu.user:min|cpu.user:max group=1 key=2", 4100000, 41000, 80990),
      std::make_tuple("cpu.user:min|cpu.user:max group=1 key=3", 4100000, 41000, 80990),
      std::make_tuple("cpu.syst:min|cpu.syst:max group=0 key=0", 8100000, 81000, 100990),
      std::make_tuple("cpu.syst:min|cpu.syst:max group=0 key=1", 8100000, 81000, 100990),
      std::make_tuple("cpu.syst:min|cpu.syst:max group=1 key=2", 8100000, 81000, 100990),
      std::make_tuple("cpu.syst:min|cpu.syst:max group=1 key=3", 8100000, 81000, 100990),
      std::make_tuple("cpu.user:min|cpu.user:max group=0 key=0", 8100000, 81000, 100990),
      std::make_tuple("cpu.user:min|cpu.user:max group=0 key=1", 8100000, 81000, 100990),
      std::make_tuple("cpu.user:min|cpu.user:max group=1 key=2", 8100000, 81000, 100990),
      std::make_tuple("cpu.user:min|cpu.user:max group=1 key=3", 8100000, 81000, 100990),
    };

    EXPECT_EQ(cursor.samples.size(), expected.size());
    EXPECT_EQ(cursor.tuples.size(), expected.size());

    int i = 0;
    for (const auto& sample: cursor.samples) {
      EXPECT_TRUE((sample.payload.type & PData::TUPLE_BIT) != 0);
      char buffer[100];
      int len = session->get_series_name(sample.paramid, buffer, 100);
      std::string sname(buffer, buffer + len);
      u64 bits;
      memcpy(&bits, &sample.payload.float64, sizeof(u64));
      EXPECT_TRUE((bits >> 58) == 2);
      EXPECT_TRUE((bits & 3) == 3);
      double pmin = cursor.tuples[i][0], pmax = cursor.tuples[i][1];
      EXPECT_EQ(std::get<0>(expected.at(i)), sname);
      EXPECT_EQ(std::get<1>(expected.at(i)), sample.timestamp);
      EXPECT_EQ(std::get<2>(expected.at(i)), pmin);
      EXPECT_EQ(std::get<3>(expected.at(i)), pmax);
      i++;
    }
  }

  {
    // Construct group-aggregate query
    const char* query = R"==(
    {
      "group-aggregate": {
        "metric": ["cpu.user", "cpu.syst"],
            "step"  : 4000000,
            "func"  : ["min", "max"]
      },
          "group-by-tag": ["key"],
          "range": {
            "from"  : 100000,
            "to"    : 10100000
          }
    })==";

    CursorMock cursor;
    session->query(&cursor, query);
    EXPECT_TRUE(cursor.done);
    EXPECT_EQ(cursor.error, common::Status::Ok());

    std::vector<std::tuple<std::string, Timestamp, double, double>> expected = {
      std::make_tuple("cpu.user:min|cpu.user:max group=0", 100000, 1000, 40990),
      std::make_tuple("cpu.user:min|cpu.user:max group=1", 100000, 1000, 40990),
      std::make_tuple("cpu.syst:min|cpu.syst:max group=0", 100000, 1000, 40990),
      std::make_tuple("cpu.syst:min|cpu.syst:max group=1", 100000, 1000, 40990),
      std::make_tuple("cpu.user:min|cpu.user:max group=0", 4100000, 41000, 80990),
      std::make_tuple("cpu.user:min|cpu.user:max group=1", 4100000, 41000, 80990),
      std::make_tuple("cpu.syst:min|cpu.syst:max group=0", 4100000, 41000, 80990),
      std::make_tuple("cpu.syst:min|cpu.syst:max group=1", 4100000, 41000, 80990),
      std::make_tuple("cpu.user:min|cpu.user:max group=0", 8100000, 81000, 100990),
      std::make_tuple("cpu.user:min|cpu.user:max group=1", 8100000, 81000, 100990),
      std::make_tuple("cpu.syst:min|cpu.syst:max group=0", 8100000, 81000, 100990),
      std::make_tuple("cpu.syst:min|cpu.syst:max group=1", 8100000, 81000, 100990),
    };

    EXPECT_EQ(cursor.samples.size(), expected.size());
    EXPECT_EQ(cursor.tuples.size(), expected.size());

    int i = 0;
    for (const auto& sample: cursor.samples) {
      EXPECT_TRUE((sample.payload.type & PData::TUPLE_BIT) != 0);
      char buffer[100];
      int len = session->get_series_name(sample.paramid, buffer, 100);
      std::string sname(buffer, buffer + len);
      u64 bits;
      memcpy(&bits, &sample.payload.float64, sizeof(u64));
      EXPECT_TRUE((bits >> 58) == 2);
      EXPECT_TRUE((bits & 3) == 3);
      double pmin = cursor.tuples[i][0], pmax = cursor.tuples[i][1];
      EXPECT_EQ(std::get<0>(expected.at(i)), sname);
      EXPECT_EQ(std::get<1>(expected.at(i)), sample.timestamp);
      EXPECT_EQ(std::get<2>(expected.at(i)), pmin);
      EXPECT_EQ(std::get<3>(expected.at(i)), pmax);
      i++;
    }
  }

  {
    // Construct group-aggregate query
    const char* query = R"==(
    {
      "group-aggregate": {
        "metric": ["cpu.user", "cpu.syst"],
            "step"  : 4000000,
            "func"  : ["min", "max"]
      },
          "group-by-tag": ["key"],
          "range": {
            "from"  : 10100000,
            "to"    : 100000
          }
    })==";

    CursorMock cursor;
    session->query(&cursor, query);
    EXPECT_TRUE(cursor.done);
    EXPECT_EQ(cursor.error, common::Status::Ok());

    std::vector<std::tuple<std::string, Timestamp, double, double>> expected = {
      std::make_tuple("cpu.syst:min|cpu.syst:max group=1", 10100000, 61010, 100990),
      std::make_tuple("cpu.syst:min|cpu.syst:max group=0", 10100000, 61010, 100990),
      std::make_tuple("cpu.user:min|cpu.user:max group=1", 10100000, 61010, 100990),
      std::make_tuple("cpu.user:min|cpu.user:max group=0", 10100000, 61010, 100990),
      std::make_tuple("cpu.syst:min|cpu.syst:max group=1", 6100000, 21010, 61000),
      std::make_tuple("cpu.syst:min|cpu.syst:max group=0", 6100000, 21010, 61000),
      std::make_tuple("cpu.user:min|cpu.user:max group=1", 6100000, 21010, 61000),
      std::make_tuple("cpu.user:min|cpu.user:max group=0", 6100000, 21010, 61000),
      std::make_tuple("cpu.syst:min|cpu.syst:max group=1", 2100000, 1010, 21000),
      std::make_tuple("cpu.syst:min|cpu.syst:max group=0", 2100000, 1010, 21000),
      std::make_tuple("cpu.user:min|cpu.user:max group=1", 2100000, 1010, 21000),
      std::make_tuple("cpu.user:min|cpu.user:max group=0", 2100000, 1010, 21000),
    };

    EXPECT_EQ(cursor.samples.size(), expected.size());
    EXPECT_EQ(cursor.tuples.size(), expected.size());

    int i = 0;
    for (const auto& sample: cursor.samples) {
      EXPECT_TRUE((sample.payload.type & PData::TUPLE_BIT) != 0);
      char buffer[100];
      int len = session->get_series_name(sample.paramid, buffer, 100);
      std::string sname(buffer, buffer + len);
      u64 bits;
      memcpy(&bits, &sample.payload.float64, sizeof(u64));
      EXPECT_TRUE((bits >> 58) == 2);
      EXPECT_TRUE((bits & 3) == 3);
      double pmin = cursor.tuples[i][0], pmax = cursor.tuples[i][1];
      EXPECT_EQ(std::get<0>(expected.at(i)), sname);
      EXPECT_EQ(std::get<1>(expected.at(i)), sample.timestamp);
      EXPECT_EQ(std::get<2>(expected.at(i)), pmin);
      EXPECT_EQ(std::get<3>(expected.at(i)), pmax);
      i++;
    }
  }

  {
    // Construct group-aggregate query
    const char* query = R"==(
    {
      "group-aggregate": {
        "metric"     : ["cpu.user", "cpu.syst"],
            "step"       : 4000000,
            "func"       : ["min", "max"]},
          "group-by-tag"   : ["key"],
          "order-by"       : "series",
          "range"          : {
            "from"       : 100000,
            "to"         : 10100000}
    })==";

    CursorMock cursor;
    session->query(&cursor, query);
    EXPECT_TRUE(cursor.done);
    EXPECT_EQ(cursor.error, common::Status::Ok());

    std::vector<std::tuple<std::string, Timestamp, double, double>> expected = {
      std::make_tuple("cpu.user:min|cpu.user:max group=0", 100000, 1000, 40990),
      std::make_tuple("cpu.user:min|cpu.user:max group=0", 4100000, 41000, 80990),
      std::make_tuple("cpu.user:min|cpu.user:max group=0", 8100000, 81000, 100990),
      std::make_tuple("cpu.user:min|cpu.user:max group=1", 100000, 1000, 40990),
      std::make_tuple("cpu.user:min|cpu.user:max group=1", 4100000, 41000, 80990),
      std::make_tuple("cpu.user:min|cpu.user:max group=1", 8100000, 81000, 100990),
      std::make_tuple("cpu.syst:min|cpu.syst:max group=0", 100000, 1000, 40990),
      std::make_tuple("cpu.syst:min|cpu.syst:max group=0", 4100000, 41000, 80990),
      std::make_tuple("cpu.syst:min|cpu.syst:max group=0", 8100000, 81000, 100990),
      std::make_tuple("cpu.syst:min|cpu.syst:max group=1", 100000, 1000, 40990),
      std::make_tuple("cpu.syst:min|cpu.syst:max group=1", 4100000, 41000, 80990),
      std::make_tuple("cpu.syst:min|cpu.syst:max group=1", 8100000, 81000, 100990),
    };

    EXPECT_EQ(cursor.samples.size(), expected.size());
    EXPECT_EQ(cursor.tuples.size(), expected.size());

    int i = 0;
    for (const auto& sample: cursor.samples) {
      EXPECT_TRUE((sample.payload.type & PData::TUPLE_BIT) != 0);
      char buffer[100];
      int len = session->get_series_name(sample.paramid, buffer, 100);
      std::string sname(buffer, buffer + len);
      u64 bits;
      memcpy(&bits, &sample.payload.float64, sizeof(u64));
      EXPECT_TRUE((bits >> 58) == 2);
      EXPECT_TRUE((bits & 3) == 3);
      double pmin = cursor.tuples[i][0], pmax = cursor.tuples[i][1];
      EXPECT_EQ(std::get<0>(expected.at(i)), sname);
      EXPECT_EQ(std::get<1>(expected.at(i)), sample.timestamp);
      EXPECT_EQ(std::get<2>(expected.at(i)), pmin);
      EXPECT_EQ(std::get<3>(expected.at(i)), pmax);
      i++;
    }
  }
}

// Test where clause
static std::string make_scan_query_with_where(Timestamp begin, Timestamp end, std::vector<int> keys) {
  std::stringstream str;
  str << "{ \"range\": { \"from\": " << begin << ", \"to\": " << end << "},";
  str << "  \"select\": \"test\",";
  str << "  \"order-by\": \"series\",";
  str << "  \"where\": { \"key\": [";
  bool first = true;
  for (auto key: keys) {
    if (first) {
      first = false;
      str << key;
    } else {
      str << ", " << key;
    }
  }
  str << "], \"zzz\": 0 }}";
  return str.str();
}

void test_storage_where_clause(Timestamp begin, Timestamp end, int nseries) {
  std::vector<std::string> series_names;
  std::vector<std::string> all_series_names;
  for (int i = 0; i < nseries; i++) {
    series_names.push_back("test key=" + std::to_string(i) + " zzz=0");
    all_series_names.push_back("test key=" + std::to_string(i) + " zzz=0");
    all_series_names.push_back("test key=" + std::to_string(i) + " zzz=1");
  }
  // We will fill all series but will read only zzz=0 series to check multi-tag queries
  auto storage = create_storage();
  auto session = storage->create_write_session();
  fill_data(session, std::min(begin, end), std::max(begin, end), all_series_names);
  auto check_case = [&](std::vector<int> ids2read) {
    CursorMock cursor;
    auto query = make_scan_query_with_where(begin, end, ids2read);
    std::vector<std::string> expected_series;
    for(auto id: ids2read) {
      expected_series.push_back(series_names[static_cast<size_t>(id)]);
    }
    session->query(&cursor, query.c_str());
    EXPECT_TRUE(cursor.done);
    EXPECT_EQ(cursor.error, common::Status::Ok());
    size_t expected_size = (end - begin)*expected_series.size();
    EXPECT_EQ(cursor.samples.size(), expected_size);
    std::vector<Timestamp> expected;
    for (Timestamp ts = begin; ts < end; ts++) {
      expected.push_back(ts);
    }
    check_timestamps(cursor, expected, OrderBy::SERIES, expected_series);
    check_paramids(*session, cursor, OrderBy::SERIES, expected_series, expected_size, true);
  };

  std::vector<int> first = { 0 };
  check_case(first);

  std::vector<int> last = { nseries - 1 };
  check_case(last);

  std::vector<int> all; for (int i = 0; i < nseries; i++) { all.push_back(i); };
  check_case(all);

  std::vector<int> even; std::copy_if(all.begin(), all.end(), std::back_inserter(even), [](int i) {return i % 2 == 0;});
  check_case(even);

  std::vector<int> odd; std::copy_if(all.begin(), all.end(), std::back_inserter(odd), [](int i) {return i % 2 != 0;});
  check_case(odd);
}

TEST(TestStorage, Test_storage_where_clause) {
  std::vector<std::tuple<Timestamp, Timestamp, int>> cases = {
    std::make_tuple(100, 200, 10),
  };
  for (auto tup: cases) {
    Timestamp begin, end;
    int nseries;
    std::tie(begin, end, nseries) = tup;
    test_storage_where_clause(begin, end, nseries);
  }
}

static void test_storage_where_clause2(Timestamp begin, Timestamp end) {
  int nseries = 100;
  std::vector<std::string> series_names;
  std::vector<std::string> expected_series = {
    "test key=10 zzz=0",
    "test key=22 zzz=0",
    "test key=42 zzz=0",
    "test key=66 zzz=0"
  };
  for (int i = 0; i < nseries; i++) {
    series_names.push_back("test key=" + std::to_string(i) + " zzz=0");
  }
  auto storage = create_storage();
  auto session = storage->create_write_session();
  fill_data(session, std::min(begin, end), std::max(begin, end), series_names);

  std::stringstream query;
  query << "{";
  query << "   \"select\": \"test\",\n";
  query << "   \"where\": [\n";
  query << "       { \"key\": 10, \"zzz\": 0 },\n";
  query << "       { \"key\": 14             },\n";  // should be missing
  query << "       { \"key\": 22, \"zzz\": 0 },\n";
  query << "       { \"key\": 42, \"zzz\": 0 },\n";
  query << "       { \"key\": 66, \"zzz\": 0 }\n";
  query << "   ],\n";
  query << "   \"order-by\": \"series\",\n";
  query << "   \"range\": { \"from\": " << begin << ", \"to\": " << end << "}\n";
  query << "}";

  CursorMock cursor;
  session->query(&cursor, query.str().c_str());
  EXPECT_TRUE(cursor.done);
  EXPECT_EQ(cursor.error, common::Status::Ok());
  size_t expected_size = (end - begin)*expected_series.size();
  EXPECT_EQ(cursor.samples.size(), expected_size);
  std::vector<Timestamp> expected;
  for (Timestamp ts = begin; ts < end; ts++) {
    expected.push_back(ts);
  }
  check_timestamps(cursor, expected, OrderBy::SERIES, expected_series);
  check_paramids(*session, cursor, OrderBy::SERIES, expected_series, expected_size, true);
}

TEST(TestStorage, Test_storage_where_form2) {
  Timestamp begin = 100, end = 200;
  test_storage_where_clause2(begin, end);
}

// Test SeriesRetreiver
void test_retreiver() {
  std::vector<u64> ids;
  std::vector<std::string> test_data = {
    "aaa foo=1 bar=1 buz=1",
    "aaa foo=1 bar=1 buz=2",
    "aaa foo=1 bar=2 buz=2",
    "aaa foo=2 bar=2 buz=2",
    "aaa foo=2 bar=2 buz=3",
    "bbb foo=2 bar=3 buz=3",
    "bbb foo=3 bar=3 buz=3",
    "bbb foo=3 bar=3 buz=4",
    "bbb foo=3 bar=4 buz=4",
    "bbb foo=4 bar=4 buz=4",
    "bbb foo=4 bar=4 buz=5",
    "bbb foo=4 bar=4 buz=6",
  };
  PlainSeriesMatcher m;
  for (auto s: test_data) {
    char buffer[0x100];
    const char* pkeys_begin;
    const char* pkeys_end;
    common::Status status = SeriesParser::to_canonical_form(s.data(), s.data() + s.size(),
                                                            buffer, buffer + 0x100,
                                                            &pkeys_begin, &pkeys_end);
    EXPECT_EQ(status, common::Status::Ok());
    auto id = m.add(buffer, pkeys_end);
    ids.push_back(id);
  }

  std::vector<u64> act;
  common::Status status;

  SeriesRetreiver rt1;
  status = rt1.add_tag("foo", "1");
  EXPECT_EQ(status, common::Status::BadArg());
  std::tie(status, act) = rt1.extract_ids(m);
  EXPECT_EQ(status, common::Status::Ok());
  EXPECT_EQ(ids.size(), act.size());
  for (size_t i = 0; i < ids.size(); ++i) {
    EXPECT_EQ(ids[i], act[i]);
  }

  SeriesRetreiver rt2({"bbb"});
  std::tie(status, act) = rt2.extract_ids(m);
  EXPECT_EQ(status, common::Status::Ok());
  EXPECT_EQ(ids.size() - 5, act.size());
  for (size_t i = 0; i < act.size(); ++i) {
    EXPECT_EQ(ids[i + 5], act[i]);
  }

  SeriesRetreiver rt3({"bbb"});
  status = rt3.add_tag("foo", "3");
  EXPECT_EQ(status, common::Status::Ok());
  status = rt3.add_tag("buz", "4");
  EXPECT_EQ(status, common::Status::Ok());
  status = rt3.add_tag("buz", "4");
  EXPECT_EQ(status, common::Status::BadArg());
  std::tie(status, act) = rt3.extract_ids(m);
  EXPECT_EQ(status, common::Status::Ok());
  EXPECT_EQ(2, act.size());
  for (auto i = 0; i < 2; ++i) {
    EXPECT_EQ(ids[7 + i], act[i]);
  }

  SeriesRetreiver rt4({"bbb"});
  status = rt4.add_tag("foo", "4");
  EXPECT_EQ(status, common::Status::Ok());
  status = rt4.add_tags("buz", {"4", "5"});
  EXPECT_EQ(status, common::Status::Ok());
  std::tie(status, act) = rt4.extract_ids(m);
  EXPECT_EQ(status, common::Status::Ok());
  EXPECT_EQ(2, act.size());
  for (size_t i = 0; i < 2UL; ++i) {
    EXPECT_EQ(ids[9 + i], act[i]);
  }
}

TEST(TestStorage, Test_series_retreiver_1) {
  test_retreiver();
}

TEST(TestStorage, Test_series_add_1) {
  const char* sname = "hello|world tag=1";
  const char* end = sname + strlen(sname);

  auto store = create_storage();
  auto session = store->create_write_session();

  ParamId ids[10];
  auto nids = session->get_series_ids(sname, end, ids, 10);
  EXPECT_EQ(nids, 2);

  char buf[100];
  auto buflen = session->get_series_name(ids[0], buf, 100);
  std::string name0(buf, buf + buflen);
  EXPECT_STREQ(name0.c_str(), "hello tag=1");
  buflen = session->get_series_name(ids[1], buf, 100);
  std::string name1(buf, buf + buflen);
  EXPECT_STREQ(name1.c_str(), "world tag=1");
}

// No input
TEST(TestStorage, Test_series_add_2) {
  const char* sname = "";
  const char* end = sname;

  auto store = create_storage();
  auto session = store->create_write_session();

  ParamId ids[10];
  auto nids = session->get_series_ids(sname, end, ids, 10);
  EXPECT_EQ(-1 * nids, common::Status::kBadData);
}

// No tags
TEST(TestStorage, Test_series_add_3) {
  const char* sname = "hello|world";
  const char* end = sname + strlen(sname);

  auto store = create_storage();
  auto session = store->create_write_session();

  ParamId ids[10];
  auto nids = session->get_series_ids(sname, end, ids, 10);
  EXPECT_EQ(-1 * nids, common::Status::kBadData);
}

// ids array is too small
TEST(TestStorage, Test_series_add_4) {
  const char* sname = "hello|world tag=val";
  const char* end = sname + strlen(sname);

  auto store = create_storage();
  auto session = store->create_write_session();

  ParamId ids[1];
  auto nids = session->get_series_ids(sname, end, ids, 1);
  EXPECT_EQ(-1 * nids, common::Status::kBadArg);
}

// Series too long
TEST(TestStorage, Test_series_add_5) {
  std::stringstream strstr;
  strstr << "metric0";
  for (int i = 1; i < 1000; i++) {
    strstr << "|metric" << i;
  }
  strstr << " tag=value";

  auto sname = strstr.str();

  auto store = create_storage();
  auto session = store->create_write_session();

  ParamId ids[100];
  auto nids = session->get_series_ids(sname.data(), sname.data() + sname.size(), ids, 100);
  EXPECT_EQ(-1 * nids, common::Status::kBadData);
}

// Test reopen
void test_wal_recovery(int cardinality, Timestamp begin, Timestamp end) {
  EXPECT_TRUE(cardinality);
  std::vector<std::string> series_names;
  for (int i = 0; i < cardinality; i++) {
    series_names.push_back(
        "test tag=" + std::to_string(i));
  }
  auto meta = create_metadatastorage();
  auto bstore = BlockStoreBuilder::create_memstore();
  auto cstore = create_cstore();
  auto store = std::make_shared<Storage>(meta, bstore, cstore, true);
  FineTuneParams params;
  params.input_log_concurrency = 1;
  params.input_log_path = "./";
  params.input_log_volume_numb = 32;
  params.input_log_volume_size = 1024*1024*24;
  store->initialize_input_log(params);
  auto session = store->create_write_session();

  fill_data(session, begin, end, series_names);
  session.reset();  // This should flush current WAL frame
  store->_kill();

  std::unordered_map<ParamId, std::vector<storage::LogicAddr>> mapping;
  store = std::make_shared<Storage>(meta, bstore, cstore, true);
  store->run_recovery(params, &mapping);
  store->initialize_input_log(params);
  session = store->create_write_session();

  CursorMock cursor;
  auto query = make_scan_query(begin, end, OrderBy::SERIES);
  session->query(&cursor, query.c_str());
  EXPECT_TRUE(cursor.done);
  EXPECT_EQ(cursor.error, common::Status::Ok());
  auto expected_size = (end - begin)*series_names.size();
  std::vector<Timestamp> expected;
  for (Timestamp ts = begin; ts < end; ts++) {
    expected.push_back(ts);
  }
  EXPECT_GE(cursor.samples.size(), expected.size());
  check_timestamps(cursor, expected, OrderBy::SERIES, series_names);
  check_paramids(*session, cursor, OrderBy::SERIES, series_names, expected_size, false);

  session.reset();
  store->close();
}

TEST(TestStorage, Test_wal_recovery_0) {
  test_wal_recovery(100, 1000, 2000);
}

TEST(TestStorage, Test_wal_recovery_1) {
  test_wal_recovery(100, 1000, 11000);
}

TEST(TestStorage, Test_wal_recovery_2) {
  test_wal_recovery(100, 1000, 101000);
}

/**
 * @brief Test WAL effect on write amplification
 * @param usewal is a flag that controls use of WAL in the test
 * @param total_cardinality is a total number of series
 * @param batch_size is a number of series in the batch
 * @param begin timestamp
 * @param end timestamp
 *
 * This test writes `total_cardinlity` series in batches.
 * When new batch is being written the previous one should
 * be evicted from the working set (the corresponding
 * nbtree instances should be closed) if WAL is enabled. So,
 * resulting write amplification should be high.
 * If WAL is not used the number of written pages should match
 * `total_cardinality`
 */
void test_wal_write_amplification_impact(bool usewal, int total_cardinality, int batch_size, Timestamp begin, Timestamp end) {
  int nbatches = total_cardinality / batch_size;
  int append_cnt = 0;
  auto counter_fn = [&](LogicAddr) {
    append_cnt++;
  };
  auto bstore = BlockStoreBuilder::create_memstore(counter_fn);
  auto meta = create_metadatastorage();
  std::shared_ptr<ColumnStore> cstore;
  cstore.reset(new ColumnStore(bstore));
  auto store = std::make_shared<Storage>(meta, bstore, cstore, true);
  FineTuneParams params;
  params.input_log_concurrency = 1;
  params.input_log_path = usewal ? "./" : nullptr;
  params.input_log_volume_numb = 4;
  params.input_log_volume_size = 10*0x1000;
  store->initialize_input_log(params);

  for (int ixbatch = 0; ixbatch < nbatches; ixbatch++) {
    auto session = store->create_write_session();
    std::vector<std::string> series_names;
    for (int i = 0; i < batch_size; i++) {
      series_names.push_back(
          "test tag=" + std::to_string(ixbatch*batch_size + i));
    }
    fill_data(session, begin, end, series_names);
    session.reset();  // This should flush current WAL frame
  }
  if (usewal) {
    EXPECT_TRUE(append_cnt != 0);
  } else {
    EXPECT_TRUE(append_cnt == 0);
  }
  store->close();
  if (usewal) {
    EXPECT_TRUE(append_cnt > total_cardinality);
  } else {
    EXPECT_TRUE(append_cnt == total_cardinality);
  }
}

TEST(TestStorage, Test_high_cardinality_0) {
  test_wal_write_amplification_impact(true, 10000, 1000, 1000, 1010);
}

TEST(TestStorage, Test_high_cardinality_1) {
  test_wal_write_amplification_impact(false, 10000, 1000, 1000, 1010);
}

TEST(TestStorage, Test_group_aggregate_join_query_0) {
  std::vector<std::string> series_names1 = {
    "cpu.user key=0 group=0",
    "cpu.user key=1 group=0",
    "cpu.user key=2 group=1",
    "cpu.user key=3 group=1",
  };
  std::vector<std::string> series_names2 = {
    "cpu.syst key=0 group=0",
    "cpu.syst key=1 group=0",
    "cpu.syst key=2 group=1",
    "cpu.syst key=3 group=1",
  };
  std::vector<double> xss;
  std::vector<Timestamp> tss;
  const Timestamp BASE_TS = 100000, STEP_TS = 1000;
  const double BASE_X = 1.0E3, STEP_X = 10.0;
  for (int i = 0; i < 10000; i++) {
    tss.push_back(BASE_TS + i*STEP_TS);
    xss.push_back(BASE_X + i*STEP_X);
  }
  auto storage = create_storage();
  auto session = storage->create_write_session();
  fill_data(session, series_names1, tss, xss);
  std::transform(xss.begin(), xss.end(), xss.begin(), [](double x) { return x*100; });
  fill_data(session, series_names2, tss, xss);

  {
    // Simple group-aggregate-join query with two columns
    const char* query = R"==(
    {
      "group-aggregate-join": {
        "metric": ["cpu.user", "cpu.syst"],
            "step"  : 4000000,
            "func"  : "min"
      },
          "range": {
            "from"  : 100000,
            "to"    : 10100000
          }
    })==";

    CursorMock cursor;
    session->query(&cursor, query);
    EXPECT_TRUE(cursor.done);
    EXPECT_EQ(cursor.error, common::Status::Ok());

    std::vector<std::tuple<std::string, Timestamp, double, double>> expected = {
      std::make_tuple("cpu.user|cpu.syst group=0 key=0",  100000,  1000,  100000),
      std::make_tuple("cpu.user|cpu.syst group=0 key=1",  100000,  1000,  100000),
      std::make_tuple("cpu.user|cpu.syst group=1 key=2",  100000,  1000,  100000),
      std::make_tuple("cpu.user|cpu.syst group=1 key=3",  100000,  1000,  100000),
      std::make_tuple("cpu.user|cpu.syst group=0 key=0", 4100000, 41000, 4100000),
      std::make_tuple("cpu.user|cpu.syst group=0 key=1", 4100000, 41000, 4100000),
      std::make_tuple("cpu.user|cpu.syst group=1 key=2", 4100000, 41000, 4100000),
      std::make_tuple("cpu.user|cpu.syst group=1 key=3", 4100000, 41000, 4100000),
      std::make_tuple("cpu.user|cpu.syst group=0 key=0", 8100000, 81000, 8100000),
      std::make_tuple("cpu.user|cpu.syst group=0 key=1", 8100000, 81000, 8100000),
      std::make_tuple("cpu.user|cpu.syst group=1 key=2", 8100000, 81000, 8100000),
      std::make_tuple("cpu.user|cpu.syst group=1 key=3", 8100000, 81000, 8100000),
    };

    EXPECT_EQ(cursor.samples.size(), expected.size());
    EXPECT_EQ(cursor.tuples.size(), expected.size());

    int i = 0;
    for (const auto& sample: cursor.samples) {
      EXPECT_TRUE((sample.payload.type & PData::TUPLE_BIT) != 0);
      char buffer[100];
      int len = session->get_series_name(sample.paramid, buffer, 100);
      std::string sname(buffer, buffer + len);
      u64 bits;
      memcpy(&bits, &sample.payload.float64, sizeof(u64));
      EXPECT_TRUE((bits >> 58) == 2);
      EXPECT_TRUE((bits & 1) == 1);
      EXPECT_EQ(std::get<0>(expected.at(i)), sname);
      EXPECT_EQ(std::get<1>(expected.at(i)), sample.timestamp);
      EXPECT_EQ(std::get<2>(expected.at(i)), cursor.tuples[i][0]);
      EXPECT_EQ(std::get<3>(expected.at(i)), cursor.tuples[i][1]);
      i++;
    }
  }

  {
    // Group-aggregate-join query with filter statement
    const char* query = R"==(
    {
      "group-aggregate-join": {
        "metric": ["cpu.user", "cpu.syst"],
            "step"  : 4000000,
            "func"  : "min"
      },
          "filter": {
            "cpu.user": { "gt": 40000, "lt": 80000 }
          },
          "range": {
            "from"  : 100000,
            "to"    : 10100000
          }
    })==";

    CursorMock cursor;
    session->query(&cursor, query);
    EXPECT_TRUE(cursor.done);
    EXPECT_EQ(cursor.error, common::Status::Ok());

    /* Only one column is filtered so we should see all data-points but some of them
     * should have only second element set
     */
    std::vector<std::tuple<std::string, Timestamp, double, double, bool>> expected = {
      std::make_tuple("cpu.user|cpu.syst group=0 key=0",  100000,  1000,  100000, false),
      std::make_tuple("cpu.user|cpu.syst group=0 key=1",  100000,  1000,  100000, false),
      std::make_tuple("cpu.user|cpu.syst group=1 key=2",  100000,  1000,  100000, false),
      std::make_tuple("cpu.user|cpu.syst group=1 key=3",  100000,  1000,  100000, false),
      std::make_tuple("cpu.user|cpu.syst group=0 key=0", 4100000, 41000, 4100000, true),
      std::make_tuple("cpu.user|cpu.syst group=0 key=1", 4100000, 41000, 4100000, true),
      std::make_tuple("cpu.user|cpu.syst group=1 key=2", 4100000, 41000, 4100000, true),
      std::make_tuple("cpu.user|cpu.syst group=1 key=3", 4100000, 41000, 4100000, true),
      std::make_tuple("cpu.user|cpu.syst group=0 key=0", 8100000, 81000, 8100000, false),
      std::make_tuple("cpu.user|cpu.syst group=0 key=1", 8100000, 81000, 8100000, false),
      std::make_tuple("cpu.user|cpu.syst group=1 key=2", 8100000, 81000, 8100000, false),
      std::make_tuple("cpu.user|cpu.syst group=1 key=3", 8100000, 81000, 8100000, false),
    };

    EXPECT_EQ(cursor.samples.size(), expected.size());
    EXPECT_EQ(cursor.tuples.size(), expected.size());

    int i = 0;
    for (const auto& sample: cursor.samples) {
      EXPECT_TRUE((sample.payload.type & PData::TUPLE_BIT) != 0);
      char buffer[100];
      int len = session->get_series_name(sample.paramid, buffer, 100);
      std::string sname(buffer, buffer + len);
      u64 bits;
      memcpy(&bits, &sample.payload.float64, sizeof(u64));
      EXPECT_TRUE((bits >> 58) == 2);
      bool col1 = std::get<4>(expected.at(i));
      EXPECT_TRUE((bits & 1) == col1);
      EXPECT_EQ(std::get<0>(expected.at(i)), sname);
      EXPECT_EQ(std::get<1>(expected.at(i)), sample.timestamp);
      if (col1) {
        EXPECT_EQ(std::get<2>(expected.at(i)), cursor.tuples[i][0]);
      }
      EXPECT_EQ(std::get<3>(expected.at(i)), cursor.tuples[i][1]);
      i++;
    }
  }

  {
    // Group-aggregate-join query with filter on two columns
    const char* query = R"==(
    {
      "group-aggregate-join": {
        "metric": ["cpu.user", "cpu.syst"],
            "step"  : 4000000,
            "func"  : "min"
      },
          "filter": {
            "cpu.user": { "gt": 3000,  "lt": 80000   },
            "cpu.syst": { "gt": 99999, "lt": 8100000 }
          },
          "range": {
            "from"  : 100000,
            "to"    : 10100000
          }
    })==";

    CursorMock cursor;
    session->query(&cursor, query);
    EXPECT_TRUE(cursor.done);
    EXPECT_EQ(cursor.error, common::Status::Ok());

    /* Here we should have datapoints that match both filters of one filter. Datapoints that doesn't
     * match both filters shouldn't be here
     */
    std::vector<std::tuple<std::string, Timestamp, double, double, bool, bool>> expected = {
      std::make_tuple("cpu.user|cpu.syst group=0 key=0",  100000,  1000,  100000, false, true),
      std::make_tuple("cpu.user|cpu.syst group=0 key=1",  100000,  1000,  100000, false, true),
      std::make_tuple("cpu.user|cpu.syst group=1 key=2",  100000,  1000,  100000, false, true),
      std::make_tuple("cpu.user|cpu.syst group=1 key=3",  100000,  1000,  100000, false, true),
      std::make_tuple("cpu.user|cpu.syst group=0 key=0", 4100000, 41000, 4100000, true, true),
      std::make_tuple("cpu.user|cpu.syst group=0 key=1", 4100000, 41000, 4100000, true, true),
      std::make_tuple("cpu.user|cpu.syst group=1 key=2", 4100000, 41000, 4100000, true, true),
      std::make_tuple("cpu.user|cpu.syst group=1 key=3", 4100000, 41000, 4100000, true, true),
    };

    EXPECT_EQ(cursor.samples.size(), expected.size());
    EXPECT_EQ(cursor.tuples.size(), expected.size());

    int i = 0;
    for (const auto& sample: cursor.samples) {
      EXPECT_TRUE((sample.payload.type & PData::TUPLE_BIT) != 0);
      char buffer[100];
      int len = session->get_series_name(sample.paramid, buffer, 100);
      std::string sname(buffer, buffer + len);
      u64 bits;
      memcpy(&bits, &sample.payload.float64, sizeof(u64));
      EXPECT_TRUE((bits >> 58) == 2);
      bool col1 = std::get<4>(expected.at(i));
      bool col2 = std::get<5>(expected.at(i));
      EXPECT_TRUE(static_cast<bool>(bits & 1) == col1);
      EXPECT_TRUE(static_cast<bool>(bits & 2) == col2);
      EXPECT_EQ(std::get<0>(expected.at(i)), sname);
      EXPECT_EQ(std::get<1>(expected.at(i)), sample.timestamp);
      if (col1) {
        EXPECT_EQ(std::get<2>(expected.at(i)), cursor.tuples[i][0]);
      }
      if (col2) {
        EXPECT_EQ(std::get<3>(expected.at(i)), cursor.tuples[i][1]);
      }
      i++;
    }
  }

  {
    // Group-aggregate-join query with two aggregation functions (not supported)
    const char* query = R"==(
    {
      "group-aggregate-join": {
        "metric": ["cpu.user", "cpu.syst"],
            "step"  : 4000000,
            "func"  : [ "min", "max" ]
      },
          "range": {
            "from"  : 100000,
            "to"    : 10100000
          }
    })==";

    CursorMock cursor;
    session->query(&cursor, query);
    EXPECT_TRUE(cursor.done);
    EXPECT_EQ(cursor.error, common::Status::QueryParsingError());
  }

  {
    // Group-aggregate-join query with single column
    const char* query = R"==(
    {
      "group-aggregate-join": {
        "metric": ["cpu.user"],
            "step"  : 4000000,
            "func"  : "min"
      },
          "range": {
            "from"  : 100000,
            "to"    : 10100000
          }
    })==";

    CursorMock cursor;
    session->query(&cursor, query);
    EXPECT_TRUE(cursor.done);
    EXPECT_EQ(cursor.error, common::Status::QueryParsingError());
  }

  {
    // Group-aggregate-join query with two columns and pivot-by-tag
    const char* query = R"==(
    {
      "group-aggregate-join": {
        "metric": ["cpu.user", "cpu.syst"],
            "step"  : 4000000,
            "func"  : "min"
      },
          "pivot-by-tag": [ "group" ],
          "range": {
            "from"  : 100000,
            "to"    : 10100000
          }
    })==";

    CursorMock cursor;
    session->query(&cursor, query);
    EXPECT_TRUE(cursor.done);
    EXPECT_EQ(cursor.error, common::Status::QueryParsingError());
  }
}

}  // namespace stdb
