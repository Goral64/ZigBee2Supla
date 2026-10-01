// SPDX-License-Identifier: GPL-2.0-or-later
//
// Protocol contract: everything zigbee2supla relies on in the vendored
// supla-common (third_party/supla-common). When one of these tests fails
// after updating supla-common, review the changes before adjusting it.
//
// Values here are Supla *wire format* (sizes, offsets, call ids, codes).
// They are also hardcoded in tools/fake_supla_server.py - update it together.

#include <gtest/gtest.h>
#include <proto.h>
#include <srpc.h>

#include <cstddef>

#include "z2s/protocol_info.h"

using namespace z2s;

// Canary: fails on every SUPLA_PROTO_VERSION change, forcing a review of the
// new protocol version (then bump kReviewedProtoVersion).
TEST(ProtoContractTest, ProtocolVersionWasReviewed) {
  EXPECT_EQ(SUPLA_PROTO_VERSION, kReviewedProtoVersion)
      << "supla-common has a new protocol version - review the protocol "
         "changes and update kReviewedProtoVersion";
  EXPECT_LE(kMinProtoVersion, kDefaultProtoVersion);
  EXPECT_LE(kDefaultProtoVersion, SUPLA_PROTO_VERSION);
}

// Every SRPC call used by SuplaSession must be available at the lowest
// protocol version we allow.
TEST(ProtoContractTest, UsedCallsAvailableAtMinVersion) {
  const unsigned int calls[] = {
      SUPLA_DS_CALL_REGISTER_DEVICE_G,
      SUPLA_SD_CALL_REGISTER_DEVICE_RESULT,
      SUPLA_SD_CALL_REGISTER_DEVICE_RESULT_B,
      SUPLA_DS_CALL_DEVICE_CHANNEL_VALUE_CHANGED_C,
      SUPLA_SD_CALL_CHANNEL_SET_VALUE,
      SUPLA_SD_CALL_CHANNELGROUP_SET_VALUE,
      SUPLA_DS_CALL_CHANNEL_SET_VALUE_RESULT,
      SUPLA_DCS_CALL_PING_SERVER,
      SUPLA_SDC_CALL_PING_SERVER_RESULT,
      SUPLA_DCS_CALL_SET_ACTIVITY_TIMEOUT,
      SUPLA_SDC_CALL_SET_ACTIVITY_TIMEOUT_RESULT,
      SUPLA_SD_CALL_DEVICE_CALCFG_REQUEST,
      SUPLA_DS_CALL_DEVICE_CALCFG_RESULT,
      SUPLA_SDC_CALL_VERSIONERROR,
  };
  for (unsigned int call : calls) {
    EXPECT_LE(srpc_call_min_version_required(nullptr, call), kMinProtoVersion)
        << "call id " << call;
  }
}

TEST(ProtoContractTest, CallIds) {
  EXPECT_EQ(SUPLA_DCS_CALL_PING_SERVER, 40);
  EXPECT_EQ(SUPLA_SDC_CALL_PING_SERVER_RESULT, 50);
  EXPECT_EQ(SUPLA_SD_CALL_REGISTER_DEVICE_RESULT, 70);
  EXPECT_EQ(SUPLA_SD_CALL_REGISTER_DEVICE_RESULT_B, 71);
  EXPECT_EQ(SUPLA_DS_CALL_REGISTER_DEVICE_G, 76);
  EXPECT_EQ(SUPLA_DS_CALL_DEVICE_CHANNEL_VALUE_CHANGED_C, 103);
  EXPECT_EQ(SUPLA_SD_CALL_CHANNEL_SET_VALUE, 110);
  EXPECT_EQ(SUPLA_DS_CALL_CHANNEL_SET_VALUE_RESULT, 120);
  EXPECT_EQ(SUPLA_DCS_CALL_SET_ACTIVITY_TIMEOUT, 210);
  EXPECT_EQ(SUPLA_SDC_CALL_SET_ACTIVITY_TIMEOUT_RESULT, 220);
}

TEST(ProtoContractTest, StructSizes) {
  EXPECT_EQ(sizeof(TDS_SuplaRegisterDeviceHeader), 584u);
  EXPECT_EQ(sizeof(TDS_SuplaDeviceChannel_E), 36u);
  EXPECT_EQ(sizeof(TSD_SuplaRegisterDeviceResult), 7u);
  EXPECT_EQ(sizeof(TSD_SuplaRegisterDeviceResult_B) - CHANNEL_REPORT_MAXSIZE,
            9u);
  EXPECT_EQ(sizeof(TDS_SuplaDeviceChannelValue_C), 14u);
  EXPECT_EQ(sizeof(TSD_SuplaChannelNewValue), 17u);
  EXPECT_EQ(sizeof(TDS_SuplaChannelNewValueResult), 6u);
  EXPECT_EQ(sizeof(TDCS_SuplaSetActivityTimeout), 1u);
  EXPECT_EQ(sizeof(TSDC_SuplaSetActivityTimeoutResult), 3u);
  EXPECT_EQ(SUPLA_CHANNELVALUE_SIZE, 8);
  EXPECT_EQ(SUPLA_GUID_SIZE, 16);
  EXPECT_EQ(SUPLA_AUTHKEY_SIZE, 16);
  EXPECT_EQ(SUPLA_CHANNELMAXCOUNT, 128);
}

// Offsets used by tools/fake_supla_server.py to decode registrations.
TEST(ProtoContractTest, RegisterHeaderOffsets) {
  EXPECT_EQ(offsetof(TDS_SuplaRegisterDeviceHeader, Email), 0u);
  EXPECT_EQ(offsetof(TDS_SuplaRegisterDeviceHeader, AuthKey), 256u);
  EXPECT_EQ(offsetof(TDS_SuplaRegisterDeviceHeader, GUID), 272u);
  EXPECT_EQ(offsetof(TDS_SuplaRegisterDeviceHeader, Name), 288u);
  EXPECT_EQ(offsetof(TDS_SuplaRegisterDeviceHeader, SoftVer), 489u);
  EXPECT_EQ(offsetof(TDS_SuplaRegisterDeviceHeader, ServerName), 510u);
  EXPECT_EQ(offsetof(TDS_SuplaRegisterDeviceHeader, channel_count), 583u);
  // Registration G = header followed by the channel array.
  EXPECT_EQ(offsetof(TDS_SuplaRegisterDevice_G, channels),
            sizeof(TDS_SuplaRegisterDeviceHeader));
}

// Channel types and functions registered by zigbee2supla. Changing them for
// an existing channel causes a channel conflict in Supla Cloud.
TEST(ProtoContractTest, ChannelTypesAndFunctions) {
  EXPECT_EQ(SUPLA_CHANNELTYPE_BINARYSENSOR, 1000);
  EXPECT_EQ(SUPLA_CHANNELTYPE_RELAY, 2900);
  EXPECT_EQ(SUPLA_CHANNELTYPE_THERMOMETER, 3034);
  EXPECT_EQ(SUPLA_CHANNELTYPE_HUMIDITYSENSOR, 3036);
  EXPECT_EQ(SUPLA_CHANNELTYPE_HUMIDITYANDTEMPSENSOR, 3038);
  EXPECT_EQ(SUPLA_CHANNELTYPE_PRESSURESENSOR, 3044);

  EXPECT_EQ(SUPLA_CHANNELFNC_THERMOMETER, 40);
  EXPECT_EQ(SUPLA_CHANNELFNC_HUMIDITY, 42);
  EXPECT_EQ(SUPLA_CHANNELFNC_HUMIDITYANDTEMPERATURE, 45);
  EXPECT_EQ(SUPLA_CHANNELFNC_OPENINGSENSOR_DOOR, 100);
  EXPECT_EQ(SUPLA_CHANNELFNC_POWERSWITCH, 130);
  EXPECT_EQ(SUPLA_CHANNELFNC_LIGHTSWITCH, 140);
  EXPECT_EQ(SUPLA_CHANNELFNC_PRESSURESENSOR, 260);
  EXPECT_EQ(SUPLA_CHANNELFNC_FLOOD_SENSOR, 1000);
  EXPECT_EQ(SUPLA_CHANNELFNC_MOTION_SENSOR, 1010);
  EXPECT_EQ(SUPLA_CHANNELFNC_BINARY_SENSOR, 1020);
  EXPECT_EQ(SUPLA_BIT_FUNC_POWERSWITCH, 0x20);
  EXPECT_EQ(SUPLA_BIT_FUNC_LIGHTSWITCH, 0x40);

  EXPECT_EQ(SUPLA_CHANNEL_OFFLINE_FLAG_ONLINE, 0);
  EXPECT_EQ(SUPLA_CHANNEL_OFFLINE_FLAG_OFFLINE, 1);
}

TEST(ProtoContractTest, ResultCodes) {
  EXPECT_EQ(SUPLA_RESULTCODE_TRUE, 3);
  EXPECT_EQ(SUPLA_RESULTCODE_TEMPORARILY_UNAVAILABLE, 4);
  EXPECT_EQ(SUPLA_RESULTCODE_BAD_CREDENTIALS, 5);
  EXPECT_EQ(SUPLA_RESULTCODE_CHANNEL_CONFLICT, 7);
  EXPECT_EQ(SUPLA_RESULTCODE_DEVICE_DISABLED, 8);
  EXPECT_EQ(SUPLA_RESULTCODE_DEVICE_LIMITEXCEEDED, 13);
  EXPECT_EQ(SUPLA_RESULTCODE_GUID_ERROR, 14);
  EXPECT_EQ(SUPLA_RESULTCODE_REGISTRATION_DISABLED, 17);
  EXPECT_EQ(SUPLA_RESULTCODE_AUTHKEY_ERROR, 19);
  EXPECT_EQ(SUPLA_RESULTCODE_RESTART_REQUESTED, 42);
  EXPECT_EQ(SUPLA_RESULTCODE_IDENTIFY_REQUESTED, 43);
  EXPECT_EQ(SUPLA_CALCFG_RESULT_NOT_SUPPORTED, 102);
}

// Device builds (SUPLA_DEVICE) limit incoming packets; the gateway must be
// able to receive every server message it handles.
TEST(ProtoContractTest, IncomingPacketsFitDeviceBuffer) {
  EXPECT_LE(sizeof(TSD_SuplaRegisterDeviceResult_B), SUPLA_MAX_DATA_SIZE);
  EXPECT_LE(sizeof(TSD_SuplaChannelNewValue), SUPLA_MAX_DATA_SIZE);
  EXPECT_LE(sizeof(TSD_SuplaChannelGroupNewValue), SUPLA_MAX_DATA_SIZE);
  EXPECT_LE(sizeof(TSD_DeviceCalCfgRequest), SUPLA_MAX_DATA_SIZE);
}
