#pragma once

#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "endpoint_validator.h"
#include "types.h"

namespace mooncake {

// Forward declaration
class MasterService;

/**
 * @brief Manages cleanup of stale endpoints after HA failover
 */
class StaleEndpointCleanupManager {
   public:
    /**
     * @brief Configuration for stale endpoint cleanup
     */
    struct CleanupConfig {
        std::chrono::milliseconds validation_timeout{
            500};  // Timeout for validating each endpoint
        size_t max_concurrent_validations{10};  // Maximum concurrent validations
        bool enable_async_cleanup{true};        // Whether to execute asynchronously
        std::chrono::seconds async_delay{5};    // Delay before async execution
    };

    /**
     * @brief Validate and cleanup stale endpoints after HA failover
     * @param master_service Pointer to MasterService instance
     * @param config Cleanup configuration
     */
    void ValidateAndCleanupStaleEndpoints(MasterService* master_service,
                                          const CleanupConfig& config);

   private:
    /**
     * @brief Collect all endpoints that need validation
     * @param master_service Pointer to MasterService instance
     * @return Vector of endpoint information to validate
     */
    std::vector<EndpointValidator::EndpointInfo> CollectEndpointsToValidate(
        MasterService* master_service);

    /**
     * @brief Perform the actual validation and cleanup (internal implementation)
     * @param master_service Pointer to MasterService instance
     * @param config Cleanup configuration
     */
    void DoValidateAndCleanup(MasterService* master_service,
                              const CleanupConfig& config);

    /**
     * @brief Cleanup stale replica transport endpoint
     * @param master_service Pointer to MasterService instance
     * @param key Object key
     * @param replica_index Index of the replica to cleanup
     * @param endpoint Endpoint that is stale
     */
    void CleanupStaleReplicaEndpoint(MasterService* master_service,
                                     const std::string& key,
                                     size_t replica_index,
                                     const std::string& endpoint);

    /**
     * @brief Cleanup stale segment te_endpoint
     * @param master_service Pointer to MasterService instance
     * @param segment_id Segment ID
     * @param endpoint Endpoint that is stale
     */
    void CleanupStaleSegmentEndpoint(MasterService* master_service,
                                     const UUID& segment_id,
                                     const std::string& endpoint);
};

}  // namespace mooncake
