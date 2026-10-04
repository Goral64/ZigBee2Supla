// SPDX-License-Identifier: GPL-2.0-or-later

#include "zha_mapper.h"

#include <proto.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <set>

namespace z2s::zha {

using json = nlohmann::json;

namespace {

std::string toLower(std::string s) {
  for (char &c : s) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return s;
}

std::string domainOf(const std::string &entityId) {
  return entityId.substr(0, entityId.find('.'));
}

std::string stringOr(const json &obj, const char *key) {
  if (!obj.is_object() || !obj.contains(key) || !obj[key].is_string()) {
    return {};
  }
  return obj[key].get<std::string>();
}

// ZHA IEEE address of a device registry entry, or "".
std::string zhaIeee(const json &device) {
  if (!device.contains("identifiers") || !device["identifiers"].is_array()) {
    return {};
  }
  for (const auto &id : device["identifiers"]) {
    if (id.is_array() && id.size() == 2 && id[0] == "zha" &&
        id[1].is_string()) {
      return id[1].get<std::string>();
    }
  }
  return {};
}

// An entity of a ZHA device with what the mapper needs to know about it.
struct Entity {
  std::string id;
  std::string domain;
  std::string deviceClass;
  std::string category;  // "", "config" or "diagnostic"
  int endpoint = 0;
  const json *state = nullptr;  // state object, if any
};

// Endpoint from a ZHA unique id "<ieee>-<endpoint>[-<cluster>...]".
int endpointOf(const std::string &uniqueId) {
  size_t dash = uniqueId.find('-');
  if (dash == std::string::npos) return 0;
  return std::atoi(uniqueId.c_str() + dash + 1);
}

// Enabled entities of a device, ordered by endpoint and entity id.
std::vector<Entity> deviceEntities(
    const Snapshot &snapshot, const std::string &haDeviceId,
    const std::map<std::string, const json *> &states) {
  std::vector<Entity> result;
  if (!snapshot.entityRegistry.is_array()) return result;
  for (const auto &e : snapshot.entityRegistry) {
    if (!e.is_object() || stringOr(e, "device_id") != haDeviceId ||
        stringOr(e, "platform") != "zha" ||
        !e.value("disabled_by", json()).is_null()) {
      continue;
    }
    Entity entity;
    entity.id = stringOr(e, "entity_id");
    if (entity.id.empty()) continue;
    entity.domain = domainOf(entity.id);
    entity.category = stringOr(e, "entity_category");
    entity.endpoint = endpointOf(stringOr(e, "unique_id"));
    auto it = states.find(entity.id);
    if (it != states.end()) {
      entity.state = it->second;
      entity.deviceClass = stringOr(
          entity.state->value("attributes", json::object()), "device_class");
    }
    result.push_back(entity);
  }
  std::sort(result.begin(), result.end(), [](const Entity &a, const Entity &b) {
    if (a.endpoint != b.endpoint) return a.endpoint < b.endpoint;
    return a.id < b.id;
  });
  return result;
}

std::map<std::string, const json *> indexStates(const Snapshot &snapshot) {
  std::map<std::string, const json *> states;
  if (!snapshot.states.is_array()) return states;
  for (const auto &s : snapshot.states) {
    std::string id = stringOr(s, "entity_id");
    if (!id.empty()) states[id] = &s;
  }
  return states;
}

const json &attributes(const json &state) {
  static const json empty = json::object();
  if (!state.is_object() || !state.contains("attributes") ||
      !state["attributes"].is_object()) {
    return empty;
  }
  return state["attributes"];
}

bool hasMode(const json &attrs, const char *key, const std::string &mode) {
  if (!attrs.contains(key) || !attrs[key].is_array()) return false;
  for (const auto &m : attrs[key]) {
    if (m.is_string() && m.get<std::string>() == mode) return true;
  }
  return false;
}

// A preset of the list, compared without case; returns its exact name.
std::string findPreset(const json &attrs,
                       std::initializer_list<const char *> names) {
  if (!attrs.contains("preset_modes") || !attrs["preset_modes"].is_array()) {
    return {};
  }
  for (const char *name : names) {
    for (const auto &p : attrs["preset_modes"]) {
      if (p.is_string() && toLower(p.get<std::string>()) == name) {
        return p.get<std::string>();
      }
    }
  }
  return {};
}

bool isMainEntity(const Entity &e) { return e.category.empty(); }

// A climate entity of a heating thermostat.
bool isHeatingClimate(const Entity &e) {
  return e.domain == "climate" && e.state != nullptr &&
         hasMode(attributes(*e.state), "hvac_modes", "heat");
}

struct BinarySensorKind {
  const char *key;
  int32_t function;
  const char *caption;
  bool invert;
};

// Home Assistant device class -> zigbee2mqtt property of the same feature.
const std::map<std::string, BinarySensorKind> &binarySensorKinds() {
  static const std::map<std::string, BinarySensorKind> kinds = {
      // Supla opening sensor: 1 = closed. Home Assistant: on = open.
      {"opening",
       {"contact", SUPLA_CHANNELFNC_OPENINGSENSOR_DOOR, "kontaktron", true}},
      {"door",
       {"contact", SUPLA_CHANNELFNC_OPENINGSENSOR_DOOR, "kontaktron", true}},
      {"window",
       {"contact", SUPLA_CHANNELFNC_OPENINGSENSOR_DOOR, "kontaktron", true}},
      {"garage_door",
       {"contact", SUPLA_CHANNELFNC_OPENINGSENSOR_DOOR, "kontaktron", true}},
      {"motion",
       {"occupancy", SUPLA_CHANNELFNC_MOTION_SENSOR, "czujnik ruchu", false}},
      {"occupancy",
       {"occupancy", SUPLA_CHANNELFNC_MOTION_SENSOR, "czujnik ruchu", false}},
      {"presence",
       {"presence", SUPLA_CHANNELFNC_MOTION_SENSOR, "czujnik obecności",
        false}},
      {"moisture",
       {"water_leak", SUPLA_CHANNELFNC_FLOOD_SENSOR, "czujnik zalania", false}},
      {"smoke",
       {"smoke", SUPLA_CHANNELFNC_BINARY_SENSOR, "czujnik dymu", false}},
      {"gas", {"gas", SUPLA_CHANNELFNC_BINARY_SENSOR, "czujnik gazu", false}},
      {"carbon_monoxide",
       {"carbon_monoxide", SUPLA_CHANNELFNC_BINARY_SENSOR, "czujnik CO",
        false}},
      {"vibration",
       {"vibration", SUPLA_CHANNELFNC_BINARY_SENSOR, "czujnik drgań", false}},
  };
  return kinds;
}

// Measurements shown as general purpose measurement channels: device class,
// zigbee2mqtt property, unit shown in Supla, caption, decimal places.
struct Measurement {
  const char *deviceClass;
  const char *key;
  const char *unit;
  const char *caption;
  int precision;
};

constexpr Measurement kPower = {"power", "power", "W", "moc", 1};
constexpr Measurement kVoltage = {"voltage", "voltage", "V", "napięcie", 1};
constexpr Measurement kCurrent = {"current", "current", "A", "natężenie prądu",
                                  2};
constexpr Measurement kIlluminance = {"illuminance", "illuminance", "lx",
                                      "natężenie światła", 0};
constexpr Measurement kFlow = {"volume_flow_rate", "flow", "m³/h", "przepływ",
                               2};

Binding measurementBinding(const Measurement &m, const std::string &entityId) {
  Binding b;
  b.spec.kind = ChannelKind::GeneralPurposeMeasurement;
  b.spec.key = m.key;
  b.spec.caption = m.caption;
  b.spec.unit = m.unit;
  b.spec.precision = m.precision;
  b.entityId = entityId;
  return b;
}

// The first main sensor entity of the device class.
const Entity *sensor(const std::vector<Entity> &entities,
                     const std::string &deviceClass) {
  for (const auto &e : entities) {
    if (e.domain == "sensor" && isMainEntity(e) &&
        e.deviceClass == deviceClass) {
      return &e;
    }
  }
  return nullptr;
}

// Relay caption: "przekaźnik" / "światło", with the endpoint of devices
// with more relays ("przekaźnik L2").
std::string relayCaption(const std::string &domain, int endpoint,
                         bool numbered) {
  std::string caption = domain == "light" ? "światło" : "przekaźnik";
  if (numbered) caption += " L" + std::to_string(endpoint);
  return caption;
}

// Device trigger type -> Supla action.
uint32_t triggerCap(const std::string &type) {
  static const std::map<std::string, uint32_t> caps = {
      {"remote_button_short_press", SUPLA_ACTION_CAP_SHORT_PRESS_x1},
      {"remote_button_double_press", SUPLA_ACTION_CAP_SHORT_PRESS_x2},
      {"remote_button_triple_press", SUPLA_ACTION_CAP_SHORT_PRESS_x3},
      {"remote_button_quadruple_press", SUPLA_ACTION_CAP_SHORT_PRESS_x4},
      {"remote_button_quintuple_press", SUPLA_ACTION_CAP_SHORT_PRESS_x5},
      {"remote_button_long_press", SUPLA_ACTION_CAP_HOLD},
  };
  auto it = caps.find(type);
  return it == caps.end() ? 0 : it->second;
}

// Number of a "button_<n>" subtype, or -1.
int buttonNumber(const std::string &subtype) {
  if (subtype.rfind("button_", 0) != 0 || subtype.size() == 7) return -1;
  for (size_t i = 7; i < subtype.size(); i++) {
    if (!std::isdigit(static_cast<unsigned char>(subtype[i]))) return -1;
  }
  return std::atoi(subtype.c_str() + 7);
}

// Buttons and remotes: one action trigger channel per button (trigger
// subtype), "action" for devices with one button and "action_<n>" for more,
// as zigbee2mqtt names them.
void addActionBindings(const json &triggers, std::vector<Binding> *bindings) {
  if (!triggers.is_array()) return;
  std::vector<std::string> subtypes;  // in order of appearance
  std::map<std::string, Binding> buttons;
  for (const auto &t : triggers) {
    if (stringOr(t, "platform") != "device" || stringOr(t, "domain") != "zha") {
      continue;
    }
    std::string type = stringOr(t, "type");
    std::string subtype = stringOr(t, "subtype");
    uint32_t cap = triggerCap(type);
    if (cap == 0 || subtype.empty()) continue;
    if (buttons.find(subtype) == buttons.end()) subtypes.push_back(subtype);
    Binding &b = buttons[subtype];
    b.spec.kind = ChannelKind::ActionTrigger;
    b.spec.actionCaps |= cap;
    b.triggerSubtype = subtype;
    b.triggers[type] = cap;
  }
  std::stable_sort(subtypes.begin(), subtypes.end(),
                   [](const std::string &a, const std::string &b) {
                     int na = buttonNumber(a), nb = buttonNumber(b);
                     if (na < 0 || nb < 0) return false;
                     return na < nb;
                   });
  for (const auto &subtype : subtypes) {
    Binding &b = buttons[subtype];
    if (subtypes.size() == 1) {
      b.spec.key = "action";
      b.spec.caption = "przycisk";
    } else {
      int n = buttonNumber(subtype);
      std::string name = n >= 0 ? std::to_string(n) : subtype;
      b.spec.key = "action_" + name;
      b.spec.caption = "przycisk " + name;
    }
    bindings->push_back(b);
  }
}

// Heating thermostat: thermometer + thermostat on the climate entity, with
// the keys of zigbee2mqtt.
void addThermostatBindings(const Entity &climate,
                           std::vector<Binding> *bindings) {
  const json &attrs = attributes(*climate.state);
  Binding thermometer;
  thermometer.spec.kind = ChannelKind::Thermometer;
  thermometer.spec.key = "local_temperature";
  thermometer.spec.caption = "termometr";
  thermometer.entityId = climate.id;
  bindings->push_back(thermometer);

  Binding b;
  b.spec.kind = ChannelKind::Thermostat;
  b.spec.key = "current_heating_setpoint";
  b.spec.caption = "termostat";
  b.spec.thermometerKey = thermometer.spec.key;
  b.spec.setpointMin = attrs.value("min_temp", NAN);
  b.spec.setpointMax = attrs.value("max_temp", NAN);
  b.spec.setpointStep = attrs.value("target_temp_step", NAN);
  b.entityId = climate.id;
  b.canTurnOff = hasMode(attrs, "hvac_modes", "off");
  b.presetManual = findPreset(attrs, {"manual", "hold", "none"});
  b.presetProgram = findPreset(attrs, {"schedule", "program"});
  bindings->push_back(b);
}

void findBattery(const std::vector<Entity> &entities, Device *device) {
  for (const auto &e : entities) {
    if (e.deviceClass != "battery") continue;
    if (e.domain == "sensor" && device->batteryEntityId.empty()) {
      device->batteryEntityId = e.id;
    } else if (e.domain == "binary_sensor" &&
               device->batteryLowEntityId.empty()) {
      device->batteryLowEntityId = e.id;
    }
  }
}

const json *zhaDevice(const Snapshot &snapshot, const std::string &ieee) {
  if (!snapshot.zhaDevices.is_array()) return nullptr;
  for (const auto &d : snapshot.zhaDevices) {
    if (stringOr(d, "ieee") == ieee) return &d;
  }
  return nullptr;
}

const char *unsupportedKind(const std::vector<Entity> &entities) {
  for (const auto &e : entities) {
    if (!isMainEntity(e)) continue;
    if (e.domain == "cover") return "cover";
    if (e.domain == "lock") return "lock";
    if (e.domain == "fan") return "fan";
    if (e.domain == "climate" && !isHeatingClimate(e)) return "thermostat";
  }
  return nullptr;
}

std::optional<Device> parseDevice(
    const Snapshot &snapshot, const json &dev,
    const std::map<std::string, const json *> &states) {
  std::string ieee = zhaIeee(dev);
  if (ieee.empty() || !dev.value("disabled_by", json()).is_null()) {
    return std::nullopt;
  }
  const json *zd = zhaDevice(snapshot, ieee);
  if (zd != nullptr && stringOr(*zd, "device_type") == "Coordinator") {
    return std::nullopt;
  }
  Device device;
  device.haDeviceId = stringOr(dev, "id");
  device.descriptor.id = deviceIdFromIeee(ieee);
  device.descriptor.name = stringOr(dev, "name_by_user");
  if (device.descriptor.name.empty()) {
    device.descriptor.name = stringOr(dev, "name");
  }
  device.descriptor.manufacturer = stringOr(dev, "manufacturer");
  device.descriptor.model = stringOr(dev, "model");
  device.descriptor.softVersion = stringOr(dev, "sw_version");

  std::vector<Entity> entities =
      deviceEntities(snapshot, device.haDeviceId, states);
  if (unsupportedKind(entities) != nullptr) return std::nullopt;
  findBattery(entities, &device);
  if (!device.batteryEntityId.empty() || !device.batteryLowEntityId.empty()) {
    device.descriptor.batteryPowered = 1;
  } else if (zd != nullptr &&
             stringOr(*zd, "power_source").rfind("Mains", 0) == 0) {
    device.descriptor.batteryPowered = 0;
  }

  // A thermostat is bridged as its thermometer and thermostat only, as with
  // zigbee2mqtt: its other entities are settings of the device itself.
  for (const auto &e : entities) {
    if (isMainEntity(e) && isHeatingClimate(e)) {
      addThermostatBindings(e, &device.bindings);
      for (const auto &b : device.bindings) {
        device.descriptor.channels.push_back(b.spec);
      }
      return device;
    }
  }

  int relayCount = 0;
  for (const auto &e : entities) {
    if (isMainEntity(e) && (e.domain == "switch" || e.domain == "light")) {
      relayCount++;
    }
  }
  std::set<std::string> keys;
  for (const auto &e : entities) {
    if (!isMainEntity(e)) continue;
    if (e.domain == "switch" || e.domain == "light") {
      Binding b;
      b.spec.kind = ChannelKind::Relay;
      b.spec.key =
          relayCount > 1 ? "state_l" + std::to_string(e.endpoint) : "state";
      b.spec.defaultFunction = e.domain == "light"
                                   ? SUPLA_CHANNELFNC_LIGHTSWITCH
                                   : SUPLA_CHANNELFNC_POWERSWITCH;
      b.spec.caption = relayCaption(e.domain, e.endpoint, relayCount > 1);
      b.entityId = e.id;
      if (keys.insert(b.spec.key).second) device.bindings.push_back(b);
    } else if (e.domain == "binary_sensor") {
      auto it = binarySensorKinds().find(e.deviceClass);
      if (it == binarySensorKinds().end()) continue;
      Binding b;
      b.spec.kind = ChannelKind::BinarySensor;
      b.spec.key = it->second.key;
      b.spec.defaultFunction = it->second.function;
      b.spec.caption = it->second.caption;
      b.entityId = e.id;
      b.invert = it->second.invert;
      if (keys.insert(b.spec.key).second) device.bindings.push_back(b);
    } else if (e.domain == "sensor" && e.deviceClass == "pressure") {
      Binding b;
      b.spec.kind = ChannelKind::Pressure;
      b.spec.key = "pressure";
      b.spec.caption = "ciśnienie";
      b.entityId = e.id;
      if (keys.insert(b.spec.key).second) device.bindings.push_back(b);
    }
  }

  const Entity *temperature = sensor(entities, "temperature");
  const Entity *humidity = sensor(entities, "humidity");
  if (temperature != nullptr && humidity != nullptr) {
    Binding b;
    b.spec.kind = ChannelKind::TempHumidity;
    b.spec.key = "temperature+humidity";
    b.spec.caption = "temperatura i wilgotność";
    b.entityId = temperature->id;
    b.secondaryEntityId = humidity->id;
    device.bindings.push_back(b);
  } else if (temperature != nullptr) {
    Binding b;
    b.spec.kind = ChannelKind::Thermometer;
    b.spec.key = "temperature";
    b.spec.caption = "termometr";
    b.entityId = temperature->id;
    device.bindings.push_back(b);
  } else if (humidity != nullptr) {
    Binding b;
    b.spec.kind = ChannelKind::Humidity;
    b.spec.key = "humidity";
    b.spec.caption = "wilgotność";
    b.entityId = humidity->id;
    device.bindings.push_back(b);
  }

  // Energy metering, as with zigbee2mqtt: an electricity meter when the
  // energy is known, otherwise power, voltage and current as measurements
  // (mains powered devices only: battery devices report their battery
  // voltage).
  if (const Entity *energy = sensor(entities, "energy")) {
    Binding b;
    b.spec.kind = ChannelKind::ElectricityMeter;
    b.spec.key = "energy";
    b.spec.caption = "licznik energii";
    b.entityId = energy->id;
    if (const Entity *e = sensor(entities, "power")) b.powerEntityId = e->id;
    if (const Entity *e = sensor(entities, "voltage")) {
      b.voltageEntityId = e->id;
    }
    if (const Entity *e = sensor(entities, "current")) {
      b.currentEntityId = e->id;
    }
    device.bindings.push_back(b);
  } else if (device.descriptor.batteryPowered != 1) {
    for (const Measurement *m : {&kPower, &kVoltage, &kCurrent}) {
      if (const Entity *e = sensor(entities, m->deviceClass)) {
        device.bindings.push_back(measurementBinding(*m, e->id));
      }
    }
  }
  for (const Measurement *m : {&kIlluminance, &kFlow}) {
    if (const Entity *e = sensor(entities, m->deviceClass)) {
      device.bindings.push_back(measurementBinding(*m, e->id));
    }
  }

  auto triggers = snapshot.triggers.find(device.haDeviceId);
  if (triggers != snapshot.triggers.end()) {
    addActionBindings(triggers->second, &device.bindings);
  }

  if (device.bindings.empty()) return std::nullopt;
  for (const auto &b : device.bindings) {
    device.descriptor.channels.push_back(b.spec);
  }
  return device;
}

double parseNumber(const json &value) {
  if (value.is_number()) return value.get<double>();
  if (!value.is_string()) return NAN;
  const std::string s = value.get<std::string>();
  if (s.empty()) return NAN;
  char *end = nullptr;
  double v = std::strtod(s.c_str(), &end);
  if (end == nullptr || *end != '\0') return NAN;
  return v;
}

// Converts a measurement in the unit set in Home Assistant (users can change
// it) to the unit used by Supla. NaN for an unknown unit.
double toSuplaUnit(double value, const std::string &deviceClass,
                   const std::string &unit) {
  if (std::isnan(value) || unit.empty()) return value;
  struct Unit {
    const char *deviceClass;
    const char *unit;
    double factor;
    double offset;
  };
  static const Unit units[] = {
      {"temperature", "°C", 1, 0},
      {"temperature", "°F", 5.0 / 9.0, -32 * 5.0 / 9.0},
      {"temperature", "K", 1, -273.15},
      {"humidity", "%", 1, 0},
      {"pressure", "hPa", 1, 0},
      {"pressure", "mbar", 1, 0},
      {"pressure", "kPa", 10, 0},
      {"pressure", "Pa", 0.01, 0},
      {"pressure", "bar", 1000, 0},
      {"pressure", "cbar", 10, 0},
      {"pressure", "inHg", 33.8639, 0},
      {"pressure", "mmHg", 1.33322, 0},
      {"pressure", "psi", 68.9476, 0},
      {"illuminance", "lx", 1, 0},
      {"volume_flow_rate", "m³/h", 1, 0},
      {"volume_flow_rate", "m³/s", 3600, 0},
      {"volume_flow_rate", "L/h", 0.001, 0},
      {"volume_flow_rate", "L/min", 0.06, 0},
      {"volume_flow_rate", "L/s", 3.6, 0},
      {"energy", "kWh", 1, 0},
      {"energy", "Wh", 0.001, 0},
      {"energy", "MWh", 1000, 0},
      {"power", "W", 1, 0},
      {"power", "kW", 1000, 0},
      {"power", "mW", 0.001, 0},
      {"voltage", "V", 1, 0},
      {"voltage", "mV", 0.001, 0},
      {"current", "A", 1, 0},
      {"current", "mA", 0.001, 0},
  };
  for (const auto &u : units) {
    if (deviceClass == u.deviceClass && unit == u.unit) {
      return value * u.factor + u.offset;
    }
  }
  return NAN;
}

// Measured value of a sensor state object, in the Supla unit.
double sensorValue(const json &state) {
  const json &attrs = attributes(state);
  return toSuplaUnit(parseNumber(state.value("state", json())),
                     stringOr(attrs, "device_class"),
                     stringOr(attrs, "unit_of_measurement"));
}

}  // namespace

std::string deviceIdFromIeee(const std::string &ieee) {
  std::string id = "0x";
  for (char c : ieee) {
    if (c != ':') {
      id += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
  }
  return id;
}

const char *unsupportedDeviceKind(const Snapshot &snapshot,
                                  const std::string &haDeviceId) {
  return unsupportedKind(
      deviceEntities(snapshot, haDeviceId, indexStates(snapshot)));
}

std::vector<Device> parseDevices(const Snapshot &snapshot) {
  std::vector<Device> result;
  if (!snapshot.deviceRegistry.is_array()) return result;
  auto states = indexStates(snapshot);
  for (const auto &dev : snapshot.deviceRegistry) {
    if (!dev.is_object()) continue;
    if (auto device = parseDevice(snapshot, dev, states)) {
      result.push_back(std::move(*device));
    }
  }
  return result;
}

std::vector<std::string> zhaDeviceIds(const Snapshot &snapshot) {
  std::vector<std::string> ids;
  if (!snapshot.deviceRegistry.is_array()) return ids;
  for (const auto &dev : snapshot.deviceRegistry) {
    std::string ieee = zhaIeee(dev);
    if (ieee.empty()) continue;
    const json *zd = zhaDevice(snapshot, ieee);
    if (zd != nullptr && stringOr(*zd, "device_type") == "Coordinator") {
      continue;
    }
    ids.push_back(stringOr(dev, "id"));
  }
  return ids;
}

std::vector<std::pair<std::string, ChannelState>> extractStates(
    const Device &device, const json &state) {
  std::vector<std::pair<std::string, ChannelState>> result;
  const std::string entityId = stringOr(state, "entity_id");
  const std::string value = stringOr(state, "state");
  if (entityId.empty() || value == "unavailable" || value == "unknown") {
    return result;
  }
  const json &attrs = attributes(state);
  for (const auto &b : device.bindings) {
    ChannelState s;
    switch (b.spec.kind) {
      case ChannelKind::Relay:
      case ChannelKind::BinarySensor:
        if (entityId != b.entityId) continue;
        if (value == "on") {
          s.primary = b.invert ? 0 : 1;
        } else if (value == "off") {
          s.primary = b.invert ? 1 : 0;
        } else {
          continue;
        }
        break;
      case ChannelKind::Thermometer:
        if (entityId != b.entityId) continue;
        if (domainOf(entityId) == "climate") {
          s.primary = parseNumber(attrs.value("current_temperature", json()));
        } else {
          s.primary = sensorValue(state);
        }
        if (std::isnan(s.primary)) continue;
        break;
      case ChannelKind::Humidity:
      case ChannelKind::Pressure:
      case ChannelKind::GeneralPurposeMeasurement:
        if (entityId != b.entityId) continue;
        s.primary = sensorValue(state);
        if (std::isnan(s.primary)) continue;
        break;
      case ChannelKind::TempHumidity:
        if (entityId == b.entityId) {
          s.primary = sensorValue(state);
        } else if (entityId == b.secondaryEntityId) {
          s.secondary = sensorValue(state);
        } else {
          continue;
        }
        if (isEmpty(s)) continue;
        break;
      case ChannelKind::ElectricityMeter:
        if (entityId == b.entityId) {
          s.primary = sensorValue(state);
        } else if (entityId == b.powerEntityId) {
          s.power = sensorValue(state);
        } else if (entityId == b.voltageEntityId) {
          s.voltage = sensorValue(state);
        } else if (entityId == b.currentEntityId) {
          s.current = sensorValue(state);
        } else {
          continue;
        }
        if (isEmpty(s)) continue;
        break;
      case ChannelKind::Thermostat: {
        if (entityId != b.entityId) continue;
        s.primary = parseNumber(attrs.value("temperature", json()));
        s.mode = value == "off" ? 0 : 1;
        std::string preset = stringOr(attrs, "preset_mode");
        if (!b.presetProgram.empty() && !preset.empty()) {
          s.program = preset == b.presetProgram ? 1 : 0;
        }
        std::string action = stringOr(attrs, "hvac_action");
        if (!action.empty()) s.heating = action == "heating" ? 1 : 0;
        break;
      }
      case ChannelKind::ActionTrigger:
        continue;  // device triggers, not states
    }
    result.emplace_back(b.spec.key, s);
  }
  return result;
}

std::optional<DeviceHealth> extractHealth(const Device &device,
                                          const json &state) {
  const std::string entityId = stringOr(state, "entity_id");
  const std::string value = stringOr(state, "state");
  DeviceHealth health;
  if (!device.batteryEntityId.empty() && entityId == device.batteryEntityId) {
    double level = parseNumber(state.value("state", json()));
    if (std::isnan(level)) return std::nullopt;
    health.batteryLevel =
        static_cast<int>(std::lround(std::clamp(level, 0.0, 100.0)));
    return health;
  }
  if (!device.batteryLowEntityId.empty() &&
      entityId == device.batteryLowEntityId &&
      (value == "on" || value == "off")) {
    health.batteryLow = value == "on" ? 1 : 0;
    return health;
  }
  return std::nullopt;
}

bool isAvailable(const json &state) {
  return stringOr(state, "state") != "unavailable";
}

std::vector<std::string> entityIds(const Device &device) {
  std::set<std::string> ids;
  for (const auto &b : device.bindings) {
    for (const std::string *id :
         {&b.entityId, &b.secondaryEntityId, &b.powerEntityId,
          &b.voltageEntityId, &b.currentEntityId}) {
      if (!id->empty()) ids.insert(*id);
    }
  }
  if (!device.batteryEntityId.empty()) ids.insert(device.batteryEntityId);
  if (!device.batteryLowEntityId.empty()) {
    ids.insert(device.batteryLowEntityId);
  }
  return {ids.begin(), ids.end()};
}

std::vector<ServiceCall> buildCommand(const Device &device,
                                      const std::string &channelKey,
                                      const ChannelCommand &command) {
  for (const auto &b : device.bindings) {
    if (b.spec.key != channelKey) continue;
    const json target = {{"entity_id", b.entityId}};
    if (b.spec.kind == ChannelKind::Thermostat &&
        command.type == ChannelCommand::Type::SetThermostat) {
      // In order: the mode, leaving the device's own schedule (which could
      // restore another setpoint), the setpoint.
      std::vector<ServiceCall> calls;
      if (command.mode == 1) {
        if (!b.canTurnOff) return {};  // the device cannot be switched off
        json data = target;
        data["hvac_mode"] = "off";
        calls.push_back({"climate", "set_hvac_mode", data});
      } else if (command.mode == 2) {
        json data = target;
        data["hvac_mode"] = "heat";
        calls.push_back({"climate", "set_hvac_mode", data});
      }
      if (command.program) {
        if (b.presetProgram.empty()) return {};  // no own schedule
        json data = target;
        data["preset_mode"] = b.presetProgram;
        calls.push_back({"climate", "set_preset_mode", data});
      } else if (command.manual && !b.presetManual.empty()) {
        json data = target;
        data["preset_mode"] = b.presetManual;
        calls.push_back({"climate", "set_preset_mode", data});
      }
      if (!std::isnan(command.setpoint)) {
        json data = target;
        data["temperature"] = command.setpoint;
        calls.push_back({"climate", "set_temperature", data});
      }
      return calls;
    }
    if (b.spec.kind != ChannelKind::Relay ||
        command.type == ChannelCommand::Type::SetThermostat) {
      continue;
    }
    bool on = command.type == ChannelCommand::Type::TurnOn;
    return {{domainOf(b.entityId), on ? "turn_on" : "turn_off", target}};
  }
  return {};
}

}  // namespace z2s::zha
