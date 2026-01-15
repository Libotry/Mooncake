#pragma once

#include <chrono>
#include <string>
#include <unordered_map>
#include <vector>

#include "types.h"

namespace mooncake {

/**
 * @brief Validates endpoint reachability (IP:Port format)
 */
class EndpointValidator {
   public:
    /**
     * @brief Information about an endpoint to validate
     */
    struct EndpointInfo {
        std::string endpoint;  // "IP:Port" format
        enum Type {
            SEGMENT_TE_ENDPOINT,        // Segment's te_endpoint
            REPLICA_TRANSPORT_ENDPOINT  // Replica's transport_endpoint
        } type;

        // For SEGMENT_TE_ENDPOINT
        UUID segment_id{0, 0};

        // For REPLICA_TRANSPORT_ENDPOINT
        std::string key;
        size_t replica_index = 0;
    };

    /**
     * @brief Result of endpoint validation
     */
    struct ValidationResult {
        bool is_reachable;
        std::chrono::milliseconds latency;  // Connection latency (if reachable)
        std::string error_msg;              // Error message (if not reachable)
    };

    /**
     * @brief Validate a single endpoint
     * @param endpoint_info Endpoint information
     * @param timeout Maximum time to wait for connection
     * @return Validation result
     */
    ValidationResult ValidateEndpoint(const EndpointInfo& endpoint_info,
                                      std::chrono::milliseconds timeout);

    /**
     * @brief Validate multiple endpoints concurrently
     * @param endpoints List of endpoints to validate
     * @param timeout Maximum time to wait for each connection
     * @param max_concurrent Maximum number of concurrent validations
     * @return Map from endpoint string to validation result
     */
    std::unordered_map<std::string, ValidationResult> ValidateEndpointsBatch(
        const std::vector<EndpointInfo>& endpoints,
        std::chrono::milliseconds timeout, size_t max_concurrent);

   private:
    /**
     * @brief Parse endpoint string (IP:Port) into IP and port
     * @param endpoint Endpoint string (e.g., "127.0.0.1:16358")
     * @param ip Output IP address
     * @param port Output port number
     * @return true if parsing succeeded, false otherwise
     */
    bool ParseEndpoint(const std::string& endpoint, std::string& ip,
                       uint16_t& port);

    /**
     * @brief Test TCP connection to an endpoint
     * @param ip IP address
     * @param port Port number
     * @param timeout Maximum time to wait for connection
     * @return Validation result
     */
    ValidationResult TestConnection(const std::string& ip, uint16_t port,
                                    std::chrono::milliseconds timeout);
};

}  // namespace mooncake
