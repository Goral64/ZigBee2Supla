// SPDX-License-Identifier: GPL-2.0-or-later
//
// Supla protocol versions supported by zigbee2supla.
// Changing any of these values affects all devices registered in Supla.
#pragma once

namespace z2s {

// Lowest protocol version zigbee2supla can use:
//  - registration uses SUPLA_DS_CALL_REGISTER_DEVICE_G (protocol >= 25),
//  - binary sensors use SUPLA_CHANNELFNC_MOTION_SENSOR, _FLOOD_SENSOR and
//    _BINARY_SENSOR as default functions (protocol >= 27).
constexpr int kMinProtoVersion = 27;

// Version used when the configuration does not specify one.
constexpr int kDefaultProtoVersion = 27;

// Highest SUPLA_PROTO_VERSION of the vendored supla-common that was reviewed
// and tested with zigbee2supla. tests/proto_contract_test.cpp fails when
// third_party/supla-common is updated to a newer protocol, until the new
// version is reviewed (tests/proto_contract_test.cpp).
constexpr int kReviewedProtoVersion = 29;

}  // namespace z2s
