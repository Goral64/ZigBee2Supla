// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>
#include <proto.h>

#include <cstring>

#include "z2s/channel.h"

using namespace z2s;

TEST(ChannelTest, RelayAndBinaryEncoding) {
  char v[8];
  encodeChannelValue(ChannelKind::Relay, ChannelState{1, NAN}, v);
  EXPECT_EQ(v[0], 1);
  encodeChannelValue(ChannelKind::Relay, ChannelState{0, NAN}, v);
  EXPECT_EQ(v[0], 0);
  encodeChannelValue(ChannelKind::BinarySensor, ChannelState{}, v);
  EXPECT_EQ(v[0], 0);
}

TEST(ChannelTest, ThermometerIsDouble) {
  char v[8];
  encodeChannelValue(ChannelKind::Thermometer, ChannelState{21.5, NAN}, v);
  double t;
  memcpy(&t, v, sizeof(t));
  EXPECT_DOUBLE_EQ(t, 21.5);

  encodeChannelValue(ChannelKind::Thermometer, ChannelState{}, v);
  memcpy(&t, v, sizeof(t));
  EXPECT_DOUBLE_EQ(t, -275.0);
}

TEST(ChannelTest, TempHumidityIsTwoInts) {
  char v[8];
  encodeChannelValue(ChannelKind::TempHumidity, ChannelState{-3.25, 55.5}, v);
  int32_t t, h;
  memcpy(&t, v, 4);
  memcpy(&h, v + 4, 4);
  EXPECT_EQ(t, -3250);
  EXPECT_EQ(h, 55500);

  encodeChannelValue(ChannelKind::Humidity, ChannelState{40, NAN}, v);
  memcpy(&t, v, 4);
  memcpy(&h, v + 4, 4);
  EXPECT_EQ(t, -275000);
  EXPECT_EQ(h, 40000);
}

TEST(ChannelTest, SuplaTypes) {
  EXPECT_EQ(suplaChannelType(ChannelKind::Relay), SUPLA_CHANNELTYPE_RELAY);
  ChannelSpec spec;
  spec.kind = ChannelKind::Relay;
  EXPECT_EQ(suplaFuncList(spec),
            SUPLA_BIT_FUNC_POWERSWITCH | SUPLA_BIT_FUNC_LIGHTSWITCH);
  EXPECT_EQ(suplaChannelFlags(spec), uint64_t{SUPLA_CHANNEL_FLAG_CHANNELSTATE});
  EXPECT_EQ(suplaDefaultFunction(spec), SUPLA_CHANNELFNC_POWERSWITCH);
  spec.defaultFunction = SUPLA_CHANNELFNC_LIGHTSWITCH;
  EXPECT_EQ(suplaDefaultFunction(spec), SUPLA_CHANNELFNC_LIGHTSWITCH);
}

TEST(ChannelTest, MergeKeepsMissingFields) {
  ChannelState s = mergeState(ChannelState{20, 50}, ChannelState{NAN, 60});
  EXPECT_DOUBLE_EQ(s.primary, 20);
  EXPECT_DOUBLE_EQ(s.secondary, 60);
}

TEST(ChannelTest, RelayWithDeviceTimer) {
  ChannelSpec spec;
  spec.kind = ChannelKind::Relay;
  spec.countdownStepMs = 1000;
  spec.countdownMaxMs = 43200000;
  EXPECT_EQ(suplaFuncList(spec), SUPLA_BIT_FUNC_POWERSWITCH |
                                     SUPLA_BIT_FUNC_LIGHTSWITCH |
                                     SUPLA_BIT_FUNC_STAIRCASETIMER);
  EXPECT_EQ(suplaChannelFlags(spec),
            uint64_t{SUPLA_CHANNEL_FLAG_CHANNELSTATE |
                     SUPLA_CHANNEL_FLAG_COUNTDOWN_TIMER_SUPPORTED});

  // The timer is not part of the channel layout stored in identities.json.
  ChannelSpec plain;
  plain.kind = ChannelKind::Relay;
  EXPECT_TRUE(spec == plain);

  // Only relays have it.
  ChannelSpec sensor;
  sensor.kind = ChannelKind::BinarySensor;
  sensor.countdownStepMs = 1000;
  EXPECT_EQ(suplaChannelFlags(sensor),
            uint64_t{SUPLA_CHANNEL_FLAG_CHANNELSTATE});
  EXPECT_EQ(suplaFuncList(sensor), 0);
}

TEST(ChannelTest, ElectricityMeter) {
  ChannelSpec spec;
  spec.kind = ChannelKind::ElectricityMeter;
  EXPECT_EQ(suplaChannelType(spec.kind), SUPLA_CHANNELTYPE_ELECTRICITY_METER);
  EXPECT_EQ(suplaDefaultFunction(spec), SUPLA_CHANNELFNC_ELECTRICITY_METER);
  EXPECT_EQ(suplaFuncList(spec), 0);
  EXPECT_EQ(suplaChannelFlags(spec),
            uint64_t{SUPLA_CHANNEL_FLAG_CHANNELSTATE |
                     SUPLA_CHANNEL_FLAG_PHASE2_UNSUPPORTED |
                     SUPLA_CHANNEL_FLAG_PHASE3_UNSUPPORTED});
  ChannelKind kind;
  ASSERT_TRUE(channelKindFromInt(7, &kind));
  EXPECT_EQ(kind, ChannelKind::ElectricityMeter);

  ChannelState s;
  s.primary = 1234.567;  // kWh
  s.voltage = 231.2;
  char v[8];
  encodeChannelValue(ChannelKind::ElectricityMeter, s, v);
  TElectricityMeter_Value value;
  memcpy(&value, v, sizeof(value));
  EXPECT_EQ(static_cast<uint32_t>(value.total_forward_active_energy), 123457u);
  EXPECT_EQ(value.flags, EM_VALUE_FLAG_PHASE1_ON);

  // High power and current switch to the coarser units.
  s.power = 25000;
  s.current = 70;
  TSuplaChannelExtendedValue ev;
  ASSERT_TRUE(encodeElectricityMeterExtendedValue(s, &ev));
  EXPECT_EQ(ev.type, EV_TYPE_ELECTRICITY_METER_MEASUREMENT_V3);
  TElectricityMeter_ExtendedValue_V3 em = {};
  memcpy(&em, ev.value, ev.size);
  EXPECT_EQ(static_cast<uint64_t>(em.total_forward_active_energy[0]),
            123456700u);
  EXPECT_EQ(em.m_count, 1);
  EXPECT_EQ(em.m[0].voltage[0], 23120);
  EXPECT_EQ(em.m[0].current[0], 7000);
  EXPECT_EQ(em.m[0].power_active[0], 2500000);
  EXPECT_EQ(em.measured_values, EM_VAR_FORWARD_ACTIVE_ENERGY | EM_VAR_VOLTAGE |
                                    EM_VAR_CURRENT_OVER_65A |
                                    EM_VAR_POWER_ACTIVE |
                                    EM_VAR_POWER_ACTIVE_KW);

  // No extended value without the energy counter.
  EXPECT_FALSE(encodeElectricityMeterExtendedValue(ChannelState{}, &ev));
}

TEST(ChannelTest, GeneralPurposeMeasurementIsDouble) {
  ChannelSpec spec;
  spec.kind = ChannelKind::GeneralPurposeMeasurement;
  EXPECT_EQ(suplaChannelType(spec.kind),
            SUPLA_CHANNELTYPE_GENERAL_PURPOSE_MEASUREMENT);
  EXPECT_EQ(suplaDefaultFunction(spec),
            SUPLA_CHANNELFNC_GENERAL_PURPOSE_MEASUREMENT);
  EXPECT_EQ(suplaChannelFlags(spec),
            uint64_t{SUPLA_CHANNEL_FLAG_CHANNELSTATE |
                     SUPLA_CHANNEL_FLAG_RUNTIME_CHANNEL_CONFIG_UPDATE});
  char v[8];
  encodeChannelValue(spec.kind, ChannelState{57.5, NAN}, v);
  double d;
  memcpy(&d, v, sizeof(d));
  EXPECT_DOUBLE_EQ(d, 57.5);
  encodeChannelValue(spec.kind, ChannelState{}, v);
  memcpy(&d, v, sizeof(d));
  EXPECT_TRUE(std::isnan(d));
}

TEST(ChannelTest, MergeKeepsMeterFields) {
  ChannelState a;
  a.primary = 1;
  a.power = 10;
  a.voltage = 230;
  ChannelState b;
  b.power = 20;
  ChannelState s = mergeState(a, b);
  EXPECT_DOUBLE_EQ(s.primary, 1);
  EXPECT_DOUBLE_EQ(s.power, 20);
  EXPECT_DOUBLE_EQ(s.voltage, 230);
  EXPECT_TRUE(std::isnan(s.current));
  EXPECT_FALSE(isEmpty(b));
  EXPECT_TRUE(isEmpty(ChannelState{}));
}

TEST(ChannelTest, ActionTrigger) {
  ChannelSpec spec;
  spec.kind = ChannelKind::ActionTrigger;
  spec.actionCaps = SUPLA_ACTION_CAP_SHORT_PRESS_x1 | SUPLA_ACTION_CAP_HOLD;
  EXPECT_EQ(suplaChannelType(spec.kind), SUPLA_CHANNELTYPE_ACTIONTRIGGER);
  EXPECT_EQ(suplaDefaultFunction(spec), SUPLA_CHANNELFNC_ACTIONTRIGGER);
  // Registered in the FuncList field (ActionTriggerCaps).
  EXPECT_EQ(suplaFuncList(spec),
            SUPLA_ACTION_CAP_SHORT_PRESS_x1 | SUPLA_ACTION_CAP_HOLD);
  ChannelKind kind;
  ASSERT_TRUE(channelKindFromInt(9, &kind));
  EXPECT_EQ(kind, ChannelKind::ActionTrigger);
  char v[8];
  memset(v, 1, sizeof(v));
  encodeChannelValue(spec.kind, ChannelState{}, v);
  for (char c : v) EXPECT_EQ(c, 0);
}
