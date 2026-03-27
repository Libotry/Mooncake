#include "metadata_store.h"

#include <glog/logging.h>

namespace mooncake {

void StandbySegmentRegistry::OnSegmentMount(const StandbySegmentInfo& info) {
    std::lock_guard<std::shared_mutex> lock(mutex_);
    segments_[info.transport_endpoint] = info;
    LOG(INFO) << "StandbySegmentRegistry: mounted segment, endpoint=" << info.transport_endpoint
              << ", name=" << info.segment_name
              << ", is_memory=" << info.is_memory_segment;
}

void StandbySegmentRegistry::OnSegmentUnmount(const std::string& transport_endpoint) {
    std::lock_guard<std::shared_mutex> lock(mutex_);
    auto it = segments_.find(transport_endpoint);
    if (it != segments_.end()) {
        LOG(INFO) << "StandbySegmentRegistry: unmounted segment, endpoint="
                  << transport_endpoint << ", name=" << it->second.segment_name;
        segments_.erase(it);
    } else {
        VLOG(1) << "StandbySegmentRegistry: unmount unknown segment, endpoint="
                << transport_endpoint;
    }
}

bool StandbySegmentRegistry::HasSegment(const std::string& transport_endpoint) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return segments_.find(transport_endpoint) != segments_.end();
}

std::optional<StandbySegmentInfo> StandbySegmentRegistry::GetSegment(
    const std::string& transport_endpoint) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    auto it = segments_.find(transport_endpoint);
    if (it != segments_.end()) {
        return it->second;
    }
    return std::nullopt;
}

std::vector<StandbySegmentInfo> StandbySegmentRegistry::GetAllSegments() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    std::vector<StandbySegmentInfo> result;
    result.reserve(segments_.size());
    for (const auto& kv : segments_) {
        result.push_back(kv.second);
    }
    return result;
}

void StandbySegmentRegistry::Clear() {
    std::lock_guard<std::shared_mutex> lock(mutex_);
    segments_.clear();
    LOG(INFO) << "StandbySegmentRegistry: cleared all segments";
}

}  // namespace mooncake
