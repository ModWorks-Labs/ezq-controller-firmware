#include "ota_service.h"

#include <cstring>
#include <cstdio>
#include <string>
#include <vector>

#include "cJSON.h"
#include "config_store.h"
#include "control_runtime.h"
#include "debug_console.h"
#include "dev_config.h"
#include "esp_http_server.h"
#include "esp_app_desc.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_spiffs.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "output_control.h"
#include "unit_identity.h"
#include "update_manager.h"
#include "wifi_manager.h"

namespace ota_service {
namespace {

constexpr char kTag[] = "ota_service";
constexpr char kStatusUri[] = "/api/dev/status";
constexpr char kSettingsUri[] = "/api/dev/settings";
constexpr char kOtaUri[] = "/api/dev/ota";
constexpr char kSettingsFsUri[] = "/api/dev/fs/settings";
constexpr char kWebUiFsUri[] = "/api/dev/fs/web_ui";
constexpr char kDashboardStatusUri[] = "/api/dashboard/status";
constexpr char kDashboardBeepUri[] = "/api/dashboard/test-beep";
constexpr char kDashboardStartCycleUri[] = "/api/dashboard/start-cycle";
constexpr char kDashboardAbortUri[] = "/api/dashboard/abort";
constexpr char kDashboardToggleBlowerUri[] = "/api/dashboard/toggle-blower";

constexpr char kProvisionRootUri[] = "/";
constexpr char kProvisionStatusUri[] = "/api/provision/status";
constexpr char kProvisionScanUri[] = "/api/provision/scan";
constexpr char kProvisionConnectUri[] = "/api/provision/connect";

constexpr int kOtaServerPort = dev_config::kOtaHttpPort;
constexpr int kProvisionServerPort = 80;
constexpr int kProvisionCtrlPort = 32769;
constexpr int64_t kDefaultRebootDelayUs = 500000;
constexpr char kWebUiPartitionLabel[] = "web_ui";
constexpr char kWebUiBasePath[] = "/webui";
constexpr char kWebUiIndexPath[] = "/webui/index.html";

constexpr char kProvisionPage[] = R"HTML(
<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>EZQ Wi-Fi Setup</title>
  <style>
    * { box-sizing: border-box; margin: 0; padding: 0; }
    :root {
      --ezq-yellow: #E8D44D;
      --ezq-bg: #161618;
      --ezq-surface: #1f1f22;
      --ezq-card: #252528;
      --ezq-inner: #2c2c30;
      --ezq-border: rgba(255,255,255,0.07);
      --ezq-border-hover: rgba(255,255,255,0.14);
      --ezq-text: #f0f0f0;
      --ezq-muted: #888890;
      --ezq-dim: #4a4a52;
    }
    body {
      min-height: 100vh;
      background: var(--ezq-bg);
      color: var(--ezq-text);
      font-family: system-ui, sans-serif;
      display: grid;
      place-items: center;
      padding: 24px;
    }
    .shell {
      background: var(--ezq-bg);
      border-radius: 12px;
      overflow: hidden;
      width: 360px;
      margin: 0 auto;
      border: 0.5px solid rgba(255,255,255,0.08);
      color: var(--ezq-text);
      position: relative;
      min-height: 520px;
    }
    .topbar {
      background: var(--ezq-surface);
      display: flex;
      align-items: center;
      justify-content: space-between;
      padding: 0 16px;
      height: 48px;
      border-bottom: 0.5px solid var(--ezq-border);
    }
    .logo {
      font-size: 20px;
      font-weight: 700;
      color: var(--ezq-yellow);
      letter-spacing: 1px;
    }
    .content { padding: 14px; }
    .card {
      background: var(--ezq-card);
      border: 0.5px solid var(--ezq-border);
      border-radius: 10px;
      padding: 16px;
    }
    .card-header {
      display: flex;
      align-items: center;
      justify-content: space-between;
      margin-bottom: 12px;
    }
    .card-header-left {
      display: flex;
      align-items: center;
      gap: 7px;
    }
    .card-header-left i {
      font-size: 14px;
      color: var(--ezq-muted);
      width: 12px;
      height: 12px;
      border: 1px solid var(--ezq-muted);
      border-radius: 50%;
      display: inline-flex;
      align-items: center;
      justify-content: center;
    }
    .card-label {
      font-size: 9px;
      letter-spacing: 1.2px;
      color: var(--ezq-muted);
      text-transform: uppercase;
    }
    .scan-btn {
      display: flex;
      align-items: center;
      gap: 4px;
      background: var(--ezq-inner);
      border: 0.5px solid var(--ezq-border);
      border-radius: 20px;
      padding: 4px 10px;
      font-size: 10px;
      color: var(--ezq-muted);
      cursor: pointer;
      letter-spacing: 0.3px;
    }
    .scan-btn:disabled {
      opacity: 0.6;
      cursor: wait;
    }
    .scan-btn i {
      font-size: 11px;
      display: inline-block;
    }
    .network-scroll {
      max-height: 380px;
      overflow-y: auto;
      display: flex;
      flex-direction: column;
      gap: 6px;
      scrollbar-width: thin;
      scrollbar-color: var(--ezq-dim) transparent;
    }
    .network-scroll::-webkit-scrollbar { width: 3px; }
    .network-scroll::-webkit-scrollbar-thumb {
      background: var(--ezq-dim);
      border-radius: 2px;
    }
    .network-row {
      background: var(--ezq-inner);
      border: 0.5px solid var(--ezq-border);
      border-radius: 8px;
      padding: 11px 12px;
      display: flex;
      align-items: center;
      justify-content: space-between;
      cursor: pointer;
      transition: border-color 0.15s, background 0.15s;
      flex-shrink: 0;
    }
    .network-row:hover {
      border-color: var(--ezq-border-hover);
      background: #313136;
    }
    .network-row.empty {
      cursor: default;
    }
    .network-row.empty:hover {
      border-color: var(--ezq-border);
      background: var(--ezq-inner);
    }
    .network-left {
      display: flex;
      align-items: center;
      gap: 10px;
    }
    .network-name {
      font-size: 13px;
      font-weight: 500;
      color: var(--ezq-text);
    }
    .network-right {
      display: flex;
      flex-direction: column;
      align-items: flex-end;
      gap: 2px;
      flex-shrink: 0;
    }
    .network-security {
      font-size: 9px;
      color: var(--ezq-dim);
      letter-spacing: 0.4px;
    }
    .network-signal {
      font-size: 9px;
      color: var(--ezq-dim);
    }
    .signal-bars {
      display: flex;
      align-items: flex-end;
      gap: 2px;
      height: 13px;
    }
    .bar {
      width: 3px;
      border-radius: 1px;
      background: var(--ezq-dim);
    }
    .bar.lit { background: var(--ezq-muted); }
    .bar:nth-child(1) { height: 4px; }
    .bar:nth-child(2) { height: 7px; }
    .bar:nth-child(3) { height: 10px; }
    .bar:nth-child(4) { height: 13px; }
    .modal-overlay {
      display: none;
      position: absolute;
      inset: 0;
      background: rgba(0,0,0,0.55);
      border-radius: 12px;
      align-items: center;
      justify-content: center;
      z-index: 10;
      padding: 20px;
    }
    .modal-overlay.open { display: flex; }
    .modal {
      background: var(--ezq-surface);
      border: 0.5px solid rgba(255,255,255,0.1);
      border-radius: 12px;
      padding: 20px;
      width: 100%;
    }
    .modal-header {
      display: flex;
      align-items: center;
      gap: 8px;
      margin-bottom: 18px;
    }
    .modal-header i {
      font-size: 17px;
      color: var(--ezq-muted);
      width: 14px;
      height: 14px;
      border: 1px solid var(--ezq-muted);
      border-radius: 50%;
      display: inline-flex;
      align-items: center;
      justify-content: center;
    }
    .modal-network-name {
      font-size: 15px;
      font-weight: 500;
      color: var(--ezq-text);
    }
    .field-label {
      font-size: 9px;
      letter-spacing: 1px;
      color: var(--ezq-muted);
      text-transform: uppercase;
      margin-bottom: 6px;
    }
    .pw-wrap { margin-bottom: 18px; }
    .pw-field {
      position: relative;
    }
    .pw-input {
      width: 100%;
      background: var(--ezq-inner);
      border: 0.5px solid var(--ezq-border);
      border-radius: 7px;
      padding: 10px 42px 10px 12px;
      font-size: 14px;
      color: var(--ezq-text);
      outline: none;
      -webkit-appearance: none;
    }
    .pw-input::placeholder { color: var(--ezq-dim); }
    .pw-input:focus { border-color: rgba(255,255,255,0.18); }
    .pw-toggle {
      position: absolute;
      right: 10px;
      top: 50%;
      transform: translateY(-50%);
      width: 24px;
      height: 24px;
      border: none;
      background: transparent;
      color: var(--ezq-muted);
      cursor: pointer;
      font-size: 15px;
      display: inline-flex;
      align-items: center;
      justify-content: center;
    }
    .modal-actions {
      display: flex;
      align-items: center;
      justify-content: center;
      gap: 8px;
    }
    .cancel-btn {
      flex: 1;
      padding: 10px;
      font-size: 13px;
      background: transparent;
      color: var(--ezq-muted);
      border: 0.5px solid var(--ezq-dim);
      border-radius: 7px;
      cursor: pointer;
    }
    .connect-btn {
      flex: 1;
      padding: 10px;
      font-size: 13px;
      font-weight: 600;
      background: var(--ezq-yellow);
      color: #111;
      border: none;
      border-radius: 7px;
      cursor: pointer;
    }
    .connect-btn:disabled {
      opacity: 0.7;
      cursor: wait;
    }
    .status-text {
      margin-top: 10px;
      min-height: 14px;
      font-size: 10px;
      letter-spacing: 0.5px;
      color: var(--ezq-muted);
      text-transform: uppercase;
    }
    .status-text.good { color: var(--ezq-yellow); }
    .status-text.bad { color: #ff8e8e; }
    .hidden { display: none; }
  </style>
</head>
<body>
<div class="shell">
  <div class="topbar">
    <div class="logo">EZQ</div>
  </div>

  <div class="content">
    <div class="card">
      <div class="card-header">
        <div class="card-header-left">
          <i aria-hidden="true"></i>
          <div class="card-label" id="cardLabel">Available Networks</div>
        </div>
        <button class="scan-btn" id="scanBtn" type="button">
          <i id="scanIcon" aria-hidden="true">&#8635;</i>
          Scan
        </button>
      </div>

      <div class="network-scroll" id="networkList"></div>
      <div class="status-text" id="statusText"></div>
    </div>
  </div>

  <div class="modal-overlay" id="modalOverlay">
    <div class="modal">
      <div class="modal-header">
        <i aria-hidden="true"></i>
        <div class="modal-network-name" id="modalNetworkName"></div>
      </div>
      <div id="pwSection">
        <div class="field-label">Password</div>
        <div class="pw-wrap">
          <div class="pw-field">
            <input class="pw-input" id="pwInput" type="text" placeholder="Enter password" autocomplete="off" spellcheck="false">
            <button class="pw-toggle" id="pwToggle" type="button" aria-label="Toggle password visibility"></button>
          </div>
        </div>
      </div>
      <div class="modal-actions">
        <button class="cancel-btn" id="cancelBtn" type="button">Cancel</button>
        <button class="connect-btn" id="connectBtn" type="button">Connect</button>
      </div>
    </div>
  </div>
</div>

<script>
  const state = {
    networks: [],
    selected: null,
    busy: false,
    passwordValue: '',
    passwordVisible: false,
    passwordRevealTimer: null,
  };

  const eyeIcon = '<svg viewBox="0 0 24 24" width="16" height="16" aria-hidden="true" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round"><path d="M1.5 12s3.75-6 10.5-6 10.5 6 10.5 6-3.75 6-10.5 6S1.5 12 1.5 12Z"></path><circle cx="12" cy="12" r="3.5"></circle></svg>';
  const eyeOffIcon = '<svg viewBox="0 0 24 24" width="16" height="16" aria-hidden="true" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round"><path d="M3 3l18 18"></path><path d="M10.6 5.2A11.2 11.2 0 0 1 12 5.1c6.75 0 10.5 6 10.5 6a18.9 18.9 0 0 1-4 4.6"></path><path d="M6.2 6.2C3.6 8.1 1.5 11.1 1.5 11.1s3.75 6 10.5 6c1.7 0 3.2-.4 4.5-1"></path><path d="M9.9 9.9a3.5 3.5 0 0 0 4.2 4.2"></path></svg>';

  function setStatus(message, tone = '') {
    const status = document.getElementById('statusText');
    status.textContent = message || '';
    status.className = 'status-text' + (tone ? ' ' + tone : '');
  }

  function setCardLabel(message) {
    document.getElementById('cardLabel').textContent = message;
  }

  function signalBars(rssi) {
    let lit = 1;
    if (rssi >= -55) lit = 4;
    else if (rssi >= -65) lit = 3;
    else if (rssi >= -75) lit = 2;
    return `
      <div class="signal-bars">
        <div class="bar${lit >= 1 ? ' lit' : ''}"></div>
        <div class="bar${lit >= 2 ? ' lit' : ''}"></div>
        <div class="bar${lit >= 3 ? ' lit' : ''}"></div>
        <div class="bar${lit >= 4 ? ' lit' : ''}"></div>
      </div>`;
  }

  function renderNetworks() {
    const list = document.getElementById('networkList');
    if (!state.networks.length) {
      list.innerHTML = `
        <div class="network-row empty">
          <div class="network-left">
            <div class="network-name">No networks found</div>
          </div>
        </div>`;
      return;
    }

    list.innerHTML = state.networks.map((network, index) => `
      <button class="network-row" type="button" data-index="${index}">
        <div class="network-left">
          ${signalBars(network.rssi)}
          <div class="network-name">${network.ssid}</div>
        </div>
        <div class="network-right">
          <div class="network-security">${network.auth_mode}</div>
          <div class="network-signal">${network.rssi} dBm</div>
        </div>
      </button>`).join('');

    list.querySelectorAll('[data-index]').forEach((item) => {
      item.addEventListener('click', () => {
        const index = Number(item.getAttribute('data-index'));
        openModal(state.networks[index]);
      });
    });
  }

  function openModal(network) {
    state.selected = network;
    state.passwordValue = '';
    state.passwordVisible = false;
    clearPasswordRevealTimer();
    document.getElementById('modalNetworkName').textContent = network.ssid;
    document.getElementById('pwSection').style.display = network.secure ? 'block' : 'none';
    updatePasswordDisplay();
    document.getElementById('modalOverlay').classList.add('open');
    if (network.secure) {
      window.setTimeout(() => document.getElementById('pwInput').focus(), 0);
    }
  }

  function closeModal() {
    if (state.busy) return;
    document.getElementById('modalOverlay').classList.remove('open');
    state.selected = null;
  }

  function clearPasswordRevealTimer() {
    if (state.passwordRevealTimer !== null) {
      window.clearTimeout(state.passwordRevealTimer);
      state.passwordRevealTimer = null;
    }
  }

  function maskedPassword(showLastChar) {
    if (!state.passwordValue) {
      return '';
    }
    if (state.passwordVisible) {
      return state.passwordValue;
    }
    if (showLastChar && state.passwordValue.length > 0) {
      return '•'.repeat(Math.max(0, state.passwordValue.length - 1)) +
             state.passwordValue.slice(-1);
    }
    return '•'.repeat(state.passwordValue.length);
  }

  function updatePasswordToggle() {
    document.getElementById('pwToggle').innerHTML = state.passwordVisible ? eyeOffIcon : eyeIcon;
  }

  function updatePasswordDisplay(showLastChar = false) {
    const input = document.getElementById('pwInput');
    input.value = maskedPassword(showLastChar);
    updatePasswordToggle();
    window.setTimeout(() => {
      const pos = input.value.length;
      input.setSelectionRange(pos, pos);
    }, 0);
  }

  function brieflyRevealLastChar() {
    clearPasswordRevealTimer();
    updatePasswordDisplay(true);
    if (!state.passwordVisible) {
      state.passwordRevealTimer = window.setTimeout(() => {
        state.passwordRevealTimer = null;
        updatePasswordDisplay(false);
      }, 700);
    }
  }

  function handlePasswordKeydown(event) {
    if (event.key === 'Enter') {
      event.preventDefault();
      connectSelected();
      return;
    }

    if (event.key === 'Backspace') {
      event.preventDefault();
      state.passwordValue = state.passwordValue.slice(0, -1);
      clearPasswordRevealTimer();
      updatePasswordDisplay(false);
      return;
    }

    if (event.key === 'Delete') {
      event.preventDefault();
      state.passwordValue = '';
      clearPasswordRevealTimer();
      updatePasswordDisplay(false);
      return;
    }

    if (event.key.length === 1 && !event.ctrlKey && !event.metaKey && !event.altKey) {
      event.preventDefault();
      state.passwordValue += event.key;
      brieflyRevealLastChar();
    }
  }

  function handlePasswordPaste(event) {
    event.preventDefault();
    const pasted = (event.clipboardData || window.clipboardData).getData('text');
    if (!pasted) {
      return;
    }
    state.passwordValue += pasted;
    brieflyRevealLastChar();
  }

  function togglePasswordVisibility() {
    state.passwordVisible = !state.passwordVisible;
    clearPasswordRevealTimer();
    updatePasswordDisplay(false);
    document.getElementById('pwInput').focus();
  }

  function animateScanIcon() {
    const icon = document.getElementById('scanIcon');
    icon.style.transition = 'transform 0.6s';
    icon.style.transform = 'rotate(360deg)';
    window.setTimeout(() => {
      icon.style.transition = '';
      icon.style.transform = '';
    }, 650);
  }

  async function refreshScan() {
    if (state.busy) {
      return;
    }

    document.getElementById('scanBtn').disabled = true;
    setCardLabel('Scanning Networks');
    setStatus('Scanning nearby Wi-Fi networks...');
    animateScanIcon();

    try {
      const response = await fetch('/api/provision/scan');
      const body = await response.json();
      state.networks = body.ok ? (body.networks || []) : [];
      renderNetworks();
      setCardLabel('Available Networks');
      setStatus(body.ok ? 'Select a network to connect.' : (body.message || 'Scan failed.'), body.ok ? '' : 'bad');
    } catch (error) {
      state.networks = [];
      renderNetworks();
      setCardLabel('Available Networks');
      setStatus('Wi-Fi scan failed. Try again.', 'bad');
    } finally {
      document.getElementById('scanBtn').disabled = false;
    }
  }

  async function connectSelected() {
    if (state.busy || !state.selected) {
      return;
    }

    const password = state.passwordValue;
    state.busy = true;
    document.getElementById('connectBtn').disabled = true;
    setStatus(`Testing ${state.selected.ssid}...`);

    try {
      const response = await fetch('/api/provision/connect', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({
          ssid: state.selected.ssid,
          password: state.selected.secure ? password : '',
        }),
      });
      const body = await response.json();
      if (body.ok) {
        setStatus(body.message || 'Connected. Rebooting...', 'good');
        document.getElementById('connectBtn').textContent = 'Connecting...';
      } else {
        setStatus(body.message || 'Connection failed.', 'bad');
      }
    } catch (error) {
      setStatus('Provisioning request failed. Try again.', 'bad');
    } finally {
      if (!document.getElementById('connectBtn').textContent.includes('Connecting')) {
        state.busy = false;
        document.getElementById('connectBtn').disabled = false;
      }
    }
  }

  document.getElementById('scanBtn').addEventListener('click', refreshScan);
  document.getElementById('cancelBtn').addEventListener('click', closeModal);
  document.getElementById('modalOverlay').addEventListener('click', (event) => {
    if (event.target === event.currentTarget) {
      closeModal();
    }
  });
  document.getElementById('connectBtn').addEventListener('click', connectSelected);
  document.getElementById('pwInput').addEventListener('keydown', handlePasswordKeydown);
  document.getElementById('pwInput').addEventListener('paste', handlePasswordPaste);
  document.getElementById('pwToggle').addEventListener('click', togglePasswordVisibility);

  renderNetworks();
  refreshScan();
  </script>
</body>
</html>
)HTML";

httpd_handle_t g_server = nullptr;
httpd_handle_t g_provision_server = nullptr;
bool g_reboot_pending = false;
int64_t g_reboot_at_us = 0;
bool g_web_ui_mounted = false;
bool g_maintenance_active = false;

const char *wifi_mode_name(wifi_manager::WifiMode mode) {
  switch (mode) {
    case wifi_manager::WifiMode::STA:
      return "STA";
    case wifi_manager::WifiMode::PROVISION_AP:
      return "PROVISION_AP";
    case wifi_manager::WifiMode::APSTA_TEST:
      return "APSTA_TEST";
    default:
      return "UNKNOWN";
  }
}

std::string partition_label(const esp_partition_t *partition) {
  return partition == nullptr ? "n/a" : std::string(partition->label);
}

const char *remote_action_result_name(control_app::RemoteActionResult result) {
  switch (result) {
    case control_app::RemoteActionResult::ACCEPTED:
      return "accepted";
    case control_app::RemoteActionResult::INVALID_STATE:
      return "invalid_state";
    case control_app::RemoteActionResult::NOT_INITIALIZED:
    default:
      return "not_initialized";
  }
}

bool mount_web_ui() {
  if (g_web_ui_mounted) {
    return true;
  }

  const esp_vfs_spiffs_conf_t conf = {
      .base_path = kWebUiBasePath,
      .partition_label = kWebUiPartitionLabel,
      .max_files = 4,
      .format_if_mount_failed = false,
  };
  const esp_err_t err = esp_vfs_spiffs_register(&conf);
  if (err != ESP_OK) {
    DEV_LOGW(kTag, "Failed to mount web_ui partition: %s", esp_err_to_name(err));
    return false;
  }
  g_web_ui_mounted = true;
  return true;
}

void unmount_web_ui() {
  if (!g_web_ui_mounted) {
    return;
  }
  esp_vfs_spiffs_unregister(kWebUiPartitionLabel);
  g_web_ui_mounted = false;
}

void schedule_reboot(int64_t delay_us) {
  g_reboot_pending = true;
  g_reboot_at_us = esp_timer_get_time() + delay_us;
  DEV_LOGI(kTag, "Reboot scheduled in %lld ms", static_cast<long long>(delay_us / 1000));
}

bool read_request_body(httpd_req_t *request, std::string &body) {
  if (request->content_len <= 0) {
    body.clear();
    return true;
  }

  body.assign(static_cast<std::size_t>(request->content_len), '\0');
  int offset = 0;
  while (offset < request->content_len) {
    const int read = httpd_req_recv(request, body.data() + offset, request->content_len - offset);
    if (read <= 0) {
      return false;
    }
    offset += read;
  }
  return true;
}

esp_err_t send_json(httpd_req_t *request, cJSON *root) {
  char *text = cJSON_PrintUnformatted(root);
  if (text == nullptr) {
    cJSON_Delete(root);
    return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "JSON encode failed");
  }

  httpd_resp_set_type(request, "application/json");
  const esp_err_t err = httpd_resp_sendstr(request, text);
  cJSON_free(text);
  cJSON_Delete(root);
  return err;
}

esp_err_t status_handler(httpd_req_t *request) {
  const auto status = get_status();

  char body[384] = {};
  snprintf(body,
           sizeof(body),
           "{\"ok\":true,\"ota_server\":%s,\"reboot_pending\":%s,"
           "\"running_partition\":\"%s\",\"boot_partition\":\"%s\","
           "\"next_update_partition\":\"%s\"}",
           status.server_running ? "true" : "false",
           status.reboot_pending ? "true" : "false",
           status.running_partition.c_str(),
           status.boot_partition.c_str(),
           status.next_update_partition.c_str());

  httpd_resp_set_type(request, "application/json");
  return httpd_resp_sendstr(request, body);
}

esp_err_t settings_handler(httpd_req_t *request) {
  const auto &settings = config_store::settings();
  char body[512] = {};
  snprintf(body,
           sizeof(body),
           "{\"ok\":true,"
           "\"idle_sleep_timeout_ms\":%lu,"
           "\"abort_blower_duration_ms\":%lu,"
           "\"temp_fault_high_c\":%.1f,"
           "\"temp_fault_low_c\":%.1f,"
           "\"battery_warning_v\":%.2f,"
           "\"battery_fault_v\":%.2f,"
           "\"sound_volume\":%u,"
           "\"button_press_beep_enabled\":%s,"
           "\"active_profile\":\"%s\"}",
           static_cast<unsigned long>(settings.idle_sleep_timeout_ms),
           static_cast<unsigned long>(settings.abort_blower_duration_ms),
           static_cast<double>(settings.temp_fault_high_c),
           static_cast<double>(settings.temp_fault_low_c),
           static_cast<double>(settings.battery_warning_v),
           static_cast<double>(settings.battery_fault_low_v),
           static_cast<unsigned>(settings.sound_volume),
           settings.button_press_beep_enabled ? "true" : "false",
           settings.active_profile_name.data());

  httpd_resp_set_type(request, "application/json");
  return httpd_resp_sendstr(request, body);
}

esp_err_t dashboard_page_handler(httpd_req_t *request) {
  if (!mount_web_ui()) {
    return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "web_ui unavailable");
  }

  FILE *file = fopen(kWebUiIndexPath, "rb");
  if (file == nullptr) {
    return httpd_resp_send_err(request, HTTPD_404_NOT_FOUND, "dashboard not found");
  }

  httpd_resp_set_type(request, "text/html; charset=utf-8");
  char buffer[1024] = {};
  std::size_t read = 0;
  while ((read = fread(buffer, 1, sizeof(buffer), file)) > 0) {
    if (httpd_resp_send_chunk(request, buffer, static_cast<ssize_t>(read)) != ESP_OK) {
      fclose(file);
      httpd_resp_sendstr_chunk(request, nullptr);
      return ESP_FAIL;
    }
  }
  fclose(file);
  return httpd_resp_sendstr_chunk(request, nullptr);
}

esp_err_t dashboard_status_handler(httpd_req_t *request) {
  const auto control = control_app::get_status();
  const auto wifi = wifi_manager::get_status();
  const auto *app = esp_app_get_description();
  const auto update = update_manager::get_status();

  cJSON *root = cJSON_CreateObject();
  cJSON_AddBoolToObject(root, "ok", true);
  cJSON_AddStringToObject(root, "board_id", unit_identity::kBoardId);
  cJSON_AddStringToObject(
      root, "unit_id", unit_identity::has_unit_id() ? unit_identity::unit_id().c_str() : "unprovisioned");
  cJSON_AddStringToObject(root, "firmware_version", app != nullptr ? app->version : "unknown");
  cJSON_AddStringToObject(root, "hostname", wifi.hostname.c_str());
  cJSON_AddStringToObject(root, "ip", wifi.ip_address.empty() ? "n/a" : wifi.ip_address.c_str());
  cJSON_AddNumberToObject(root, "http_port", kOtaServerPort);
  cJSON_AddStringToObject(root, "state", control.state_name);
  cJSON_AddStringToObject(root, "detail", control.detail_name[0] != '\0' ? control.detail_name : "-");
  cJSON_AddStringToObject(
      root, "update_current_version", app != nullptr ? app->version : update.current_version.c_str());
  cJSON_AddStringToObject(
      root,
      "update_available_version",
      update.available_version.empty() ? "n/a" : update.available_version.c_str());
  cJSON_AddStringToObject(root, "update_last_result", update.last_result.c_str());
  cJSON_AddStringToObject(root, "update_last_message", update.last_message.c_str());
  cJSON_AddBoolToObject(root, "update_available", update.update_available);
  cJSON_AddBoolToObject(root, "update_pending_confirmation", update.pending_confirmation);
  cJSON_AddBoolToObject(root, "update_rollback_armed", update.rollback_armed);
  cJSON_AddBoolToObject(root, "temp_valid", control.temp_valid);
  cJSON_AddBoolToObject(root, "battery_valid", control.battery_valid);
  cJSON_AddNumberToObject(root, "temp_c", control.temp_c);
  cJSON_AddNumberToObject(root, "battery_v", control.battery_v);
  return send_json(request, root);
}

esp_err_t dashboard_beep_handler(httpd_req_t *request) {
  output_control::play_button_press();
  httpd_resp_set_type(request, "application/json");
  return httpd_resp_sendstr(
      request, "{\"ok\":true,\"message\":\"Test beep sent to the board.\"}");
}

esp_err_t dashboard_start_cycle_handler(httpd_req_t *request) {
  const auto result = control_app::request_start_cycle();
  const bool ok = result == control_app::RemoteActionResult::ACCEPTED;
  const char *message = ok ? "Ignition cycle countdown requested." :
                             "Ignition cycle can only start from READY_IDLE.";
  char body[192] = {};
  snprintf(body,
           sizeof(body),
           "{\"ok\":%s,\"result\":\"%s\",\"message\":\"%s\"}",
           ok ? "true" : "false",
           remote_action_result_name(result),
           message);
  httpd_resp_set_type(request, "application/json");
  return httpd_resp_sendstr(request, body);
}

esp_err_t dashboard_abort_handler(httpd_req_t *request) {
  const auto result = control_app::request_abort();
  const bool ok = result == control_app::RemoteActionResult::ACCEPTED;
  const char *message = ok ? "Abort requested." :
                             "Abort is only available during IGNITION_CYCLE.";
  char body[192] = {};
  snprintf(body,
           sizeof(body),
           "{\"ok\":%s,\"result\":\"%s\",\"message\":\"%s\"}",
           ok ? "true" : "false",
           remote_action_result_name(result),
           message);
  httpd_resp_set_type(request, "application/json");
  return httpd_resp_sendstr(request, body);
}

esp_err_t dashboard_toggle_blower_handler(httpd_req_t *request) {
  const auto status = control_app::get_status();
  const bool turning_on = status.state == ControlStateId::READY_IDLE;
  const auto result = control_app::request_toggle_blower();
  const bool ok = result == control_app::RemoteActionResult::ACCEPTED;
  const char *message = ok
                            ? (turning_on ? "Blower mode requested." : "Blower mode exit requested.")
                            : "Blower toggle is only available from READY_IDLE or BLOWER_MODE.";
  char body[224] = {};
  snprintf(body,
           sizeof(body),
           "{\"ok\":%s,\"result\":\"%s\",\"message\":\"%s\",\"turning_on\":%s}",
           ok ? "true" : "false",
           remote_action_result_name(result),
           message,
           turning_on ? "true" : "false");
  httpd_resp_set_type(request, "application/json");
  return httpd_resp_sendstr(request, body);
}

esp_err_t ota_post_handler(httpd_req_t *request) {
  const esp_partition_t *update_partition = esp_ota_get_next_update_partition(nullptr);
  if (update_partition == nullptr) {
    httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "No OTA partition available");
    return ESP_FAIL;
  }

  g_maintenance_active = true;
  esp_ota_handle_t ota_handle = 0;
  esp_err_t err = esp_ota_begin(update_partition, OTA_SIZE_UNKNOWN, &ota_handle);
  if (err != ESP_OK) {
    g_maintenance_active = false;
    DEV_LOGE(kTag, "esp_ota_begin failed: %s", esp_err_to_name(err));
    httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA begin failed");
    return err;
  }

  char buffer[1024] = {};
  int remaining = request->content_len;
  while (remaining > 0) {
    const int read = httpd_req_recv(request,
                                    buffer,
                                    remaining > static_cast<int>(sizeof(buffer))
                                        ? static_cast<int>(sizeof(buffer))
                                        : remaining);
    if (read <= 0) {
      g_maintenance_active = false;
      esp_ota_abort(ota_handle);
      httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA receive failed");
      return ESP_FAIL;
    }

    err = esp_ota_write(ota_handle, buffer, static_cast<size_t>(read));
    if (err != ESP_OK) {
      DEV_LOGE(kTag, "esp_ota_write failed: %s", esp_err_to_name(err));
      g_maintenance_active = false;
      esp_ota_abort(ota_handle);
      httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA write failed");
      return err;
    }

    remaining -= read;
  }

  err = esp_ota_end(ota_handle);
  if (err != ESP_OK) {
    g_maintenance_active = false;
    DEV_LOGE(kTag, "esp_ota_end failed: %s", esp_err_to_name(err));
    httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Invalid firmware image");
    return err;
  }

  err = esp_ota_set_boot_partition(update_partition);
  if (err != ESP_OK) {
    g_maintenance_active = false;
    DEV_LOGE(kTag, "esp_ota_set_boot_partition failed: %s", esp_err_to_name(err));
    httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to select boot slot");
    return err;
  }

  DEV_LOGI(kTag, "OTA image written to partition '%s'", update_partition->label);
  schedule_reboot(kDefaultRebootDelayUs);

  httpd_resp_set_type(request, "application/json");
  httpd_resp_sendstr(
      request, "{\"ok\":true,\"message\":\"Firmware uploaded successfully. Rebooting.\"}");

  return ESP_OK;
}

esp_err_t settings_fs_post_handler(httpd_req_t *request) {
  const esp_partition_t *partition = esp_partition_find_first(
      ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, "settings");
  if (partition == nullptr) {
    httpd_resp_send_err(
        request, HTTPD_500_INTERNAL_SERVER_ERROR, "Settings partition not found");
    return ESP_FAIL;
  }

  if (request->content_len <= 0 || request->content_len > static_cast<int>(partition->size)) {
    httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Invalid settings image size");
    return ESP_FAIL;
  }

  config_store::prepare_settings_partition_update();

  esp_err_t err = esp_partition_erase_range(partition, 0, partition->size);
  if (err != ESP_OK) {
    DEV_LOGE(kTag, "Failed erasing settings partition: %s", esp_err_to_name(err));
    config_store::finalize_settings_partition_update();
    httpd_resp_send_err(
        request, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to erase settings partition");
    return err;
  }

  char buffer[1024] = {};
  std::size_t offset = 0;
  int remaining = request->content_len;
  while (remaining > 0) {
    const int read = httpd_req_recv(request,
                                    buffer,
                                    remaining > static_cast<int>(sizeof(buffer))
                                        ? static_cast<int>(sizeof(buffer))
                                        : remaining);
    if (read <= 0) {
      config_store::finalize_settings_partition_update();
      httpd_resp_send_err(
          request, HTTPD_500_INTERNAL_SERVER_ERROR, "Settings receive failed");
      return ESP_FAIL;
    }

    err = esp_partition_write(partition, offset, buffer, static_cast<std::size_t>(read));
    if (err != ESP_OK) {
      DEV_LOGE(kTag, "Failed writing settings partition: %s", esp_err_to_name(err));
      config_store::finalize_settings_partition_update();
      httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "Settings write failed");
      return err;
    }

    offset += static_cast<std::size_t>(read);
    remaining -= read;
  }

  config_store::finalize_settings_partition_update();
  DEV_LOGI(kTag, "Settings filesystem image written to partition '%s'", partition->label);

  httpd_resp_set_type(request, "application/json");
  return httpd_resp_sendstr(
      request, "{\"ok\":true,\"message\":\"Settings filesystem uploaded successfully.\"}");
}

esp_err_t web_ui_fs_post_handler(httpd_req_t *request) {
  const esp_partition_t *partition = esp_partition_find_first(
      ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, kWebUiPartitionLabel);
  if (partition == nullptr) {
    httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "Web UI partition not found");
    return ESP_FAIL;
  }

  if (request->content_len <= 0 || request->content_len > static_cast<int>(partition->size)) {
    httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Invalid web_ui image size");
    return ESP_FAIL;
  }

  unmount_web_ui();

  esp_err_t err = esp_partition_erase_range(partition, 0, partition->size);
  if (err != ESP_OK) {
    DEV_LOGE(kTag, "Failed erasing web_ui partition: %s", esp_err_to_name(err));
    mount_web_ui();
    httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to erase web_ui partition");
    return err;
  }

  char buffer[1024] = {};
  std::size_t offset = 0;
  int remaining = request->content_len;
  while (remaining > 0) {
    const int read = httpd_req_recv(request,
                                    buffer,
                                    remaining > static_cast<int>(sizeof(buffer))
                                        ? static_cast<int>(sizeof(buffer))
                                        : remaining);
    if (read <= 0) {
      mount_web_ui();
      httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "Web UI receive failed");
      return ESP_FAIL;
    }

    err = esp_partition_write(partition, offset, buffer, static_cast<std::size_t>(read));
    if (err != ESP_OK) {
      DEV_LOGE(kTag, "Failed writing web_ui partition: %s", esp_err_to_name(err));
      mount_web_ui();
      httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "Web UI write failed");
      return err;
    }

    offset += static_cast<std::size_t>(read);
    remaining -= read;
  }

  mount_web_ui();
  DEV_LOGI(kTag, "Web UI filesystem image written to partition '%s'", partition->label);

  httpd_resp_set_type(request, "application/json");
  return httpd_resp_sendstr(
      request, "{\"ok\":true,\"message\":\"Web UI filesystem uploaded successfully.\"}");
}

esp_err_t provision_page_handler(httpd_req_t *request) {
  httpd_resp_set_type(request, "text/html; charset=utf-8");
  return httpd_resp_sendstr(request, kProvisionPage);
}

esp_err_t provision_status_handler(httpd_req_t *request) {
  const auto wifi = wifi_manager::get_status();

  cJSON *root = cJSON_CreateObject();
  cJSON_AddBoolToObject(root, "ok", true);
  cJSON_AddStringToObject(root, "mode", wifi_mode_name(wifi.mode));
  cJSON_AddStringToObject(root, "board_id", unit_identity::kBoardId);
  cJSON_AddStringToObject(
      root, "unit_id", unit_identity::has_unit_id() ? unit_identity::unit_id().c_str() : "unprovisioned");
  cJSON_AddBoolToObject(root, "credentials_stored", wifi.credentials_stored);
  cJSON_AddBoolToObject(root, "connect_test_active", wifi.connect_test_active);
  cJSON_AddBoolToObject(root, "last_connect_success", wifi.last_connect_success);
  cJSON_AddStringToObject(root, "ap_ssid", wifi.ap_ssid.c_str());
  cJSON_AddStringToObject(root, "ap_ip", wifi.ap_ip_address.c_str());
  cJSON_AddStringToObject(root, "configured_ssid", wifi.configured_ssid.c_str());
  cJSON_AddStringToObject(root, "message", wifi.last_connect_message.c_str());
  return send_json(request, root);
}

esp_err_t provision_scan_handler(httpd_req_t *request) {
  std::vector<wifi_manager::ScanResult> results;
  std::string message;
  if (!wifi_manager::scan_networks(results, message)) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", false);
    cJSON_AddStringToObject(root, "message", message.c_str());
    return send_json(request, root);
  }

  cJSON *root = cJSON_CreateObject();
  cJSON_AddBoolToObject(root, "ok", true);
  cJSON_AddStringToObject(root, "message", message.c_str());
  cJSON *networks = cJSON_AddArrayToObject(root, "networks");
  for (const auto &network : results) {
    cJSON *item = cJSON_CreateObject();
    cJSON_AddStringToObject(item, "ssid", network.ssid.c_str());
    cJSON_AddNumberToObject(item, "rssi", network.rssi);
    cJSON_AddBoolToObject(item, "secure", network.secure);
    cJSON_AddStringToObject(item, "auth_mode", network.auth_mode.c_str());
    cJSON_AddItemToArray(networks, item);
  }
  return send_json(request, root);
}

esp_err_t provision_connect_handler(httpd_req_t *request) {
  std::string body;
  if (!read_request_body(request, body)) {
    return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Failed reading request body");
  }

  cJSON *root = cJSON_Parse(body.c_str());
  if (root == nullptr) {
    return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Invalid JSON");
  }

  cJSON *ssid = cJSON_GetObjectItemCaseSensitive(root, "ssid");
  cJSON *password = cJSON_GetObjectItemCaseSensitive(root, "password");
  const std::string ssid_value = cJSON_IsString(ssid) && ssid->valuestring != nullptr
                                     ? ssid->valuestring
                                     : "";
  const std::string password_value = cJSON_IsString(password) && password->valuestring != nullptr
                                         ? password->valuestring
                                         : "";
  cJSON_Delete(root);

  std::string message;
  const bool ok = wifi_manager::test_and_store_credentials(ssid_value, password_value, message);

  cJSON *response = cJSON_CreateObject();
  cJSON_AddBoolToObject(response, "ok", ok);
  cJSON_AddStringToObject(response, "message", message.c_str());
  cJSON_AddBoolToObject(response, "rebooting", ok);
  const esp_err_t err = send_json(request, response);
  if (ok) {
    DEV_LOGI(kTag, "Provisioning successful for SSID '%s'. Rebooting into normal STA mode.", ssid_value.c_str());
    schedule_reboot(1200000);
  } else {
    DEV_LOGW(kTag, "Provisioning connect test failed for SSID '%s': %s", ssid_value.c_str(), message.c_str());
  }
  return err;
}

void start_main_server() {
  if (g_server != nullptr) {
    return;
  }

  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = kOtaServerPort;
  config.stack_size = 8192;
  config.max_uri_handlers = 16;
  ESP_ERROR_CHECK(httpd_start(&g_server, &config));

  const httpd_uri_t status_uri = {
      .uri = kStatusUri,
      .method = HTTP_GET,
      .handler = status_handler,
      .user_ctx = nullptr,
  };
  const httpd_uri_t dashboard_page_uri = {
      .uri = "/",
      .method = HTTP_GET,
      .handler = dashboard_page_handler,
      .user_ctx = nullptr,
  };
  const httpd_uri_t dashboard_status_uri = {
      .uri = kDashboardStatusUri,
      .method = HTTP_GET,
      .handler = dashboard_status_handler,
      .user_ctx = nullptr,
  };
  const httpd_uri_t dashboard_beep_uri = {
      .uri = kDashboardBeepUri,
      .method = HTTP_POST,
      .handler = dashboard_beep_handler,
      .user_ctx = nullptr,
  };
  const httpd_uri_t dashboard_start_cycle_uri = {
      .uri = kDashboardStartCycleUri,
      .method = HTTP_POST,
      .handler = dashboard_start_cycle_handler,
      .user_ctx = nullptr,
  };
  const httpd_uri_t dashboard_abort_uri = {
      .uri = kDashboardAbortUri,
      .method = HTTP_POST,
      .handler = dashboard_abort_handler,
      .user_ctx = nullptr,
  };
  const httpd_uri_t dashboard_toggle_blower_uri = {
      .uri = kDashboardToggleBlowerUri,
      .method = HTTP_POST,
      .handler = dashboard_toggle_blower_handler,
      .user_ctx = nullptr,
  };
  const httpd_uri_t ota_uri = {
      .uri = kOtaUri,
      .method = HTTP_POST,
      .handler = ota_post_handler,
      .user_ctx = nullptr,
  };
  const httpd_uri_t settings_uri = {
      .uri = kSettingsUri,
      .method = HTTP_GET,
      .handler = settings_handler,
      .user_ctx = nullptr,
  };
  const httpd_uri_t settings_fs_uri = {
      .uri = kSettingsFsUri,
      .method = HTTP_POST,
      .handler = settings_fs_post_handler,
      .user_ctx = nullptr,
  };
  const httpd_uri_t web_ui_fs_uri = {
      .uri = kWebUiFsUri,
      .method = HTTP_POST,
      .handler = web_ui_fs_post_handler,
      .user_ctx = nullptr,
  };

  ESP_ERROR_CHECK(httpd_register_uri_handler(g_server, &dashboard_page_uri));
  ESP_ERROR_CHECK(httpd_register_uri_handler(g_server, &dashboard_status_uri));
  ESP_ERROR_CHECK(httpd_register_uri_handler(g_server, &dashboard_beep_uri));
  ESP_ERROR_CHECK(httpd_register_uri_handler(g_server, &dashboard_start_cycle_uri));
  ESP_ERROR_CHECK(httpd_register_uri_handler(g_server, &dashboard_abort_uri));
  ESP_ERROR_CHECK(httpd_register_uri_handler(g_server, &dashboard_toggle_blower_uri));
  ESP_ERROR_CHECK(httpd_register_uri_handler(g_server, &status_uri));
  ESP_ERROR_CHECK(httpd_register_uri_handler(g_server, &settings_uri));
  ESP_ERROR_CHECK(httpd_register_uri_handler(g_server, &ota_uri));
  ESP_ERROR_CHECK(httpd_register_uri_handler(g_server, &settings_fs_uri));
  ESP_ERROR_CHECK(httpd_register_uri_handler(g_server, &web_ui_fs_uri));
  DEV_LOGI(kTag, "OTA HTTP server listening on port %d", kOtaServerPort);
}

void start_provision_server_if_needed() {
  if (g_provision_server != nullptr) {
    return;
  }

  const auto wifi = wifi_manager::get_status();
  if (!wifi.ap_active) {
    return;
  }

  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = kProvisionServerPort;
  config.ctrl_port = kProvisionCtrlPort;
  config.stack_size = 8192;
  config.max_uri_handlers = 8;
  ESP_ERROR_CHECK(httpd_start(&g_provision_server, &config));

  const httpd_uri_t root_uri = {
      .uri = kProvisionRootUri,
      .method = HTTP_GET,
      .handler = provision_page_handler,
      .user_ctx = nullptr,
  };
  const httpd_uri_t status_uri = {
      .uri = kProvisionStatusUri,
      .method = HTTP_GET,
      .handler = provision_status_handler,
      .user_ctx = nullptr,
  };
  const httpd_uri_t scan_uri = {
      .uri = kProvisionScanUri,
      .method = HTTP_GET,
      .handler = provision_scan_handler,
      .user_ctx = nullptr,
  };
  const httpd_uri_t connect_uri = {
      .uri = kProvisionConnectUri,
      .method = HTTP_POST,
      .handler = provision_connect_handler,
      .user_ctx = nullptr,
  };

  ESP_ERROR_CHECK(httpd_register_uri_handler(g_provision_server, &root_uri));
  ESP_ERROR_CHECK(httpd_register_uri_handler(g_provision_server, &status_uri));
  ESP_ERROR_CHECK(httpd_register_uri_handler(g_provision_server, &scan_uri));
  ESP_ERROR_CHECK(httpd_register_uri_handler(g_provision_server, &connect_uri));

  DEV_LOGI(kTag,
           "Provisioning portal listening on http://%s/",
           wifi.ap_ip_address.empty() ? "192.168.4.1" : wifi.ap_ip_address.c_str());
}

}  // namespace

void init() {
  start_main_server();
  start_provision_server_if_needed();
}

void poll() {
  if (g_reboot_pending && esp_timer_get_time() >= g_reboot_at_us) {
    DEV_LOGI(kTag, "Rebooting device");
    esp_restart();
  }
}

OtaStatus get_status() {
  const esp_partition_t *running = esp_ota_get_running_partition();
  const esp_partition_t *boot = esp_ota_get_boot_partition();
  const esp_partition_t *next = esp_ota_get_next_update_partition(nullptr);

  return {
      .server_running = g_server != nullptr,
      .reboot_pending = g_reboot_pending,
      .running_partition = partition_label(running),
      .boot_partition = partition_label(boot),
      .next_update_partition = partition_label(next),
  };
}

bool maintenance_active() {
  return g_maintenance_active;
}

}  // namespace ota_service
