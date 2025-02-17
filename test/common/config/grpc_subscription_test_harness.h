#pragma once

#include <memory>

#include "envoy/api/v2/eds.pb.h"

#include "common/common/hash.h"
#include "common/config/grpc_subscription_impl.h"
#include "common/config/resources.h"

#include "test/common/config/subscription_test_harness.h"
#include "test/mocks/config/mocks.h"
#include "test/mocks/event/mocks.h"
#include "test/mocks/grpc/mocks.h"
#include "test/mocks/local_info/mocks.h"
#include "test/mocks/upstream/mocks.h"
#include "test/test_common/utility.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"

using testing::_;
using testing::Invoke;
using testing::Mock;
using testing::NiceMock;
using testing::Return;

namespace Envoy {
namespace Config {

class GrpcSubscriptionTestHarness : public SubscriptionTestHarness {
public:
  GrpcSubscriptionTestHarness() : GrpcSubscriptionTestHarness(std::chrono::milliseconds(0)) {}

  GrpcSubscriptionTestHarness(std::chrono::milliseconds init_fetch_timeout)
      : method_descriptor_(Protobuf::DescriptorPool::generated_pool()->FindMethodByName(
            "envoy.api.v2.EndpointDiscoveryService.StreamEndpoints")),
        async_client_(new NiceMock<Grpc::MockAsyncClient>()), timer_(new Event::MockTimer()) {
    node_.set_id("node_name");
    node_.set_cluster("cluster_name");
    node_.mutable_locality()->set_zone("zone_name");
    EXPECT_CALL(local_info_, node()).WillRepeatedly(testing::ReturnRef(node_));
    EXPECT_CALL(dispatcher_, createTimer_(_)).WillOnce(Invoke([this](Event::TimerCb timer_cb) {
      timer_cb_ = timer_cb;
      return timer_;
    }));

    subscription_ = std::make_unique<GrpcSubscriptionImpl>(
        std::make_shared<GrpcMuxSotw>(std::unique_ptr<Grpc::MockAsyncClient>(async_client_),
                                      dispatcher_, *method_descriptor_, random_, stats_store_,
                                      rate_limit_settings_, local_info_,
                                      /*skip_subsequent_node=*/true),
        Config::TypeUrl::get().ClusterLoadAssignment, callbacks_, stats_, init_fetch_timeout,
        /*is_aggregated=*/false);
  }

  ~GrpcSubscriptionTestHarness() override { EXPECT_CALL(async_stream_, sendMessageRaw_(_, false)); }

  void expectSendMessage(const std::set<std::string>& cluster_names, const std::string& version,
                         bool expect_node = false) override {
    expectSendMessage(cluster_names, version, expect_node, Grpc::Status::GrpcStatus::Ok, "");
  }

  void expectSendMessage(const std::set<std::string>& cluster_names, const std::string& version,
                         bool expect_node, const Protobuf::int32 error_code,
                         const std::string& error_message) {
    UNREFERENCED_PARAMETER(expect_node);
    envoy::api::v2::DiscoveryRequest expected_request;
    if (expect_node) {
      expected_request.mutable_node()->CopyFrom(node_);
    }
    for (const auto& cluster : cluster_names) {
      expected_request.add_resource_names(cluster);
    }
    if (!version.empty()) {
      expected_request.set_version_info(version);
    }
    expected_request.set_response_nonce(last_response_nonce_);
    expected_request.set_type_url(Config::TypeUrl::get().ClusterLoadAssignment);
    if (error_code != Grpc::Status::GrpcStatus::Ok) {
      ::google::rpc::Status* error_detail = expected_request.mutable_error_detail();
      error_detail->set_code(error_code);
      error_detail->set_message(error_message);
    }
    EXPECT_CALL(
        async_stream_,
        sendMessageRaw_(Grpc::ProtoBufferEqIgnoreRepeatedFieldOrdering(expected_request), false));
  }

  void startSubscription(const std::set<std::string>& cluster_names) override {
    EXPECT_CALL(*async_client_, startRaw(_, _, _, _)).WillOnce(Return(&async_stream_));
    last_cluster_names_ = cluster_names;
    expectSendMessage(last_cluster_names_, "", true);
    subscription_->start(cluster_names);
  }

  void deliverConfigUpdate(const std::vector<std::string>& cluster_names,
                           const std::string& version, bool accept) override {
    auto response = std::make_unique<envoy::api::v2::DiscoveryResponse>();
    response->set_version_info(version);
    last_response_nonce_ = std::to_string(HashUtil::xxHash64(version));
    response->set_nonce(last_response_nonce_);
    response->set_type_url(Config::TypeUrl::get().ClusterLoadAssignment);
    Protobuf::RepeatedPtrField<envoy::api::v2::ClusterLoadAssignment> typed_resources;
    for (const auto& cluster : cluster_names) {
      if (std::find(last_cluster_names_.begin(), last_cluster_names_.end(), cluster) !=
          last_cluster_names_.end()) {
        envoy::api::v2::ClusterLoadAssignment* load_assignment = typed_resources.Add();
        load_assignment->set_cluster_name(cluster);
        response->add_resources()->PackFrom(*load_assignment);
      }
    }
    EXPECT_CALL(callbacks_, onConfigUpdate(RepeatedProtoEq(response->resources()), version))
        .WillOnce(ThrowOnRejectedConfig(accept));
    if (accept) {
      expectSendMessage(last_cluster_names_, version, false);
      version_ = version;
    } else {
      EXPECT_CALL(callbacks_, onConfigUpdateFailed(
                                  Envoy::Config::ConfigUpdateFailureReason::UpdateRejected, _));
      expectSendMessage(last_cluster_names_, version_, false, Grpc::Status::GrpcStatus::Internal,
                        "bad config");
    }
    auto shared_mux = subscription_->getGrpcMuxForTest();
    static_cast<GrpcMuxSotw*>(shared_mux.get())->onDiscoveryResponse(std::move(response));
    Mock::VerifyAndClearExpectations(&async_stream_);
  }

  void updateResourceInterest(const std::set<std::string>& cluster_names) override {
    expectSendMessage(cluster_names, version_);
    subscription_->updateResourceInterest(cluster_names);
    last_cluster_names_ = cluster_names;
  }

  void expectConfigUpdateFailed() override {
    EXPECT_CALL(callbacks_, onConfigUpdateFailed(_, nullptr));
  }

  void expectEnableInitFetchTimeoutTimer(std::chrono::milliseconds timeout) override {
    init_timeout_timer_ = new Event::MockTimer(&dispatcher_);
    EXPECT_CALL(*init_timeout_timer_, enableTimer(timeout, _));
  }

  void expectDisableInitFetchTimeoutTimer() override {
    EXPECT_CALL(*init_timeout_timer_, disableTimer());
  }

  void callInitFetchTimeoutCb() override { init_timeout_timer_->invokeCallback(); }

  std::string version_;
  const Protobuf::MethodDescriptor* method_descriptor_;
  Grpc::MockAsyncClient* async_client_;
  NiceMock<Upstream::MockClusterManager> cm_;
  Event::MockDispatcher dispatcher_;
  Runtime::MockRandomGenerator random_;
  Event::MockTimer* timer_;
  Event::TimerCb timer_cb_;
  envoy::api::v2::core::Node node_;
  NiceMock<Config::MockSubscriptionCallbacks<envoy::api::v2::ClusterLoadAssignment>> callbacks_;
  NiceMock<Grpc::MockAsyncStream> async_stream_;
  std::unique_ptr<GrpcSubscriptionImpl> subscription_;
  std::string last_response_nonce_;
  std::set<std::string> last_cluster_names_;
  NiceMock<LocalInfo::MockLocalInfo> local_info_;
  Envoy::Config::RateLimitSettings rate_limit_settings_;
  Event::MockTimer* init_timeout_timer_;
};

// TODO(danielhochman): test with RDS and ensure version_info is same as what API returned

} // namespace Config
} // namespace Envoy
