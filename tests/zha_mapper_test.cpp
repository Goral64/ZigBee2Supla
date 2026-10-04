// SPDX-License-Identifier: GPL-2.0-or-later
// zha_snapshot.json: Home Assistant registries and states of a test ZHA
// network, plus a few made-up devices (aa:bb:cc:dd:ee:...).

#include "zha_mapper.h"

#include <gtest/gtest.h>
#include <proto.h>

#include "test_util.h"

using namespace z2s;
using namespace z2s::zha;
using json = nlohmann::json;

namespace {

Snapshot loadSnapshot() {
  json j = loadTestJson("zha_snapshot.json");
  Snapshot s;
  s.deviceRegistry = j["device_registry"];
  s.entityRegistry = j["entity_registry"];
  s.states = j["states"];
  s.zhaDevices = j["zha_devices"];
  for (const auto &[id, triggers] : j["triggers"].items()) {
    s.triggers[id] = triggers;
  }
  return s;
}

const Device *findDevice(const std::vector<Device> &devices,
                         const std::string &id) {
  for (const auto &d : devices) {
    if (d.descriptor.id == id) return &d;
  }
  return nullptr;
}

json state(const std::string &entityId, const std::string &value,
           json attributes = json::object()) {
  return {{"entity_id", entityId},
          {"state", value},
          {"attributes", std::move(attributes)}};
}

class ZhaMapperTest : public ::testing::Test {
 protected:
  void SetUp() override { devices = parseDevices(loadSnapshot()); }
  std::vector<Device> devices;
};

}  // namespace

TEST(ZhaIdTest, IeeeToZigbee2mqttId) {
  EXPECT_EQ(deviceIdFromIeee("00:12:4B:00:22:e9:5c:cf"), "0x00124b0022e95ccf");
}

TEST_F(ZhaMapperTest, SkipsCoordinatorUnsupportedDisabledAndUnmapped) {
  EXPECT_EQ(devices.size(), 8u);
  EXPECT_EQ(findDevice(devices, "0x00124b00257bf328"), nullptr);  // coord.
  EXPECT_EQ(findDevice(devices, "0xaabbccddee000003"), nullptr);  // cover
  EXPECT_EQ(findDevice(devices, "0xaabbccddee000004"), nullptr);  // disabled
  // Motion sensor without a device class in ZHA: nothing to map.
  EXPECT_EQ(findDevice(devices, "0x00124b0022e95ccf"), nullptr);

  Snapshot s = loadSnapshot();
  EXPECT_STREQ(unsupportedDeviceKind(s, "synthcover"), "cover");
  EXPECT_EQ(unsupportedDeviceKind(s, "synth2gang"), nullptr);
  EXPECT_EQ(zhaDeviceIds(s).size(), 11u);
}

TEST_F(ZhaMapperTest, LightAsRelay) {
  auto *d = findDevice(devices, "0x00124b0024c08edc");
  ASSERT_NE(d, nullptr);
  EXPECT_EQ(d->descriptor.name, "SONOFF 01MINIZB");
  EXPECT_EQ(d->descriptor.manufacturer, "SONOFF");
  EXPECT_EQ(d->descriptor.model, "01MINIZB");
  EXPECT_EQ(d->descriptor.softVersion, "0x00002000");
  EXPECT_EQ(d->descriptor.batteryPowered, 0);
  ASSERT_EQ(d->descriptor.channels.size(), 1u);
  const auto &ch = d->descriptor.channels[0];
  EXPECT_EQ(ch.kind, ChannelKind::Relay);
  EXPECT_EQ(ch.key, "state");
  EXPECT_EQ(ch.defaultFunction, SUPLA_CHANNELFNC_LIGHTSWITCH);
  EXPECT_EQ(ch.caption, "światło");
  EXPECT_EQ(ch.countdownStepMs, 0u);

  auto states = extractStates(*d, state("light.sonoff_01minizb", "on"));
  ASSERT_EQ(states.size(), 1u);
  EXPECT_EQ(states[0].first, "state");
  EXPECT_DOUBLE_EQ(states[0].second.primary, 1);
  EXPECT_TRUE(
      extractStates(*d, state("light.sonoff_01minizb", "unavailable")).empty());

  ChannelCommand on;
  on.type = ChannelCommand::Type::TurnOn;
  auto calls = buildCommand(*d, "state", on);
  ASSERT_EQ(calls.size(), 1u);
  EXPECT_EQ(calls[0].domain, "light");
  EXPECT_EQ(calls[0].service, "turn_on");
  EXPECT_EQ(calls[0].data, json({{"entity_id", "light.sonoff_01minizb"}}));
  ChannelCommand off;
  off.type = ChannelCommand::Type::TurnOff;
  calls = buildCommand(*d, "state", off);
  ASSERT_EQ(calls.size(), 1u);
  EXPECT_EQ(calls[0].service, "turn_off");
  EXPECT_TRUE(buildCommand(*d, "other", on).empty());
}

TEST_F(ZhaMapperTest, PlugWithEnergyMeter) {
  auto *d = findDevice(devices, "0xa4c13897bc9d1029");
  ASSERT_NE(d, nullptr);
  ASSERT_EQ(d->descriptor.channels.size(), 2u);  // no child lock
  EXPECT_EQ(d->descriptor.channels[0].key, "state");
  EXPECT_EQ(d->descriptor.channels[0].defaultFunction,
            SUPLA_CHANNELFNC_POWERSWITCH);
  EXPECT_EQ(d->descriptor.channels[1].kind, ChannelKind::ElectricityMeter);
  EXPECT_EQ(d->descriptor.channels[1].key, "energy");

  auto states = extractStates(
      *d, state("sensor.tz3210_2putqrmw_ts011f_summation_delivered", "1500",
                {{"device_class", "energy"}, {"unit_of_measurement", "Wh"}}));
  ASSERT_EQ(states.size(), 1u);
  EXPECT_EQ(states[0].first, "energy");
  EXPECT_DOUBLE_EQ(states[0].second.primary, 1.5);
  EXPECT_TRUE(std::isnan(states[0].second.power));

  states = extractStates(
      *d, state("sensor.tz3210_2putqrmw_ts011f_power", "12.5",
                {{"device_class", "power"}, {"unit_of_measurement", "W"}}));
  ASSERT_EQ(states.size(), 1u);
  EXPECT_DOUBLE_EQ(states[0].second.power, 12.5);
  EXPECT_TRUE(std::isnan(states[0].second.primary));
  states = extractStates(
      *d, state("sensor.tz3210_2putqrmw_ts011f_current", "120",
                {{"device_class", "current"}, {"unit_of_measurement", "mA"}}));
  ASSERT_EQ(states.size(), 1u);
  EXPECT_DOUBLE_EQ(states[0].second.current, 0.12);

  // Unknown unit: the value is not passed on.
  EXPECT_TRUE(
      extractStates(
          *d, state("sensor.tz3210_2putqrmw_ts011f_power", "1",
                    {{"device_class", "power"}, {"unit_of_measurement", "hp"}}))
          .empty());
  EXPECT_TRUE(
      extractStates(*d, state("switch.tz3210_2putqrmw_ts011f_child_lock", "on"))
          .empty());
}

TEST_F(ZhaMapperTest, ContactSensorIsInverted) {
  auto *d = findDevice(devices, "0x00124b0022ea2033");
  ASSERT_NE(d, nullptr);
  EXPECT_EQ(d->descriptor.batteryPowered, 1);
  ASSERT_EQ(d->descriptor.channels.size(), 1u);
  EXPECT_EQ(d->descriptor.channels[0].kind, ChannelKind::BinarySensor);
  EXPECT_EQ(d->descriptor.channels[0].key, "contact");
  EXPECT_EQ(d->descriptor.channels[0].defaultFunction,
            SUPLA_CHANNELFNC_OPENINGSENSOR_DOOR);

  auto states = extractStates(*d, state("binary_sensor.ewelink_ds01", "on"));
  ASSERT_EQ(states.size(), 1u);
  EXPECT_DOUBLE_EQ(states[0].second.primary, 0);  // open
  states = extractStates(*d, state("binary_sensor.ewelink_ds01", "off"));
  EXPECT_DOUBLE_EQ(states[0].second.primary, 1);  // closed

  auto health = extractHealth(*d, state("sensor.ewelink_ds01_battery", "27.0"));
  ASSERT_TRUE(health.has_value());
  EXPECT_EQ(health->batteryLevel, 27);
  EXPECT_FALSE(extractHealth(*d, state("binary_sensor.ewelink_ds01", "on")));

  auto ids = entityIds(*d);
  EXPECT_EQ(ids, (std::vector<std::string>{"binary_sensor.ewelink_ds01",
                                           "sensor.ewelink_ds01_battery"}));
}

TEST_F(ZhaMapperTest, ButtonFromDeviceTriggers) {
  auto *d = findDevice(devices, "0xa4c1385d607115bd");
  ASSERT_NE(d, nullptr);
  ASSERT_EQ(d->descriptor.channels.size(), 1u);
  const auto &ch = d->descriptor.channels[0];
  EXPECT_EQ(ch.kind, ChannelKind::ActionTrigger);
  EXPECT_EQ(ch.key, "action");
  EXPECT_EQ(ch.caption, "przycisk");
  EXPECT_EQ(ch.actionCaps,
            static_cast<uint32_t>(SUPLA_ACTION_CAP_SHORT_PRESS_x1 |
                                  SUPLA_ACTION_CAP_SHORT_PRESS_x2 |
                                  SUPLA_ACTION_CAP_HOLD));
  ASSERT_EQ(d->bindings.size(), 1u);
  EXPECT_EQ(d->bindings[0].triggerSubtype, "button_1");
  EXPECT_EQ(d->bindings[0].triggers.at("remote_button_long_press"),
            static_cast<uint32_t>(SUPLA_ACTION_CAP_HOLD));
}

TEST(ZhaButtonsTest, SeveralButtonsAreNumbered) {
  Snapshot s;
  s.deviceRegistry = json::parse(R"([{"id": "r", "name": "Pilot",
      "identifiers": [["zha", "aa:00:00:00:00:00:00:01"]],
      "disabled_by": null}])");
  json triggers = json::array();
  for (const char *sub : {"button_2", "button_1", "button_10"}) {
    for (const char *type :
         {"remote_button_short_press", "remote_button_long_release"}) {
      triggers.push_back({{"platform", "device"},
                          {"domain", "zha"},
                          {"device_id", "r"},
                          {"type", type},
                          {"subtype", sub}});
    }
  }
  s.triggers["r"] = triggers;
  auto devices = parseDevices(s);
  ASSERT_EQ(devices.size(), 1u);
  const auto &ch = devices[0].descriptor.channels;
  ASSERT_EQ(ch.size(), 3u);
  EXPECT_EQ(ch[0].key, "action_1");
  EXPECT_EQ(ch[0].caption, "przycisk 1");
  EXPECT_EQ(ch[1].key, "action_2");
  EXPECT_EQ(ch[2].key, "action_10");
  EXPECT_EQ(ch[0].actionCaps,
            static_cast<uint32_t>(SUPLA_ACTION_CAP_SHORT_PRESS_x1));
}

TEST_F(ZhaMapperTest, RadiatorThermostat) {
  auto *d = findDevice(devices, "0x50325ffffe632655");
  ASSERT_NE(d, nullptr);
  ASSERT_EQ(d->descriptor.channels.size(), 2u);
  EXPECT_EQ(d->descriptor.channels[0].kind, ChannelKind::Thermometer);
  EXPECT_EQ(d->descriptor.channels[0].key, "local_temperature");
  const auto &t = d->descriptor.channels[1];
  EXPECT_EQ(t.kind, ChannelKind::Thermostat);
  EXPECT_EQ(t.key, "current_heating_setpoint");
  EXPECT_EQ(t.thermometerKey, "local_temperature");
  EXPECT_DOUBLE_EQ(t.setpointMin, 5);
  EXPECT_DOUBLE_EQ(t.setpointMax, 35);
  EXPECT_TRUE(std::isnan(t.setpointStep));
  EXPECT_FALSE(d->bindings[1].canTurnOff);
  EXPECT_EQ(d->bindings[1].presetManual, "none");
  EXPECT_EQ(d->bindings[1].presetProgram, "Schedule");

  auto states =
      extractStates(*d, state("climate.tze200_cwnjrr72_ts0601", "heat",
                              {{"current_temperature", 27.5},
                               {"temperature", 23.0},
                               {"hvac_action", "heating"},
                               {"preset_mode", "Schedule"}}));
  ASSERT_EQ(states.size(), 2u);
  EXPECT_EQ(states[0].first, "local_temperature");
  EXPECT_DOUBLE_EQ(states[0].second.primary, 27.5);
  EXPECT_EQ(states[1].first, "current_heating_setpoint");
  EXPECT_DOUBLE_EQ(states[1].second.primary, 23);
  EXPECT_DOUBLE_EQ(states[1].second.mode, 1);
  EXPECT_DOUBLE_EQ(states[1].second.heating, 1);
  EXPECT_DOUBLE_EQ(states[1].second.program, 1);

  ChannelCommand cmd;
  cmd.type = ChannelCommand::Type::SetThermostat;
  cmd.mode = 1;  // off: the device has no "off" mode
  EXPECT_TRUE(buildCommand(*d, "current_heating_setpoint", cmd).empty());

  cmd.mode = 0;
  cmd.manual = true;
  cmd.setpoint = 21.5;
  auto calls = buildCommand(*d, "current_heating_setpoint", cmd);
  ASSERT_EQ(calls.size(), 2u);
  EXPECT_EQ(calls[0].service, "set_preset_mode");
  EXPECT_EQ(calls[0].data["preset_mode"], "none");
  EXPECT_EQ(calls[1].domain, "climate");
  EXPECT_EQ(calls[1].service, "set_temperature");
  EXPECT_EQ(calls[1].data["temperature"], 21.5);
  EXPECT_EQ(calls[1].data["entity_id"], "climate.tze200_cwnjrr72_ts0601");

  cmd = ChannelCommand();
  cmd.type = ChannelCommand::Type::SetThermostat;
  cmd.mode = 2;
  cmd.program = true;
  calls = buildCommand(*d, "current_heating_setpoint", cmd);
  ASSERT_EQ(calls.size(), 2u);
  EXPECT_EQ(calls[0].service, "set_hvac_mode");
  EXPECT_EQ(calls[0].data["hvac_mode"], "heat");
  EXPECT_EQ(calls[1].data["preset_mode"], "Schedule");
}

TEST_F(ZhaMapperTest, TwoGangSwitchUsesEndpoints) {
  auto *d = findDevice(devices, "0xaabbccddee000001");
  ASSERT_NE(d, nullptr);
  ASSERT_EQ(d->descriptor.channels.size(), 2u);  // no backlight setting
  EXPECT_EQ(d->descriptor.channels[0].key, "state_l1");
  EXPECT_EQ(d->descriptor.channels[0].caption, "przekaźnik L1");
  EXPECT_EQ(d->descriptor.channels[1].key, "state_l2");
  auto states = extractStates(*d, state("switch.dwukanalowy_l2", "on"));
  ASSERT_EQ(states.size(), 1u);
  EXPECT_EQ(states[0].first, "state_l2");
}

TEST_F(ZhaMapperTest, TemperatureHumidityInFahrenheit) {
  auto *d = findDevice(devices, "0xaabbccddee000002");
  ASSERT_NE(d, nullptr);
  EXPECT_EQ(d->descriptor.name, "Salon/czujnik");
  EXPECT_EQ(d->descriptor.batteryPowered, 1);
  // The battery voltage is a diagnostic entity, not a measurement channel.
  ASSERT_EQ(d->descriptor.channels.size(), 1u);
  EXPECT_EQ(d->descriptor.channels[0].kind, ChannelKind::TempHumidity);
  EXPECT_EQ(d->descriptor.channels[0].key, "temperature+humidity");

  auto states = extractStates(*d, state("sensor.czujnik_th_temperature", "70.7",
                                        {{"device_class", "temperature"},
                                         {"unit_of_measurement", "°F"}}));
  ASSERT_EQ(states.size(), 1u);
  EXPECT_NEAR(states[0].second.primary, 21.5, 1e-9);
  EXPECT_TRUE(std::isnan(states[0].second.secondary));
  states = extractStates(
      *d, state("sensor.czujnik_th_humidity", "48.2",
                {{"device_class", "humidity"}, {"unit_of_measurement", "%"}}));
  ASSERT_EQ(states.size(), 1u);
  EXPECT_DOUBLE_EQ(states[0].second.secondary, 48.2);
}

TEST_F(ZhaMapperTest, ValveWithFlowSensor) {
  auto *d = findDevice(devices, "0xaabbccddee000005");
  ASSERT_NE(d, nullptr);
  EXPECT_EQ(d->descriptor.batteryPowered, 1);
  ASSERT_EQ(d->descriptor.channels.size(), 2u);
  EXPECT_EQ(d->descriptor.channels[0].kind, ChannelKind::Relay);
  const ChannelSpec &flow = d->descriptor.channels[1];
  EXPECT_EQ(flow.kind, ChannelKind::GeneralPurposeMeasurement);
  EXPECT_EQ(flow.key, "flow");
  EXPECT_EQ(flow.caption, "przepływ");
  EXPECT_EQ(flow.unit, "m³/h");
  EXPECT_EQ(flow.precision, 2);

  auto states = extractStates(*d, state("sensor.zawor_flow", "12",
                                        {{"device_class", "volume_flow_rate"},
                                         {"unit_of_measurement", "L/min"}}));
  ASSERT_EQ(states.size(), 1u);
  EXPECT_EQ(states[0].first, "flow");
  EXPECT_NEAR(states[0].second.primary, 0.72, 1e-9);
}

TEST(ZhaAvailabilityTest, UnavailableState) {
  EXPECT_FALSE(isAvailable(state("switch.x", "unavailable")));
  EXPECT_TRUE(isAvailable(state("switch.x", "unknown")));
  EXPECT_TRUE(isAvailable(state("switch.x", "off")));
}
