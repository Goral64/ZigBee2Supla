// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <string>
#include <vector>

#include "z2s/channel.h"

namespace z2s {

// A ZigBee device as seen by the gateway. Every descriptor becomes a separate
// Supla device.
struct DeviceDescriptor {
  // Stable identifier (IEEE address). Used to derive the Supla identity.
  std::string id;
  // Name shown in Supla Cloud.
  std::string name;
  std::string manufacturer;
  std::string model;
  std::string softVersion;
  // 1 = battery powered, 0 = mains powered, -1 = unknown.
  int batteryPowered = -1;
  std::vector<ChannelSpec> channels;
};

// Health of a device as a whole, shown in Supla as the state of each of its
// channels. -1 = unknown (in an update: not changed).
struct DeviceHealth {
  int batteryLevel = -1;  // 0..100 %
  int batteryLow = -1;    // 1 = low, 0 = ok; used without batteryLevel
  int linkQuality = -1;   // 0..100 %
};

// Merges a partial update into an existing health (unknown fields are kept).
DeviceHealth mergeHealth(const DeviceHealth &current,
                         const DeviceHealth &update);

// Events emitted by a backend. All callbacks are invoked from the thread that
// calls Backend::poll().
class BackendListener {
 public:
  virtual ~BackendListener() = default;
  // Full list of devices known to the backend (replaces the previous one).
  virtual void onDeviceList(const std::vector<DeviceDescriptor> &devices) = 0;
  // Partial or full state update of a single channel.
  virtual void onChannelState(const std::string &deviceId,
                              const std::string &channelKey,
                              const ChannelState &state) = 0;
  // A button of the device was used (SUPLA_ACTION_CAP_*). Only for live
  // events: never for states replayed or retained by the backend.
  virtual void onChannelAction(const std::string &deviceId,
                               const std::string &channelKey,
                               uint32_t action) = 0;
  // Partial update of the device health.
  virtual void onDeviceHealth(const std::string &deviceId,
                              const DeviceHealth &health) = 0;
  // Device reachability in the ZigBee network.
  virtual void onDeviceAvailability(const std::string &deviceId,
                                    bool online) = 0;
  // Backend (e.g. MQTT broker or zigbee2mqtt bridge) reachability. When
  // false, all devices are considered offline.
  virtual void onBackendAvailability(bool online) = 0;
};

// Source of ZigBee devices (zigbee2mqtt, native coordinator, ...).
class Backend {
 public:
  virtual ~Backend() = default;
  virtual void setListener(BackendListener *listener) = 0;
  virtual bool start() = 0;
  virtual void stop() = 0;
  // Dispatches pending events to the listener. Non-blocking.
  virtual void poll() = 0;
  virtual bool sendCommand(const std::string &deviceId,
                           const std::string &channelKey,
                           const ChannelCommand &command) = 0;
  // Asks the backend to report the current state of a device. The gateway
  // calls it for every device it bridges, after each device list, so devices
  // that are filtered out are never queried.
  virtual void requestState(const std::string &deviceId) = 0;
  // Number of ZigBee devices known to the backend (bridged or not, without
  // the coordinator), for the status shown to the user.
  virtual size_t deviceCount() const = 0;
};

}  // namespace z2s
