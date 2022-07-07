/*!
 * \file controller_test.cc
 */
#include "stdb/core/controller.h"

#include "gtest/gtest.h"

namespace stdb {

TEST(TestController, Test1) {
  initialize();
  auto controller = Controller::Get();
}

TEST(TestController, Test2) {
  auto controller = Controller::Get();
  controller->new_standalone_database(
      "test1",
      "/tmp/test_controller/metapath/",
      "/tmp/test_controller/volumes/",
      2,
      1024 * 1024,
      true);

  auto database = controller->open_standalone_database("test1");
  {
    auto session = database->create_session();
    std::string series = "cpu ip=127.0.0.1";
    Sample sample;
    auto status = session->init_series_id(series.c_str(), series.c_str() + series.size(), &sample);
    LOG(INFO) << "sample.paramid=" << sample.paramid;

    sample.payload.float64 = 120.0;
    sample.timestamp = 20120010;

    session->write(sample);
  }
  controller->close();
}

}  // namespace stdb
