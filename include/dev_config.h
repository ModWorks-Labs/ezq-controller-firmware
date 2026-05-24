#pragma once

namespace dev_config {

inline constexpr char kWifiSsid[] = "ModNet";
inline constexpr char kWifiPassword[] = "TacoTuesday456";
inline constexpr char kDeviceHostname[] = "ezq-ctlr-b-dev";
inline constexpr char kMdnsInstanceName[] = "EZQ Controller Dev";
inline constexpr int kOtaHttpPort = 8032;
inline constexpr int kDebugTcpPort = 2323;
inline constexpr int kWifiConnectMaxRetries = 8;
inline constexpr int kStatusDisconnectedLogMs = 1000;
inline constexpr int kStatusConnectedLogMs = 5000;
inline constexpr int kDevHoldPollMs = 100;
inline constexpr int kDevHoldStatusMs = 1000;
inline constexpr int kDevHoldSettleMs = 3000;
inline constexpr int kFaultMonitorArmDelayMs = 5000;
inline constexpr int kWakeButtonSettleMs = 250;

}  // namespace dev_config
