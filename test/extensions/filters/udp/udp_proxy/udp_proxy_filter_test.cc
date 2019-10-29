#include "envoy/config/filter/udp/udp_proxy/v2alpha/udp_proxy.pb.validate.h"

#include "extensions/filters/udp/udp_proxy/udp_proxy_filter.h"

#include "test/mocks/network/io_handle.h"
#include "test/mocks/upstream/mocks.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"

using testing::AtLeast;
using testing::ByMove;
using testing::InSequence;
using testing::Return;
using testing::SaveArg;

namespace Envoy {
namespace Extensions {
namespace UdpFilters {
namespace UdpProxy {
namespace {

class TestUdpProxyFilter : public UdpProxyFilter {
public:
  using UdpProxyFilter::UdpProxyFilter;

  MOCK_METHOD1(createIoHandle, Network::IoHandlePtr(const Upstream::HostConstSharedPtr& host));
};

class UdpProxyFilterTest : public testing::Test {
public:
  struct TestSession {
    TestSession() : io_handle_(new Network::MockIoHandle()) {}

    void expectUpstreamWrite(const std::string&, Api::IoCallUint64Result&& rc) {
      // fixfix better checking, return the right amount of data.
      EXPECT_CALL(*io_handle_, sendmsg(_, _, 0, nullptr, _))
          .WillOnce(Return(ByMove(std::move(rc))));
    }

    Event::MockTimer* idle_timer_{};
    Network::MockIoHandle* io_handle_;
    Event::FileReadyCb file_event_cb_;
  };

  UdpProxyFilterTest() {
    // Disable strict mock warnings.
    EXPECT_CALL(callbacks_, udpListener()).Times(AtLeast(0));
    EXPECT_CALL(*cluster_manager_.thread_local_cluster_.lb_.host_, address())
        .WillRepeatedly(Return(Network::Utility::parseInternetAddressAndPort("20.0.0.1:443")));
  }

  ~UdpProxyFilterTest() { EXPECT_CALL(callbacks_.udp_listener_, onDestroy()); }

  void setup(const std::string& yaml) {
    envoy::config::filter::udp::udp_proxy::v2alpha::UdpProxyConfig config;
    TestUtility::loadFromYamlAndValidate(yaml, config);
    config_ = std::make_shared<UdpProxyFilterConfig>(cluster_manager_, time_system_, config);
    filter_ = std::make_unique<TestUdpProxyFilter>(callbacks_, config_);
  }

  void recvData(const std::string& peer_address, const std::string& local_address,
                const std::string& buffer) {
    Network::UdpRecvData data;
    data.addresses_.peer_ = Network::Utility::parseInternetAddressAndPort(peer_address);
    data.addresses_.local_ = Network::Utility::parseInternetAddressAndPort(local_address);
    data.buffer_ = std::make_unique<Buffer::OwnedImpl>(buffer);
    data.receive_time_ = MonotonicTime(std::chrono::seconds(0));
    filter_->onData(data);
  }

  void expectSessionCreate() {
    test_sessions_.emplace_back();
    TestSession& new_session = test_sessions_.back();
    EXPECT_CALL(cluster_manager_, get(_));
    new_session.idle_timer_ = new Event::MockTimer(&callbacks_.udp_listener_.dispatcher_);
    EXPECT_CALL(*filter_, createIoHandle(_))
        .WillOnce(Return(ByMove(Network::IoHandlePtr{test_sessions_.back().io_handle_})));
    EXPECT_CALL(*new_session.io_handle_, fd()).WillOnce(Return(0));
    EXPECT_CALL(callbacks_.udp_listener_.dispatcher_,
                createFileEvent_(_, _, Event::FileTriggerType::Edge, Event::FileReadyType::Read))
        .WillOnce(DoAll(SaveArg<1>(&new_session.file_event_cb_), Return(nullptr)));
  }

  Upstream::MockClusterManager cluster_manager_;
  MockTimeSystem time_system_;
  UdpProxyFilterConfigSharedPtr config_;
  Network::MockUdpReadFilterCallbacks callbacks_;
  std::unique_ptr<TestUdpProxyFilter> filter_;
  std::vector<TestSession> test_sessions_;
};

// fixfix
TEST_F(UdpProxyFilterTest, BasicFlow) {
  InSequence s;

  setup(R"EOF(
cluster: fake_cluster
  )EOF");

  expectSessionCreate();
  test_sessions_[0].expectUpstreamWrite("hello", Api::ioCallUint64ResultNoError());
  recvData("10.0.0.1:1000", "10.0.0.2:80", "hello");
}

} // namespace
} // namespace UdpProxy
} // namespace UdpFilters
} // namespace Extensions
} // namespace Envoy
