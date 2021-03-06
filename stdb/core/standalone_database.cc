/*!
 * \file standalone_database.cc
 */
#include "stdb/core/standalone_database.h"

#include <memory>

#include "stdb/core/controller.h"
#include "stdb/core/standalone_database_session.h"

namespace stdb {

StandaloneDatabase::StandaloneDatabase(
    std::shared_ptr<Synchronization> synchronization,
    std::shared_ptr<SyncWaiter> sync_waiter,
    bool is_moving) : Database(is_moving), sync_waiter_(sync_waiter) {
  worker_database_.reset(new WorkerDatabase(synchronization, is_moving));
  server_database_.reset(new ServerDatabase(is_moving));
}

StandaloneDatabase::StandaloneDatabase(
    const char* server_path, const char* worker_path,
    const FineTuneParams& params,
    std::shared_ptr<Synchronization> synchronization,
    std::shared_ptr<SyncWaiter> sync_waiter,
    bool is_moving) : Database(is_moving), sync_waiter_(sync_waiter) {
  server_database_.reset(new ServerDatabase(server_path, params, is_moving));
  worker_database_.reset(new WorkerDatabase(worker_path, params, synchronization, is_moving));
}

void StandaloneDatabase::initialize(const FineTuneParams& params) {
  server_database_->run_recovery(params, this);
  worker_database_->run_recovery(params, this);

  Database::initialize(params);

  server_database_->set_input_log(inputlog(), input_log_path());
  worker_database_->set_input_log(inputlog(), input_log_path());
}

void StandaloneDatabase::close() {
  server_database_->close();
  worker_database_->close();
}

void StandaloneDatabase::sync() {
  server_database_->sync();
  worker_database_->sync();
}

std::shared_ptr<DatabaseSession> StandaloneDatabase::create_session() {
  std::shared_ptr<storage::CStoreSession> session =
      std::make_shared<storage::CStoreSession>(worker_database_->cstore());
  return std::make_shared<StandaloneDatabaseSession>(shared_from_this(),
                                                     session,
                                                     sync_waiter_);
}

common::Status StandaloneDatabase::new_database(
    bool ismoving,
    const char* base_file_name,
    const char* metadata_path,
    const char* volumes_path,
    i32 num_volumes,
    u64 volume_size,
    bool allocate) {
  std::string server_metadata_path = std::string(metadata_path) + "/server";
  auto status = ServerDatabase::new_database(
      ismoving,
      base_file_name,
      server_metadata_path.c_str(),
      num_volumes == 0 ? "ExpandableFileStorage" : "FixedSizeFileStorage");
  if (!status.IsOk()) {
    return status;
  }

  std::string worker_metadata_path = std::string(metadata_path) + "/worker";
  status = WorkerDatabase::new_database(
      ismoving,
      base_file_name,
      worker_metadata_path.c_str(),
      volumes_path,
      num_volumes,
      volume_size,
      allocate);
  return status;
}

// Create new column store.
void StandaloneDatabase::recovery_create_new_column(ParamId id) {
  auto cstore = worker_database_->cstore();
  cstore->create_new_column(id);
}

// Update rescue points
void StandaloneDatabase::recovery_update_rescue_points(ParamId id, const std::vector<storage::LogicAddr>& addrs) {
  std::vector<storage::LogicAddr> copy_addrs = addrs;
  worker_database_->update_rescue_point(id, std::move(copy_addrs));
}

// Recovery write.
storage::NBTreeAppendResult StandaloneDatabase::recovery_write(Sample const& sample, bool allow_duplicates) {

}
  
SeriesMatcher* StandaloneDatabase::global_matcher() {
  return server_database_->global_matcher();
}

}  // namespace stdb
