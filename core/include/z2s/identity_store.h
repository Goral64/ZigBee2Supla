// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "z2s/backend.h"

namespace z2s {

// Persistent Supla identity of one ZigBee device.
struct DeviceIdentity {
  std::string id;
  std::array<uint8_t, 16> guid{};
  std::array<uint8_t, 16> authKey{};
  // Registered channel layout. The index is the Supla channel number. Supla
  // does not allow changing the type of an existing channel or removing it,
  // so this list is append-only.
  std::vector<ChannelSpec> channels;
};

using RandomBytesFn = std::function<bool(uint8_t *buf, size_t len)>;

// Keeps GUID/AuthKey and channel layout of every bridged device in a JSON
// file, so that each ZigBee device keeps its Supla identity across restarts.
class IdentityStore {
 public:
  static constexpr size_t kMaxChannels = 128;  // SUPLA_CHANNELMAXCOUNT

  IdentityStore(std::string path, RandomBytesFn randomBytes);

  // Loads the store. A missing file is not an error (empty store).
  bool load();
  // Atomically writes the store (temporary file + rename).
  bool save() const;

  // Returns the identity of the device, creating it when needed, and makes
  // sure every channel of the descriptor has a channel number: channels
  // already known keep their numbers, new ones are appended. Channels that
  // disappeared from the descriptor are kept (Supla forbids removal).
  // Sets *changed when the identity was created or modified.
  // Returns nullptr when a new GUID/AuthKey could not be generated.
  const DeviceIdentity *reconcile(const DeviceDescriptor &device,
                                  bool *changed);

  const DeviceIdentity *find(const std::string &id) const;
  size_t size() const { return identities_.size(); }

 private:
  std::string path_;
  RandomBytesFn randomBytes_;
  std::map<std::string, DeviceIdentity> identities_;
};

std::string toHex(const uint8_t *data, size_t len);
bool fromHex(const std::string &hex, uint8_t *out, size_t len);

}  // namespace z2s
