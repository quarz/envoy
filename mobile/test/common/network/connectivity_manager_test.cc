#include <net/if.h>

#include "test/extensions/common/dynamic_forward_proxy/mocks.h"
#include "test/mocks/upstream/cluster_manager.h"

#include "gtest/gtest.h"
#include "library/common/network/connectivity_manager.h"

using testing::_;
using testing::Ref;
using testing::Return;

namespace Envoy {
namespace Network {

class ConnectivityManagerTest : public testing::Test {
public:
  ConnectivityManagerTest()
      : dns_cache_manager_(
            new NiceMock<Extensions::Common::DynamicForwardProxy::MockDnsCacheManager>()),
        dns_cache_(dns_cache_manager_->dns_cache_),
        connectivity_manager_(std::make_shared<ConnectivityManagerImpl>(cm_, dns_cache_manager_)) {
    ON_CALL(*dns_cache_manager_, lookUpCacheByName(_)).WillByDefault(Return(dns_cache_));
    // Toggle network to reset network state.
    ConnectivityManagerImpl::setPreferredNetwork(1);
    ConnectivityManagerImpl::setPreferredNetwork(2);
  }

  std::shared_ptr<NiceMock<Extensions::Common::DynamicForwardProxy::MockDnsCacheManager>>
      dns_cache_manager_;
  std::shared_ptr<Extensions::Common::DynamicForwardProxy::MockDnsCache> dns_cache_;
  NiceMock<Upstream::MockClusterManager> cm_{};
  std::shared_ptr<ConnectivityManagerImpl> connectivity_manager_;
};

TEST_F(ConnectivityManagerTest, SetPreferredNetworkWithNewNetworkChangesConfigurationKey) {
  envoy_netconf_t original_key = connectivity_manager_->getConfigurationKey();
  envoy_netconf_t new_key = ConnectivityManagerImpl::setPreferredNetwork(4);
  EXPECT_NE(original_key, new_key);
  EXPECT_EQ(new_key, connectivity_manager_->getConfigurationKey());
}

TEST_F(ConnectivityManagerTest,
       DISABLED_SetPreferredNetworkWithUnchangedNetworkReturnsStaleConfigurationKey) {
  envoy_netconf_t original_key = connectivity_manager_->getConfigurationKey();
  envoy_netconf_t stale_key = ConnectivityManagerImpl::setPreferredNetwork(2);
  EXPECT_NE(original_key, stale_key);
  EXPECT_EQ(original_key, connectivity_manager_->getConfigurationKey());
}

TEST_F(ConnectivityManagerTest, RefreshDnsForCurrentConfigurationTriggersDnsRefresh) {
  EXPECT_CALL(*dns_cache_, forceRefreshHosts());
  envoy_netconf_t configuration_key = connectivity_manager_->getConfigurationKey();
  connectivity_manager_->refreshDns(configuration_key, false);
}

TEST_F(ConnectivityManagerTest, RefreshDnsForStaleConfigurationDoesntTriggerDnsRefresh) {
  EXPECT_CALL(*dns_cache_, forceRefreshHosts()).Times(0);
  envoy_netconf_t configuration_key = connectivity_manager_->getConfigurationKey();
  connectivity_manager_->refreshDns(configuration_key - 1, false);
}

TEST_F(ConnectivityManagerTest, WhenDrainPostDnsRefreshEnabledDrainsPostDnsRefresh) {
  Extensions::Common::DynamicForwardProxy::DnsCache::UpdateCallbacks* dns_completion_callback{
      nullptr};
  EXPECT_CALL(*dns_cache_, addUpdateCallbacks_(_))
      .WillOnce(Invoke([&dns_completion_callback](
                           Extensions::Common::DynamicForwardProxy::DnsCache::UpdateCallbacks& cb) {
        dns_completion_callback = &cb;
        return nullptr;
      }));
  connectivity_manager_->setDrainPostDnsRefreshEnabled(true);

  auto host_info = std::make_shared<Extensions::Common::DynamicForwardProxy::MockDnsHostInfo>();
  EXPECT_CALL(*dns_cache_, iterateHostMap(_))
      .WillOnce(
          Invoke([&](Extensions::Common::DynamicForwardProxy::DnsCache::IterateHostMapCb callback) {
            callback("cached.example.com", host_info);
            callback("cached2.example.com", host_info);
            callback("cached3.example.com", host_info);
          }));

  EXPECT_CALL(*dns_cache_, forceRefreshHosts());
  envoy_netconf_t configuration_key = connectivity_manager_->getConfigurationKey();
  connectivity_manager_->refreshDns(configuration_key, true);

  EXPECT_CALL(cm_, drainConnections(_));
  dns_completion_callback->onDnsResolutionComplete(
      "cached.example.com",
      std::make_shared<Extensions::Common::DynamicForwardProxy::MockDnsHostInfo>(),
      Network::DnsResolver::ResolutionStatus::Completed);
  dns_completion_callback->onDnsResolutionComplete(
      "not-cached.example.com",
      std::make_shared<Extensions::Common::DynamicForwardProxy::MockDnsHostInfo>(),
      Network::DnsResolver::ResolutionStatus::Completed);
  dns_completion_callback->onDnsResolutionComplete(
      "not-cached2.example.com",
      std::make_shared<Extensions::Common::DynamicForwardProxy::MockDnsHostInfo>(),
      Network::DnsResolver::ResolutionStatus::Completed);
}

TEST_F(ConnectivityManagerTest, WhenDrainPostDnsNotEnabledDoesntDrainPostDnsRefresh) {
  EXPECT_CALL(*dns_cache_, addUpdateCallbacks_(_)).Times(0);
  connectivity_manager_->setDrainPostDnsRefreshEnabled(false);

  EXPECT_CALL(*dns_cache_, iterateHostMap(_)).Times(0);
  EXPECT_CALL(*dns_cache_, forceRefreshHosts());
  envoy_netconf_t configuration_key = connectivity_manager_->getConfigurationKey();
  connectivity_manager_->refreshDns(configuration_key, true);
}

TEST_F(ConnectivityManagerTest,
       ReportNetworkUsageDoesntAlterNetworkConfigurationWhenBoundInterfacesAreDisabled) {
  envoy_netconf_t configuration_key = connectivity_manager_->getConfigurationKey();
  connectivity_manager_->setInterfaceBindingEnabled(false);
  EXPECT_EQ(SocketMode::DefaultPreferredNetworkMode, connectivity_manager_->getSocketMode());

  connectivity_manager_->reportNetworkUsage(configuration_key, true /* network_fault */);
  connectivity_manager_->reportNetworkUsage(configuration_key, true /* network_fault */);
  connectivity_manager_->reportNetworkUsage(configuration_key, true /* network_fault */);

  EXPECT_EQ(configuration_key, connectivity_manager_->getConfigurationKey());
  EXPECT_EQ(SocketMode::DefaultPreferredNetworkMode, connectivity_manager_->getSocketMode());
}

TEST_F(ConnectivityManagerTest,
       ReportNetworkUsageTriggersOverrideAfterFirstFaultAfterNetworkUpdate) {
  envoy_netconf_t configuration_key = connectivity_manager_->getConfigurationKey();
  connectivity_manager_->setInterfaceBindingEnabled(true);
  EXPECT_EQ(SocketMode::DefaultPreferredNetworkMode, connectivity_manager_->getSocketMode());

  connectivity_manager_->reportNetworkUsage(configuration_key, true /* network_fault */);

  EXPECT_NE(configuration_key, connectivity_manager_->getConfigurationKey());
  EXPECT_EQ(SocketMode::AlternateBoundInterfaceMode, connectivity_manager_->getSocketMode());
}

TEST_F(ConnectivityManagerTest, ReportNetworkUsageDisablesOverrideAfterFirstFaultAfterOverride) {
  envoy_netconf_t configuration_key = connectivity_manager_->getConfigurationKey();
  connectivity_manager_->setInterfaceBindingEnabled(true);
  EXPECT_EQ(SocketMode::DefaultPreferredNetworkMode, connectivity_manager_->getSocketMode());

  connectivity_manager_->reportNetworkUsage(configuration_key, true /* network_fault */);

  EXPECT_NE(configuration_key, connectivity_manager_->getConfigurationKey());
  configuration_key = connectivity_manager_->getConfigurationKey();
  EXPECT_EQ(SocketMode::AlternateBoundInterfaceMode, connectivity_manager_->getSocketMode());

  connectivity_manager_->reportNetworkUsage(configuration_key, true /* network_fault */);

  EXPECT_NE(configuration_key, connectivity_manager_->getConfigurationKey());
  EXPECT_EQ(SocketMode::DefaultPreferredNetworkMode, connectivity_manager_->getSocketMode());
}

TEST_F(ConnectivityManagerTest, ReportNetworkUsageDisablesOverrideAfterThirdFaultAfterSuccess) {
  envoy_netconf_t configuration_key = connectivity_manager_->getConfigurationKey();
  connectivity_manager_->setInterfaceBindingEnabled(true);
  EXPECT_EQ(SocketMode::DefaultPreferredNetworkMode, connectivity_manager_->getSocketMode());

  connectivity_manager_->reportNetworkUsage(configuration_key, false /* network_fault */);
  connectivity_manager_->reportNetworkUsage(configuration_key, true /* network_fault */);

  EXPECT_EQ(configuration_key, connectivity_manager_->getConfigurationKey());
  EXPECT_EQ(SocketMode::DefaultPreferredNetworkMode, connectivity_manager_->getSocketMode());

  connectivity_manager_->reportNetworkUsage(configuration_key, true /* network_fault */);
  connectivity_manager_->reportNetworkUsage(configuration_key, true /* network_fault */);

  EXPECT_NE(configuration_key, connectivity_manager_->getConfigurationKey());
  EXPECT_EQ(SocketMode::AlternateBoundInterfaceMode, connectivity_manager_->getSocketMode());
}

TEST_F(ConnectivityManagerTest, ReportNetworkUsageDisregardsCallsWithStaleConfigurationKey) {
  envoy_netconf_t stale_key = connectivity_manager_->getConfigurationKey();
  envoy_netconf_t current_key = ConnectivityManagerImpl::setPreferredNetwork(4);
  EXPECT_NE(stale_key, current_key);

  connectivity_manager_->setInterfaceBindingEnabled(true);
  EXPECT_EQ(SocketMode::DefaultPreferredNetworkMode, connectivity_manager_->getSocketMode());

  connectivity_manager_->reportNetworkUsage(stale_key, true /* network_fault */);
  connectivity_manager_->reportNetworkUsage(stale_key, true /* network_fault */);
  connectivity_manager_->reportNetworkUsage(stale_key, true /* network_fault */);

  EXPECT_EQ(current_key, connectivity_manager_->getConfigurationKey());
  EXPECT_EQ(SocketMode::DefaultPreferredNetworkMode, connectivity_manager_->getSocketMode());

  connectivity_manager_->reportNetworkUsage(stale_key, false /* network_fault */);
  connectivity_manager_->reportNetworkUsage(current_key, true /* network_fault */);

  EXPECT_NE(current_key, connectivity_manager_->getConfigurationKey());
  EXPECT_EQ(SocketMode::AlternateBoundInterfaceMode, connectivity_manager_->getSocketMode());
}

TEST_F(ConnectivityManagerTest, EnumerateInterfacesFiltersByFlags) {
  // Select loopback.
  auto loopbacks = connectivity_manager_->enumerateInterfaces(AF_INET, IFF_LOOPBACK, 0);
  EXPECT_EQ(loopbacks.size(), 1);
  EXPECT_EQ(std::get<const std::string>(loopbacks[0]).rfind("lo", 0), 0);

  // Reject loopback.
  auto nonloopbacks = connectivity_manager_->enumerateInterfaces(AF_INET, 0, IFF_LOOPBACK);
  for (const auto& interface : nonloopbacks) {
    EXPECT_NE(std::get<const std::string>(interface).rfind("lo", 0), 0);
  }

  // Select AND reject loopback.
  auto empty = connectivity_manager_->enumerateInterfaces(AF_INET, IFF_LOOPBACK, IFF_LOOPBACK);
  EXPECT_EQ(empty.size(), 0);
}

TEST_F(ConnectivityManagerTest, OverridesNoProxySettingsWithNewProxySettings) {
  EXPECT_EQ(nullptr, connectivity_manager_->getProxySettings());

  const auto proxy_settings = ProxySettings::parseHostAndPort("127.0.0.1", 9999);
  connectivity_manager_->setProxySettings(proxy_settings);
  EXPECT_EQ("127.0.0.1:9999", connectivity_manager_->getProxySettings()->asString());
}

TEST_F(ConnectivityManagerTest, OverridesCurrentProxySettingsWithNoProxySettings) {
  const auto proxy_settings = ProxySettings::parseHostAndPort("127.0.0.1", 9999);
  connectivity_manager_->setProxySettings(proxy_settings);
  EXPECT_EQ("127.0.0.1:9999", connectivity_manager_->getProxySettings()->asString());

  connectivity_manager_->setProxySettings(nullptr);
  EXPECT_EQ(nullptr, connectivity_manager_->getProxySettings());
}

TEST_F(ConnectivityManagerTest, OverridesCurrentProxySettingsWithNewProxySettings) {
  const auto proxy_settings1 = ProxySettings::parseHostAndPort("127.0.0.1", 9999);
  connectivity_manager_->setProxySettings(proxy_settings1);
  EXPECT_EQ("127.0.0.1:9999", connectivity_manager_->getProxySettings()->asString());

  const auto proxy_settings2 = ProxySettings::parseHostAndPort("127.0.0.1", 8888);
  connectivity_manager_->setProxySettings(proxy_settings2);
  EXPECT_EQ(proxy_settings2, connectivity_manager_->getProxySettings());
}

TEST_F(ConnectivityManagerTest, IgnoresDuplicatedProxySettingsUpdates) {
  const auto proxy_settings1 = ProxySettings::parseHostAndPort("127.0.0.1", 9999);
  connectivity_manager_->setProxySettings(proxy_settings1);
  EXPECT_EQ("127.0.0.1:9999", connectivity_manager_->getProxySettings()->asString());

  const auto proxy_settings2 = ProxySettings::parseHostAndPort("127.0.0.1", 9999);
  connectivity_manager_->setProxySettings(proxy_settings2);
  EXPECT_EQ(proxy_settings1, connectivity_manager_->getProxySettings());
}

TEST_F(ConnectivityManagerTest, NetworkChangeResultsInDifferentSocketOptionsHash) {
  auto options1 = std::make_shared<Socket::Options>();
  connectivity_manager_->addUpstreamSocketOptions(options1);
  std::vector<uint8_t> hash1;
  for (const auto& option : *options1) {
    option->hashKey(hash1);
  }
  ConnectivityManagerImpl::setPreferredNetwork(64);
  auto options2 = std::make_shared<Socket::Options>();
  connectivity_manager_->addUpstreamSocketOptions(options2);
  std::vector<uint8_t> hash2;
  for (const auto& option : *options2) {
    option->hashKey(hash2);
  }
  EXPECT_NE(hash1, hash2);
}

} // namespace Network
} // namespace Envoy
