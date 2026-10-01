// SPDX-License-Identifier: GPL-2.0-or-later

#include "ha_duplicates.h"

#include <gtest/gtest.h>

using namespace z2s::ha;
using json = nlohmann::json;

namespace {

json registry() {
  return json::parse(R"([
    {"id": "d1", "name": "Salon/czujnik", "sw_version": "z2s 0.1.0",
     "manufacturer": "Unknown", "disabled_by": null,
     "identifiers": [["mqtt", "supla-iodevice-101"]]},
    {"id": "d2", "name": "Salon/czujnik", "sw_version": "3000-0001",
     "manufacturer": "Aqara", "disabled_by": null,
     "identifiers": [["mqtt", "zigbee2mqtt_0x00158d0001a2b3c4"]]},
    {"id": "d3", "name": "ZAMEL ROW-01", "sw_version": "2.8.53",
     "manufacturer": "Zamel", "disabled_by": null,
     "identifiers": [["mqtt", "supla-iodevice-7"]]},
    {"id": "d4", "name": "Drzwi", "sw_version": "z2s 0.1.0",
     "disabled_by": "user",
     "identifiers": [["mqtt", "supla-iodevice-102"]]},
    {"id": "d5", "name": "Lampa", "name_by_user": "Lampa w sypialni",
     "sw_version": "z2s 0.2.0", "disabled_by": null,
     "identifiers": [["other", "x"], ["mqtt", "supla-iodevice-103"]]},
    {"id": "d6", "name": "Fake", "sw_version": "z2s 0.1.0",
     "disabled_by": null, "identifiers": [["mqtt", "zigbee2mqtt_x"]]},
    {"id": "d7", "name": "Broken", "sw_version": null, "identifiers": null}
  ])");
}

}  // namespace

TEST(HaDuplicatesTest, SelectsOnlyEnabledSuplaCopiesOfGatewayDevices) {
  auto selected = selectDevicesToDisable(registry(), {});
  ASSERT_EQ(selected.size(), 2u);
  EXPECT_EQ(selected[0].id, "d1");
  EXPECT_EQ(selected[1].id, "d5");
  EXPECT_EQ(selected[1].name, "Lampa w sypialni");
}

TEST(HaDuplicatesTest, SkipsDevicesHandledBefore) {
  // d1 was disabled before and enabled again by the user: leave it alone.
  auto selected = selectDevicesToDisable(registry(), {"d1"});
  ASSERT_EQ(selected.size(), 1u);
  EXPECT_EQ(selected[0].id, "d5");
}

TEST(HaDuplicatesTest, InvalidInput) {
  EXPECT_TRUE(selectDevicesToDisable(json::object(), {}).empty());
  EXPECT_FALSE(isSuplaDeviceFromGateway(json("x")));
}
