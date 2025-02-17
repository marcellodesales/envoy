#include "test/common/config/grpc_subscription_test_harness.h"

#include "gtest/gtest.h"

using testing::InSequence;

namespace Envoy {
namespace Config {
namespace {

class GrpcSubscriptionImplTest : public testing::Test, public GrpcSubscriptionTestHarness {};

// Validate that stream creation results in a timer based retry and can recover.
TEST_F(GrpcSubscriptionImplTest, StreamCreationFailure) {
  InSequence s;
  EXPECT_CALL(*async_client_, startRaw(_, _, _, _)).WillOnce(Return(nullptr));

  // onConfigUpdateFailed() should not be called for gRPC stream connection failure
  EXPECT_CALL(callbacks_,
              onConfigUpdateFailed(Envoy::Config::ConfigUpdateFailureReason::ConnectionFailure, _))
      .Times(0);
  EXPECT_CALL(random_, random());
  EXPECT_CALL(*timer_, enableTimer(_, _));
  subscription_->start({"cluster0", "cluster1"});
  EXPECT_TRUE(statsAre(2, 0, 0, 0, 0, 0));
  // Ensure this doesn't cause an issue by sending a request, since we don't
  // have a gRPC stream.
  subscription_->updateResourceInterest({"cluster2"});

  // Retry and succeed.
  EXPECT_CALL(*async_client_, startRaw(_, _, _, _)).WillOnce(Return(&async_stream_));

  expectSendMessage({"cluster2"}, "", true);
  timer_cb_();
  EXPECT_TRUE(statsAre(3, 0, 0, 0, 0, 0));
  verifyControlPlaneStats(1);
}

// Validate that the client can recover from a remote stream closure via retry.
TEST_F(GrpcSubscriptionImplTest, RemoteStreamClose) {
  startSubscription({"cluster0", "cluster1"});
  EXPECT_TRUE(statsAre(1, 0, 0, 0, 0, 0));
  // onConfigUpdateFailed() should not be called for gRPC stream connection failure
  EXPECT_CALL(callbacks_,
              onConfigUpdateFailed(Envoy::Config::ConfigUpdateFailureReason::ConnectionFailure, _))
      .Times(0);
  EXPECT_CALL(*timer_, enableTimer(_, _));
  EXPECT_CALL(random_, random());
  auto shared_mux = subscription_->getGrpcMuxForTest();
  static_cast<GrpcMuxSotw*>(shared_mux.get())
      ->grpcStreamForTest()
      .onRemoteClose(Grpc::Status::GrpcStatus::Canceled, "");
  EXPECT_TRUE(statsAre(2, 0, 0, 1, 0, 0));
  verifyControlPlaneStats(0);

  // Retry and succeed.
  EXPECT_CALL(*async_client_, startRaw(_, _, _, _)).WillOnce(Return(&async_stream_));
  expectSendMessage({"cluster0", "cluster1"}, "", true);
  timer_cb_();
  EXPECT_TRUE(statsAre(2, 0, 0, 1, 0, 0));
}

// Validate that when the management server gets multiple requests for the same version, it can
// ignore later ones. This allows the nonce to be used.
TEST_F(GrpcSubscriptionImplTest, RepeatedNonce) {
  InSequence s;
  startSubscription({"cluster0", "cluster1"});
  EXPECT_TRUE(statsAre(1, 0, 0, 0, 0, 0));
  // First with the initial, empty version update to "0".
  updateResourceInterest({"cluster2"});
  EXPECT_TRUE(statsAre(2, 0, 0, 0, 0, 0));
  deliverConfigUpdate({"cluster0", "cluster2"}, "0", false);
  EXPECT_TRUE(statsAre(3, 0, 1, 0, 0, 0));
  deliverConfigUpdate({"cluster0", "cluster2"}, "0", true);
  EXPECT_TRUE(statsAre(4, 1, 1, 0, 0, 7148434200721666028));
  // Now with version "0" update to "1".
  updateResourceInterest({"cluster3"});
  EXPECT_TRUE(statsAre(5, 1, 1, 0, 0, 7148434200721666028));
  deliverConfigUpdate({"cluster3"}, "1", false);
  EXPECT_TRUE(statsAre(6, 1, 2, 0, 0, 7148434200721666028));
  deliverConfigUpdate({"cluster3"}, "1", true);
  EXPECT_TRUE(statsAre(7, 2, 2, 0, 0, 13237225503670494420U));
}

} // namespace
} // namespace Config
} // namespace Envoy
