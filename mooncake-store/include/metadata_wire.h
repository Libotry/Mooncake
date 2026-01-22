#pragma once

#include <cstdint>
#include <string>
#include <variant>

#include "replica.h"

namespace mooncake {

// Wire-format for JSON serialization/deserialization.
// Keeps schema stable with split UUID fields.
struct LocalDiskDescriptorWire {
    uint64_t client_id_first{0};   // UUID.first
    uint64_t client_id_second{0};  // UUID.second
    uint64_t object_size{0};
    std::string transport_endpoint;

    UUID GetClientId() const { return {client_id_first, client_id_second}; }

    YLT_REFL(LocalDiskDescriptorWire, client_id_first, client_id_second,
             object_size, transport_endpoint);
};

// Wire-format replica descriptor for JSON. This is intentionally separate from
// Replica::Descriptor so we can keep internal types clean while maintaining a
// stable JSON schema.
struct ReplicaDescriptorWire {
    std::variant<MemoryDescriptor, DiskDescriptor, LocalDiskDescriptorWire>
        descriptor_variant;
    ReplicaStatus status;
    YLT_REFL(ReplicaDescriptorWire, descriptor_variant, status);
};

static inline LocalDiskDescriptorWire ToWireLocalDiskDescriptor(
    const LocalDiskDescriptor& d) {
    return LocalDiskDescriptorWire{d.client_id.first, d.client_id.second,
                                   d.object_size, d.transport_endpoint};
}

static inline LocalDiskDescriptor FromWireLocalDiskDescriptor(
    const LocalDiskDescriptorWire& wire) {
    return LocalDiskDescriptor{wire.GetClientId(), wire.object_size,
                               wire.transport_endpoint};
}

static inline ReplicaDescriptorWire ToWireReplicaDescriptor(
    const Replica::Descriptor& d) {
    ReplicaDescriptorWire out;
    out.status = d.status;

    if (std::holds_alternative<MemoryDescriptor>(d.descriptor_variant)) {
        out.descriptor_variant = std::get<MemoryDescriptor>(d.descriptor_variant);
    } else if (std::holds_alternative<DiskDescriptor>(d.descriptor_variant)) {
        out.descriptor_variant = std::get<DiskDescriptor>(d.descriptor_variant);
    } else {
        const auto& ld = std::get<LocalDiskDescriptor>(d.descriptor_variant);
        out.descriptor_variant = ToWireLocalDiskDescriptor(ld);
    }

    return out;
}

static inline Replica::Descriptor FromWireReplicaDescriptor(
    const ReplicaDescriptorWire& wire) {
    Replica::Descriptor out;
    out.status = wire.status;

    if (std::holds_alternative<MemoryDescriptor>(wire.descriptor_variant)) {
        out.descriptor_variant =
            std::get<MemoryDescriptor>(wire.descriptor_variant);
    } else if (std::holds_alternative<DiskDescriptor>(wire.descriptor_variant)) {
        out.descriptor_variant = std::get<DiskDescriptor>(wire.descriptor_variant);
    } else {
        const auto& ld =
            std::get<LocalDiskDescriptorWire>(wire.descriptor_variant);
        out.descriptor_variant = FromWireLocalDiskDescriptor(ld);
    }

    return out;
}

}  // namespace mooncake
