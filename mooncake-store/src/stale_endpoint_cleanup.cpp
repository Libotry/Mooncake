#include "stale_endpoint_cleanup.h"

#include <glog/logging.h>

#include <algorithm>
#include <thread>
#include <unordered_map>

#include "master_service.h"
#include "mutex.h"
#include "replica.h"
#include "segment.h"
#include "types.h"

namespace mooncake {

void StaleEndpointCleanupManager::ValidateAndCleanupStaleEndpoints(
    MasterService* master_service, const CleanupConfig& config) {
    if (config.enable_async_cleanup) {
        // Execute asynchronously to avoid blocking failover
        std::thread cleanup_thread([this, master_service, config]() {
            if (config.async_delay.count() > 0) {
                std::this_thread::sleep_for(config.async_delay);
            }
            this->DoValidateAndCleanup(master_service, config);
        });
        cleanup_thread.detach();
    } else {
        // Execute synchronously
        DoValidateAndCleanup(master_service, config);
    }
}

std::vector<EndpointValidator::EndpointInfo>
StaleEndpointCleanupManager::CollectEndpointsToValidate(
    MasterService* master_service) {
    std::vector<EndpointValidator::EndpointInfo> endpoints;

    // Phase 1: Collect replica transport_endpoints
    // Access metadata_shards_ directly through friend access
    for (size_t shard_idx = 0; shard_idx < master_service->kNumShards; ++shard_idx) {
        MutexLocker lock(&master_service->metadata_shards_[shard_idx].mutex);

        for (const auto& [key, metadata] : master_service->metadata_shards_[shard_idx].metadata) {
            // Iterate through all replicas (not just COMPLETE ones)
            for (size_t i = 0; i < metadata.replicas.size(); ++i) {
                const auto& replica = metadata.replicas[i];
                if (replica.type() == ReplicaType::LOCAL_DISK) {
                    // Get transport_endpoint from descriptor
                    const auto& desc = replica.get_descriptor();
                    if (desc.is_local_disk_replica()) {
                        const auto& ld_desc = desc.get_local_disk_descriptor();
                        if (!ld_desc.transport_endpoint.empty()) {
                            EndpointValidator::EndpointInfo info;
                            info.endpoint = ld_desc.transport_endpoint;
                            info.type = EndpointValidator::EndpointInfo::REPLICA_TRANSPORT_ENDPOINT;
                            info.key = key;
                            info.replica_index = i;
                            endpoints.push_back(std::move(info));
                        }
                    }
                }
            }
        }
    }

    LOG(INFO) << "Collected " << endpoints.size()
              << " replica transport endpoints to validate";

    // Phase 2: Collect segment te_endpoints
    {
        ScopedSegmentAccess segment_access =
            master_service->segment_manager_.getSegmentAccess();
        // Access mounted_segments_ through friend access (ScopedSegmentAccess is friend)
        for (const auto& [segment_id, mounted_segment] :
             segment_access.segment_manager_->mounted_segments_) {
            if (mounted_segment.status == SegmentStatus::OK &&
                !mounted_segment.segment.te_endpoint.empty()) {
                EndpointValidator::EndpointInfo info;
                info.endpoint = mounted_segment.segment.te_endpoint;
                info.type = EndpointValidator::EndpointInfo::SEGMENT_TE_ENDPOINT;
                info.segment_id = segment_id;
                endpoints.push_back(std::move(info));
            }
        }
    }

    size_t replica_endpoint_count = std::count_if(
        endpoints.begin(), endpoints.end(),
        [](const auto& e) {
            return e.type ==
                   EndpointValidator::EndpointInfo::REPLICA_TRANSPORT_ENDPOINT;
        });
    size_t segment_endpoint_count = endpoints.size() - replica_endpoint_count;

    LOG(INFO) << "Collected " << endpoints.size()
              << " total endpoints to validate ("
              << replica_endpoint_count << " replica transport_endpoints, "
              << segment_endpoint_count << " segment te_endpoints)";

    return endpoints;
}

void StaleEndpointCleanupManager::DoValidateAndCleanup(
    MasterService* master_service, const CleanupConfig& config) {
    LOG(INFO) << "Starting stale endpoint validation and cleanup after HA failover";

    // 1. Collect all endpoints that need validation
    auto endpoints = CollectEndpointsToValidate(master_service);
    if (endpoints.empty()) {
        LOG(INFO) << "No endpoints to validate";
        return;
    }

    LOG(INFO) << "Collected " << endpoints.size() << " endpoints to validate";

    // 2. Validate endpoints concurrently
    EndpointValidator validator;
    auto results = validator.ValidateEndpointsBatch(
        endpoints, config.validation_timeout, config.max_concurrent_validations);

    // 3. Statistics
    size_t reachable_count = 0;
    size_t stale_count = 0;
    for (const auto& [endpoint, result] : results) {
        if (result.is_reachable) {
            reachable_count++;
        } else {
            stale_count++;
        }
    }

    LOG(INFO) << "Endpoint validation completed: " << reachable_count
              << " reachable, " << stale_count << " stale";

    // 4. Cleanup stale endpoints
    for (const auto& endpoint_info : endpoints) {
        const auto it = results.find(endpoint_info.endpoint);
        if (it == results.end()) {
            LOG(WARNING) << "Validation result not found for endpoint: "
                         << endpoint_info.endpoint;
            continue;
        }

        const auto& result = it->second;
        if (!result.is_reachable) {
            LOG(WARNING) << "Detected stale endpoint: " << endpoint_info.endpoint
                         << ", error: " << result.error_msg;

            if (endpoint_info.type ==
                EndpointValidator::EndpointInfo::REPLICA_TRANSPORT_ENDPOINT) {
                CleanupStaleReplicaEndpoint(master_service, endpoint_info.key,
                                           endpoint_info.replica_index,
                                           endpoint_info.endpoint);
            } else if (endpoint_info.type ==
                       EndpointValidator::EndpointInfo::SEGMENT_TE_ENDPOINT) {
                CleanupStaleSegmentEndpoint(master_service, endpoint_info.segment_id,
                                           endpoint_info.endpoint);
            }
        }
    }

    LOG(INFO) << "Stale endpoint cleanup completed";
}

void StaleEndpointCleanupManager::CleanupStaleReplicaEndpoint(
    MasterService* master_service, const std::string& key,
    size_t replica_index, const std::string& endpoint) {
    // Use the helper method to remove the stale replica
    ErrorCode err = master_service->RemoveReplicaByIndex(key, replica_index);
    if (err != ErrorCode::OK) {
        LOG(WARNING) << "Failed to cleanup stale replica endpoint: key=" << key
                     << ", replica_index=" << replica_index
                     << ", endpoint=" << endpoint << ", error=" << toString(err);
        return;
    }

    LOG(INFO) << "Cleaned up stale replica transport_endpoint: key=" << key
              << ", replica_index=" << replica_index << ", endpoint=" << endpoint;
}

void StaleEndpointCleanupManager::CleanupStaleSegmentEndpoint(
    MasterService* master_service, const UUID& segment_id,
    const std::string& endpoint) {
    // Clear the segment's te_endpoint to mark it as invalid
    // This doesn't unmount the segment, just marks the endpoint as stale
    ScopedSegmentAccess segment_access =
        master_service->segment_manager_.getSegmentAccess();

    auto segment_it = segment_access.segment_manager_->mounted_segments_.find(segment_id);
    if (segment_it == segment_access.segment_manager_->mounted_segments_.end()) {
        LOG(WARNING) << "Segment not found during stale endpoint cleanup: "
                     << "segment_id=" << segment_id << ", endpoint=" << endpoint;
        return;
    }

    // Clear the te_endpoint
    segment_it->second.segment.te_endpoint.clear();

    // Clear invalid handles (MEMORY replicas that reference this segment)
    master_service->ClearInvalidHandles();

    LOG(INFO) << "Cleaned up stale segment te_endpoint: segment_id=" << segment_id
              << ", segment_name=" << segment_it->second.segment.name
              << ", endpoint=" << endpoint;
}

}  // namespace mooncake
