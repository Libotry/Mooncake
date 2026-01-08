#include <gflags/gflags.h>
#include <glog/logging.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <random>
#include <thread>

#include "master_client.h"
#include "replica.h"
#include "segment.h"
#include "types.h"

DEFINE_string(master_address, "127.0.0.1:50051",
              "Master service address (IP:Port)");
DEFINE_int32(write_interval_ms, 1000, "Interval between writes in milliseconds");
DEFINE_int32(max_keys, 100, "Maximum number of different keys to use");
DEFINE_int32(delete_interval, 10,
             "Delete one key every N writes (0 to disable deletion)");
DEFINE_bool(enable_new_keys, true,
            "Allow new keys every 100 writes (beyond the max_keys limit)");
DEFINE_int32(value_size, 1024, "Size of value (kvcache) in bytes");
DEFINE_bool(mount_segment, true,
            "Mount a segment to master_service before starting operations");
DEFINE_string(segment_name, "mock_client_segment",
              "Name of the segment to mount");
DEFINE_int64(segment_size, 1024 * 1024 * 64,
             "Size of the segment to mount in bytes (default: 64MB)");
DEFINE_int64(segment_base, 0x300000000,
             "Base address of the segment (virtual address, default: 0x300000000)");

namespace mooncake {

// Generate a key name
std::string GenerateKey(int index, bool is_new_key = false) {
    if (is_new_key) {
        static std::atomic<int> new_key_counter{0};
        return "mock_key_new_" + std::to_string(new_key_counter.fetch_add(1));
    }
    return "mock_key_" + std::to_string(index);
}

// Mock client simulator
class MockClientSimulator {
   public:
    MockClientSimulator(const std::string& master_address)
        : master_address_(master_address),
          client_id_(generate_uuid()),
          client_(client_id_),
          write_count_(0),
          key_index_(0),
          running_(true) {
        // Connect to master service
        ErrorCode err = client_.Connect(master_address);
        if (err != ErrorCode::OK) {
            LOG(FATAL) << "Failed to connect to master service: "
                       << master_address << ", error=" << static_cast<int>(err);
        }

        // Default replication config
        config_.replica_num = 1;
        config_.with_soft_pin = false;

        LOG(INFO) << "Mock client simulator initialized";
        LOG(INFO) << "  master_address: " << master_address;
        LOG(INFO) << "  client_id: " << client_id_;
        LOG(INFO) << "  max_keys: " << FLAGS_max_keys;
        LOG(INFO) << "  write_interval_ms: " << FLAGS_write_interval_ms;
        LOG(INFO) << "  delete_interval: " << FLAGS_delete_interval;
        LOG(INFO) << "  value_size: " << FLAGS_value_size;
        LOG(INFO) << "  mount_segment: " << FLAGS_mount_segment;

        // Mount segment if enabled
        if (FLAGS_mount_segment) {
            Segment segment;
            segment.id = generate_uuid();
            segment.name = FLAGS_segment_name;
            segment.base = static_cast<uintptr_t>(FLAGS_segment_base);
            segment.size = static_cast<size_t>(FLAGS_segment_size);
            segment.te_endpoint = FLAGS_segment_name;

            LOG(INFO) << "[Mount] Attempting to mount segment:";
            LOG(INFO) << "  name: " << segment.name;
            LOG(INFO) << "  id: " << segment.id;
            LOG(INFO) << "  base: 0x" << std::hex << segment.base << std::dec;
            LOG(INFO) << "  size: " << segment.size << " bytes ("
                      << (segment.size / (1024 * 1024)) << " MB)";
            LOG(INFO) << "  te_endpoint: " << segment.te_endpoint;

            auto mount_result = client_.MountSegment(segment);
            if (mount_result.has_value()) {
                LOG(INFO) << "[Mount] Segment mounted successfully";
                mounted_segment_ = segment;
            } else {
                LOG(WARNING) << "[Mount] Failed to mount segment: error="
                             << static_cast<int>(mount_result.error());
                LOG(WARNING) << "[Mount] PutStart operations may fail with "
                                "NO_AVAILABLE_HANDLE error";
                LOG(WARNING) << "[Mount] You may need to start mooncake_client "
                                "to provide real storage";
            }
        } else {
            LOG(INFO) << "[Mount] Segment mounting disabled (--nomount_segment)";
            LOG(INFO) << "[Mount] PutStart operations may fail if no segment is "
                         "available";
        }
    }

    void Run() {
        LOG(INFO) << "=== Starting mock client simulation ===";
        LOG(INFO) << "Press Ctrl+C to stop";

        int success_count = 0;
        int failure_count = 0;
        int delete_count = 0;
        int delete_success_count = 0;
        int delete_failure_count = 0;
        auto start_time = std::chrono::steady_clock::now();
        const int stats_interval = 50;  // Print stats every N operations

        while (running_.load()) {
            int current_write_count = write_count_.load();
            LOG(INFO) << "--- Operation #" << (current_write_count + 1)
                      << " ---";

            // Determine which key to use
            std::string key;

            if (FLAGS_enable_new_keys && write_count_ % 100 == 0 &&
                write_count_ > 0) {
                // Every 100 writes, use a new key (beyond max_keys limit)
                key = GenerateKey(0, true);
                LOG(INFO) << "[Key Selection] Using new key (every 100 writes): "
                          << key;
            } else {
                // Use one of the max_keys keys
                key_index_ = (key_index_ + 1) % FLAGS_max_keys;
                key = GenerateKey(key_index_);
                LOG(INFO) << "[Key Selection] Using key from pool: " << key
                          << " (index=" << key_index_.load() << ")";
            }

            // Simulate GET operation: check if key exists
            LOG(INFO) << "[GET] Checking if key exists: " << key;
            auto exist_result = client_.ExistKey(key);
            bool exists = false;
            if (exist_result.has_value()) {
                exists = exist_result.value();
                LOG(INFO) << "[GET] Key " << key
                          << (exists ? " EXISTS" : " does NOT exist");
            } else {
                LOG(WARNING) << "[GET] ExistKey failed for key=" << key
                             << ", error="
                             << static_cast<int>(exist_result.error());
            }

            if (!exists) {
                // Key doesn't exist, simulate PUT operation
                LOG(INFO) << "[PUT] Starting PUT operation for new key: " << key
                          << ", value_size=" << FLAGS_value_size;

                // Step 1: PutStart
                std::vector<size_t> slice_lengths = {
                    static_cast<size_t>(FLAGS_value_size)};
                LOG(INFO) << "[PUT] Step 1/2: Calling PutStart...";
                auto put_start_result =
                    client_.PutStart(key, slice_lengths, config_);
                if (!put_start_result.has_value()) {
                    ErrorCode err = put_start_result.error();
                    failure_count++;
                    if (err == ErrorCode::NO_AVAILABLE_HANDLE) {
                        LOG(WARNING) << "[PUT] PutStart FAILED: key=" << key
                                     << ", error=NO_AVAILABLE_HANDLE (-200) "
                                     << "(master_service may not have segments configured or is out of space)";
                    } else {
                        LOG(ERROR) << "[PUT] PutStart FAILED: key=" << key
                                   << ", error=" << static_cast<int>(err);
                    }
                    write_count_++;
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds(FLAGS_write_interval_ms));
                    continue;
                }

                LOG(INFO) << "[PUT] PutStart SUCCESS: key=" << key
                          << ", replicas=" << put_start_result.value().size();

                // Step 2: PutEnd (complete the put operation)
                LOG(INFO) << "[PUT] Step 2/2: Calling PutEnd...";
                auto put_end_result =
                    client_.PutEnd(key, ReplicaType::MEMORY);
                if (put_end_result.has_value()) {
                    success_count++;
                    write_count_++;
                    LOG(INFO) << "[PUT] PutEnd SUCCESS: key=" << key
                              << " (new key created)";
                    LOG(INFO) << "[PUT] Operation COMPLETE: key=" << key;
                } else {
                    failure_count++;
                    write_count_++;
                    LOG(ERROR) << "[PUT] PutEnd FAILED: key=" << key
                               << ", error="
                               << static_cast<int>(put_end_result.error());
                    LOG(ERROR) << "[PUT] Operation INCOMPLETE: key=" << key
                               << " (PutStart succeeded but PutEnd failed)";
                }
            } else {
                // Key exists, simulate PUT (update) operation
                LOG(INFO) << "[PUT] Starting PUT operation for existing key: "
                          << key << ", value_size=" << FLAGS_value_size;

                // For update, we also use PutStart + PutEnd
                std::vector<size_t> slice_lengths = {
                    static_cast<size_t>(FLAGS_value_size)};
                LOG(INFO) << "[PUT] Step 1/2: Calling PutStart (update)...";
                auto put_start_result =
                    client_.PutStart(key, slice_lengths, config_);
                if (!put_start_result.has_value()) {
                    ErrorCode err = put_start_result.error();
                    failure_count++;
                    if (err == ErrorCode::NO_AVAILABLE_HANDLE) {
                        LOG(WARNING) << "[PUT] PutStart (update) FAILED: key="
                                     << key
                                     << ", error=NO_AVAILABLE_HANDLE (-200) "
                                     << "(master_service may not have segments configured or is out of space)";
                    } else {
                        LOG(ERROR) << "[PUT] PutStart (update) FAILED: key="
                                   << key << ", error="
                                   << static_cast<int>(err);
                    }
                    write_count_++;
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds(FLAGS_write_interval_ms));
                    continue;
                }

                LOG(INFO) << "[PUT] PutStart (update) SUCCESS: key=" << key
                          << ", replicas=" << put_start_result.value().size();

                LOG(INFO) << "[PUT] Step 2/2: Calling PutEnd (update)...";
                auto put_end_result =
                    client_.PutEnd(key, ReplicaType::MEMORY);
                if (put_end_result.has_value()) {
                    success_count++;
                    write_count_++;
                    LOG(INFO) << "[PUT] PutEnd (update) SUCCESS: key=" << key;
                    LOG(INFO) << "[PUT] Operation COMPLETE: key=" << key
                              << " (key updated)";
                } else {
                    failure_count++;
                    write_count_++;
                    LOG(ERROR) << "[PUT] PutEnd (update) FAILED: key=" << key
                               << ", error="
                               << static_cast<int>(put_end_result.error());
                    LOG(ERROR) << "[PUT] Operation INCOMPLETE: key=" << key
                               << " (PutStart succeeded but PutEnd failed)";
                }
            }

            // Simulate DELETE operation periodically
            if (FLAGS_delete_interval > 0 &&
                write_count_ % FLAGS_delete_interval == 0 &&
                write_count_ > 0) {
                delete_count++;
                // Delete one of the existing keys (not new keys)
                int delete_key_index = (key_index_ + 1) % FLAGS_max_keys;
                std::string delete_key = GenerateKey(delete_key_index);
                LOG(INFO) << "[DELETE] Attempting to delete key: " << delete_key
                          << " (delete operation #" << delete_count << ")";
                auto remove_result = client_.Remove(delete_key);
                if (remove_result.has_value()) {
                    delete_success_count++;
                    LOG(INFO) << "[DELETE] SUCCESS: key=" << delete_key
                              << " deleted";
                } else {
                    delete_failure_count++;
                    LOG(ERROR) << "[DELETE] FAILED: key=" << delete_key
                               << ", error="
                               << static_cast<int>(remove_result.error());
                }
            }

            // Print statistics periodically
            int total_ops = success_count + failure_count;
            if (total_ops > 0 && total_ops % stats_interval == 0) {
                auto current_time = std::chrono::steady_clock::now();
                auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                    current_time - start_time).count();
                double success_rate =
                    (total_ops > 0) ? (100.0 * success_count / total_ops) : 0.0;
                double ops_per_sec = (elapsed > 0) ? (total_ops / elapsed) : 0.0;

                LOG(INFO) << "=== Statistics (last " << stats_interval
                          << " operations) ===";
                LOG(INFO) << "  Total operations: " << total_ops;
                LOG(INFO) << "  Successful: " << success_count
                          << " (" << success_rate << "%)";
                LOG(INFO) << "  Failed: " << failure_count;
                LOG(INFO) << "  Delete operations: " << delete_count
                          << " (success: " << delete_success_count
                          << ", failed: " << delete_failure_count << ")";
                LOG(INFO) << "  Elapsed time: " << elapsed << " seconds";
                LOG(INFO) << "  Operations/sec: " << ops_per_sec;
                LOG(INFO) << "========================================";
            }

            // Sleep for the specified interval
            LOG(INFO) << "[Sleep] Waiting " << FLAGS_write_interval_ms
                      << " ms before next operation...";
            std::this_thread::sleep_for(
                std::chrono::milliseconds(FLAGS_write_interval_ms));
        }

        // Print final statistics
        auto end_time = std::chrono::steady_clock::now();
        auto total_elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            end_time - start_time).count();
        int total_ops = success_count + failure_count;
        double final_success_rate =
            (total_ops > 0) ? (100.0 * success_count / total_ops) : 0.0;
        double final_ops_per_sec =
            (total_elapsed > 0) ? (total_ops / total_elapsed) : 0.0;

        LOG(INFO) << "=== Final Statistics ===";
        LOG(INFO) << "  Total operations: " << total_ops;
        LOG(INFO) << "  Successful: " << success_count << " ("
                  << final_success_rate << "%)";
        LOG(INFO) << "  Failed: " << failure_count;
        LOG(INFO) << "  Delete operations: " << delete_count
                  << " (success: " << delete_success_count
                  << ", failed: " << delete_failure_count << ")";
        LOG(INFO) << "  Total elapsed time: " << total_elapsed << " seconds";
        LOG(INFO) << "  Average operations/sec: " << final_ops_per_sec;
        LOG(INFO) << "=========================";
    }

    void Stop() { running_.store(false); }

   private:
    std::string master_address_;
    UUID client_id_;
    MasterClient client_;
    ReplicateConfig config_;
    Segment mounted_segment_;
    std::atomic<int> write_count_;
    std::atomic<int> key_index_;
    std::atomic<bool> running_;
};

}  // namespace mooncake

int main(int argc, char* argv[]) {
    gflags::ParseCommandLineFlags(&argc, &argv, true);
    google::InitGoogleLogging(argv[0]);

    if (FLAGS_master_address.empty()) {
        LOG(FATAL) << "master_address must be specified";
    }

    mooncake::MockClientSimulator simulator(FLAGS_master_address);

    // Handle Ctrl+C gracefully
    std::signal(SIGINT, [](int) {
        LOG(INFO) << "Received SIGINT, stopping...";
        exit(0);
    });

    simulator.Run();

    return 0;
}
