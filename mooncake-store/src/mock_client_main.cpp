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
    }

    void Run() {
        LOG(INFO) << "Starting mock client simulation...";

        while (running_.load()) {
            // Determine which key to use
            std::string key;

            if (FLAGS_enable_new_keys && write_count_ % 100 == 0 &&
                write_count_ > 0) {
                // Every 100 writes, use a new key (beyond max_keys limit)
                key = GenerateKey(0, true);
                LOG(INFO) << "Using new key (every 100 writes): " << key;
            } else {
                // Use one of the max_keys keys
                key_index_ = (key_index_ + 1) % FLAGS_max_keys;
                key = GenerateKey(key_index_);
            }

            // Simulate GET operation: check if key exists
            auto exist_result = client_.ExistKey(key);
            bool exists = false;
            if (exist_result.has_value()) {
                exists = exist_result.value();
            } else {
                LOG(WARNING) << "ExistKey failed for key=" << key
                             << ", error="
                             << static_cast<int>(exist_result.error());
            }

            if (!exists) {
                // Key doesn't exist, simulate PUT operation
                // Step 1: PutStart
                std::vector<size_t> slice_lengths = {
                    static_cast<size_t>(FLAGS_value_size)};
                auto put_start_result =
                    client_.PutStart(key, slice_lengths, config_);
                if (!put_start_result.has_value()) {
                    LOG(ERROR) << "PutStart failed: key=" << key
                               << ", error="
                               << static_cast<int>(put_start_result.error());
                    write_count_++;
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds(FLAGS_write_interval_ms));
                    continue;
                }

                // Step 2: PutEnd (complete the put operation)
                auto put_end_result =
                    client_.PutEnd(key, ReplicaType::MEMORY);
                if (put_end_result.has_value()) {
                    LOG(INFO) << "PUT: key=" << key << " (new)";
                    write_count_++;
                } else {
                    LOG(ERROR) << "PutEnd failed: key=" << key
                               << ", error="
                               << static_cast<int>(put_end_result.error());
                    write_count_++;
                }
            } else {
                // Key exists, simulate PUT (update) operation
                // For update, we also use PutStart + PutEnd
                std::vector<size_t> slice_lengths = {
                    static_cast<size_t>(FLAGS_value_size)};
                auto put_start_result =
                    client_.PutStart(key, slice_lengths, config_);
                if (!put_start_result.has_value()) {
                    LOG(ERROR) << "PutStart (update) failed: key=" << key
                               << ", error="
                               << static_cast<int>(put_start_result.error());
                    write_count_++;
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds(FLAGS_write_interval_ms));
                    continue;
                }

                auto put_end_result =
                    client_.PutEnd(key, ReplicaType::MEMORY);
                if (put_end_result.has_value()) {
                    LOG(INFO) << "PUT (update): key=" << key;
                    write_count_++;
                } else {
                    LOG(ERROR) << "PutEnd (update) failed: key=" << key
                               << ", error="
                               << static_cast<int>(put_end_result.error());
                    write_count_++;
                }
            }

            // Simulate DELETE operation periodically
            if (FLAGS_delete_interval > 0 &&
                write_count_ % FLAGS_delete_interval == 0 &&
                write_count_ > 0) {
                // Delete one of the existing keys (not new keys)
                int delete_key_index = (key_index_ + 1) % FLAGS_max_keys;
                std::string delete_key = GenerateKey(delete_key_index);
                auto remove_result = client_.Remove(delete_key);
                if (remove_result.has_value()) {
                    LOG(INFO) << "DELETE: key=" << delete_key;
                } else {
                    LOG(ERROR) << "DELETE failed: key=" << delete_key
                               << ", error="
                               << static_cast<int>(remove_result.error());
                }
            }

            // Sleep for the specified interval
            std::this_thread::sleep_for(
                std::chrono::milliseconds(FLAGS_write_interval_ms));
        }
    }

    void Stop() { running_.store(false); }

   private:
    std::string master_address_;
    UUID client_id_;
    MasterClient client_;
    ReplicateConfig config_;
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
