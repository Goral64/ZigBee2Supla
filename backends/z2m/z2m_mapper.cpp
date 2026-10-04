// SPDX-License-Identifier: GPL-2.0-or-later

#include "z2m_mapper.h"

#include <proto.h>

#include <algorithm>
#include <cctype>
#include <map>

namespace z2s::z2m {

using json = nlohmann::json;

namespace {

// zigbee2mqtt "access" bits
constexpr int kAccessState = 1;  // published in the device state
constexpr int kAccessSet = 2;    // can be set with /set

// A leaf feature of an expose, with the type of its parent composite
// (e.g. "switch" or "light"), if any.
struct Feature {
  std::string parentType;
  const json *def;
};

void collectFeatures(const json &exposes, const std::string &parentType,
                     std::vector<Feature> *out) {
  if (!exposes.is_array()) return;
  for (const auto &expose : exposes) {
    if (!expose.is_object()) continue;
    if (expose.contains("features") && expose["features"].is_array()) {
      collectFeatures(expose["features"], expose.value("type", ""), out);
    } else {
      out->push_back({parentType, &expose});
    }
  }
}

int access(const json &def) { return def.value("access", 0); }

struct BinarySensorKind {
  int32_t function;
  const char *caption;
  bool invert;
};

const std::map<std::string, BinarySensorKind> &binarySensorKinds() {
  static const std::map<std::string, BinarySensorKind> kinds = {
      // Supla opening sensor: 1 = closed. zigbee2mqtt: value_on = open.
      {"contact", {SUPLA_CHANNELFNC_OPENINGSENSOR_DOOR, "kontaktron", true}},
      {"occupancy", {SUPLA_CHANNELFNC_MOTION_SENSOR, "czujnik ruchu", false}},
      {"presence",
       {SUPLA_CHANNELFNC_MOTION_SENSOR, "czujnik obecności", false}},
      {"water_leak", {SUPLA_CHANNELFNC_FLOOD_SENSOR, "czujnik zalania", false}},
      {"smoke", {SUPLA_CHANNELFNC_BINARY_SENSOR, "czujnik dymu", false}},
      {"gas", {SUPLA_CHANNELFNC_BINARY_SENSOR, "czujnik gazu", false}},
      {"carbon_monoxide",
       {SUPLA_CHANNELFNC_BINARY_SENSOR, "czujnik CO", false}},
      {"vibration", {SUPLA_CHANNELFNC_BINARY_SENSOR, "czujnik drgań", false}},
  };
  return kinds;
}

// Channel caption of a relay: "przekaźnik" / "światło", with the endpoint
// of multi-channel devices ("state_l1" -> "przekaźnik L1").
std::string relayCaption(const std::string &parentType,
                         const std::string &property) {
  std::string caption = parentType == "light" ? "światło" : "przekaźnik";
  if (property.rfind("state_", 0) == 0 && property.size() > 6) {
    std::string endpoint = property.substr(6);
    for (char &c : endpoint) {
      c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    caption += " " + endpoint;
  }
  return caption;
}

// A top level numeric feature that can be set.
const json *settableNumber(const std::vector<Feature> &features,
                           const std::string &property) {
  for (const auto &f : features) {
    const json &def = *f.def;
    if (f.parentType.empty() && def.value("type", "") == "numeric" &&
        def.value("property", "") == property && (access(def) & kAccessSet)) {
      return &def;
    }
  }
  return nullptr;
}

bool hasProperty(const std::vector<Feature> &features,
                 const std::string &property) {
  for (const auto &f : features) {
    if (f.parentType.empty() && f.def->value("property", "") == property) {
      return true;
    }
  }
  return false;
}

uint32_t unitMs(const json &def) {
  const std::string unit = def.value("unit", "");
  if (unit == "s") return 1000;
  if (unit == "min") return 60000;
  return 0;
}

// Finds the device's own turn-on timer of a relay binding:
//  * "countdown" / "countdown_l1"…: switches the relay over after the given
//    time, started after turning on (plugs and switches, e.g. Tuya),
//  * "timer" with "time_left": auto-off time for the next turn-on (watering
//    timers), only for single-relay devices.
void findRelayTimer(const std::vector<Feature> &features, int relayCount,
                    Binding *b) {
  std::string countdown = "countdown";
  if (b->property != "state") {
    if (b->property.rfind("state_", 0) != 0) return;
    countdown += b->property.substr(5);
  }
  const json *def = settableNumber(features, countdown);
  bool beforeOn = false;
  if (def == nullptr && b->property == "state" && relayCount == 1 &&
      hasProperty(features, "time_left")) {
    def = settableNumber(features, "timer");
    beforeOn = true;
  }
  if (def == nullptr) return;
  uint32_t unit = unitMs(*def);
  double max = def->value("value_max", 0.0);
  if (unit == 0 || max < 1) return;
  b->timerProperty = def->value("property", "");
  b->timerUnitMs = unit;
  b->timerBeforeOn = beforeOn;
  b->spec.countdownStepMs = unit;
  b->spec.countdownMaxMs = static_cast<uint32_t>(max) * unit;
}

// Measurements shown as general purpose measurement channels: zigbee2mqtt
// property (= expose name), required unit (also the unit shown in Supla),
// caption and number of decimal places.
struct Measurement {
  const char *property;
  const char *unit;
  const char *caption;
  int precision;
};

constexpr Measurement kPower = {"power", "W", "moc", 1};
constexpr Measurement kVoltage = {"voltage", "V", "napięcie", 1};
constexpr Measurement kCurrent = {"current", "A", "natężenie prądu", 2};
constexpr Measurement kIlluminance = {"illuminance", "lx", "natężenie światła",
                                      0};
constexpr Measurement kFlow = {"flow", "m³/h", "przepływ", 2};

Binding measurementBinding(const Measurement &m) {
  Binding b;
  b.spec.kind = ChannelKind::GeneralPurposeMeasurement;
  b.spec.key = m.property;
  b.spec.caption = m.caption;
  b.spec.unit = m.unit;
  b.spec.precision = m.precision;
  b.property = m.property;
  return b;
}

// A top level numeric feature published in the device state, with the given
// name, property and unit.
const json *stateNumber(const std::vector<Feature> &features,
                        const std::string &property, const std::string &unit) {
  for (const auto &f : features) {
    const json &def = *f.def;
    if (f.parentType.empty() && def.value("type", "") == "numeric" &&
        def.value("name", "") == property &&
        def.value("property", "") == property &&
        def.value("unit", "") == unit && (access(def) & kAccessState)) {
      return &def;
    }
  }
  return nullptr;
}

// Energy metering of plugs and switches:
//  * "energy" [kWh] -> an electricity meter channel, which also shows power,
//    voltage and current,
//  * without "energy", each of power [W], voltage [V] and current [A] ->
//    a general purpose measurement channel. Only on mains powered devices:
//    battery devices report their battery voltage.
void addEnergyBindings(const std::vector<Feature> &features,
                       bool batteryPowered, std::vector<Binding> *bindings) {
  if (stateNumber(features, "energy", "kWh") != nullptr) {
    Binding b;
    b.spec.kind = ChannelKind::ElectricityMeter;
    b.spec.key = "energy";
    b.spec.caption = "licznik energii";
    b.property = "energy";
    for (auto [m, out] : {std::pair{&kPower, &b.powerProperty},
                          std::pair{&kVoltage, &b.voltageProperty},
                          std::pair{&kCurrent, &b.currentProperty}}) {
      if (stateNumber(features, m->property, m->unit) != nullptr) {
        *out = m->property;
      }
    }
    bindings->push_back(b);
    return;
  }
  if (batteryPowered) return;
  for (const Measurement *m : {&kPower, &kVoltage, &kCurrent}) {
    if (stateNumber(features, m->property, m->unit) == nullptr) continue;
    bindings->push_back(measurementBinding(*m));
  }
}

// Other measurements without a dedicated Supla channel type.
void addMeasurementBindings(const std::vector<Feature> &features,
                            std::vector<Binding> *bindings) {
  for (const Measurement *m : {&kIlluminance, &kFlow}) {
    if (stateNumber(features, m->property, m->unit) == nullptr) continue;
    bindings->push_back(measurementBinding(*m));
  }
}

// Supla action of a zigbee2mqtt action name (the part after the button
// number), 0 when there is none.
uint32_t actionCap(const std::string &name) {
  static const std::map<std::string, uint32_t> caps = {
      {"single", SUPLA_ACTION_CAP_SHORT_PRESS_x1},
      {"click", SUPLA_ACTION_CAP_SHORT_PRESS_x1},
      {"press", SUPLA_ACTION_CAP_SHORT_PRESS_x1},
      {"double", SUPLA_ACTION_CAP_SHORT_PRESS_x2},
      {"triple", SUPLA_ACTION_CAP_SHORT_PRESS_x3},
      {"quadruple", SUPLA_ACTION_CAP_SHORT_PRESS_x4},
      {"hold", SUPLA_ACTION_CAP_HOLD},
      {"long", SUPLA_ACTION_CAP_HOLD},
      {"on", SUPLA_ACTION_CAP_TURN_ON},
      {"off", SUPLA_ACTION_CAP_TURN_OFF},
      {"rotate_left", SUPLA_ACTION_CAP_ROTATE_LEFT},
      {"rotate_right", SUPLA_ACTION_CAP_ROTATE_RIGHT},
  };
  auto it = caps.find(name);
  return it == caps.end() ? 0 : it->second;
}

bool isNumber(const std::string &s) {
  return !s.empty() && std::all_of(s.begin(), s.end(), [](unsigned char c) {
    return std::isdigit(c) != 0;
  });
}

// Splits an action value into the button number ("" for single-button
// devices) and the action name: "single", "1_single", "button_2_hold".
void splitAction(const std::string &value, std::string *button,
                 std::string *name) {
  std::string rest = value;
  if (rest.rfind("button_", 0) == 0) rest = rest.substr(7);
  size_t sep = rest.find('_');
  if (sep != std::string::npos && isNumber(rest.substr(0, sep))) {
    *button = rest.substr(0, sep);
    *name = rest.substr(sep + 1);
  } else {
    button->clear();
    *name = value;
  }
}

// Buttons and remotes: one action trigger channel per button ("action" or
// "action_<n>"), with the actions it reports. Values without a Supla
// counterpart (e.g. "brightness_move_up", "release") are ignored.
void addActionBindings(const std::vector<Feature> &features,
                       std::vector<Binding> *bindings) {
  for (const auto &f : features) {
    const json &def = *f.def;
    if (!f.parentType.empty() || def.value("type", "") != "enum" ||
        def.value("property", "") != "action" || !def.contains("values") ||
        !def["values"].is_array()) {
      continue;
    }
    std::map<std::string, Binding> buttons;  // ordered by button number
    for (const auto &v : def["values"]) {
      if (!v.is_string()) continue;
      std::string button, name;
      splitAction(v.get<std::string>(), &button, &name);
      uint32_t cap = actionCap(name);
      if (cap == 0) continue;
      Binding &b = buttons[button];
      b.spec.kind = ChannelKind::ActionTrigger;
      b.spec.key = button.empty() ? "action" : "action_" + button;
      b.spec.caption = button.empty() ? "przycisk" : "przycisk " + button;
      b.spec.actionCaps |= cap;
      b.property = "action";
      b.actions[v.get<std::string>()] = cap;
    }
    std::vector<std::pair<std::string, Binding>> sorted(buttons.begin(),
                                                        buttons.end());
    std::sort(sorted.begin(), sorted.end(), [](const auto &a, const auto &b) {
      if (a.first.size() != b.first.size()) {
        return a.first.size() < b.first.size();
      }
      return a.first < b.first;
    });
    for (auto &[button, b] : sorted) bindings->push_back(b);
    return;
  }
}

bool hasEnumValue(const json &def, const std::string &value) {
  if (!def.contains("values") || !def["values"].is_array()) return false;
  for (const auto &v : def["values"]) {
    if (v.is_string() && v.get<std::string>() == value) return true;
  }
  return false;
}

const json *climateFeature(const json &climate, const std::string &property) {
  if (!climate.contains("features") || !climate["features"].is_array()) {
    return nullptr;
  }
  for (const auto &f : climate["features"]) {
    if (f.is_object() && f.value("property", "") == property) return &f;
  }
  return nullptr;
}

// A heating thermostat (radiator head, wall thermostat): settable heating
// setpoint, room temperature and a system mode with "heat". Its thermometer
// and thermostat channels, or nothing for other climate devices (cooling
// only, fan coils...).
std::optional<std::pair<Binding, Binding>> thermostatBindings(
    const json &climate, const std::vector<Feature> &features) {
  const json *setpoint = climateFeature(climate, "current_heating_setpoint");
  const json *temperature = climateFeature(climate, "local_temperature");
  const json *mode = climateFeature(climate, "system_mode");
  if (setpoint == nullptr || temperature == nullptr || mode == nullptr ||
      !(access(*setpoint) & kAccessSet) ||
      !(access(*temperature) & kAccessState) || !(access(*mode) & kAccessSet) ||
      !hasEnumValue(*mode, "heat")) {
    return std::nullopt;
  }
  Binding thermometer;
  thermometer.spec.kind = ChannelKind::Thermometer;
  thermometer.spec.key = "local_temperature";
  thermometer.spec.caption = "termometr";
  thermometer.property = "local_temperature";

  Binding b;
  b.spec.kind = ChannelKind::Thermostat;
  b.spec.key = "current_heating_setpoint";
  b.spec.caption = "termostat";
  b.spec.thermometerKey = thermometer.spec.key;
  b.spec.setpointMin = setpoint->value("value_min", NAN);
  b.spec.setpointMax = setpoint->value("value_max", NAN);
  b.spec.setpointStep = setpoint->value("value_step", NAN);
  b.property = "current_heating_setpoint";
  b.modeProperty = "system_mode";
  b.modeHeat = "heat";
  if (hasEnumValue(*mode, "off")) b.modeOff = "off";
  const json *preset = climateFeature(climate, "preset");
  if (preset != nullptr && (access(*preset) & kAccessSet)) {
    for (const char *manual : {"manual", "hold"}) {
      if (hasEnumValue(*preset, manual)) {
        b.presetProperty = "preset";
        b.presetManual = manual;
        break;
      }
    }
    for (const char *program : {"schedule", "program"}) {
      if (!b.presetProperty.empty() && hasEnumValue(*preset, program)) {
        b.presetProgram = program;
        break;
      }
    }
  }
  if (climateFeature(climate, "running_state") != nullptr) {
    b.runningProperty = "running_state";
  }
  for (const auto &f : features) {
    const json &def = *f.def;
    if (f.parentType.empty() && def.value("property", "") == "position" &&
        def.value("type", "") == "numeric" && def.value("unit", "") == "%" &&
        (access(def) & kAccessState)) {
      b.valveProperty = "position";
    }
  }
  return std::make_pair(thermometer, b);
}

const json *climateExpose(const json &exposes) {
  if (!exposes.is_array()) return nullptr;
  for (const auto &expose : exposes) {
    if (expose.is_object() && expose.value("type", "") == "climate") {
      return &expose;
    }
  }
  return nullptr;
}

// Expose types that describe the main function of a device and are not
// mapped to Supla channels yet.
const std::map<std::string, const char *> &unsupportedKinds() {
  static const std::map<std::string, const char *> kinds = {
      {"climate", "thermostat"},
      {"cover", "cover"},
      {"lock", "lock"},
      {"fan", "fan"},
  };
  return kinds;
}

std::optional<Device> parseDevice(const json &dev) {
  if (!dev.is_object()) return std::nullopt;
  if (dev.value("type", "") == "Coordinator") return std::nullopt;
  if (dev.value("disabled", false)) return std::nullopt;
  if (!dev.value("interview_completed", false)) return std::nullopt;
  if (!dev.contains("definition") || !dev["definition"].is_object()) {
    return std::nullopt;  // unsupported by zigbee2mqtt
  }
  if (unsupportedDeviceKind(dev) != nullptr) return std::nullopt;
  const json &definition = dev["definition"];

  Device device;
  device.friendlyName = dev.value("friendly_name", "");
  device.descriptor.id = dev.value("ieee_address", "");
  if (device.descriptor.id.empty() || device.friendlyName.empty()) {
    return std::nullopt;
  }
  device.descriptor.name = device.friendlyName;
  device.descriptor.manufacturer = definition.value("vendor", "");
  device.descriptor.model = definition.value("model", "");
  const std::string powerSource = dev.value("power_source", "");
  if (powerSource == "Battery") {
    device.descriptor.batteryPowered = 1;
  } else if (powerSource.rfind("Mains", 0) == 0 || powerSource == "DC Source") {
    device.descriptor.batteryPowered = 0;
  }
  if (dev.contains("software_build_id") &&
      dev["software_build_id"].is_string()) {
    device.descriptor.softVersion = dev["software_build_id"];
  }

  // Feature pointers refer into `definition`, so it must not be a copy.
  std::vector<Feature> features;
  if (definition.contains("exposes")) {
    collectFeatures(definition["exposes"], "", &features);
  }

  // A thermostat is bridged as its thermometer and thermostat only: its
  // other switches and sensors are settings of the device itself (window
  // detection, child lock...).
  if (definition.contains("exposes")) {
    if (const json *climate = climateExpose(definition["exposes"])) {
      auto thermostat = thermostatBindings(*climate, features);
      if (!thermostat) return std::nullopt;
      device.bindings.push_back(thermostat->first);
      device.bindings.push_back(thermostat->second);
      for (const auto &b : device.bindings) {
        device.descriptor.channels.push_back(b.spec);
      }
      return device;
    }
  }

  const json *temperature = nullptr;
  const json *humidity = nullptr;

  for (const auto &feature : features) {
    const json &def = *feature.def;
    const std::string type = def.value("type", "");
    const std::string name = def.value("name", "");
    const std::string property = def.value("property", "");
    if (property.empty()) continue;

    if ((feature.parentType == "switch" || feature.parentType == "light") &&
        name == "state" && type == "binary" && (access(def) & kAccessSet)) {
      Binding b;
      b.spec.kind = ChannelKind::Relay;
      b.spec.key = property;
      b.spec.defaultFunction = feature.parentType == "light"
                                   ? SUPLA_CHANNELFNC_LIGHTSWITCH
                                   : SUPLA_CHANNELFNC_POWERSWITCH;
      b.spec.caption = relayCaption(feature.parentType, property);
      b.property = property;
      b.valueOn = def.value("value_on", json("ON"));
      b.valueOff = def.value("value_off", json("OFF"));
      device.bindings.push_back(b);
      continue;
    }

    if (!feature.parentType.empty() || !(access(def) & kAccessState)) {
      continue;
    }

    if (type == "binary") {
      auto it = binarySensorKinds().find(name);
      if (it == binarySensorKinds().end()) continue;
      Binding b;
      b.spec.kind = ChannelKind::BinarySensor;
      b.spec.key = property;
      b.spec.defaultFunction = it->second.function;
      b.spec.caption = it->second.caption;
      b.property = property;
      b.valueOn = def.value("value_on", json(true));
      b.valueOff = def.value("value_off", json(false));
      b.invert = it->second.invert;
      device.bindings.push_back(b);
    } else if (type == "numeric") {
      if (name == "temperature" && property == "temperature") {
        temperature = &def;
      } else if (name == "humidity" && property == "humidity") {
        humidity = &def;
      } else if (name == "pressure" && property == "pressure") {
        Binding b;
        b.spec.kind = ChannelKind::Pressure;
        b.spec.key = property;
        b.spec.caption = "ciśnienie";
        b.property = property;
        device.bindings.push_back(b);
      }
    }
  }

  if (temperature != nullptr && humidity != nullptr) {
    Binding b;
    b.spec.kind = ChannelKind::TempHumidity;
    b.spec.key = "temperature+humidity";
    b.spec.caption = "temperatura i wilgotność";
    b.property = "temperature";
    b.secondaryProperty = "humidity";
    device.bindings.push_back(b);
  } else if (temperature != nullptr) {
    Binding b;
    b.spec.kind = ChannelKind::Thermometer;
    b.spec.key = "temperature";
    b.spec.caption = "termometr";
    b.property = "temperature";
    device.bindings.push_back(b);
  } else if (humidity != nullptr) {
    Binding b;
    b.spec.kind = ChannelKind::Humidity;
    b.spec.key = "humidity";
    b.spec.caption = "wilgotność";
    b.property = "humidity";
    device.bindings.push_back(b);
  }

  addEnergyBindings(features, device.descriptor.batteryPowered == 1,
                    &device.bindings);
  addMeasurementBindings(features, &device.bindings);
  addActionBindings(features, &device.bindings);

  if (device.bindings.empty()) return std::nullopt;
  int relayCount = 0;
  for (const auto &b : device.bindings) {
    if (b.spec.kind == ChannelKind::Relay) relayCount++;
  }
  for (auto &b : device.bindings) {
    if (b.spec.kind == ChannelKind::Relay) {
      findRelayTimer(features, relayCount, &b);
    }
  }
  for (const auto &b : device.bindings) {
    device.descriptor.channels.push_back(b.spec);
  }
  return device;
}

double numberOrNan(const json &payload, const std::string &property) {
  if (property.empty() || !payload.contains(property)) return NAN;
  const json &v = payload[property];
  return v.is_number() ? v.get<double>() : NAN;
}

}  // namespace

const char *unsupportedDeviceKind(const json &device) {
  if (!device.is_object() || !device.contains("definition") ||
      !device["definition"].is_object()) {
    return nullptr;
  }
  const json &definition = device["definition"];
  if (!definition.contains("exposes") || !definition["exposes"].is_array()) {
    return nullptr;
  }
  for (const auto &expose : definition["exposes"]) {
    if (!expose.is_object()) continue;
    auto it = unsupportedKinds().find(expose.value("type", ""));
    if (it == unsupportedKinds().end()) continue;
    if (it->first == "climate") {
      std::vector<Feature> features;
      collectFeatures(definition["exposes"], "", &features);
      if (thermostatBindings(expose, features)) continue;
    }
    return it->second;
  }
  return nullptr;
}

std::vector<Device> parseBridgeDevices(const json &devices) {
  std::vector<Device> result;
  if (!devices.is_array()) return result;
  for (const auto &dev : devices) {
    if (auto device = parseDevice(dev)) {
      result.push_back(std::move(*device));
    }
  }
  return result;
}

std::vector<std::pair<std::string, ChannelState>> extractStates(
    const Device &device, const json &payload) {
  std::vector<std::pair<std::string, ChannelState>> result;
  if (!payload.is_object()) return result;
  for (const auto &b : device.bindings) {
    ChannelState state;
    switch (b.spec.kind) {
      case ChannelKind::Relay:
      case ChannelKind::BinarySensor: {
        if (!payload.contains(b.property)) continue;
        const json &v = payload[b.property];
        if (v == b.valueOn) {
          state.primary = b.invert ? 0 : 1;
        } else if (v == b.valueOff) {
          state.primary = b.invert ? 1 : 0;
        } else {
          continue;
        }
        break;
      }
      case ChannelKind::Thermometer:
      case ChannelKind::Humidity:
      case ChannelKind::Pressure:
      case ChannelKind::TempHumidity:
      case ChannelKind::GeneralPurposeMeasurement:
        state.primary = numberOrNan(payload, b.property);
        state.secondary = numberOrNan(payload, b.secondaryProperty);
        if (std::isnan(state.primary) && std::isnan(state.secondary)) continue;
        break;
      case ChannelKind::ActionTrigger:
        continue;  // see extractActions()
      case ChannelKind::Thermostat: {
        state.primary = numberOrNan(payload, b.property);
        state.valve = numberOrNan(payload, b.valveProperty);
        if (payload.contains(b.modeProperty) &&
            payload[b.modeProperty].is_string()) {
          state.mode = payload[b.modeProperty] == b.modeOff ? 0 : 1;
        }
        if (!b.presetProperty.empty() && payload.contains(b.presetProperty) &&
            payload[b.presetProperty].is_string()) {
          state.program = payload[b.presetProperty] == b.presetProgram ? 1 : 0;
        }
        if (!b.runningProperty.empty() && payload.contains(b.runningProperty) &&
            payload[b.runningProperty].is_string()) {
          state.heating = payload[b.runningProperty] == "heat" ? 1 : 0;
        }
        if (isEmpty(state)) continue;
        break;
      }
      case ChannelKind::ElectricityMeter:
        state.primary = numberOrNan(payload, b.property);
        state.power = numberOrNan(payload, b.powerProperty);
        state.voltage = numberOrNan(payload, b.voltageProperty);
        state.current = numberOrNan(payload, b.currentProperty);
        if (isEmpty(state)) continue;
        break;
    }
    result.emplace_back(b.spec.key, state);
  }
  return result;
}

std::vector<std::pair<std::string, uint32_t>> extractActions(
    const Device &device, const json &payload) {
  std::vector<std::pair<std::string, uint32_t>> result;
  if (!payload.is_object() || !payload.contains("action") ||
      !payload["action"].is_string()) {
    return result;
  }
  const std::string value = payload["action"];
  for (const auto &b : device.bindings) {
    if (b.spec.kind != ChannelKind::ActionTrigger) continue;
    auto it = b.actions.find(value);
    if (it != b.actions.end()) result.emplace_back(b.spec.key, it->second);
  }
  return result;
}

std::optional<DeviceHealth> extractHealth(const json &payload) {
  if (!payload.is_object()) return std::nullopt;
  DeviceHealth health;
  bool found = false;
  double battery = numberOrNan(payload, "battery");
  if (!std::isnan(battery)) {
    health.batteryLevel =
        static_cast<int>(std::lround(std::clamp(battery, 0.0, 100.0)));
    found = true;
  }
  if (payload.contains("battery_low") && payload["battery_low"].is_boolean()) {
    health.batteryLow = payload["battery_low"].get<bool>() ? 1 : 0;
    found = true;
  }
  // zigbee2mqtt link quality (LQI) is 0..255.
  double lqi = numberOrNan(payload, "linkquality");
  if (!std::isnan(lqi)) {
    health.linkQuality = static_cast<int>(
        std::lround(std::clamp(lqi, 0.0, 255.0) * 100.0 / 255.0));
    found = true;
  }
  if (!found) return std::nullopt;
  return health;
}

std::vector<json> buildCommand(const Device &device,
                               const std::string &channelKey,
                               const ChannelCommand &command) {
  for (const auto &b : device.bindings) {
    if (b.spec.key != channelKey) continue;
    if (b.spec.kind == ChannelKind::Thermostat &&
        command.type == ChannelCommand::Type::SetThermostat) {
      // Separate messages, in order: the mode, leaving the device's own
      // schedule (which could restore another setpoint), the setpoint.
      std::vector<json> messages;
      if (command.mode == 1 && !b.modeOff.empty()) {
        messages.push_back({{b.modeProperty, b.modeOff}});
      } else if (command.mode == 2) {
        messages.push_back({{b.modeProperty, b.modeHeat}});
      } else if (command.mode == 1) {
        return {};  // the device cannot be switched off
      }
      if (command.program) {
        if (b.presetProgram.empty()) return {};  // no own schedule
        messages.push_back({{b.presetProperty, b.presetProgram}});
      } else if (command.manual && !b.presetProperty.empty()) {
        messages.push_back({{b.presetProperty, b.presetManual}});
      }
      if (!std::isnan(command.setpoint)) {
        messages.push_back({{b.property, command.setpoint}});
      }
      return messages;
    }
    if (b.spec.kind != ChannelKind::Relay ||
        command.type == ChannelCommand::Type::SetThermostat) {
      continue;
    }
    bool on = command.type == ChannelCommand::Type::TurnOn;
    json state = json::object();
    state[b.property] = on ? b.valueOn : b.valueOff;
    if (!on || command.durationMs == 0 || b.timerProperty.empty() ||
        b.timerUnitMs == 0) {
      return {state};
    }
    json timer = json::object();
    timer[b.timerProperty] = command.durationMs / b.timerUnitMs;
    if (b.timerBeforeOn) return {timer, state};
    return {state, timer};
  }
  return {};
}

std::optional<bool> parseAvailability(const std::string &payload) {
  std::string state = payload;
  json parsed = json::parse(payload, nullptr, false);
  if (!parsed.is_discarded() && parsed.is_object() &&
      parsed.contains("state") && parsed["state"].is_string()) {
    state = parsed["state"];
  }
  if (state == "online") return true;
  if (state == "offline") return false;
  return std::nullopt;
}

}  // namespace z2s::z2m
