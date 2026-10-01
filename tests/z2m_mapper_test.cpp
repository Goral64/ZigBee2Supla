// SPDX-License-Identifier: GPL-2.0-or-later

#include "z2m_mapper.h"

#include <gtest/gtest.h>
#include <proto.h>

#include "test_util.h"

using namespace z2s;
using namespace z2s::z2m;
using json = nlohmann::json;

namespace {

const Device *findDevice(const std::vector<Device> &devices,
                         const std::string &name) {
  for (const auto &d : devices) {
    if (d.friendlyName == name) return &d;
  }
  return nullptr;
}

}  // namespace

TEST(Z2mMapperTest, SkipsCoordinatorDisabledUnsupportedAndNotInterviewed) {
  auto devices = parseBridgeDevices(loadTestJson("bridge_devices.json"));
  EXPECT_EQ(devices.size(), 5u);
  EXPECT_EQ(findDevice(devices, "Coordinator"), nullptr);
  EXPECT_EQ(findDevice(devices, "W trakcie parowania"), nullptr);
  EXPECT_EQ(findDevice(devices, "Nieobslugiwane"), nullptr);
  EXPECT_EQ(findDevice(devices, "Wylaczone"), nullptr);
}

TEST(Z2mMapperTest, TemperatureHumidityPressureSensor) {
  auto devices = parseBridgeDevices(loadTestJson("bridge_devices.json"));
  auto *d = findDevice(devices, "Salon/czujnik");
  ASSERT_NE(d, nullptr);
  EXPECT_EQ(d->descriptor.id, "0x00158d0001a2b3c4");
  EXPECT_EQ(d->descriptor.manufacturer, "Aqara");
  EXPECT_EQ(d->descriptor.model, "WSDCGQ11LM");
  ASSERT_EQ(d->descriptor.channels.size(), 2u);
  EXPECT_EQ(d->descriptor.channels[0].kind, ChannelKind::Pressure);
  EXPECT_EQ(d->descriptor.channels[1].kind, ChannelKind::TempHumidity);
  EXPECT_EQ(d->descriptor.channels[1].key, "temperature+humidity");

  auto states = extractStates(
      *d, json::parse(R"({"temperature":21.4,"humidity":48.2,"pressure":1003,
                          "battery":97,"linkquality":120})"));
  ASSERT_EQ(states.size(), 2u);
  EXPECT_EQ(states[0].first, "pressure");
  EXPECT_DOUBLE_EQ(states[0].second.primary, 1003);
  EXPECT_EQ(states[1].first, "temperature+humidity");
  EXPECT_DOUBLE_EQ(states[1].second.primary, 21.4);
  EXPECT_DOUBLE_EQ(states[1].second.secondary, 48.2);

  // Partial update: only humidity.
  states = extractStates(*d, json::parse(R"({"humidity":50})"));
  ASSERT_EQ(states.size(), 1u);
  EXPECT_TRUE(std::isnan(states[0].second.primary));
  EXPECT_DOUBLE_EQ(states[0].second.secondary, 50);
}

TEST(Z2mMapperTest, ContactSensorIsInvertedForSupla) {
  auto devices = parseBridgeDevices(loadTestJson("bridge_devices.json"));
  auto *d = findDevice(devices, "Drzwi wejsciowe");
  ASSERT_NE(d, nullptr);
  ASSERT_EQ(d->descriptor.channels.size(), 1u);
  EXPECT_EQ(d->descriptor.channels[0].kind, ChannelKind::BinarySensor);
  EXPECT_EQ(d->descriptor.channels[0].defaultFunction,
            SUPLA_CHANNELFNC_OPENINGSENSOR_DOOR);

  // contact=true -> door closed -> Supla value 1
  auto states = extractStates(*d, json::parse(R"({"contact":true})"));
  ASSERT_EQ(states.size(), 1u);
  EXPECT_DOUBLE_EQ(states[0].second.primary, 1);
  states = extractStates(*d, json::parse(R"({"contact":false})"));
  EXPECT_DOUBLE_EQ(states[0].second.primary, 0);
}

TEST(Z2mMapperTest, MultiGangSwitch) {
  auto devices = parseBridgeDevices(loadTestJson("bridge_devices.json"));
  auto *d = findDevice(devices, "Wlacznik kuchnia");
  ASSERT_NE(d, nullptr);
  ASSERT_EQ(d->descriptor.channels.size(), 2u);
  EXPECT_EQ(d->descriptor.channels[0].kind, ChannelKind::Relay);
  EXPECT_EQ(d->descriptor.channels[0].key, "state_l1");
  EXPECT_EQ(d->descriptor.channels[1].key, "state_l2");
  EXPECT_EQ(d->descriptor.channels[0].defaultFunction,
            SUPLA_CHANNELFNC_POWERSWITCH);

  auto states =
      extractStates(*d, json::parse(R"({"state_l1":"ON","state_l2":"OFF"})"));
  ASSERT_EQ(states.size(), 2u);
  EXPECT_DOUBLE_EQ(states[0].second.primary, 1);
  EXPECT_DOUBLE_EQ(states[1].second.primary, 0);

  ChannelCommand on;
  on.type = ChannelCommand::Type::TurnOn;
  auto cmd = buildCommand(*d, "state_l2", on);
  ASSERT_EQ(cmd.size(), 1u);
  EXPECT_EQ(cmd[0], json::parse(R"({"state_l2":"ON"})"));
  EXPECT_TRUE(buildCommand(*d, "unknown", on).empty());
}

TEST(Z2mMapperTest, LightAndMotionSensor) {
  auto devices = parseBridgeDevices(loadTestJson("bridge_devices.json"));
  auto *light = findDevice(devices, "Lampa sypialnia");
  ASSERT_NE(light, nullptr);
  ASSERT_EQ(light->descriptor.channels.size(), 1u);
  EXPECT_EQ(light->descriptor.channels[0].defaultFunction,
            SUPLA_CHANNELFNC_LIGHTSWITCH);

  auto *motion = findDevice(devices, "Ruch korytarz");
  ASSERT_NE(motion, nullptr);
  ASSERT_EQ(motion->descriptor.channels.size(), 2u);
  EXPECT_EQ(motion->descriptor.channels[0].defaultFunction,
            SUPLA_CHANNELFNC_MOTION_SENSOR);
  EXPECT_EQ(motion->descriptor.channels[1].kind, ChannelKind::Thermometer);
  auto states = extractStates(*motion, json::parse(R"({"occupancy":true})"));
  ASSERT_EQ(states.size(), 1u);
  EXPECT_DOUBLE_EQ(states[0].second.primary, 1);
}

TEST(Z2mMapperTest, Availability) {
  EXPECT_EQ(parseAvailability(R"({"state":"online"})"), true);
  EXPECT_EQ(parseAvailability(R"({"state":"offline"})"), false);
  EXPECT_EQ(parseAvailability("online"), true);
  EXPECT_EQ(parseAvailability("offline"), false);
  EXPECT_FALSE(parseAvailability("garbage").has_value());
}

TEST(Z2mMapperTest, DevicesOfUnsupportedKindAreNotBridged) {
  // A radiator valve: its main function ("climate") is not supported yet, and
  // its settings are exposed as switches.
  json valve = json::parse(R"({
    "ieee_address": "0xa4c1380000000099",
    "friendly_name": "Glowica",
    "type": "EndDevice",
    "interview_completed": true,
    "definition": {
      "vendor": "Tuya", "model": "TS0601_thermostat",
      "exposes": [
        {"type": "switch", "features": [
          {"type": "binary", "name": "state", "property": "window_detection",
           "access": 3, "value_on": "ON", "value_off": "OFF"}]},
        {"type": "climate", "features": [
          {"type": "numeric", "name": "local_temperature",
           "property": "local_temperature", "access": 1}]},
        {"type": "numeric", "name": "linkquality", "property": "linkquality",
         "access": 1}
      ]
    }
  })");
  EXPECT_STREQ(unsupportedDeviceKind(valve), "thermostat");
  EXPECT_TRUE(parseBridgeDevices(json::array({valve})).empty());

  // Without the climate expose the same switch is a relay.
  json plain = valve;
  plain["definition"]["exposes"].erase(1);
  EXPECT_EQ(unsupportedDeviceKind(plain), nullptr);
  auto devices = parseBridgeDevices(json::array({plain}));
  ASSERT_EQ(devices.size(), 1u);
  ASSERT_EQ(devices[0].bindings.size(), 1u);
  EXPECT_EQ(devices[0].bindings[0].spec.kind, ChannelKind::Relay);

  for (const char *type : {"cover", "lock", "fan"}) {
    json other = valve;
    other["definition"]["exposes"][1]["type"] = type;
    EXPECT_NE(unsupportedDeviceKind(other), nullptr) << type;
    EXPECT_TRUE(parseBridgeDevices(json::array({other})).empty()) << type;
  }

  // Supported devices from the sample data are not affected.
  for (const auto &dev : loadTestJson("bridge_devices.json")) {
    EXPECT_EQ(unsupportedDeviceKind(dev), nullptr);
  }
}

namespace {

json deviceWithExposes(const json &exposes) {
  json dev = json::parse(R"({
    "ieee_address": "0xa4c1380000000077",
    "friendly_name": "Timer test",
    "type": "Router",
    "interview_completed": true,
    "definition": {"vendor": "Test", "model": "T1"}
  })");
  dev["definition"]["exposes"] = exposes;
  return dev;
}

json relay(const std::string &property, int access = 7) {
  return json{{"type", "switch"},
              {"features", json::array({{{"type", "binary"},
                                         {"name", "state"},
                                         {"property", property},
                                         {"access", access},
                                         {"value_on", "ON"},
                                         {"value_off", "OFF"}}})}};
}

json number(const std::string &property, const std::string &unit, int access,
            double max) {
  return json{{"type", "numeric"}, {"name", property}, {"property", property},
              {"access", access},  {"unit", unit},     {"value_min", 0},
              {"value_max", max}};
}

ChannelCommand turnOn(uint32_t durationMs) {
  ChannelCommand c;
  c.type = ChannelCommand::Type::TurnOn;
  c.durationMs = durationMs;
  return c;
}

}  // namespace

TEST(Z2mMapperTest, PlugCountdownIsDeviceTimer) {
  auto devices = parseBridgeDevices(json::array({deviceWithExposes(
      json::array({relay("state"), number("countdown", "s", 3, 43200)}))}));
  ASSERT_EQ(devices.size(), 1u);
  const Device &d = devices[0];
  ASSERT_EQ(d.descriptor.channels.size(), 1u);
  EXPECT_EQ(d.descriptor.channels[0].countdownStepMs, 1000u);
  EXPECT_EQ(d.descriptor.channels[0].countdownMaxMs, 43200000u);

  // Countdown is started after turning on.
  auto cmd = buildCommand(d, "state", turnOn(30000));
  ASSERT_EQ(cmd.size(), 2u);
  EXPECT_EQ(cmd[0], json::parse(R"({"state":"ON"})"));
  EXPECT_EQ(cmd[1], json::parse(R"({"countdown":30})"));

  cmd = buildCommand(d, "state", turnOn(0));
  ASSERT_EQ(cmd.size(), 1u);
  EXPECT_EQ(cmd[0], json::parse(R"({"state":"ON"})"));

  ChannelCommand off;
  off.type = ChannelCommand::Type::TurnOff;
  cmd = buildCommand(d, "state", off);
  ASSERT_EQ(cmd.size(), 1u);
  EXPECT_EQ(cmd[0], json::parse(R"({"state":"OFF"})"));
}

TEST(Z2mMapperTest, MultiGangCountdownPerChannel) {
  auto devices = parseBridgeDevices(json::array({deviceWithExposes(
      json::array({relay("state_l1"), relay("state_l2"),
                   number("countdown_l1", "s", 7, 43200),
                   number("countdown_l2", "s", 7, 43200)}))}));
  ASSERT_EQ(devices.size(), 1u);
  ASSERT_EQ(devices[0].bindings.size(), 2u);
  EXPECT_EQ(devices[0].bindings[0].timerProperty, "countdown_l1");
  EXPECT_EQ(devices[0].bindings[1].timerProperty, "countdown_l2");
  auto cmd = buildCommand(devices[0], "state_l2", turnOn(5000));
  ASSERT_EQ(cmd.size(), 2u);
  EXPECT_EQ(cmd[1], json::parse(R"({"countdown_l2":5})"));
}

TEST(Z2mMapperTest, WateringTimerIsSetBeforeTurningOn) {
  auto devices = parseBridgeDevices(json::array({deviceWithExposes(
      json::array({relay("state", 3), number("timer", "min", 3, 599),
                   number("time_left", "min", 1, 0)}))}));
  ASSERT_EQ(devices.size(), 1u);
  const Device &d = devices[0];
  EXPECT_EQ(d.descriptor.channels[0].countdownStepMs, 60000u);
  EXPECT_EQ(d.descriptor.channels[0].countdownMaxMs, 599u * 60000u);

  auto cmd = buildCommand(d, "state", turnOn(120000));
  ASSERT_EQ(cmd.size(), 2u);
  EXPECT_EQ(cmd[0], json::parse(R"({"timer":2})"));
  EXPECT_EQ(cmd[1], json::parse(R"({"state":"ON"})"));
}

TEST(Z2mMapperTest, NoDeviceTimerWithoutMatchingProperties) {
  // "timer" alone (no "time_left") may mean something else; a read-only
  // countdown or one without a time unit cannot be used either.
  for (const json &extra :
       {number("timer", "min", 3, 599), number("countdown", "s", 1, 43200),
        number("countdown", "", 3, 43200)}) {
    auto devices = parseBridgeDevices(
        json::array({deviceWithExposes(json::array({relay("state"), extra}))}));
    ASSERT_EQ(devices.size(), 1u);
    EXPECT_EQ(devices[0].descriptor.channels[0].countdownStepMs, 0u)
        << extra.dump();
    EXPECT_TRUE(devices[0].bindings[0].timerProperty.empty());
  }
}

TEST(Z2mMapperTest, ChannelCaptionsDescribeTheChannel) {
  auto devices = parseBridgeDevices(loadTestJson("bridge_devices.json"));
  auto caption = [&](const std::string &device, size_t channel) {
    const Device *d = findDevice(devices, device);
    EXPECT_NE(d, nullptr) << device;
    if (d == nullptr || channel >= d->descriptor.channels.size())
      return std::string();
    return d->descriptor.channels[channel].caption;
  };
  EXPECT_EQ(caption("Wlacznik kuchnia", 0), "przekaźnik L1");
  EXPECT_EQ(caption("Wlacznik kuchnia", 1), "przekaźnik L2");
  EXPECT_EQ(caption("Lampa sypialnia", 0), "światło");
  EXPECT_EQ(caption("Drzwi wejsciowe", 0), "kontaktron");
  EXPECT_EQ(caption("Salon/czujnik", 0), "ciśnienie");
  EXPECT_EQ(caption("Salon/czujnik", 1), "temperatura i wilgotność");
  EXPECT_EQ(caption("Ruch korytarz", 0), "czujnik ruchu");
}

TEST(Z2mMapperTest, PlugWithEnergyGetsElectricityMeter) {
  // Exposes of a Nous A1Z / Tuya TS011F plug.
  auto devices = parseBridgeDevices(json::array({deviceWithExposes(json::array(
      {relay("state"), number("power", "W", 1, 0), number("current", "A", 1, 0),
       number("voltage", "V", 1, 0), number("energy", "kWh", 1, 0)}))}));
  ASSERT_EQ(devices.size(), 1u);
  const Device &d = devices[0];
  ASSERT_EQ(d.descriptor.channels.size(), 2u);
  EXPECT_EQ(d.descriptor.channels[0].kind, ChannelKind::Relay);
  EXPECT_EQ(d.descriptor.channels[1].kind, ChannelKind::ElectricityMeter);
  EXPECT_EQ(d.descriptor.channels[1].key, "energy");
  EXPECT_EQ(d.descriptor.channels[1].caption, "licznik energii");

  auto states =
      extractStates(d, json::parse(R"({"state":"ON","power":57.3,"current":0.25,
                          "voltage":230,"energy":12.34,"linkquality":80})"));
  ASSERT_EQ(states.size(), 2u);
  EXPECT_EQ(states[1].first, "energy");
  EXPECT_DOUBLE_EQ(states[1].second.primary, 12.34);
  EXPECT_DOUBLE_EQ(states[1].second.power, 57.3);
  EXPECT_DOUBLE_EQ(states[1].second.voltage, 230);
  EXPECT_DOUBLE_EQ(states[1].second.current, 0.25);

  // Partial update.
  states = extractStates(d, json::parse(R"({"power":60})"));
  ASSERT_EQ(states.size(), 1u);
  EXPECT_TRUE(std::isnan(states[0].second.primary));
  EXPECT_DOUBLE_EQ(states[0].second.power, 60);
}

TEST(Z2mMapperTest, MeasurementsWithoutEnergyAreGeneralPurpose) {
  auto devices = parseBridgeDevices(json::array({deviceWithExposes(
      json::array({relay("state"), number("power", "W", 1, 0),
                   number("voltage", "V", 1, 0)}))}));
  ASSERT_EQ(devices.size(), 1u);
  const Device &d = devices[0];
  ASSERT_EQ(d.descriptor.channels.size(), 3u);
  EXPECT_EQ(d.descriptor.channels[1].kind,
            ChannelKind::GeneralPurposeMeasurement);
  EXPECT_EQ(d.descriptor.channels[1].key, "power");
  EXPECT_EQ(d.descriptor.channels[1].caption, "moc");
  EXPECT_EQ(d.descriptor.channels[2].key, "voltage");
  EXPECT_EQ(d.descriptor.channels[2].caption, "napięcie");

  auto states = extractStates(d, json::parse(R"({"voltage":229.5})"));
  ASSERT_EQ(states.size(), 1u);
  EXPECT_EQ(states[0].first, "voltage");
  EXPECT_DOUBLE_EQ(states[0].second.primary, 229.5);
}

TEST(Z2mMapperTest, BatteryVoltageIsNotMeasurement) {
  // Battery sensors report their battery voltage in mV.
  json contact =
      json{{"type", "binary"}, {"name", "contact"}, {"property", "contact"},
           {"access", 1},      {"value_on", false}, {"value_off", true}};
  auto devices = parseBridgeDevices(json::array({deviceWithExposes(
      json::array({contact, number("voltage", "mV", 1, 0)}))}));
  ASSERT_EQ(devices.size(), 1u);
  EXPECT_EQ(devices[0].descriptor.channels.size(), 1u);

  // A battery device does not get measurement channels even in volts.
  json battery =
      deviceWithExposes(json::array({contact, number("voltage", "V", 1, 0)}));
  battery["power_source"] = "Battery";
  devices = parseBridgeDevices(json::array({battery}));
  ASSERT_EQ(devices.size(), 1u);
  EXPECT_EQ(devices[0].descriptor.channels.size(), 1u);
}

TEST(Z2mMapperTest, DeviceHealth) {
  auto health = extractHealth(json::parse(
      R"({"contact":true,"battery":99.6,"battery_low":false,
          "linkquality":255})"));
  ASSERT_TRUE(health.has_value());
  EXPECT_EQ(health->batteryLevel, 100);
  EXPECT_EQ(health->batteryLow, 0);
  EXPECT_EQ(health->linkQuality, 100);

  health = extractHealth(json::parse(R"({"battery_low":true})"));
  ASSERT_TRUE(health.has_value());
  EXPECT_EQ(health->batteryLevel, -1);
  EXPECT_EQ(health->batteryLow, 1);
  EXPECT_EQ(health->linkQuality, -1);

  EXPECT_FALSE(extractHealth(json::parse(R"({"state":"ON"})")).has_value());
}

TEST(Z2mMapperTest, PowerSource) {
  json dev = deviceWithExposes(json::array({relay("state")}));
  EXPECT_EQ(parseBridgeDevices(json::array({dev}))[0].descriptor.batteryPowered,
            -1);
  dev["power_source"] = "Mains (single phase)";
  EXPECT_EQ(parseBridgeDevices(json::array({dev}))[0].descriptor.batteryPowered,
            0);
  dev["power_source"] = "Battery";
  EXPECT_EQ(parseBridgeDevices(json::array({dev}))[0].descriptor.batteryPowered,
            1);
}

TEST(Z2mMapperTest, LightSensorIsGeneralPurposeMeasurement) {
  json sensor = deviceWithExposes(json::array(
      {number("battery", "%", 1, 100), number("illuminance", "lx", 5, 0)}));
  sensor["power_source"] = "Battery";
  auto devices = parseBridgeDevices(json::array({sensor}));
  ASSERT_EQ(devices.size(), 1u);
  ASSERT_EQ(devices[0].descriptor.channels.size(), 1u);
  const ChannelSpec &spec = devices[0].descriptor.channels[0];
  EXPECT_EQ(spec.kind, ChannelKind::GeneralPurposeMeasurement);
  EXPECT_EQ(spec.key, "illuminance");
  EXPECT_EQ(spec.caption, "natężenie światła");
  EXPECT_EQ(spec.unit, "lx");
  EXPECT_EQ(spec.precision, 0);

  auto states =
      extractStates(devices[0], json::parse(R"({"illuminance":312})"));
  ASSERT_EQ(states.size(), 1u);
  EXPECT_DOUBLE_EQ(states[0].second.primary, 312);
}

namespace {

json actionExpose(const std::vector<std::string> &values) {
  return json{{"type", "enum"},
              {"name", "action"},
              {"property", "action"},
              {"access", 1},
              {"values", values}};
}

}  // namespace

TEST(Z2mMapperTest, ButtonIsActionTrigger) {
  // Tuya TS0041.
  json button = deviceWithExposes(
      json::array({number("battery", "%", 1, 100),
                   actionExpose({"single", "double", "hold"})}));
  auto devices = parseBridgeDevices(json::array({button}));
  ASSERT_EQ(devices.size(), 1u);
  const Device &d = devices[0];
  ASSERT_EQ(d.descriptor.channels.size(), 1u);
  const ChannelSpec &spec = d.descriptor.channels[0];
  EXPECT_EQ(spec.kind, ChannelKind::ActionTrigger);
  EXPECT_EQ(spec.key, "action");
  EXPECT_EQ(spec.caption, "przycisk");
  EXPECT_EQ(spec.actionCaps,
            uint32_t{SUPLA_ACTION_CAP_SHORT_PRESS_x1 |
                     SUPLA_ACTION_CAP_SHORT_PRESS_x2 | SUPLA_ACTION_CAP_HOLD});

  auto actions = extractActions(d, json::parse(R"({"action":"double"})"));
  ASSERT_EQ(actions.size(), 1u);
  EXPECT_EQ(actions[0].first, "action");
  EXPECT_EQ(actions[0].second, uint32_t{SUPLA_ACTION_CAP_SHORT_PRESS_x2});
  EXPECT_TRUE(extractActions(d, json::parse(R"({"action":""})")).empty());
  EXPECT_TRUE(extractActions(d, json::parse(R"({"battery":90})")).empty());
  // Not a channel state.
  EXPECT_TRUE(extractStates(d, json::parse(R"({"action":"single"})")).empty());
}

TEST(Z2mMapperTest, RemoteHasChannelPerButton) {
  json remote = deviceWithExposes(json::array(
      {actionExpose({"1_single", "1_double", "2_single", "2_hold", "10_single",
                     "button_3_triple", "brightness_move_up", "2_release"})}));
  auto devices = parseBridgeDevices(json::array({remote}));
  ASSERT_EQ(devices.size(), 1u);
  const auto &channels = devices[0].descriptor.channels;
  ASSERT_EQ(channels.size(), 4u);
  EXPECT_EQ(channels[0].key, "action_1");
  EXPECT_EQ(channels[0].caption, "przycisk 1");
  EXPECT_EQ(channels[0].actionCaps, uint32_t{SUPLA_ACTION_CAP_SHORT_PRESS_x1 |
                                             SUPLA_ACTION_CAP_SHORT_PRESS_x2});
  EXPECT_EQ(channels[1].key, "action_2");
  EXPECT_EQ(channels[1].actionCaps,
            uint32_t{SUPLA_ACTION_CAP_SHORT_PRESS_x1 | SUPLA_ACTION_CAP_HOLD});
  EXPECT_EQ(channels[2].key, "action_3");
  EXPECT_EQ(channels[2].actionCaps, uint32_t{SUPLA_ACTION_CAP_SHORT_PRESS_x3});
  EXPECT_EQ(channels[3].key, "action_10");

  auto actions =
      extractActions(devices[0], json::parse(R"({"action":"2_hold"})"));
  ASSERT_EQ(actions.size(), 1u);
  EXPECT_EQ(actions[0].first, "action_2");
  EXPECT_EQ(actions[0].second, uint32_t{SUPLA_ACTION_CAP_HOLD});
  EXPECT_TRUE(extractActions(devices[0],
                             json::parse(R"({"action":"brightness_move_up"})"))
                  .empty());
}

TEST(Z2mMapperTest, OnOffRemote) {
  json remote = deviceWithExposes(json::array(
      {actionExpose({"on", "off", "brightness_move_up", "brightness_stop"})}));
  auto devices = parseBridgeDevices(json::array({remote}));
  ASSERT_EQ(devices.size(), 1u);
  ASSERT_EQ(devices[0].descriptor.channels.size(), 1u);
  EXPECT_EQ(devices[0].descriptor.channels[0].actionCaps,
            uint32_t{SUPLA_ACTION_CAP_TURN_ON | SUPLA_ACTION_CAP_TURN_OFF});
}

namespace {

// Exposes of a Tuya TS0601 radiator head (shortened).
json radiatorHead() {
  return deviceWithExposes(json::parse(R"([
    {"type": "switch", "features": [
      {"type": "binary", "name": "state", "property": "window_detection",
       "access": 3, "value_on": "ON", "value_off": "OFF"}]},
    {"type": "binary", "name": "window_open", "property": "window_open",
     "access": 1, "value_on": true, "value_off": false},
    {"type": "numeric", "name": "position", "property": "position",
     "unit": "%", "access": 1},
    {"type": "climate", "features": [
      {"type": "numeric", "name": "current_heating_setpoint",
       "property": "current_heating_setpoint", "unit": "°C", "access": 3,
       "value_min": 5, "value_max": 35, "value_step": 0.5},
      {"type": "numeric", "name": "local_temperature",
       "property": "local_temperature", "unit": "°C", "access": 1},
      {"type": "enum", "name": "system_mode", "property": "system_mode",
       "access": 3, "values": ["heat", "auto", "off"]},
      {"type": "enum", "name": "preset", "property": "preset", "access": 3,
       "values": ["schedule", "manual", "boost", "eco"]},
      {"type": "enum", "name": "running_state", "property": "running_state",
       "access": 1, "values": ["idle", "heat"]}]},
    {"type": "numeric", "name": "battery", "property": "battery",
     "unit": "%", "access": 1}
  ])"));
}

ChannelCommand thermostatCommand(int mode, double setpoint, bool manual) {
  ChannelCommand c;
  c.type = ChannelCommand::Type::SetThermostat;
  c.mode = mode;
  c.setpoint = setpoint;
  c.manual = manual;
  return c;
}

}  // namespace

TEST(Z2mMapperTest, RadiatorHeadIsThermostat) {
  json head = radiatorHead();
  EXPECT_EQ(unsupportedDeviceKind(head), nullptr);
  auto devices = parseBridgeDevices(json::array({head}));
  ASSERT_EQ(devices.size(), 1u);
  const Device &d = devices[0];
  // Only the thermometer and the thermostat: window detection and the
  // window sensor are settings of the head itself.
  ASSERT_EQ(d.descriptor.channels.size(), 2u);
  EXPECT_EQ(d.descriptor.channels[0].kind, ChannelKind::Thermometer);
  EXPECT_EQ(d.descriptor.channels[0].key, "local_temperature");
  const ChannelSpec &t = d.descriptor.channels[1];
  EXPECT_EQ(t.kind, ChannelKind::Thermostat);
  EXPECT_EQ(t.key, "current_heating_setpoint");
  EXPECT_EQ(t.caption, "termostat");
  EXPECT_EQ(t.thermometerKey, "local_temperature");
  EXPECT_DOUBLE_EQ(t.setpointMin, 5);
  EXPECT_DOUBLE_EQ(t.setpointMax, 35);
  EXPECT_DOUBLE_EQ(t.setpointStep, 0.5);

  auto states = extractStates(d, json::parse(R"({"local_temperature":20.5,
                          "current_heating_setpoint":21,"system_mode":"auto",
                          "running_state":"heat","position":35})"));
  ASSERT_EQ(states.size(), 2u);
  EXPECT_EQ(states[0].first, "local_temperature");
  EXPECT_DOUBLE_EQ(states[0].second.primary, 20.5);
  EXPECT_EQ(states[1].first, "current_heating_setpoint");
  EXPECT_DOUBLE_EQ(states[1].second.primary, 21);
  EXPECT_DOUBLE_EQ(states[1].second.mode, 1);  // own schedule = heating
  EXPECT_DOUBLE_EQ(states[1].second.heating, 1);
  EXPECT_DOUBLE_EQ(states[1].second.valve, 35);
  states = extractStates(d, json::parse(R"({"system_mode":"off"})"));
  ASSERT_EQ(states.size(), 1u);
  EXPECT_DOUBLE_EQ(states[0].second.mode, 0);

  // The setpoint goes last: leaving the schedule could change it.
  auto cmd = buildCommand(d, "current_heating_setpoint",
                          thermostatCommand(2, 22.5, true));
  ASSERT_EQ(cmd.size(), 3u);
  EXPECT_EQ(cmd[0], json::parse(R"({"system_mode":"heat"})"));
  EXPECT_EQ(cmd[1], json::parse(R"({"preset":"manual"})"));
  EXPECT_EQ(cmd[2], json::parse(R"({"current_heating_setpoint":22.5})"));
  cmd = buildCommand(d, "current_heating_setpoint",
                     thermostatCommand(1, NAN, false));
  ASSERT_EQ(cmd.size(), 1u);
  EXPECT_EQ(cmd[0], json::parse(R"({"system_mode":"off"})"));
  // Relay commands do not apply.
  ChannelCommand on;
  on.type = ChannelCommand::Type::TurnOn;
  EXPECT_TRUE(buildCommand(d, "current_heating_setpoint", on).empty());
}

TEST(Z2mMapperTest, WallThermostatHoldPreset) {
  // Moes BHT-002: "hold" keeps the setpoint, "program" is its schedule.
  json wall = radiatorHead();
  auto &climate = wall["definition"]["exposes"][3]["features"];
  climate[2]["values"] = {"off", "heat"};
  climate[3]["values"] = {"hold", "program"};
  auto devices = parseBridgeDevices(json::array({wall}));
  ASSERT_EQ(devices.size(), 1u);
  auto cmd = buildCommand(devices[0], "current_heating_setpoint",
                          thermostatCommand(0, 19, true));
  ASSERT_EQ(cmd.size(), 2u);
  EXPECT_EQ(cmd[0], json::parse(R"({"preset":"hold"})"));
  EXPECT_EQ(cmd[1], json::parse(R"({"current_heating_setpoint":19})"));
}

TEST(Z2mMapperTest, ClimateWithoutHeatingSetpointIsSkipped) {
  json cooler = radiatorHead();
  cooler["definition"]["exposes"][3]["features"][0]["access"] = 1;
  EXPECT_STREQ(unsupportedDeviceKind(cooler), "thermostat");
  EXPECT_TRUE(parseBridgeDevices(json::array({cooler})).empty());
}

TEST(Z2mMapperTest, ThermostatProgram) {
  auto devices = parseBridgeDevices(json::array({radiatorHead()}));
  ASSERT_EQ(devices.size(), 1u);
  const Device &d = devices[0];
  auto states = extractStates(d, json::parse(R"({"preset":"schedule"})"));
  ASSERT_EQ(states.size(), 1u);
  EXPECT_DOUBLE_EQ(states[0].second.program, 1);
  states = extractStates(d, json::parse(R"({"preset":"boost"})"));
  EXPECT_DOUBLE_EQ(states[0].second.program, 0);

  ChannelCommand program = thermostatCommand(0, NAN, false);
  program.program = true;
  auto cmd = buildCommand(d, "current_heating_setpoint", program);
  ASSERT_EQ(cmd.size(), 1u);
  EXPECT_EQ(cmd[0], json::parse(R"({"preset":"schedule"})"));

  // A device without its own schedule cannot do it.
  json noProgram = radiatorHead();
  noProgram["definition"]["exposes"][3]["features"][3]["values"] = {"manual",
                                                                    "boost"};
  devices = parseBridgeDevices(json::array({noProgram}));
  ASSERT_EQ(devices.size(), 1u);
  EXPECT_TRUE(
      buildCommand(devices[0], "current_heating_setpoint", program).empty());
}
