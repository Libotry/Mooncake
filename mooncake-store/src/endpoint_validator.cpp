#include "endpoint_validator.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <sstream>
#include <thread>
#include <vector>

#include <glog/logging.h>
#include <boost/algorithm/string.hpp>

namespace mooncake {

bool EndpointValidator::ParseEndpoint(const std::string& endpoint,
                                      std::string& ip, uint16_t& port) {
    if (endpoint.empty()) {
        return false;
    }

    // Find the last colon (to handle IPv6 addresses)
    size_t colon_pos = endpoint.rfind(':');
    if (colon_pos == std::string::npos || colon_pos == 0 ||
        colon_pos == endpoint.length() - 1) {
        return false;
    }

    ip = endpoint.substr(0, colon_pos);
    std::string port_str = endpoint.substr(colon_pos + 1);

    // Validate IP address (basic check)
    if (ip.empty() || ip.length() > 64) {
        return false;
    }

    // Parse port
    try {
        int port_int = std::stoi(port_str);
        if (port_int < 1 || port_int > 65535) {
            return false;
        }
        port = static_cast<uint16_t>(port_int);
    } catch (const std::exception&) {
        return false;
    }

    return true;
}

EndpointValidator::ValidationResult EndpointValidator::TestConnection(
    const std::string& ip, uint16_t port, std::chrono::milliseconds timeout) {
    auto start_time = std::chrono::steady_clock::now();

    // Create socket
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        return {false, {}, "Failed to create socket: " + std::string(strerror(errno))};
    }

    // Set socket to non-blocking mode
    int flags = fcntl(sock, F_GETFL, 0);
    if (flags < 0 || fcntl(sock, F_SETFL, flags | O_NONBLOCK) < 0) {
        close(sock);
        return {false, {}, "Failed to set socket to non-blocking: " + std::string(strerror(errno))};
    }

    // Prepare address
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);

    // Convert IP string to binary format
    if (inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) <= 0) {
        close(sock);
        return {false, {}, "Invalid IP address: " + ip};
    }

    // Try to connect (non-blocking)
    int result = connect(sock, (struct sockaddr*)&addr, sizeof(addr));

    if (result == 0) {
        // Connection succeeded immediately
        auto end_time = std::chrono::steady_clock::now();
        auto latency = std::chrono::duration_cast<std::chrono::milliseconds>(
            end_time - start_time);
        close(sock);
        return {true, latency, ""};
    }

    if (errno != EINPROGRESS) {
        std::string error_msg = "Connection failed: " + std::string(strerror(errno));
        close(sock);
        return {false, {}, error_msg};
    }

    // Connection is in progress, wait for it to complete using select
    fd_set writefds, errorfds;
    FD_ZERO(&writefds);
    FD_ZERO(&errorfds);
    FD_SET(sock, &writefds);
    FD_SET(sock, &errorfds);

    struct timeval tv;
    tv.tv_sec = timeout.count() / 1000;
    tv.tv_usec = (timeout.count() % 1000) * 1000;

    result = select(sock + 1, nullptr, &writefds, &errorfds, &tv);

    if (result > 0) {
        if (FD_ISSET(sock, &errorfds)) {
            // Connection error
            int so_error;
            socklen_t len = sizeof(so_error);
            getsockopt(sock, SOL_SOCKET, SO_ERROR, &so_error, &len);
            close(sock);
            return {false, {}, "Connection error: " + std::string(strerror(so_error))};
        }

        if (FD_ISSET(sock, &writefds)) {
            // Connection succeeded
            int so_error;
            socklen_t len = sizeof(so_error);
            getsockopt(sock, SOL_SOCKET, SO_ERROR, &so_error, &len);

            if (so_error == 0) {
                auto end_time = std::chrono::steady_clock::now();
                auto latency = std::chrono::duration_cast<std::chrono::milliseconds>(
                    end_time - start_time);
                close(sock);
                return {true, latency, ""};
            } else {
                close(sock);
                return {false, {}, "Connection failed: " + std::string(strerror(so_error))};
            }
        }
    }

    // Timeout or select error
    close(sock);
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start_time);
    return {false, elapsed, "Connection timeout"};
}

EndpointValidator::ValidationResult EndpointValidator::ValidateEndpoint(
    const EndpointInfo& endpoint_info, std::chrono::milliseconds timeout) {
    std::string ip;
    uint16_t port;

    if (!ParseEndpoint(endpoint_info.endpoint, ip, port)) {
        return {false, {}, "Invalid endpoint format: " + endpoint_info.endpoint};
    }

    return TestConnection(ip, port, timeout);
}

std::unordered_map<std::string, EndpointValidator::ValidationResult>
EndpointValidator::ValidateEndpointsBatch(
    const std::vector<EndpointInfo>& endpoints,
    std::chrono::milliseconds timeout, size_t max_concurrent) {
    std::unordered_map<std::string, ValidationResult> results;

    if (endpoints.empty()) {
        return results;
    }

    // Limit concurrent validations
    max_concurrent = std::min(max_concurrent, endpoints.size());

    // Group endpoints by their string (to deduplicate)
    std::unordered_map<std::string, std::vector<const EndpointInfo*>> endpoint_map;
    for (const auto& info : endpoints) {
        endpoint_map[info.endpoint].push_back(&info);
    }

    // Validate unique endpoints
    std::vector<std::string> unique_endpoints;
    unique_endpoints.reserve(endpoint_map.size());
    for (const auto& [endpoint, _] : endpoint_map) {
        unique_endpoints.push_back(endpoint);
    }

    LOG(INFO) << "Validating " << unique_endpoints.size() << " unique endpoints "
              << "(from " << endpoints.size() << " total), "
              << "max_concurrent=" << max_concurrent;

    // Validate in batches with limited concurrency
    size_t num_unique = unique_endpoints.size();
    for (size_t i = 0; i < num_unique; i += max_concurrent) {
        size_t batch_size = std::min(max_concurrent, num_unique - i);

        std::vector<std::thread> threads;
        std::vector<std::pair<std::string, ValidationResult>> batch_results(
            batch_size);

        // Start concurrent validations for this batch
        for (size_t j = 0; j < batch_size; ++j) {
            size_t idx = i + j;
            const std::string& endpoint = unique_endpoints[idx];

            threads.emplace_back([this, endpoint, timeout, &batch_results, j]() {
                EndpointInfo info;
                info.endpoint = endpoint;
                info.type = EndpointInfo::REPLICA_TRANSPORT_ENDPOINT;  // Default type
                batch_results[j] = {endpoint, ValidateEndpoint(info, timeout)};
            });
        }

        // Wait for all threads in this batch to complete
        for (auto& thread : threads) {
            thread.join();
        }

        // Store results
        for (const auto& [endpoint, result] : batch_results) {
            results[endpoint] = result;
        }
    }

    LOG(INFO) << "Endpoint validation completed: "
              << std::count_if(results.begin(), results.end(),
                               [](const auto& p) { return p.second.is_reachable; })
              << " reachable, "
              << std::count_if(results.begin(), results.end(),
                               [](const auto& p) { return !p.second.is_reachable; })
              << " stale";

    return results;
}

}  // namespace mooncake
