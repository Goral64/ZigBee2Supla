// SPDX-License-Identifier: GPL-2.0-or-later
// End-to-end tests of SuplaSession and Gateway against an in-memory Supla
// server speaking the real SRPC protocol.

#include <gtest/gtest.h>
#include <proto.h>
#include <unistd.h>

#include <cstring>
#include <set>

#include "fake_supla_server.h"
#include "z2s/gateway.h"
#include "z2s/supla_session.h"

using namespace z2s;
using namespace z2s::test;

namespace {

SuplaServerConfig serverConfig() {
  SuplaServerConfig c;
  c.server = "svr1.supla.org";
  c.email = "user@example.com";
  c.activityTimeoutS = 120;
  return c;
}

DeviceIdentity makeIdentity(const std::string &id, uint8_t seed) {
  DeviceIdentity identity;
  identity.id = id;
  for (int i = 0; i < 16; i++) {
    identity.guid[i] = static_cast<uint8_t>(seed + i);
    identity.authKey[i] = static_cast<uint8_t>(0x80 + seed + i);
  }
  ChannelSpec relay;
  relay.kind = ChannelKind::Relay;
  relay.key = "state";
  relay.defaultFunction = SUPLA_CHANNELFNC_LIGHTSWITCH;
  ChannelSpec thermo;
  thermo.kind = ChannelKind::Thermometer;
  thermo.key = "temperature";
  identity.channels = {relay, thermo};
  return identity;
}

DeviceDescriptor makeDescriptor(const DeviceIdentity &identity,
                                const std::string &name) {
  DeviceDescriptor d;
  d.id = identity.id;
  d.name = name;
  d.channels = identity.channels;
  for (auto &c : d.channels) {
    c.caption = c.kind == ChannelKind::Relay ? "światło" : "termometr";
  }
  return d;
}

class SessionTest : public ::testing::Test {
 protected:
  SessionTest() : factory_(&server_), session_(serverConfig(), &factory_) {
    identity_ = makeIdentity("0x0000000000000001", 1);
    session_.configure(identity_, makeDescriptor(identity_, "Lampa"));
    session_.setChannelPresent(0, true);
    session_.setChannelPresent(1, true);
  }

  // Runs the session and the server for the given number of 10 ms steps.
  void run(int steps) {
    for (int i = 0; i < steps; i++) {
      now_ += 10;
      if (session_.wantsToConnect(now_)) session_.startConnect(now_);
      session_.iterate(now_);
      server_.iterate();
    }
  }

  FakeConnection &conn() { return *server_.connections.back(); }

  FakeSuplaServer server_;
  FakeTransportFactory factory_;
  SuplaSession session_;
  DeviceIdentity identity_;
  uint64_t now_ = 1000;
};

}  // namespace

TEST_F(SessionTest, RegistersAsSeparateDevice) {
  session_.setChannelState(0, ChannelState{1, NAN});
  session_.setChannelState(1, ChannelState{21.5, NAN});
  run(5);

  ASSERT_TRUE(session_.isRegistered());
  ASSERT_EQ(conn().registrations.size(), 1u);
  const Registration &reg = conn().registrations[0];
  EXPECT_EQ(reg.email, "user@example.com");
  EXPECT_EQ(reg.name, "Lampa");
  EXPECT_EQ(reg.serverName, "svr1.supla.org");
  EXPECT_EQ(reg.guid, identity_.guid);
  EXPECT_EQ(reg.authKey, identity_.authKey);
  ASSERT_EQ(reg.channels.size(), 2u);

  EXPECT_EQ(reg.channels[0].Number, 0);
  EXPECT_EQ(reg.channels[0].Type, SUPLA_CHANNELTYPE_RELAY);
  EXPECT_EQ(reg.channels[0].Default, SUPLA_CHANNELFNC_LIGHTSWITCH);
  EXPECT_EQ(reg.channels[0].value[0], 1);
  EXPECT_EQ(reg.channels[0].Offline, SUPLA_CHANNEL_OFFLINE_FLAG_ONLINE);

  EXPECT_EQ(reg.channels[1].Type, SUPLA_CHANNELTYPE_THERMOMETER);
  double t;
  memcpy(&t, reg.channels[1].value, sizeof(t));
  EXPECT_DOUBLE_EQ(t, 21.5);

  // Server proposed the requested timeout, so no change request is sent.
  EXPECT_EQ(conn().activityTimeoutRequests, 0);
}

TEST_F(SessionTest, ChannelsWithoutDataAreOffline) {
  run(5);
  ASSERT_TRUE(session_.isRegistered());
  const Registration &reg = conn().registrations[0];
  EXPECT_EQ(reg.channels[0].Offline, SUPLA_CHANNEL_OFFLINE_FLAG_OFFLINE);
  EXPECT_EQ(reg.channels[1].Offline, SUPLA_CHANNEL_OFFLINE_FLAG_OFFLINE);

  session_.setChannelState(1, ChannelState{19.0, NAN});
  run(3);
  ASSERT_EQ(conn().values.size(), 1u);
  EXPECT_EQ(conn().values[0].channel, 1);
  EXPECT_EQ(conn().values[0].offline, SUPLA_CHANNEL_OFFLINE_FLAG_ONLINE);
}

TEST_F(SessionTest, PushesOnlyChangedValues) {
  session_.setChannelState(0, ChannelState{0, NAN});
  session_.setChannelState(1, ChannelState{20.0, NAN});
  run(5);
  ASSERT_TRUE(session_.isRegistered());
  EXPECT_TRUE(conn().values.empty());

  session_.setChannelState(1, ChannelState{20.0, NAN});
  run(3);
  EXPECT_TRUE(conn().values.empty());

  session_.setChannelState(1, ChannelState{22.25, NAN});
  run(3);
  ASSERT_EQ(conn().values.size(), 1u);
  double t;
  memcpy(&t, conn().values[0].value, sizeof(t));
  EXPECT_DOUBLE_EQ(t, 22.25);

  session_.setDeviceOnline(false);
  run(3);
  ASSERT_EQ(conn().values.size(), 3u);
  EXPECT_EQ(conn().values[1].offline, SUPLA_CHANNEL_OFFLINE_FLAG_OFFLINE);
  EXPECT_EQ(conn().values[2].offline, SUPLA_CHANNEL_OFFLINE_FLAG_OFFLINE);
}

TEST_F(SessionTest, RelayCommandFromServerReachesHandler) {
  std::vector<std::pair<int, ChannelCommand::Type>> commands;
  session_.setCommandHandler([&](int ch, const ChannelCommand &cmd) {
    commands.push_back({ch, cmd.type});
    return true;
  });
  session_.setChannelState(0, ChannelState{0, NAN});
  run(5);
  ASSERT_TRUE(session_.isRegistered());

  server_.sendSetValue(&conn(), 0, 77, true);
  run(3);
  ASSERT_EQ(commands.size(), 1u);
  EXPECT_EQ(commands[0].first, 0);
  EXPECT_EQ(commands[0].second, ChannelCommand::Type::TurnOn);
  ASSERT_EQ(conn().setResults.size(), 1u);
  EXPECT_EQ(conn().setResults[0].channel, 0);
  EXPECT_EQ(conn().setResults[0].senderId, 77);
  EXPECT_EQ(conn().setResults[0].success, 1);

  // Thermometer channel cannot be controlled.
  server_.sendSetValue(&conn(), 1, 78, true);
  run(3);
  EXPECT_EQ(commands.size(), 1u);
  ASSERT_EQ(conn().setResults.size(), 2u);
  EXPECT_EQ(conn().setResults[1].success, 0);
}

TEST_F(SessionTest, RegistrationDisabledIsRetriedLater) {
  server_.registerResultCode = SUPLA_RESULTCODE_REGISTRATION_DISABLED;
  run(5);
  EXPECT_FALSE(session_.isRegistered());
  EXPECT_EQ(session_.lastResultCode(), SUPLA_RESULTCODE_REGISTRATION_DISABLED);
  EXPECT_EQ(server_.acceptedCount, 1);

  run(100);  // 1 s later: still waiting
  EXPECT_EQ(server_.acceptedCount, 1);

  server_.registerResultCode = SUPLA_RESULTCODE_TRUE;
  run(8000);  // 80 s later: retried and registered
  EXPECT_EQ(server_.acceptedCount, 2);
  EXPECT_TRUE(session_.isRegistered());
}

TEST_F(SessionTest, ReconnectsWhenServerClosesConnection) {
  run(5);
  ASSERT_TRUE(session_.isRegistered());
  server_.closeAll();
  run(2);
  EXPECT_FALSE(session_.isRegistered());
  run(1000);  // 10 s, first backoff is 5-6.25 s
  EXPECT_EQ(server_.acceptedCount, 2);
  EXPECT_TRUE(session_.isRegistered());
}

TEST_F(SessionTest, PingsWhenIdle) {
  run(5);
  ASSERT_TRUE(session_.isRegistered());
  run(12000);  // 120 s
  EXPECT_GE(conn().pings, 1);
  EXPECT_TRUE(session_.isRegistered());
}

TEST_F(SessionTest, ReRegistersWhenLayoutChanges) {
  run(5);
  ASSERT_TRUE(session_.isRegistered());
  ChannelSpec contact;
  contact.kind = ChannelKind::BinarySensor;
  contact.key = "contact";
  identity_.channels.push_back(contact);
  session_.configure(identity_, makeDescriptor(identity_, "Lampa"));
  run(5);
  EXPECT_EQ(server_.acceptedCount, 2);
  ASSERT_TRUE(session_.isRegistered());
  EXPECT_EQ(conn().registrations[0].channels.size(), 3u);
}

TEST_F(SessionTest, TurnOnWithDeviceTimer) {
  std::vector<ChannelCommand> commands;
  session_.setCommandHandler([&](int, const ChannelCommand &cmd) {
    commands.push_back(cmd);
    return true;
  });
  // Like a watering timer: whole minutes, up to 599 min.
  session_.setChannelCountdown(0, 60000, 599 * 60000);
  session_.setChannelState(0, ChannelState{0, NAN});
  run(5);
  ASSERT_TRUE(session_.isRegistered());
  const auto &relayReg = conn().registrations[0].channels[0];
  EXPECT_TRUE(relayReg.Flags & SUPLA_CHANNEL_FLAG_COUNTDOWN_TIMER_SUPPORTED);
  EXPECT_TRUE(relayReg.FuncList & SUPLA_BIT_FUNC_STAIRCASETIMER);
  // Protocol structs are packed: EXPECT_EQ takes its arguments by reference,
  // and an unaligned 64-bit reference faults on ARMv7 (Bus error), so fields
  // are copied first.
  EXPECT_EQ(static_cast<int64_t>(conn().registrations[0].channels[1].Flags),
            SUPLA_CHANNEL_FLAG_CHANNELSTATE);

  // 1.5 min is rounded up to whole minutes of the device timer.
  server_.sendSetValue(&conn(), 0, 7, true, 90000);
  run(3);
  ASSERT_EQ(commands.size(), 1u);
  EXPECT_EQ(commands[0].type, ChannelCommand::Type::TurnOn);
  EXPECT_EQ(commands[0].durationMs, 120000u);
  ASSERT_EQ(conn().setResults.size(), 1u);
  EXPECT_EQ(conn().setResults[0].success, 1);
  ASSERT_EQ(conn().timers.size(), 1u);
  EXPECT_EQ(conn().timers[0].channel, 0);
  EXPECT_NEAR(conn().timers[0].remainingMs, 120000, 100);
  EXPECT_EQ(conn().timers[0].targetValue, 0);
  EXPECT_EQ(conn().timers[0].senderId, 7);

  // The device turns on, then off by itself: the timer is reported as over.
  session_.setChannelState(0, ChannelState{1, NAN});
  run(3);
  EXPECT_EQ(conn().timers.size(), 1u);
  session_.setChannelState(0, ChannelState{0, NAN});
  run(3);
  ASSERT_EQ(conn().timers.size(), 2u);
  EXPECT_EQ(conn().timers[1].remainingMs, 0u);

  // Longer than the device can do: rejected, nothing sent to the device.
  server_.sendSetValue(&conn(), 0, 8, true, 600 * 60000);
  run(3);
  EXPECT_EQ(commands.size(), 1u);
  ASSERT_EQ(conn().setResults.size(), 2u);
  EXPECT_EQ(conn().setResults[1].success, 0);

  // Switched off before the end: the timer is reported as cancelled.
  server_.sendSetValue(&conn(), 0, 9, true, 60000);
  run(3);
  server_.sendSetValue(&conn(), 0, 9, false);
  run(3);
  ASSERT_EQ(commands.size(), 3u);
  EXPECT_EQ(commands[2].type, ChannelCommand::Type::TurnOff);
  ASSERT_EQ(conn().timers.size(), 4u);
  EXPECT_EQ(conn().timers[3].remainingMs, 0u);
}

TEST_F(SessionTest, TimerRequestWithoutDeviceTimerIsRejected) {
  int calls = 0;
  session_.setCommandHandler([&](int, const ChannelCommand &cmd) {
    calls++;
    EXPECT_EQ(cmd.durationMs, 0u);
    return true;
  });
  session_.setChannelState(0, ChannelState{0, NAN});
  run(5);
  ASSERT_TRUE(session_.isRegistered());
  const auto &relayReg = conn().registrations[0].channels[0];
  EXPECT_EQ(static_cast<int64_t>(relayReg.Flags),
            SUPLA_CHANNEL_FLAG_CHANNELSTATE);
  EXPECT_FALSE(relayReg.FuncList & SUPLA_BIT_FUNC_STAIRCASETIMER);

  // Without a timer in the device the relay would stay on: rejected.
  server_.sendSetValue(&conn(), 0, 5, true, 5000);
  run(3);
  EXPECT_EQ(calls, 0);
  ASSERT_EQ(conn().setResults.size(), 1u);
  EXPECT_EQ(conn().setResults[0].success, 0);
  EXPECT_TRUE(conn().timers.empty());

  server_.sendSetValue(&conn(), 0, 6, true);
  run(3);
  EXPECT_EQ(calls, 1);
}

TEST_F(SessionTest, TimerSupportChangeRegistersAgain) {
  session_.setChannelState(0, ChannelState{0, NAN});
  run(5);
  ASSERT_TRUE(session_.isRegistered());
  ASSERT_EQ(server_.connections.size(), 1u);

  session_.setChannelCountdown(0, 1000, 43200000);
  run(20);
  ASSERT_TRUE(session_.isRegistered());
  ASSERT_EQ(server_.connections.size(), 2u);
  EXPECT_TRUE(conn().registrations[0].channels[0].Flags &
              SUPLA_CHANNEL_FLAG_COUNTDOWN_TIMER_SUPPORTED);

  // The same capability again changes nothing.
  session_.setChannelCountdown(0, 1000, 43200000);
  run(20);
  EXPECT_EQ(server_.connections.size(), 2u);
}

TEST_F(SessionTest, SendsInitialCaptionsAfterRegistration) {
  session_.setChannelState(0, ChannelState{0, NAN});
  run(5);
  ASSERT_TRUE(session_.isRegistered());
  ASSERT_EQ(conn().captions.size(), 2u);
  EXPECT_EQ(conn().captions[0].channel, 0);
  EXPECT_EQ(conn().captions[0].caption, "Lampa – światło");
  EXPECT_EQ(conn().captions[1].channel, 1);
  EXPECT_EQ(conn().captions[1].caption, "Lampa – termometr");

  // Sent once per registration; after a rename the device registers again
  // and sends the new captions (the server applies them only to channels
  // without a caption).
  run(50);
  EXPECT_EQ(conn().captions.size(), 2u);
  DeviceIdentity renamed = identity_;
  session_.configure(identity_, makeDescriptor(identity_, "Kuchnia"));
  run(20);
  ASSERT_TRUE(session_.isRegistered());
  ASSERT_EQ(conn().captions.size(), 2u);
  EXPECT_EQ(conn().captions[0].caption, "Kuchnia – światło");
}

namespace {

// A plug: relay + electricity meter.
void configurePlug(SuplaSession *session, DeviceIdentity *identity) {
  ChannelSpec relay;
  relay.kind = ChannelKind::Relay;
  relay.key = "state";
  ChannelSpec meter;
  meter.kind = ChannelKind::ElectricityMeter;
  meter.key = "energy";
  identity->channels = {relay, meter};
  session->configure(*identity, makeDescriptor(*identity, "Gniazdko"));
  session->setChannelPresent(0, true);
  session->setChannelPresent(1, true);
}

ChannelState meterState(double energy, double power, double voltage,
                        double current) {
  ChannelState s;
  s.primary = energy;
  s.power = power;
  s.voltage = voltage;
  s.current = current;
  return s;
}

}  // namespace

TEST_F(SessionTest, ElectricityMeterSendsCounterAndMeasurements) {
  configurePlug(&session_, &identity_);
  session_.setChannelState(0, ChannelState{1, NAN});
  // Without the energy counter the meter is offline: a zero would end up in
  // the history.
  session_.setChannelState(1, meterState(NAN, 57.3, NAN, NAN));
  run(5);
  ASSERT_TRUE(session_.isRegistered());
  const auto &channel = conn().registrations[0].channels[1];
  EXPECT_EQ(channel.Type, SUPLA_CHANNELTYPE_ELECTRICITY_METER);
  EXPECT_EQ(channel.Default, SUPLA_CHANNELFNC_ELECTRICITY_METER);
  EXPECT_EQ(channel.FuncList, 0);
  EXPECT_EQ(static_cast<int64_t>(channel.Flags),
            SUPLA_CHANNEL_FLAG_CHANNELSTATE |
                SUPLA_CHANNEL_FLAG_PHASE2_UNSUPPORTED |
                SUPLA_CHANNEL_FLAG_PHASE3_UNSUPPORTED);
  EXPECT_EQ(channel.Offline, SUPLA_CHANNEL_OFFLINE_FLAG_OFFLINE);
  EXPECT_TRUE(conn().meters.empty());

  session_.setChannelState(1, meterState(1.23456, NAN, 230.5, 0.25));
  run(3);
  ASSERT_EQ(conn().values.size(), 1u);
  EXPECT_EQ(conn().values[0].channel, 1);
  EXPECT_EQ(conn().values[0].offline, SUPLA_CHANNEL_OFFLINE_FLAG_ONLINE);
  TElectricityMeter_Value value;
  memcpy(&value, conn().values[0].value, sizeof(value));
  EXPECT_EQ(static_cast<uint32_t>(value.total_forward_active_energy),
            123u);  // 0.01 kWh
  EXPECT_EQ(value.flags, EM_VALUE_FLAG_PHASE1_ON);

  ASSERT_EQ(conn().meters.size(), 1u);
  const MeterReport &m = conn().meters[0];
  EXPECT_EQ(m.channel, 1);
  EXPECT_EQ(m.energy, 123456u);
  EXPECT_EQ(m.measuredValues, EM_VAR_FORWARD_ACTIVE_ENERGY | EM_VAR_VOLTAGE |
                                  EM_VAR_CURRENT | EM_VAR_POWER_ACTIVE);
  EXPECT_EQ(m.voltage, 23050);
  EXPECT_EQ(m.current, 250);
  EXPECT_EQ(m.power, 5730000);
}

TEST_F(SessionTest, ElectricityMeterMeasurementsAreThrottled) {
  configurePlug(&session_, &identity_);
  session_.setChannelState(1, meterState(10, 100, 230, 0.5));
  run(5);
  ASSERT_TRUE(session_.isRegistered());
  ASSERT_EQ(conn().meters.size(), 1u);

  // Power changes often; only the latest value is sent after 5 s.
  session_.setChannelState(1, meterState(NAN, 110, NAN, NAN));
  run(10);
  session_.setChannelState(1, meterState(NAN, 120, NAN, NAN));
  run(10);
  EXPECT_EQ(conn().meters.size(), 1u);
  run(500);
  ASSERT_EQ(conn().meters.size(), 2u);
  EXPECT_EQ(conn().meters[1].power, 12000000);

  // A new energy reading is sent at once.
  session_.setChannelState(1, meterState(10.01, NAN, NAN, NAN));
  run(2);
  ASSERT_EQ(conn().meters.size(), 3u);
  EXPECT_EQ(conn().meters[2].energy, 1001000u);

  // After reconnecting, the meter is sent again.
  server_.closeAll();
  run(1000);
  ASSERT_TRUE(session_.isRegistered());
  EXPECT_EQ(conn().meters.size(), 1u);
}

TEST_F(SessionTest, RelayDeclaresItsMeter) {
  configurePlug(&session_, &identity_);
  run(5);
  ASSERT_TRUE(session_.isRegistered());
  ASSERT_EQ(conn().configs.size(), 1u);
  const ChannelConfigRequest &c = conn().configs[0];
  EXPECT_EQ(c.channel, 0);
  EXPECT_EQ(c.func, SUPLA_CHANNELFNC_POWERSWITCH);
  EXPECT_EQ(c.configType, SUPLA_CONFIG_TYPE_DEFAULT);
  EXPECT_EQ(c.powerSwitch.DefaultRelatedMeterIsSet, 1);
  EXPECT_EQ(c.powerSwitch.DefaultRelatedMeterChannelNo, 1);

  // Once is enough: the server keeps it. A rejection is only logged.
  server_.channelConfigResult = SUPLA_CONFIG_RESULT_FALSE;
  server_.closeAll();
  run(1000);
  ASSERT_TRUE(session_.isRegistered());
  EXPECT_TRUE(conn().configs.empty());
}

TEST_F(SessionTest, DeviceWithoutMeterSendsNoConfig) {
  run(5);
  ASSERT_TRUE(session_.isRegistered());
  EXPECT_TRUE(conn().configs.empty());
}

TEST_F(SessionTest, BatteryIsSentInChannelState) {
  DeviceDescriptor descriptor = makeDescriptor(identity_, "Lampa");
  descriptor.batteryPowered = 1;
  session_.configure(identity_, descriptor);
  DeviceHealth health;
  health.batteryLevel = 87;
  health.linkQuality = 40;
  session_.setDeviceHealth(health);
  run(5);
  ASSERT_TRUE(session_.isRegistered());
  // Every channel shows the battery of the device.
  ASSERT_EQ(conn().channelStates.size(), 2u);
  for (int i = 0; i < 2; i++) {
    const ChannelStateReport &r = conn().channelStates[i];
    EXPECT_EQ(r.channel, i);
    EXPECT_EQ(r.receiverId, 0);
    EXPECT_EQ(static_cast<int>(r.state.Fields),
              SUPLA_CHANNELSTATE_FIELD_BATTERYPOWERED |
                  SUPLA_CHANNELSTATE_FIELD_BATTERYLEVEL |
                  SUPLA_CHANNELSTATE_FIELD_BRIDGENODESIGNALSTRENGTH |
                  SUPLA_CHANNELSTATE_FIELD_BRIDGENODEONLINE);
    EXPECT_EQ(r.state.BatteryPowered, 1);
    EXPECT_EQ(r.state.BatteryLevel, 87);
    EXPECT_EQ(r.state.BridgeNodeSignalStrength, 40);
    EXPECT_EQ(r.state.BridgeNodeOnline, 1);
  }

  // Link quality changes with almost every message: not sent on its own.
  DeviceHealth lqi;
  lqi.linkQuality = 70;
  session_.setDeviceHealth(lqi);
  run(3);
  EXPECT_EQ(conn().channelStates.size(), 2u);

  // A battery change is.
  DeviceHealth battery;
  battery.batteryLevel = 86;
  session_.setDeviceHealth(battery);
  run(3);
  ASSERT_EQ(conn().channelStates.size(), 4u);
  EXPECT_EQ(conn().channelStates[3].state.BatteryLevel, 86);
  EXPECT_EQ(conn().channelStates[3].state.BridgeNodeSignalStrength, 70);

  // Channel information requested in the app: answered to the client.
  server_.sendGetChannelState(&conn(), 1, 1234);
  run(3);
  ASSERT_EQ(conn().channelStates.size(), 5u);
  EXPECT_EQ(conn().channelStates[4].channel, 1);
  EXPECT_EQ(conn().channelStates[4].receiverId, 1234);
}

TEST_F(SessionTest, BatteryLowWithoutLevel) {
  DeviceHealth health;
  health.batteryLow = 1;
  session_.setDeviceHealth(health);
  run(5);
  ASSERT_TRUE(session_.isRegistered());
  ASSERT_EQ(conn().channelStates.size(), 2u);
  const auto &state = conn().channelStates[0].state;
  EXPECT_EQ(static_cast<int>(state.Fields),
            SUPLA_CHANNELSTATE_FIELD_BATTERY_STATE |
                SUPLA_CHANNELSTATE_FIELD_BRIDGENODEONLINE);
  EXPECT_EQ(state.BatteryState, SUPLA_BATTERY_STATE_LOW);
}

TEST_F(SessionTest, MainsDeviceSendsStateOnlyOnRequest) {
  DeviceHealth health;
  health.linkQuality = 55;
  session_.setDeviceHealth(health);
  run(5);
  ASSERT_TRUE(session_.isRegistered());
  EXPECT_TRUE(conn().channelStates.empty());

  server_.sendGetChannelState(&conn(), 0, 99);
  run(3);
  ASSERT_EQ(conn().channelStates.size(), 1u);
  EXPECT_EQ(static_cast<int>(conn().channelStates[0].state.Fields),
            SUPLA_CHANNELSTATE_FIELD_BRIDGENODESIGNALSTRENGTH |
                SUPLA_CHANNELSTATE_FIELD_BRIDGENODEONLINE);
  EXPECT_EQ(conn().channelStates[0].state.BridgeNodeSignalStrength, 55);
}

namespace {

// A light sensor: one general purpose measurement channel in lux.
void configureLightSensor(SuplaSession *session, DeviceIdentity *identity) {
  ChannelSpec lux;
  lux.kind = ChannelKind::GeneralPurposeMeasurement;
  lux.key = "illuminance";
  identity->channels = {lux};
  DeviceDescriptor descriptor = makeDescriptor(*identity, "Czujnik");
  descriptor.channels[0].unit = "lx";
  descriptor.channels[0].precision = 0;
  session->configure(*identity, descriptor);
  session->setChannelPresent(0, true);
}

}  // namespace

TEST_F(SessionTest, MeasurementGetsDefaultUnit) {
  configureLightSensor(&session_, &identity_);
  run(10);
  ASSERT_TRUE(session_.isRegistered());
  EXPECT_TRUE(static_cast<int64_t>(conn().registrations[0].channels[0].Flags) &
              SUPLA_CHANNEL_FLAG_RUNTIME_CHANNEL_CONFIG_UPDATE);
  // A new channel: the defaults are also the user settings, with history.
  ASSERT_EQ(conn().configs.size(), 1u);
  const auto &gpm = conn().configs[0].gpm;
  EXPECT_EQ(conn().configs[0].func,
            SUPLA_CHANNELFNC_GENERAL_PURPOSE_MEASUREMENT);
  EXPECT_STREQ(gpm.DefaultUnitAfterValue, "lx");
  EXPECT_EQ(gpm.DefaultValuePrecision, 0);
  EXPECT_STREQ(gpm.UnitAfterValue, "lx");
  EXPECT_EQ(gpm.KeepHistory, 1);
  // Every config from the server is answered; the one sent back after the
  // change already has the defaults, so nothing more is sent.
  run(50);
  EXPECT_EQ(conn().configs.size(), 1u);
  ASSERT_EQ(conn().configResults.size(), 2u);
  EXPECT_EQ(conn().configResults[1].result, SUPLA_CONFIG_RESULT_TRUE);

  // After reconnecting the server already has the defaults.
  server_.closeAll();
  run(1000);
  ASSERT_TRUE(session_.isRegistered());
  EXPECT_TRUE(conn().configs.empty());
  EXPECT_EQ(conn().configResults.size(), 1u);
}

TEST_F(SessionTest, MeasurementKeepsUserSettings) {
  // Set by the user in Cloud before the gateway knew the unit.
  TChannelConfig_GeneralPurposeMeasurement user = {};
  user.ValueDivider = 1000;
  user.ValuePrecision = 2;
  strncpy(user.UnitAfterValue, "klx", sizeof(user.UnitAfterValue));
  server_.gpmConfigs[0] = user;
  configureLightSensor(&session_, &identity_);
  run(10);
  ASSERT_TRUE(session_.isRegistered());
  ASSERT_EQ(conn().configs.size(), 1u);
  const auto &gpm = conn().configs[0].gpm;
  EXPECT_STREQ(gpm.DefaultUnitAfterValue, "lx");
  EXPECT_EQ(static_cast<int>(gpm.ValueDivider), 1000);
  EXPECT_EQ(gpm.ValuePrecision, 2);
  EXPECT_STREQ(gpm.UnitAfterValue, "klx");
  EXPECT_EQ(gpm.KeepHistory, 0);
}

TEST_F(SessionTest, MeasurementConfigIsNotRetriedForever) {
  server_.channelConfigResult = SUPLA_CONFIG_RESULT_FALSE;
  configureLightSensor(&session_, &identity_);
  run(100);
  ASSERT_TRUE(session_.isRegistered());
  EXPECT_EQ(conn().configs.size(), 3u);
}

TEST_F(SessionTest, ButtonReportsActions) {
  ChannelSpec button;
  button.kind = ChannelKind::ActionTrigger;
  button.key = "action";
  identity_.channels = {button};
  session_.configure(identity_, makeDescriptor(identity_, "Przycisk"));
  session_.setChannelPresent(0, true);
  const uint32_t caps =
      SUPLA_ACTION_CAP_SHORT_PRESS_x1 | SUPLA_ACTION_CAP_SHORT_PRESS_x2;
  session_.setChannelActionCaps(0, caps);

  // Pressed before the connection: dropped, it would come too late.
  session_.triggerAction(0, SUPLA_ACTION_CAP_SHORT_PRESS_x1);
  run(5);
  ASSERT_TRUE(session_.isRegistered());
  const auto &channel = conn().registrations[0].channels[0];
  EXPECT_EQ(channel.Type, SUPLA_CHANNELTYPE_ACTIONTRIGGER);
  EXPECT_EQ(channel.Default, SUPLA_CHANNELFNC_ACTIONTRIGGER);
  EXPECT_EQ(static_cast<uint32_t>(channel.ActionTriggerCaps), caps);
  // No state of its own: online as long as the device is.
  EXPECT_EQ(channel.Offline, SUPLA_CHANNEL_OFFLINE_FLAG_ONLINE);
  EXPECT_TRUE(conn().actions.empty());

  session_.triggerAction(0, SUPLA_ACTION_CAP_SHORT_PRESS_x2);
  session_.triggerAction(0, SUPLA_ACTION_CAP_HOLD);  // not supported
  run(3);
  ASSERT_EQ(conn().actions.size(), 1u);
  EXPECT_EQ(conn().actions[0].channel, 0);
  EXPECT_EQ(conn().actions[0].action, SUPLA_ACTION_CAP_SHORT_PRESS_x2);

  // New actions (e.g. after a zigbee2mqtt update): registered again.
  session_.setChannelActionCaps(0, caps | SUPLA_ACTION_CAP_HOLD);
  run(5);
  EXPECT_EQ(server_.acceptedCount, 2);
  ASSERT_TRUE(session_.isRegistered());
  EXPECT_EQ(static_cast<uint32_t>(
                conn().registrations[0].channels[0].ActionTriggerCaps),
            caps | SUPLA_ACTION_CAP_HOLD);
}

namespace {

// A radiator head: thermometer + thermostat.
void configureThermostat(SuplaSession *session, DeviceIdentity *identity) {
  ChannelSpec thermometer;
  thermometer.kind = ChannelKind::Thermometer;
  thermometer.key = "local_temperature";
  ChannelSpec thermostat;
  thermostat.kind = ChannelKind::Thermostat;
  thermostat.key = "current_heating_setpoint";
  identity->channels = {thermometer, thermostat};
  DeviceDescriptor descriptor = makeDescriptor(*identity, "Glowica");
  descriptor.channels[1].thermometerKey = "local_temperature";
  descriptor.channels[1].setpointMin = 5;
  descriptor.channels[1].setpointMax = 35;
  descriptor.channels[1].setpointStep = 0.5;
  session->configure(*identity, descriptor);
  session->setChannelPresent(0, true);
  session->setChannelPresent(1, true);
}

ChannelState thermostatState(double setpoint, double mode, double heating,
                             double valve) {
  ChannelState s;
  s.primary = setpoint;
  s.mode = mode;
  s.heating = heating;
  s.valve = valve;
  return s;
}

void sendHvacValue(FakeSuplaServer *server, FakeConnection *conn, int channel,
                   unsigned char mode, double setpoint) {
  THVACValue hvac = {};
  hvac.Mode = mode;
  if (!std::isnan(setpoint)) {
    hvac.SetpointTemperatureHeat =
        static_cast<_supla_int16_t>(std::lround(setpoint * 100));
    hvac.Flags = SUPLA_HVAC_VALUE_FLAG_SETPOINT_TEMP_HEAT_SET;
  }
  char value[8];
  memcpy(value, &hvac, sizeof(hvac));
  server->sendSetValueRaw(conn, channel, 31, value);
}

}  // namespace

TEST_F(SessionTest, ThermostatValueAndConfig) {
  configureThermostat(&session_, &identity_);
  session_.setChannelState(0, ChannelState{20.5, NAN});
  session_.setChannelState(1, thermostatState(21.5, 1, 1, 40));
  run(10);
  ASSERT_TRUE(session_.isRegistered());
  const auto &reg = conn().registrations[0].channels[1];
  EXPECT_EQ(reg.Type, SUPLA_CHANNELTYPE_HVAC);
  EXPECT_EQ(reg.Default, SUPLA_CHANNELFNC_HVAC_THERMOSTAT);
  EXPECT_EQ(reg.FuncList, SUPLA_BIT_FUNC_HVAC_THERMOSTAT);
  EXPECT_TRUE(static_cast<int64_t>(reg.Flags) &
              SUPLA_CHANNEL_FLAG_RUNTIME_CHANNEL_CONFIG_UPDATE);
  // Supla apps need a weekly schedule to show the thermostat.
  EXPECT_TRUE(static_cast<int64_t>(reg.Flags) &
              SUPLA_CHANNEL_FLAG_WEEKLY_SCHEDULE);
  THVACValue value;
  memcpy(&value, reg.value, sizeof(value));
  EXPECT_EQ(value.Mode, SUPLA_HVAC_MODE_HEAT);
  EXPECT_EQ(value.IsOn, 42);  // valve 40 %
  EXPECT_EQ(static_cast<int>(value.SetpointTemperatureHeat), 2150);
  EXPECT_EQ(static_cast<int>(value.Flags),
            SUPLA_HVAC_VALUE_FLAG_SETPOINT_TEMP_HEAT_SET |
                SUPLA_HVAC_VALUE_FLAG_HEATING);

  // The server had no config: the device part is sent once, and a default
  // weekly schedule.
  ASSERT_EQ(conn().configs.size(), 2u);
  EXPECT_EQ(conn().configs[1].configType, SUPLA_CONFIG_TYPE_WEEKLY_SCHEDULE);
  ASSERT_EQ(server_.schedules.count(1), 1u);
  const TChannelConfig_WeeklySchedule &schedule = server_.schedules[1];
  EXPECT_EQ(schedule.Program[0].Mode, SUPLA_HVAC_MODE_HEAT);
  EXPECT_EQ(static_cast<int>(schedule.Program[1].SetpointTemperatureHeat),
            2100);
  EXPECT_EQ(schedule.Quarters[0], 0x11);       // Sunday 0:00, program 1
  EXPECT_EQ(schedule.Quarters[6 * 2], 0x22);   // Sunday 6:00, program 2
  EXPECT_EQ(schedule.Quarters[21 * 2], 0x11);  // Sunday 21:00, program 1
  // The schedule from the server is accepted.
  EXPECT_FALSE(conn().configResults.empty());
  for (const auto &r : conn().configResults) {
    EXPECT_EQ(r.result, SUPLA_CONFIG_RESULT_TRUE);
  }
  const TChannelConfig_HVAC &c = conn().configs[0].hvac;
  EXPECT_EQ(conn().configs[0].channel, 1);
  EXPECT_EQ(c.MainThermometerChannelNo, 0);
  // Relations not set point to the channel itself, as the server stores
  // them (otherwise the config would be sent again and again).
  EXPECT_EQ(c.MasterThermostatIsSet, 0);
  EXPECT_EQ(c.MasterThermostatChannelNo, 1);
  EXPECT_EQ(c.PumpSwitchChannelNo, 1);
  EXPECT_EQ(c.Subfunction, SUPLA_HVAC_SUBFUNCTION_HEAT);
  EXPECT_EQ(
      static_cast<unsigned>(c.ParameterFlags.MainThermometerChannelNoReadonly),
      1u);
  EXPECT_EQ(static_cast<unsigned>(c.ParameterFlags.TemperaturesEcoHidden), 1u);
  EXPECT_TRUE(c.Temperatures.Index & TEMPERATURE_ROOM_MIN);
  EXPECT_EQ(static_cast<int>(c.Temperatures.Temperature[10]), 500);
  EXPECT_EQ(static_cast<int>(c.Temperatures.Temperature[11]), 3500);
  run(50);
  EXPECT_EQ(conn().configs.size(), 2u);

  // A user setting is kept: nothing to fix after reconnecting.
  server_.hvacConfigs[1].TemperatureSetpointChangeSwitchesToManualMode = 1;
  server_.closeAll();
  run(1000);
  ASSERT_TRUE(session_.isRegistered());
  EXPECT_TRUE(conn().configs.empty());
}

TEST_F(SessionTest, ThermostatCommands) {
  std::vector<ChannelCommand> commands;
  session_.setCommandHandler([&](int, const ChannelCommand &cmd) {
    commands.push_back(cmd);
    return true;
  });
  configureThermostat(&session_, &identity_);
  ChannelState initial = thermostatState(20, 1, 0, NAN);
  initial.program = 1;  // running its own schedule
  session_.setChannelState(1, initial);
  run(10);
  ASSERT_TRUE(session_.isRegistered());
  auto lastValue = [&]() {
    THVACValue v = {};
    for (const auto &c : conn().values) {
      if (c.channel == 1) memcpy(&v, c.value, sizeof(v));
    }
    return v;
  };

  // Setpoint from the app: rounded to the device step; the device leaves
  // its schedule. Supla sees the new value at once.
  sendHvacValue(&server_, &conn(), 1, SUPLA_HVAC_MODE_HEAT, 21.3);
  run(3);
  ASSERT_EQ(commands.size(), 1u);
  EXPECT_EQ(commands[0].type, ChannelCommand::Type::SetThermostat);
  EXPECT_EQ(commands[0].mode, 0);  // already heating
  EXPECT_DOUBLE_EQ(commands[0].setpoint, 21.5);
  EXPECT_TRUE(commands[0].manual);
  EXPECT_FALSE(commands[0].program);
  EXPECT_EQ(static_cast<int>(lastValue().SetpointTemperatureHeat), 2150);
  EXPECT_FALSE(lastValue().Flags & SUPLA_HVAC_VALUE_FLAG_WEEKLY_SCHEDULE);

  // An older report of the device does not undo it; its confirmation is
  // taken as it is.
  size_t sent = conn().values.size();
  session_.setChannelState(1, thermostatState(20, NAN, NAN, NAN));
  run(3);
  EXPECT_EQ(conn().values.size(), sent);
  session_.setChannelState(1, thermostatState(21.5, NAN, NAN, NAN));
  session_.setChannelState(1, thermostatState(22, NAN, NAN, NAN));
  run(3);
  EXPECT_EQ(static_cast<int>(lastValue().SetpointTemperatureHeat), 2200);

  // "Off" comes with the unchanged setpoint: only the mode is sent.
  sendHvacValue(&server_, &conn(), 1, SUPLA_HVAC_MODE_OFF, 22);
  run(3);
  ASSERT_EQ(commands.size(), 2u);
  EXPECT_EQ(commands[1].mode, 1);
  EXPECT_TRUE(std::isnan(commands[1].setpoint));
  EXPECT_FALSE(commands[1].manual);
  EXPECT_EQ(lastValue().Mode, SUPLA_HVAC_MODE_OFF);

  // Nothing changes: accepted, nothing sent to the device.
  sendHvacValue(&server_, &conn(), 1, SUPLA_HVAC_MODE_OFF, 22);
  run(3);
  EXPECT_EQ(commands.size(), 2u);
  EXPECT_EQ(conn().setResults.back().success, 1);

  // "Program" of Supla: the device's own schedule, turned on if needed.
  sendHvacValue(&server_, &conn(), 1, SUPLA_HVAC_MODE_CMD_WEEKLY_SCHEDULE, NAN);
  run(3);
  ASSERT_EQ(commands.size(), 3u);
  EXPECT_TRUE(commands[2].program);
  EXPECT_EQ(commands[2].mode, 2);
  EXPECT_TRUE(lastValue().Flags & SUPLA_HVAC_VALUE_FLAG_WEEKLY_SCHEDULE);

  // Above the range of the device: limited.
  sendHvacValue(&server_, &conn(), 1, SUPLA_HVAC_MODE_NOT_SET, 40);
  run(3);
  ASSERT_EQ(commands.size(), 4u);
  EXPECT_DOUBLE_EQ(commands[3].setpoint, 35);
  EXPECT_TRUE(commands[3].manual);

  // Cooling is not supported.
  sendHvacValue(&server_, &conn(), 1, SUPLA_HVAC_MODE_COOL, NAN);
  run(3);
  EXPECT_EQ(commands.size(), 4u);
  EXPECT_EQ(conn().setResults.back().success, 0);

  // After a while the reports of the device are taken as they are.
  run(9000);
  session_.setChannelState(1, thermostatState(18, 0, NAN, NAN));
  run(3);
  EXPECT_EQ(static_cast<int>(lastValue().SetpointTemperatureHeat), 1800);
  EXPECT_EQ(lastValue().Mode, SUPLA_HVAC_MODE_OFF);
}

// --- Gateway ----------------------------------------------------------------

namespace {

class FakeBackend : public Backend {
 public:
  void setListener(BackendListener *l) override { listener = l; }
  bool start() override { return true; }
  void stop() override {}
  void poll() override {}
  bool sendCommand(const std::string &id, const std::string &key,
                   const ChannelCommand &command) override {
    commands.push_back({id, key, command.type});
    return true;
  }
  void requestState(const std::string &id) override {
    stateRequests.push_back(id);
  }
  size_t deviceCount() const override { return 0; }
  struct Cmd {
    std::string id, key;
    ChannelCommand::Type type;
  };
  BackendListener *listener = nullptr;
  std::vector<Cmd> commands;
  std::vector<std::string> stateRequests;
};

DeviceDescriptor switchDevice(int index) {
  DeviceDescriptor d;
  d.id = "0x00000000000000" + std::to_string(10 + index);
  d.name = "Switch " + std::to_string(index);
  ChannelSpec relay;
  relay.kind = ChannelKind::Relay;
  relay.key = "state";
  d.channels.push_back(relay);
  return d;
}

class GatewayTest : public ::testing::Test {
 protected:
  void SetUp() override {
    char tmpl[] = "/tmp/z2s_gateway_XXXXXX";
    dir_ = mkdtemp(tmpl);
    store_ = std::make_unique<IdentityStore>(dir_ + "/identities.json",
                                             [this](uint8_t *buf, size_t len) {
                                               for (size_t i = 0; i < len; i++)
                                                 buf[i] = ++counter_;
                                               return true;
                                             });
    config_.supla = serverConfig();
    config_.maxParallelConnects = 2;
    config_.connectIntervalMs = 100;
    gateway_ =
        std::make_unique<Gateway>(config_, store_.get(), &factory_, &backend_);
    gateway_->onBackendAvailability(true);
  }
  void TearDown() override {
    unlink((dir_ + "/identities.json").c_str());
    rmdir(dir_.c_str());
  }
  void run(int steps) {
    for (int i = 0; i < steps; i++) {
      now_ += 10;
      gateway_->iterate(now_);
      server_.iterate();
    }
  }

  std::string dir_;
  uint8_t counter_ = 0;
  FakeSuplaServer server_;
  FakeTransportFactory factory_{&server_};
  FakeBackend backend_;
  GatewayConfig config_;
  std::unique_ptr<IdentityStore> store_;
  std::unique_ptr<Gateway> gateway_;
  uint64_t now_ = 1000;
};

}  // namespace

TEST_F(GatewayTest, EachZigbeeDeviceBecomesSeparateSuplaDevice) {
  std::vector<DeviceDescriptor> devices;
  for (int i = 0; i < 5; i++) devices.push_back(switchDevice(i));
  gateway_->onDeviceList(devices);
  EXPECT_EQ(gateway_->sessionCount(), 5u);

  run(1);
  // Connections are started gradually.
  EXPECT_LE(server_.acceptedCount, 1);
  run(100);
  EXPECT_EQ(gateway_->registeredCount(), 5u);
  EXPECT_EQ(server_.acceptedCount, 5);

  // Every device has its own GUID.
  std::set<std::array<uint8_t, 16>> guids;
  for (auto &conn : server_.connections) {
    ASSERT_EQ(conn->registrations.size(), 1u);
    guids.insert(conn->registrations[0].guid);
  }
  EXPECT_EQ(guids.size(), 5u);

  // Identities were persisted.
  IdentityStore reloaded(dir_ + "/identities.json",
                         [](uint8_t *, size_t) { return false; });
  ASSERT_TRUE(reloaded.load());
  EXPECT_EQ(reloaded.size(), 5u);
}

TEST_F(GatewayTest, RoutesStatesAndCommands) {
  gateway_->onDeviceList({switchDevice(1)});
  gateway_->onChannelState(switchDevice(1).id, "state", ChannelState{1, NAN});
  run(20);
  ASSERT_EQ(gateway_->registeredCount(), 1u);
  auto &conn = *server_.connections[0];
  EXPECT_EQ(conn.registrations[0].channels[0].value[0], 1);

  server_.sendSetValue(&conn, 0, 5, false);
  run(3);
  ASSERT_EQ(backend_.commands.size(), 1u);
  EXPECT_EQ(backend_.commands[0].id, switchDevice(1).id);
  EXPECT_EQ(backend_.commands[0].key, "state");
  EXPECT_EQ(backend_.commands[0].type, ChannelCommand::Type::TurnOff);

  DeviceHealth health;
  health.batteryLevel = 50;
  gateway_->onDeviceHealth(switchDevice(1).id, health);
  run(3);
  ASSERT_EQ(conn.channelStates.size(), 1u);
  EXPECT_EQ(conn.channelStates[0].state.BatteryLevel, 50);

  // The backend going down keeps the connection; channels go offline.
  gateway_->onBackendAvailability(false);
  run(3);
  EXPECT_EQ(gateway_->registeredCount(), 1u);
  EXPECT_FALSE(conn.closed);
  ASSERT_FALSE(conn.values.empty());
  EXPECT_EQ(conn.values.back().offline, SUPLA_CHANNEL_OFFLINE_FLAG_OFFLINE);

  gateway_->onBackendAvailability(true);
  run(3);
  EXPECT_EQ(conn.values.back().offline, SUPLA_CHANNEL_OFFLINE_FLAG_ONLINE);
  EXPECT_EQ(server_.acceptedCount, 1);
}

TEST_F(GatewayTest, RemovedDeviceIsDisconnectedAndExcludeWorks) {
  config_.exclude = {"Switch 2"};
  gateway_ =
      std::make_unique<Gateway>(config_, store_.get(), &factory_, &backend_);
  gateway_->onBackendAvailability(true);
  gateway_->onDeviceList({switchDevice(1), switchDevice(2)});
  EXPECT_EQ(gateway_->sessionCount(), 1u);
  run(20);
  EXPECT_EQ(gateway_->registeredCount(), 1u);

  gateway_->onDeviceList({});
  run(2);
  EXPECT_EQ(gateway_->registeredCount(), 0u);
  EXPECT_TRUE(server_.connections[0]->closed);
  run(2000);
  EXPECT_EQ(server_.acceptedCount, 1);
}

TEST_F(GatewayTest, RequestsStateOnlyFromBridgedDevices) {
  config_.include = {"Switch 1", switchDevice(3).id};
  config_.exclude = {"Switch 3"};
  gateway_ =
      std::make_unique<Gateway>(config_, store_.get(), &factory_, &backend_);
  gateway_->onBackendAvailability(true);

  gateway_->onDeviceList({switchDevice(1), switchDevice(2), switchDevice(3)});
  ASSERT_EQ(backend_.stateRequests.size(), 1u);
  EXPECT_EQ(backend_.stateRequests[0], switchDevice(1).id);

  // Every device list triggers a new request, still only for bridged devices.
  gateway_->onDeviceList({switchDevice(1), switchDevice(2), switchDevice(3)});
  ASSERT_EQ(backend_.stateRequests.size(), 2u);
  EXPECT_EQ(backend_.stateRequests[1], switchDevice(1).id);
}

TEST_F(GatewayTest, IeeeAddressMatchesInAnyNotation) {
  config_.include = {"00:00:00:00:00:00:00:11", "0X0000000000000012",
                     "00:00:00:00:00:00:00:13"};
  config_.exclude = {"00:00:00:00:00:00:00:13"};
  gateway_ =
      std::make_unique<Gateway>(config_, store_.get(), &factory_, &backend_);
  gateway_->onBackendAvailability(true);

  gateway_->onDeviceList(
      {switchDevice(1), switchDevice(2), switchDevice(3), switchDevice(4)});
  EXPECT_EQ(gateway_->sessionCount(), 2u);
  ASSERT_EQ(backend_.stateRequests.size(), 2u);
  EXPECT_EQ(backend_.stateRequests[0], switchDevice(1).id);
  EXPECT_EQ(backend_.stateRequests[1], switchDevice(2).id);
}

TEST_F(GatewayTest, RenamedDeviceKeepsItsIdentity) {
  DeviceDescriptor device = switchDevice(1);
  gateway_->onDeviceList({device});
  gateway_->onChannelState(device.id, "state", ChannelState{1, NAN});
  run(20);
  ASSERT_EQ(gateway_->registeredCount(), 1u);
  ASSERT_EQ(server_.connections.size(), 1u);
  const Registration first = server_.connections[0]->registrations[0];
  EXPECT_EQ(first.name, "Switch 1");

  // The backend reports the same device (same IEEE address) under a new name.
  device.name = "Lampa w kuchni";
  gateway_->onDeviceList({device});
  run(50);

  // The device registers again: same GUID and AuthKey, new name, and the
  // last known channel value is not lost.
  EXPECT_EQ(gateway_->sessionCount(), 1u);
  ASSERT_EQ(gateway_->registeredCount(), 1u);
  ASSERT_EQ(server_.connections.size(), 2u);
  EXPECT_TRUE(server_.connections[0]->closed);
  ASSERT_EQ(server_.connections[1]->registrations.size(), 1u);
  const Registration &second = server_.connections[1]->registrations[0];
  EXPECT_EQ(second.name, "Lampa w kuchni");
  EXPECT_EQ(second.guid, first.guid);
  EXPECT_EQ(second.authKey, first.authKey);
  ASSERT_EQ(second.channels.size(), 1u);
  EXPECT_EQ(second.channels[0].value[0], 1);
  EXPECT_EQ(second.channels[0].Offline, SUPLA_CHANNEL_OFFLINE_FLAG_ONLINE);

  // No second identity was created.
  IdentityStore reloaded(dir_ + "/identities.json",
                         [](uint8_t *, size_t) { return false; });
  ASSERT_TRUE(reloaded.load());
  EXPECT_EQ(reloaded.size(), 1u);

  // The same list again does not cause another registration.
  gateway_->onDeviceList({device});
  run(50);
  EXPECT_EQ(server_.connections.size(), 2u);
  EXPECT_EQ(server_.connections[1]->registrations.size(), 1u);
}

TEST_F(GatewayTest, UnreachableDeviceIsDisconnectedFromSupla) {
  DeviceDescriptor kept = switchDevice(1);
  DeviceDescriptor lost = switchDevice(2);
  gateway_->onDeviceList({kept, lost});
  gateway_->onChannelState(kept.id, "state", ChannelState{0, NAN});
  gateway_->onChannelState(lost.id, "state", ChannelState{1, NAN});
  run(50);
  ASSERT_EQ(gateway_->registeredCount(), 2u);
  ASSERT_EQ(server_.connections.size(), 2u);
  EXPECT_EQ(gateway_->offlineCount(), 0u);

  // Find the connection of the device that is about to disappear.
  SuplaSession *lostSession = gateway_->session(lost.id);
  ASSERT_NE(lostSession, nullptr);
  auto lostConn = server_.connections[0];
  auto keptConn = server_.connections[1];
  if (lostConn->registrations[0].name != lost.name)
    std::swap(lostConn, keptConn);
  ASSERT_EQ(lostConn->registrations[0].name, lost.name);
  const auto guid = lostConn->registrations[0].guid;

  // The backend reports the device as unreachable: its connection is closed
  // and it does not try to reconnect; the other device is not affected.
  gateway_->onDeviceAvailability(lost.id, false);
  run(3);
  EXPECT_TRUE(lostConn->closed);
  EXPECT_FALSE(keptConn->closed);
  EXPECT_EQ(gateway_->registeredCount(), 1u);
  EXPECT_EQ(gateway_->sessionCount(), 2u);
  EXPECT_EQ(gateway_->offlineCount(), 1u);
  run(3000);
  EXPECT_EQ(server_.acceptedCount, 2);

  // A new device list does not bring it back either.
  gateway_->onDeviceList({kept, lost});
  run(100);
  EXPECT_EQ(server_.acceptedCount, 2);
  EXPECT_FALSE(keptConn->closed);

  // Back in the network: it registers again with the same identity and its
  // last known value.
  gateway_->onDeviceAvailability(lost.id, true);
  run(50);
  EXPECT_EQ(gateway_->registeredCount(), 2u);
  EXPECT_EQ(gateway_->offlineCount(), 0u);
  ASSERT_EQ(server_.connections.size(), 3u);
  const Registration &again = server_.connections[2]->registrations[0];
  EXPECT_EQ(again.guid, guid);
  EXPECT_EQ(again.name, lost.name);
  EXPECT_EQ(again.channels[0].value[0], 1);
  EXPECT_EQ(again.channels[0].Offline, SUPLA_CHANNEL_OFFLINE_FLAG_ONLINE);
}

TEST_F(GatewayTest, DeviceUnreachableFromTheStartDoesNotConnect) {
  DeviceDescriptor device = switchDevice(1);
  gateway_->onDeviceList({device});
  gateway_->onDeviceAvailability(device.id, false);
  run(500);
  EXPECT_EQ(server_.acceptedCount, 0);
  EXPECT_EQ(gateway_->sessionCount(), 1u);
  EXPECT_EQ(gateway_->offlineCount(), 1u);

  gateway_->onDeviceAvailability(device.id, true);
  run(50);
  EXPECT_EQ(gateway_->registeredCount(), 1u);
  EXPECT_EQ(server_.acceptedCount, 1);
}

TEST_F(GatewayTest, ButtonActionReachesSupla) {
  DeviceDescriptor d;
  d.id = "0x0000000000000041";
  d.name = "Przycisk";
  ChannelSpec button;
  button.kind = ChannelKind::ActionTrigger;
  button.key = "action";
  button.actionCaps = SUPLA_ACTION_CAP_SHORT_PRESS_x1 | SUPLA_ACTION_CAP_HOLD;
  d.channels.push_back(button);
  gateway_->onDeviceList({d});
  run(20);
  ASSERT_EQ(gateway_->registeredCount(), 1u);
  auto &conn = *server_.connections[0];
  EXPECT_EQ(static_cast<uint32_t>(
                conn.registrations[0].channels[0].ActionTriggerCaps),
            button.actionCaps);

  gateway_->onChannelAction(d.id, "action", SUPLA_ACTION_CAP_HOLD);
  gateway_->onChannelAction(d.id, "unknown", SUPLA_ACTION_CAP_HOLD);
  run(3);
  ASSERT_EQ(conn.actions.size(), 1u);
  EXPECT_EQ(conn.actions[0].action, SUPLA_ACTION_CAP_HOLD);
}
