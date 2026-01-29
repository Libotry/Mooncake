#include "etcd_snapshot_provider.h"

#include <glog/logging.h>
#include <chrono>

#if __has_include(<jsoncpp/json/json.h>)
#include <jsoncpp/json/json.h>  // Ubuntu
#else
#include <json/json.h>  // CentOS
#endif

#include "etcd_helper.h"

namespace mooncake {

// Key prefix for snapshots
static constexpr const char* kSnapshotPrefix = "/snapshot/";
static constexpr const char* kLatestSuffix = "/latest";
static constexpr const char* kDataSuffix = "/data";
static constexpr const char* kSeqSuffix = "/seq";

// Helper to get current timestamp in milliseconds for timing
static inline int64_t GetCurrentTimeMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

EtcdSnapshotProvider::EtcdSnapshotProvider(const std::string& cluster_id)
    : cluster_id_(cluster_id) {
    // Normalize cluster_id to avoid double slashes
    while (!cluster_id_.empty() && cluster_id_.back() == '/') {
        cluster_id_.pop_back();
    }
    LOG(INFO) << "[EtcdSnapshotProvider] Initialized with cluster_id=" << cluster_id_
              << ", latest_key=" << BuildLatestKey();
}

std::string EtcdSnapshotProvider::BuildLatestKey() const {
    return std::string(kSnapshotPrefix) + cluster_id_ + kLatestSuffix;
}

std::string EtcdSnapshotProvider::BuildDataKey(const std::string& snapshot_id) const {
    return std::string(kSnapshotPrefix) + cluster_id_ + "/" + snapshot_id + kDataSuffix;
}

std::string EtcdSnapshotProvider::BuildSeqKey(const std::string& snapshot_id) const {
    return std::string(kSnapshotPrefix) + cluster_id_ + "/" + snapshot_id + kSeqSuffix;
}

std::string EtcdSnapshotProvider::SerializeMetadata(
    const std::vector<std::pair<std::string, StandbyObjectMetadata>>& metadata) const {
    int64_t start_time = GetCurrentTimeMs();
    size_t memory_replicas = 0, disk_replicas = 0, local_disk_replicas = 0;
    
    Json::Value root(Json::arrayValue);
    
    for (const auto& [key, meta] : metadata) {
        Json::Value entry;
        entry["key"] = key;
        entry["client_id_high"] = static_cast<Json::UInt64>(meta.client_id.first);
        entry["client_id_low"] = static_cast<Json::UInt64>(meta.client_id.second);
        entry["size"] = static_cast<Json::UInt64>(meta.size);
        entry["last_sequence_id"] = static_cast<Json::UInt64>(meta.last_sequence_id);
        
        // Serialize replicas
        Json::Value replicas_array(Json::arrayValue);
        for (const auto& replica_desc : meta.replicas) {
            Json::Value replica_json;
            // Check which variant type this is
            if (std::holds_alternative<MemoryDescriptor>(replica_desc.descriptor_variant)) {
                const auto& mem_desc = std::get<MemoryDescriptor>(replica_desc.descriptor_variant);
                replica_json["type"] = "memory";
                // Serialize AllocatedBuffer::Descriptor
                const auto& buf_desc = mem_desc.buffer_descriptor;
                replica_json["size"] = static_cast<Json::UInt64>(buf_desc.size_);
                replica_json["buffer_address"] = static_cast<Json::UInt64>(buf_desc.buffer_address_);
                replica_json["transport_endpoint"] = buf_desc.transport_endpoint_;
                ++memory_replicas;
            } else if (std::holds_alternative<DiskDescriptor>(replica_desc.descriptor_variant)) {
                const auto& disk_desc = std::get<DiskDescriptor>(replica_desc.descriptor_variant);
                replica_json["type"] = "disk";
                replica_json["file_path"] = disk_desc.file_path;
                replica_json["object_size"] = static_cast<Json::UInt64>(disk_desc.object_size);
                ++disk_replicas;
            } else if (std::holds_alternative<LocalDiskDescriptor>(replica_desc.descriptor_variant)) {
                const auto& local_desc = std::get<LocalDiskDescriptor>(replica_desc.descriptor_variant);
                replica_json["type"] = "local_disk";
                replica_json["client_id_high"] = static_cast<Json::UInt64>(local_desc.client_id.first);
                replica_json["client_id_low"] = static_cast<Json::UInt64>(local_desc.client_id.second);
                replica_json["object_size"] = static_cast<Json::UInt64>(local_desc.object_size);
                replica_json["transport_endpoint"] = local_desc.transport_endpoint;
                ++local_disk_replicas;
            }
            replica_json["status"] = static_cast<int>(replica_desc.status);
            replicas_array.append(replica_json);
        }
        entry["replicas"] = replicas_array;
        
        root.append(entry);
    }
    
    Json::StreamWriterBuilder writer;
    writer["indentation"] = "";  // Compact format
    std::string result = Json::writeString(writer, root);
    
    int64_t elapsed = GetCurrentTimeMs() - start_time;
    VLOG(1) << "[EtcdSnapshotProvider] SerializeMetadata: entries=" << metadata.size()
            << ", memory_replicas=" << memory_replicas
            << ", disk_replicas=" << disk_replicas
            << ", local_disk_replicas=" << local_disk_replicas
            << ", json_size=" << result.size() << " bytes"
            << ", elapsed=" << elapsed << "ms";
    
    return result;
}

bool EtcdSnapshotProvider::DeserializeMetadata(
    const std::string& json,
    std::vector<std::pair<std::string, StandbyObjectMetadata>>& metadata) const {
    int64_t start_time = GetCurrentTimeMs();
    size_t memory_replicas = 0, disk_replicas = 0, local_disk_replicas = 0;
    
    Json::CharReaderBuilder reader;
    Json::Value root;
    std::string errs;
    std::istringstream stream(json);
    
    if (!Json::parseFromStream(reader, stream, &root, &errs)) {
        LOG(ERROR) << "[EtcdSnapshotProvider] Failed to parse snapshot JSON: " << errs
                   << ", json_size=" << json.size()
                   << ", first_100_chars=" << json.substr(0, std::min(json.size(), size_t(100)));
        return false;
    }
    
    if (!root.isArray()) {
        LOG(ERROR) << "[EtcdSnapshotProvider] Snapshot JSON root is not an array"
                   << ", type=" << root.type();
        return false;
    }
    
    VLOG(1) << "[EtcdSnapshotProvider] DeserializeMetadata: parsing " << root.size() << " entries";
    
    metadata.clear();
    metadata.reserve(root.size());
    
    for (const auto& entry : root) {
        std::string key = entry["key"].asString();
        StandbyObjectMetadata meta;
        meta.client_id.first = entry["client_id_high"].asUInt64();
        meta.client_id.second = entry["client_id_low"].asUInt64();
        meta.size = entry["size"].asUInt64();
        meta.last_sequence_id = entry["last_sequence_id"].asUInt64();
        
        // Deserialize replicas
        const Json::Value& replicas_array = entry["replicas"];
        for (const auto& replica_json : replicas_array) {
            Replica::Descriptor replica_desc;
            std::string type = replica_json["type"].asString();
            
            if (type == "memory") {
                MemoryDescriptor mem_desc;
                mem_desc.buffer_descriptor.size_ = replica_json["size"].asUInt64();
                mem_desc.buffer_descriptor.buffer_address_ = replica_json["buffer_address"].asUInt64();
                mem_desc.buffer_descriptor.transport_endpoint_ = replica_json["transport_endpoint"].asString();
                replica_desc.descriptor_variant = mem_desc;
                ++memory_replicas;
            } else if (type == "disk") {
                DiskDescriptor disk_desc;
                disk_desc.file_path = replica_json["file_path"].asString();
                disk_desc.object_size = replica_json["object_size"].asUInt64();
                replica_desc.descriptor_variant = disk_desc;
                ++disk_replicas;
            } else if (type == "local_disk") {
                LocalDiskDescriptor local_desc;
                local_desc.client_id.first = replica_json["client_id_high"].asUInt64();
                local_desc.client_id.second = replica_json["client_id_low"].asUInt64();
                local_desc.object_size = replica_json["object_size"].asUInt64();
                local_desc.transport_endpoint = replica_json["transport_endpoint"].asString();
                replica_desc.descriptor_variant = local_desc;
                ++local_disk_replicas;
            } else {
                LOG(WARNING) << "[EtcdSnapshotProvider] Unknown replica type: " << type
                             << " for key=" << key;
                continue;
            }
            
            replica_desc.status = static_cast<ReplicaStatus>(replica_json["status"].asInt());
            meta.replicas.push_back(replica_desc);
        }
        
        metadata.emplace_back(key, std::move(meta));
    }
    
    int64_t elapsed = GetCurrentTimeMs() - start_time;
    VLOG(1) << "[EtcdSnapshotProvider] DeserializeMetadata: entries=" << metadata.size()
            << ", memory_replicas=" << memory_replicas
            << ", disk_replicas=" << disk_replicas
            << ", local_disk_replicas=" << local_disk_replicas
            << ", json_size=" << json.size() << " bytes"
            << ", elapsed=" << elapsed << "ms";
    
    return true;
}

bool EtcdSnapshotProvider::GetLatestSnapshotId(std::string& snapshot_id) const {
    std::string latest_key = BuildLatestKey();
    EtcdRevisionId revision_id;
    ErrorCode err = EtcdHelper::Get(latest_key.c_str(), latest_key.size(),
                                    snapshot_id, revision_id);
    if (err == ErrorCode::OK && !snapshot_id.empty()) {
        VLOG(1) << "[EtcdSnapshotProvider] GetLatestSnapshotId: snapshot_id=" << snapshot_id
                << ", etcd_revision=" << revision_id;
        return true;
    }
    VLOG(1) << "[EtcdSnapshotProvider] GetLatestSnapshotId: no snapshot found"
            << ", error=" << static_cast<int>(err);
    return false;
}

bool EtcdSnapshotProvider::LoadLatestSnapshot(
    const std::string& cluster_id, std::string& snapshot_id,
    uint64_t& snapshot_sequence_id,
    std::vector<std::pair<std::string, StandbyObjectMetadata>>& snapshot) {
    
    int64_t start_time = GetCurrentTimeMs();
    LOG(INFO) << "[EtcdSnapshotProvider] LoadLatestSnapshot: starting for cluster_id=" << cluster_id;
    
    if (cluster_id != cluster_id_) {
        LOG(ERROR) << "[EtcdSnapshotProvider] Cluster ID mismatch: expected=" << cluster_id_
                   << ", got=" << cluster_id;
        return false;
    }
    
    // Get the latest snapshot ID
    if (!GetLatestSnapshotId(snapshot_id)) {
        LOG(INFO) << "[EtcdSnapshotProvider] No snapshot available for cluster_id=" << cluster_id_;
        snapshot_id.clear();
        snapshot_sequence_id = 0;
        snapshot.clear();
        return false;
    }
    
    // Get the sequence_id
    std::string seq_key = BuildSeqKey(snapshot_id);
    std::string seq_value;
    EtcdRevisionId revision_id;
    VLOG(1) << "[EtcdSnapshotProvider] LoadLatestSnapshot: fetching seq_key=" << seq_key;
    ErrorCode err = EtcdHelper::Get(seq_key.c_str(), seq_key.size(),
                                    seq_value, revision_id);
    if (err != ErrorCode::OK) {
        LOG(ERROR) << "[EtcdSnapshotProvider] Failed to get snapshot sequence_id"
                   << ", snapshot_id=" << snapshot_id
                   << ", seq_key=" << seq_key
                   << ", error=" << static_cast<int>(err);
        return false;
    }
    
    try {
        snapshot_sequence_id = std::stoull(seq_value);
    } catch (const std::exception& e) {
        LOG(ERROR) << "[EtcdSnapshotProvider] Failed to parse snapshot sequence_id"
                   << ", seq_value=" << seq_value
                   << ", error=" << e.what();
        return false;
    }
    
    // Get the snapshot data
    std::string data_key = BuildDataKey(snapshot_id);
    std::string data_value;
    VLOG(1) << "[EtcdSnapshotProvider] LoadLatestSnapshot: fetching data_key=" << data_key;
    err = EtcdHelper::Get(data_key.c_str(), data_key.size(),
                          data_value, revision_id);
    if (err != ErrorCode::OK) {
        LOG(ERROR) << "[EtcdSnapshotProvider] Failed to get snapshot data"
                   << ", snapshot_id=" << snapshot_id
                   << ", data_key=" << data_key
                   << ", error=" << static_cast<int>(err);
        return false;
    }
    
    VLOG(1) << "[EtcdSnapshotProvider] LoadLatestSnapshot: data_size=" << data_value.size()
            << ", deserializing...";
    
    // Deserialize the metadata
    if (!DeserializeMetadata(data_value, snapshot)) {
        LOG(ERROR) << "[EtcdSnapshotProvider] Failed to deserialize snapshot data"
                   << ", snapshot_id=" << snapshot_id
                   << ", data_size=" << data_value.size();
        return false;
    }
    
    int64_t elapsed = GetCurrentTimeMs() - start_time;
    LOG(INFO) << "[EtcdSnapshotProvider] LoadLatestSnapshot: SUCCESS"
              << ", snapshot_id=" << snapshot_id
              << ", sequence_id=" << snapshot_sequence_id
              << ", entries=" << snapshot.size()
              << ", data_size=" << data_value.size() << " bytes"
              << ", elapsed=" << elapsed << "ms";
    return true;
}

ErrorCode EtcdSnapshotProvider::SaveSnapshot(
    const std::string& snapshot_id,
    uint64_t sequence_id,
    const std::vector<std::pair<std::string, StandbyObjectMetadata>>& metadata) {
    
    int64_t start_time = GetCurrentTimeMs();
    LOG(INFO) << "[EtcdSnapshotProvider] SaveSnapshot: starting"
              << ", snapshot_id=" << snapshot_id
              << ", sequence_id=" << sequence_id
              << ", entries=" << metadata.size();
    
    // Get the current latest snapshot ID (to delete later)
    std::string old_snapshot_id;
    bool has_old_snapshot = GetLatestSnapshotId(old_snapshot_id);
    
    // Don't delete if the old snapshot is the same as the new one
    if (has_old_snapshot && old_snapshot_id == snapshot_id) {
        VLOG(1) << "[EtcdSnapshotProvider] SaveSnapshot: old snapshot same as new, skipping delete";
        has_old_snapshot = false;
    }
    
    // Build keys for new snapshot
    std::string latest_key = BuildLatestKey();
    std::string data_key = BuildDataKey(snapshot_id);
    std::string seq_key = BuildSeqKey(snapshot_id);
    
    VLOG(1) << "[EtcdSnapshotProvider] SaveSnapshot: keys"
            << ", latest_key=" << latest_key
            << ", data_key=" << data_key
            << ", seq_key=" << seq_key;
    
    // Build keys for old snapshot (if exists)
    std::string old_data_key;
    std::string old_seq_key;
    if (has_old_snapshot) {
        old_data_key = BuildDataKey(old_snapshot_id);
        old_seq_key = BuildSeqKey(old_snapshot_id);
        VLOG(1) << "[EtcdSnapshotProvider] SaveSnapshot: will delete old snapshot"
                << ", old_snapshot_id=" << old_snapshot_id
                << ", old_data_key=" << old_data_key
                << ", old_seq_key=" << old_seq_key;
    }
    
    // Serialize metadata
    int64_t serialize_start = GetCurrentTimeMs();
    std::string data_value = SerializeMetadata(metadata);
    int64_t serialize_elapsed = GetCurrentTimeMs() - serialize_start;
    std::string seq_value = std::to_string(sequence_id);
    
    LOG(INFO) << "[EtcdSnapshotProvider] SaveSnapshot: serialized"
              << ", snapshot_id=" << snapshot_id
              << ", sequence_id=" << sequence_id
              << ", entries=" << metadata.size()
              << ", data_size=" << data_value.size() << " bytes"
              << ", serialize_time=" << serialize_elapsed << "ms"
              << (has_old_snapshot ? ", deleting_old=" + old_snapshot_id : "");
    
    // Execute the atomic transaction
    int64_t txn_start = GetCurrentTimeMs();
    ErrorCode err = EtcdHelper::SaveSnapshotTxn(
        data_key, data_value,
        seq_key, seq_value,
        latest_key, snapshot_id,
        old_data_key, old_seq_key);
    int64_t txn_elapsed = GetCurrentTimeMs() - txn_start;
    
    if (err != ErrorCode::OK) {
        LOG(ERROR) << "[EtcdSnapshotProvider] SaveSnapshot: FAILED"
                   << ", snapshot_id=" << snapshot_id
                   << ", error=" << static_cast<int>(err)
                   << ", txn_time=" << txn_elapsed << "ms";
        return err;
    }
    
    int64_t total_elapsed = GetCurrentTimeMs() - start_time;
    LOG(INFO) << "[EtcdSnapshotProvider] SaveSnapshot: SUCCESS"
              << ", snapshot_id=" << snapshot_id
              << ", sequence_id=" << sequence_id
              << ", entries=" << metadata.size()
              << ", data_size=" << data_value.size() << " bytes"
              << ", serialize_time=" << serialize_elapsed << "ms"
              << ", txn_time=" << txn_elapsed << "ms"
              << ", total_time=" << total_elapsed << "ms";
    return ErrorCode::OK;
}

void EtcdSnapshotProvider::DumpSnapshotStatus() const {
    std::string snapshot_id;
    uint64_t sequence_id = 0;
    size_t data_size_bytes = 0;
    
    bool has_snapshot = GetSnapshotStats(snapshot_id, sequence_id, data_size_bytes);
    
    if (has_snapshot) {
        LOG(INFO) << "[EtcdSnapshotProvider] === Snapshot Status ==="
                  << "\n  cluster_id: " << cluster_id_
                  << "\n  latest_key: " << BuildLatestKey()
                  << "\n  snapshot_id: " << snapshot_id
                  << "\n  sequence_id: " << sequence_id
                  << "\n  data_size: " << data_size_bytes << " bytes"
                  << "\n  data_key: " << BuildDataKey(snapshot_id)
                  << "\n  seq_key: " << BuildSeqKey(snapshot_id);
    } else {
        LOG(INFO) << "[EtcdSnapshotProvider] === Snapshot Status ==="
                  << "\n  cluster_id: " << cluster_id_
                  << "\n  latest_key: " << BuildLatestKey()
                  << "\n  status: NO SNAPSHOT AVAILABLE";
    }
}

bool EtcdSnapshotProvider::GetSnapshotStats(std::string& snapshot_id,
                                            uint64_t& sequence_id,
                                            size_t& data_size_bytes) const {
    snapshot_id.clear();
    sequence_id = 0;
    data_size_bytes = 0;
    
    // Get latest snapshot ID
    if (!GetLatestSnapshotId(snapshot_id)) {
        return false;
    }
    
    // Get sequence_id
    std::string seq_key = BuildSeqKey(snapshot_id);
    std::string seq_value;
    EtcdRevisionId revision_id;
    ErrorCode err = EtcdHelper::Get(seq_key.c_str(), seq_key.size(),
                                    seq_value, revision_id);
    if (err == ErrorCode::OK) {
        try {
            sequence_id = std::stoull(seq_value);
        } catch (...) {
            // Ignore parse errors
        }
    }
    
    // Get data size (fetch data to get size, but don't parse it)
    std::string data_key = BuildDataKey(snapshot_id);
    std::string data_value;
    err = EtcdHelper::Get(data_key.c_str(), data_key.size(),
                          data_value, revision_id);
    if (err == ErrorCode::OK) {
        data_size_bytes = data_value.size();
    }
    
    return true;
}

}  // namespace mooncake
