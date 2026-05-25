#pragma once

namespace dev_config {

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
inline constexpr int kUpdateCheckWifiWaitMs = 10000;
inline constexpr int kUpdateConfirmStableMs = 10000;
inline constexpr int kUpdateTimeSyncWaitMs = 10000;
inline constexpr char kUpdateManifestUrl[] =
    "https://raw.githubusercontent.com/ModWorks-Labs/ezq-controller-firmware/main/ezq-update-manifest.json";

}  // namespace dev_config
