#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "snapshot_provider.h"
#include "metadata_store.h"
#include "types.h"

namespace mooncake {

/**
 * @brief EtcdSnapshotProvider is an implementation of SnapshotProvider that
 *        stores and loads metadata snapshots from etcd.
 *
 * Key structure:
 *   /snapshot/{cluster_id}/latest             -> snapshot_id (string)
 *   /snapshot/{cluster_id}/{snapshot_id}/data -> JSON serialized metadata
 *   /snapshot/{cluster_id}/{snapshot_id}/seq  -> sequence_id (string)
 *
 * This implementation:
 * - Uses etcd transactions to atomically save new snapshot and delete old one
 * - Only keeps the latest snapshot (deletes previous snapshot when saving new one)
 * - Serializes metadata to JSON format
 */
class EtcdSnapshotProvider : public SnapshotProvider {
   public:
    /**
     * @brief Constructor.
     * @param cluster_id: The cluster ID for this snapshot provider.
     */
    explicit EtcdSnapshotProvider(const std::string& cluster_id);

    /**
     * @brief Load the latest available snapshot for `cluster_id`.
     * @param cluster_id: The cluster ID (must match the one in constructor).
     * @param snapshot_id: Output param, opaque identifier of the snapshot.
     * @param snapshot_sequence_id: Output param, global OpLog sequence_id at snapshot boundary.
     * @param snapshot: Output param, full metadata baseline as key -> StandbyObjectMetadata.
     * @return true on success, false if no snapshot available or on error.
     */
    bool LoadLatestSnapshot(
        const std::string& cluster_id, std::string& snapshot_id,
        uint64_t& snapshot_sequence_id,
        std::vector<std::pair<std::string, StandbyObjectMetadata>>& snapshot) override;

    /**
     * @brief Save a snapshot to etcd.
     *        Atomically writes new snapshot data and deletes the previous snapshot.
     * @param snapshot_id: Unique identifier for this snapshot (e.g., timestamp).
     * @param sequence_id: The OpLog sequence_id at which this snapshot was taken.
     * @param metadata: The metadata entries to save (key -> StandbyObjectMetadata).
     * @return ErrorCode::OK on success, error code on failure.
     */
    ErrorCode SaveSnapshot(
        const std::string& snapshot_id,
        uint64_t sequence_id,
        const std::vector<std::pair<std::string, StandbyObjectMetadata>>& metadata);

    /**
     * @brief Get the current latest snapshot ID without loading data.
     * @param snapshot_id: Output param, the latest snapshot ID.
     * @return true if a snapshot exists, false otherwise.
     */
    bool GetLatestSnapshotId(std::string& snapshot_id) const;

    /**
     * @brief Dump current snapshot status for debugging.
     *        Logs the latest snapshot ID, sequence_id, and data size.
     *        Does NOT load the full metadata.
     */
    void DumpSnapshotStatus() const;

    /**
     * @brief Get snapshot statistics without loading full data.
     * @param snapshot_id: Output - latest snapshot ID (empty if none).
     * @param sequence_id: Output - sequence_id of latest snapshot (0 if none).
     * @param data_size_bytes: Output - size of snapshot data in bytes (0 if none).
     * @return true if a snapshot exists, false otherwise.
     */
    bool GetSnapshotStats(std::string& snapshot_id,
                          uint64_t& sequence_id,
                          size_t& data_size_bytes) const;

   private:
    // Build etcd key paths
    std::string BuildLatestKey() const;
    std::string BuildDataKey(const std::string& snapshot_id) const;
    std::string BuildSeqKey(const std::string& snapshot_id) const;

    // Serialize metadata to JSON
    std::string SerializeMetadata(
        const std::vector<std::pair<std::string, StandbyObjectMetadata>>& metadata) const;

    // Deserialize metadata from JSON
    bool DeserializeMetadata(
        const std::string& json,
        std::vector<std::pair<std::string, StandbyObjectMetadata>>& metadata) const;

    std::string cluster_id_;
};

}  // namespace mooncake
