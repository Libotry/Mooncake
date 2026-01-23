#include <iostream>
#include <iomanip>
#include "../include/types.h"
#include "../include/metadata_store.h"
#include "../include/replica.h"
#include "ylt/struct_pack.hpp"
#include <xxhash.h>

void print_hex(const std::string& data, const std::string& label) {
    std::cout << label << " (size=" << data.size() << "): ";
    for (size_t i = 0; i < std::min(data.size(), size_t(64)); ++i) {
        std::cout << std::hex << std::setfill('0') << std::setw(2) 
                  << (int)(unsigned char)data[i] << " ";
    }
    if (data.size() > 64) {
        std::cout << "...";
    }
    std::cout << std::dec << std::endl;
}

uint32_t compute_checksum(const std::string& data) {
    return static_cast<uint32_t>(XXH32(data.data(), data.size(), 0));
}

int main() {
    using namespace mooncake;
    
    std::cout << "=== Testing struct_pack serialization stability ===" << std::endl;
    
    // Create a simple MetadataPayload
    MetadataPayload payload1;
    payload1.client_id = std::make_pair(12345678ULL, 87654321ULL);
    payload1.size = 1024 * 1024;  // 1MB
    
    // Add a LocalDiskDescriptor
    LocalDiskDescriptor local_disk;
    local_disk.client_id = std::make_pair(11111111ULL, 22222222ULL);
    local_disk.object_size = 2048;
    local_disk.transport_endpoint = "tcp://localhost:5000";
    
    Replica::Descriptor desc_variant = local_disk;
    payload1.replicas.push_back(desc_variant);
    
    // Serialize twice
    auto result1 = struct_pack::serialize(payload1);
    std::string serialized1(result1.begin(), result1.end());
    
    auto result2 = struct_pack::serialize(payload1);
    std::string serialized2(result2.begin(), result2.end());
    
    print_hex(serialized1, "Serialized1");
    print_hex(serialized2, "Serialized2");
    
    uint32_t checksum1 = compute_checksum(serialized1);
    uint32_t checksum2 = compute_checksum(serialized2);
    
    std::cout << "Checksum1: 0x" << std::hex << checksum1 << std::endl;
    std::cout << "Checksum2: 0x" << std::hex << checksum2 << std::endl;
    std::cout << "Checksums match: " << (checksum1 == checksum2 ? "YES" : "NO") << std::endl;
    std::cout << std::dec;
    
    // Test deserialization
    MetadataPayload payload_decoded;
    auto decode_result = struct_pack::deserialize_to(payload_decoded, serialized1);
    
    if (decode_result == struct_pack::errc::ok) {
        std::cout << "\nDeserialization: SUCCESS" << std::endl;
        std::cout << "  client_id: " << payload_decoded.client_id.first 
                  << "-" << payload_decoded.client_id.second << std::endl;
        std::cout << "  size: " << payload_decoded.size << std::endl;
        std::cout << "  replicas count: " << payload_decoded.replicas.size() << std::endl;
        
        if (!payload_decoded.replicas.empty()) {
            auto& replica_desc = payload_decoded.replicas[0];
            if (std::holds_alternative<LocalDiskDescriptor>(replica_desc)) {
                auto& ld = std::get<LocalDiskDescriptor>(replica_desc);
                std::cout << "  replica[0] LocalDiskDescriptor:" << std::endl;
                std::cout << "    client_id: " << ld.client_id.first << "-" << ld.client_id.second << std::endl;
                std::cout << "    object_size: " << ld.object_size << std::endl;
                std::cout << "    transport_endpoint: " << ld.transport_endpoint << std::endl;
            }
        }
    } else {
        std::cout << "\nDeserialization: FAILED" << std::endl;
        std::cout << "  error_code: " << static_cast<int>(decode_result) << std::endl;
    }
    
    return 0;
}
