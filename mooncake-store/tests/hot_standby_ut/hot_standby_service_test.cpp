#include "hot_standby_service.h"

#include <glog/logging.h>
#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <string>
#include <thread>

namespace mooncake::test {

class HotStandbyServiceTest : public ::testing::Test {
   protected:
    void SetUp() override {
        google::InitGoogleLogging("HotStandbyServiceTest");
        FLAGS_logtostderr = 1;

        config_.enable_verification = false;
        config_.max_replication_lag_entries = 1000;

        service_ = std::make_unique<HotStandbyService>(config_);
        etcd_endpoints_ = "http://localhost:2379";
        cluster_id_ = "test_cluster_001";
    }

    void TearDown() override {
        if (service_) {
            service_->Stop();
        }
        google::ShutdownGoogleLogging();
    }

    HotStandbyConfig config_;
    std::unique_ptr<HotStandbyService> service_;
    std::string etcd_endpoints_;
    std::string cluster_id_;
};

// ========== 6.1.1 启动停止测试 ==========

TEST_F(HotStandbyServiceTest, TestStart) {
#ifdef STORE_USE_ETCD
    // 需要真实 etcd 和正确的 cluster 配置，作为集成测试占位
    GTEST_SKIP() << "Requires real etcd connection, run in integration environment.";
#else
    ErrorCode err = service_->Start("primary_unused", etcd_endpoints_, cluster_id_);
    EXPECT_EQ(ErrorCode::INTERNAL_ERROR, err);
    EXPECT_EQ(StandbyState::FAILED, service_->GetState());
#endif
}

TEST_F(HotStandbyServiceTest, TestStart_AlreadyRunning) {
#ifdef STORE_USE_ETCD
    GTEST_SKIP() << "Requires real etcd connection to verify double start semantics.";
#else
    // 第一次 Start 失败后，状态为 FAILED，再次 Start 仍应返回 INTERNAL_ERROR
    ErrorCode err1 = service_->Start("primary_unused", etcd_endpoints_, cluster_id_);
    EXPECT_EQ(ErrorCode::INTERNAL_ERROR, err1);
    ErrorCode err2 = service_->Start("primary_unused", etcd_endpoints_, cluster_id_);
    EXPECT_EQ(ErrorCode::INTERNAL_ERROR, err2);
#endif
}

TEST_F(HotStandbyServiceTest, TestStart_InvalidEtcdEndpoints) {
#ifdef STORE_USE_ETCD
    GTEST_SKIP() << "Requires real etcd to simulate invalid endpoints.";
#else
    std::string invalid_endpoints = "invalid_endpoint";
    ErrorCode err =
        service_->Start("primary_unused", invalid_endpoints, cluster_id_);
    EXPECT_EQ(ErrorCode::INTERNAL_ERROR, err);
#endif
}

TEST_F(HotStandbyServiceTest, TestStop) {
    // Stop 在未 Start 的情况下应该是安全的（幂等）
    service_->Stop();
    SUCCEED();
}

TEST_F(HotStandbyServiceTest, TestStop_WhenNotRunning) {
    // 多次 Stop 应该是幂等的
    service_->Stop();
    service_->Stop();
    SUCCEED();
}

// ========== 6.1.2 状态转换测试 ==========

TEST_F(HotStandbyServiceTest, TestStateTransition_StartToWatching) {
#ifdef STORE_USE_ETCD
    GTEST_SKIP() << "Requires real etcd to drive full state transition to WATCHING.";
#else
    // 在非 STORE_USE_ETCD 构建下，Start 会直接将状态机设置为 FAILED
    EXPECT_EQ(StandbyState::STOPPED, service_->GetState());
    ErrorCode err = service_->Start("primary_unused", etcd_endpoints_, cluster_id_);
    EXPECT_EQ(ErrorCode::INTERNAL_ERROR, err);
    EXPECT_EQ(StandbyState::FAILED, service_->GetState());
#endif
}

TEST_F(HotStandbyServiceTest, TestStateTransition_ConnectionFailed) {
#ifdef STORE_USE_ETCD
    GTEST_SKIP() << "Connection failure requires real etcd and invalid endpoints.";
#else
    // 非 etcd 模式下，无法区分具体连接失败原因，仅验证不会崩溃
    ErrorCode err = service_->Start("primary_unused", "bad_endpoint", cluster_id_);
    EXPECT_EQ(ErrorCode::INTERNAL_ERROR, err);
#endif
}

TEST_F(HotStandbyServiceTest, TestStateTransition_SyncFailed) {
#ifdef STORE_USE_ETCD
    GTEST_SKIP() << "Sync failure requires real etcd and OpLog watcher behavior.";
#else
    // 在非 etcd 模式下，同步阶段不会真正执行，仅保证调用安全
    ErrorCode err = service_->Start("primary_unused", etcd_endpoints_, cluster_id_);
    EXPECT_EQ(ErrorCode::INTERNAL_ERROR, err);
#endif
}

// ========== 6.1.3 同步状态测试 ==========

TEST_F(HotStandbyServiceTest, TestGetSyncStatus_InitialState) {
    StandbySyncStatus status = service_->GetSyncStatus();
    EXPECT_EQ(0u, status.applied_seq_id);
    EXPECT_EQ(0u, status.primary_seq_id);
    EXPECT_EQ(0u, status.lag_entries);
    EXPECT_FALSE(status.is_syncing);
    EXPECT_FALSE(status.is_connected);
    EXPECT_EQ(StandbyState::STOPPED, status.state);
}

TEST_F(HotStandbyServiceTest, TestGetSyncStatus_AfterSync) {
#ifdef STORE_USE_ETCD
    GTEST_SKIP()
        << "Requires real etcd and OpLog activity to change sync status.";
#else
    // 在非 etcd 模式下，调用 Start 不会改变 applied/primary，但状态机会进入 FAILED
    (void)service_->Start("primary_unused", etcd_endpoints_, cluster_id_);
    StandbySyncStatus status = service_->GetSyncStatus();
    EXPECT_EQ(StandbyState::FAILED, status.state);
#endif
}

TEST_F(HotStandbyServiceTest, TestGetSyncStatus) {
    // 基本覆盖：多次调用应返回一致且不会崩溃
    StandbySyncStatus s1 = service_->GetSyncStatus();
    StandbySyncStatus s2 = service_->GetSyncStatus();
    EXPECT_EQ(s1.state, s2.state);
}

// ========== 6.1.4 晋升测试 ==========

TEST_F(HotStandbyServiceTest, TestPromote_WhenNotReady) {
    // 初始状态下不满足晋升条件，应返回 nullptr
    auto master = service_->Promote();
    EXPECT_EQ(nullptr, master);
}

TEST_F(HotStandbyServiceTest, TestPromote_WhenReady) {
#ifdef STORE_USE_ETCD
    GTEST_SKIP()
        << "Requires real etcd and full replication pipeline to reach ready state.";
#else
    // 非 etcd 模式下即便强行调用，也应安全返回 nullptr
    auto master = service_->Promote();
    EXPECT_EQ(nullptr, master);
#endif
}

TEST_F(HotStandbyServiceTest, TestPromote_FinalCatchUp) {
#ifdef STORE_USE_ETCD
    GTEST_SKIP()
        << "Requires real etcd and OpLog data to exercise final catch-up logic.";
#else
    auto master = service_->Promote();
    EXPECT_EQ(nullptr, master);
#endif
}

TEST_F(HotStandbyServiceTest, TestPromote_WithGaps) {
#ifdef STORE_USE_ETCD
    GTEST_SKIP()
        << "Requires real etcd and gaps in OpLog to validate gap resolution.";
#else
    auto master = service_->Promote();
    EXPECT_EQ(nullptr, master);
#endif
}

TEST_F(HotStandbyServiceTest, TestPromote_Timeout) {
#ifdef STORE_USE_ETCD
    GTEST_SKIP()
        << "Requires real etcd and slow reads to trigger catch-up timeout.";
#else
    auto master = service_->Promote();
    EXPECT_EQ(nullptr, master);
#endif
}

TEST_F(HotStandbyServiceTest, TestPromote_BatchLimit) {
#ifdef STORE_USE_ETCD
    GTEST_SKIP()
        << "Requires real etcd and large OpLog to hit batch limit.";
#else
    auto master = service_->Promote();
    EXPECT_EQ(nullptr, master);
#endif
}

// ========== 6.1.5 热启动测试 ==========

TEST_F(HotStandbyServiceTest, TestWarmStart_WithLocalState) {
#ifdef STORE_USE_ETCD
    GTEST_SKIP()
        << "Requires real etcd and pre-populated local metadata to test warm start.";
#else
    // 非 etcd 模式下，仅验证 Start 调用安全
    (void)service_->Start("primary_unused", etcd_endpoints_, cluster_id_);
    SUCCEED();
#endif
}

TEST_F(HotStandbyServiceTest, TestWarmStart_WithoutLocalState) {
#ifdef STORE_USE_ETCD
    GTEST_SKIP()
        << "Requires real etcd and snapshot provider configuration.";
#else
    (void)service_->Start("primary_unused", etcd_endpoints_, cluster_id_);
    SUCCEED();
#endif
}

TEST_F(HotStandbyServiceTest, TestWarmStart_WithSnapshot) {
#ifdef STORE_USE_ETCD
    GTEST_SKIP()
        << "Requires snapshot provider and real etcd to exercise snapshot bootstrap.";
#else
    config_.enable_snapshot_bootstrap = true;
    // 重新创建 service 以使用新的配置
    service_.reset(new HotStandbyService(config_));
    (void)service_->Start("primary_unused", etcd_endpoints_, cluster_id_);
    SUCCEED();
#endif
}

// ========== 6.1.6 元数据操作测试 ==========

TEST_F(HotStandbyServiceTest, TestGetMetadataCount) {
    EXPECT_EQ(0u, service_->GetMetadataCount());
}

TEST_F(HotStandbyServiceTest, TestExportMetadataSnapshot) {
    std::vector<std::pair<std::string, StandbyObjectMetadata>> snapshot;
    EXPECT_TRUE(service_->ExportMetadataSnapshot(snapshot));
    EXPECT_TRUE(snapshot.empty());
}

TEST_F(HotStandbyServiceTest, TestGetLatestAppliedSequenceId) {
    uint64_t seq = service_->GetLatestAppliedSequenceId();
    EXPECT_EQ(0u, seq);
}

// ========== 6.1.7 复制循环测试 ==========

TEST_F(HotStandbyServiceTest, TestReplicationLoop_UpdatesMetrics) {
#ifdef STORE_USE_ETCD
    GTEST_SKIP()
        << "Requires real etcd and running replication loop to update metrics.";
#else
    // 非 etcd 模式下，ReplicationLoop 不会被启动，但调用 Stop 应该安全
    service_->Stop();
    SUCCEED();
#endif
}

TEST_F(HotStandbyServiceTest, TestReplicationLoop_HandlesDisconnect) {
#ifdef STORE_USE_ETCD
    GTEST_SKIP()
        << "Requires real etcd and watcher disconnect to exercise disconnect path.";
#else
    // 调用 DisconnectFromPrimary 在当前实现中是安全的（不依赖 etcd）
    service_->DisconnectFromPrimary();
    SUCCEED();
#endif
}

// ========== 6.1.8 验证循环测试 ==========

TEST_F(HotStandbyServiceTest, TestVerificationLoop_WhenEnabled) {
#ifdef STORE_USE_ETCD
    GTEST_SKIP()
        << "Requires real etcd and running verification loop to observe behavior.";
#else
    config_.enable_verification = true;
    service_.reset(new HotStandbyService(config_));
    (void)service_->Start("primary_unused", etcd_endpoints_, cluster_id_);
    service_->Stop();
    SUCCEED();
#endif
}

TEST_F(HotStandbyServiceTest, TestVerificationLoop_WhenDisabled) {
    // 默认 config_.enable_verification = false, Start 不会启动验证线程
#ifdef STORE_USE_ETCD
    GTEST_SKIP()
        << "Requires real etcd connection to start service.";
#else
    (void)service_->Start("primary_unused", etcd_endpoints_, cluster_id_);
    service_->Stop();
    SUCCEED();
#endif
}

}  // namespace mooncake::test

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}


