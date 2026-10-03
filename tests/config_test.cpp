// SPDX-License-Identifier: GPL-2.0-or-later

#include "config.h"

#include <gtest/gtest.h>
#include <unistd.h>

#include <cstdio>
#include <map>
#include <string>

using namespace z2s;

namespace {

class ConfigTest : public ::testing::Test {
 protected:
  void SetUp() override {
    char tmpl[] = "/tmp/z2s_config_XXXXXX";
    dir_ = mkdtemp(tmpl);
  }
  void TearDown() override {
    for (const auto &f : files_) unlink(f.c_str());
    rmdir(dir_.c_str());
  }

  std::string writeFile(const std::string &name, const std::string &content) {
    std::string path = dir_ + "/" + name;
    FILE *f = fopen(path.c_str(), "w");
    fputs(content.c_str(), f);
    fclose(f);
    files_.push_back(path);
    return path;
  }

  bool load(const std::string &path, AppConfig *config, std::string *error) {
    return loadConfig(
        path,
        [this](const char *name) -> const char * {
          auto it = env_.find(name);
          return it == env_.end() ? nullptr : it->second.c_str();
        },
        config, error);
  }

  std::string dir_;
  std::vector<std::string> files_;
  std::map<std::string, std::string> env_;
};

}  // namespace

TEST_F(ConfigTest, FileOnly) {
  auto path = writeFile("c.json", R"({"supla_server":"svr1.supla.org",
      "supla_email":"a@b.pl","mqtt_port":1884,"include":["x"]})");
  AppConfig config;
  std::string error;
  ASSERT_TRUE(load(path, &config, &error)) << error;
  EXPECT_EQ(config.gateway.supla.server, "svr1.supla.org");
  EXPECT_EQ(config.mqtt.port, 1884);
  EXPECT_EQ(config.gateway.include, std::vector<std::string>{"x"});
  EXPECT_EQ(config.tls.securityLevel, 0);
  EXPECT_EQ(config.mqtt.baseTopic, "zigbee2mqtt");
  // Two instances on one broker must not share the client id.
  char host[256] = {};
  gethostname(host, sizeof(host) - 1);
  EXPECT_EQ(config.mqtt.clientId, std::string("zigbee2supla-") + host);
}

TEST_F(ConfigTest, MqttClientIdCanBeSet) {
  env_ = {{"Z2S_SUPLA_SERVER", "s"},
          {"Z2S_SUPLA_EMAIL", "a@b.pl"},
          {"Z2S_MQTT_CLIENT_ID", "z2s-test"}};
  AppConfig config;
  std::string error;
  ASSERT_TRUE(load("", &config, &error)) << error;
  EXPECT_EQ(config.mqtt.clientId, "z2s-test");
}

TEST_F(ConfigTest, ZhaSourceNeedsHomeAssistant) {
  env_ = {{"Z2S_SUPLA_SERVER", "s"}, {"Z2S_SUPLA_EMAIL", "a@b.pl"}};
  AppConfig config;
  std::string error;
  ASSERT_TRUE(load("", &config, &error)) << error;
  EXPECT_EQ(config.source, "z2m");

  env_["Z2S_SOURCE"] = "deconz";
  EXPECT_FALSE(load("", &config, &error));
  EXPECT_NE(error.find("source"), std::string::npos);

  env_["Z2S_SOURCE"] = "zha";
  EXPECT_FALSE(load("", &config, &error));
  EXPECT_NE(error.find("ha_websocket_url"), std::string::npos);
  env_["Z2S_HA_WEBSOCKET_URL"] = "ws://ha.local:8123/api/websocket";
  EXPECT_FALSE(load("", &config, &error));
  EXPECT_NE(error.find("ha_token"), std::string::npos);
  env_["Z2S_HA_TOKEN"] = "t";
  ASSERT_TRUE(load("", &config, &error)) << error;
  EXPECT_EQ(config.source, "zha");
  EXPECT_EQ(config.zha.websocketUrl, "ws://ha.local:8123/api/websocket");
  EXPECT_EQ(config.zha.token, "t");
  // The duplicates option is independent of the source.
  EXPECT_FALSE(config.ha.enabled);
}

TEST_F(ConfigTest, EnvironmentOnlyWithoutFile) {
  env_ = {{"Z2S_SUPLA_SERVER", "supla-server"},
          {"Z2S_SUPLA_EMAIL", "a@b.pl"},
          {"Z2S_SUPLA_SECURITY_LEVEL", "3"},
          {"Z2S_SUPLA_CA_FILE", "/etc/supla-server-ssl/cert.crt"},
          {"Z2S_MQTT_PORT", " 1885 "},
          {"Z2S_DISABLE_TLS", "no"},
          {"Z2S_EXCLUDE", "Lampa, 0x0011 ,,"},
          {"Z2S_STATE_DIR", "/data"}};
  AppConfig config;
  std::string error;
  ASSERT_TRUE(load("", &config, &error)) << error;
  EXPECT_EQ(config.gateway.supla.server, "supla-server");
  EXPECT_EQ(config.tls.securityLevel, 3);
  EXPECT_EQ(config.tls.caFile, "/etc/supla-server-ssl/cert.crt");
  EXPECT_EQ(config.mqtt.port, 1885);
  EXPECT_FALSE(config.tls.disableTls);
  EXPECT_EQ(config.gateway.exclude,
            (std::vector<std::string>{"Lampa", "0x0011"}));
  EXPECT_EQ(config.stateDir, "/data");
}

TEST_F(ConfigTest, EnvironmentOverridesFile) {
  auto path = writeFile("c.json", R"({"supla_server":"svr1.supla.org",
      "supla_email":"a@b.pl","mqtt_host":"file-host"})");
  env_ = {{"Z2S_MQTT_HOST", "env-host"}};
  AppConfig config;
  std::string error;
  ASSERT_TRUE(load(path, &config, &error)) << error;
  EXPECT_EQ(config.mqtt.host, "env-host");
  EXPECT_EQ(config.gateway.supla.server, "svr1.supla.org");
}

TEST_F(ConfigTest, SecretFromFile) {
  auto secret = writeFile("password", "s3cret\n");
  env_ = {{"Z2S_SUPLA_SERVER", "x"},
          {"Z2S_SUPLA_EMAIL", "a@b.pl"},
          {"Z2S_MQTT_PASSWORD", "ignored"},
          {"Z2S_MQTT_PASSWORD_FILE", secret}};
  AppConfig config;
  std::string error;
  ASSERT_TRUE(load("", &config, &error)) << error;
  EXPECT_EQ(config.mqtt.password, "s3cret");

  env_["Z2S_MQTT_PASSWORD_FILE"] = dir_ + "/missing";
  EXPECT_FALSE(load("", &config, &error));
  EXPECT_NE(error.find("Z2S_MQTT_PASSWORD_FILE"), std::string::npos);
}

TEST_F(ConfigTest, InvalidValuesAreReported) {
  env_ = {{"Z2S_SUPLA_SERVER", "x"},
          {"Z2S_SUPLA_EMAIL", "a@b.pl"},
          {"Z2S_MQTT_PORT", "18x"}};
  AppConfig config;
  std::string error;
  EXPECT_FALSE(load("", &config, &error));
  EXPECT_NE(error.find("Z2S_MQTT_PORT"), std::string::npos);

  env_["Z2S_MQTT_PORT"] = "1883";
  env_["Z2S_DISABLE_TLS"] = "maybe";
  EXPECT_FALSE(load("", &config, &error));
  EXPECT_NE(error.find("Z2S_DISABLE_TLS"), std::string::npos);
}

TEST_F(ConfigTest, Validation) {
  AppConfig config;
  std::string error;
  EXPECT_FALSE(load("", &config, &error));
  EXPECT_NE(error.find("supla_server"), std::string::npos);

  env_ = {{"Z2S_SUPLA_SERVER", "x"},
          {"Z2S_SUPLA_EMAIL", "a@b.pl"},
          {"Z2S_SUPLA_SECURITY_LEVEL", "3"}};
  EXPECT_FALSE(load("", &config, &error));
  EXPECT_NE(error.find("supla_ca_file"), std::string::npos);

  env_["Z2S_SUPLA_SECURITY_LEVEL"] = "4";
  EXPECT_FALSE(load("", &config, &error));

  env_["Z2S_SUPLA_SECURITY_LEVEL"] = "0";
  env_["Z2S_SUPLA_PROTO_VERSION"] = "25";
  EXPECT_FALSE(load("", &config, &error));
  EXPECT_NE(error.find("supla_proto_version"), std::string::npos);

  EXPECT_FALSE(load(dir_ + "/missing.json", &config, &error));
}

TEST_F(ConfigTest, YamlFile) {
  auto path = writeFile("c.yaml", R"(# comment
supla_server: svr1.supla.org
supla_email: "a@b.pl"
supla_security_level: 2   # trailing comment
mqtt_port: 1884
mqtt_password: 0123       # a string option keeps its text form
disable_tls: yes
include:
  - Lampa
  - "Salon/czujnik"
exclude: []
unknown_option: ignored
)");
  AppConfig config;
  std::string error;
  ASSERT_TRUE(load(path, &config, &error)) << error;
  EXPECT_EQ(config.gateway.supla.server, "svr1.supla.org");
  EXPECT_EQ(config.gateway.supla.email, "a@b.pl");
  EXPECT_EQ(config.tls.securityLevel, 2);
  EXPECT_EQ(config.mqtt.port, 1884);
  EXPECT_EQ(config.mqtt.password, "0123");
  EXPECT_TRUE(config.tls.disableTls);
  EXPECT_EQ(config.gateway.include,
            (std::vector<std::string>{"Lampa", "Salon/czujnik"}));
  EXPECT_TRUE(config.gateway.exclude.empty());
  EXPECT_EQ(config.mqtt.baseTopic, "zigbee2mqtt");

  // The environment overrides a YAML file as well; .yml is accepted too.
  env_ = {{"Z2S_MQTT_PORT", "1999"}};
  auto yml = writeFile(
      "c.yml", "supla_server: x\nsupla_email: a@b.pl\ninclude: One, Two\n");
  AppConfig second;
  ASSERT_TRUE(load(yml, &second, &error)) << error;
  EXPECT_EQ(second.mqtt.port, 1999);
  EXPECT_EQ(second.gateway.include, (std::vector<std::string>{"One", "Two"}));
}

TEST_F(ConfigTest, InvalidYamlIsReported) {
  AppConfig config;
  std::string error;

  EXPECT_FALSE(load(dir_ + "/missing.yaml", &config, &error));
  EXPECT_NE(error.find("cannot open"), std::string::npos);

  EXPECT_FALSE(
      load(writeFile("a.yaml", "supla_server: [unclosed\n"), &config, &error));
  EXPECT_NE(error.find("invalid YAML"), std::string::npos);

  EXPECT_FALSE(load(writeFile("b.yaml", "- a\n- b\n"), &config, &error));
  EXPECT_NE(error.find("mapping"), std::string::npos);

  const std::string base = "supla_server: x\nsupla_email: a@b.pl\n";
  EXPECT_FALSE(
      load(writeFile("c.yaml", base + "mqtt_port: 18x\n"), &config, &error));
  EXPECT_NE(error.find("mqtt_port"), std::string::npos);

  EXPECT_FALSE(load(writeFile("d.yaml", base + "mqtt_host:\n  nested: 1\n"),
                    &config, &error));
  EXPECT_NE(error.find("mqtt_host"), std::string::npos);

  EXPECT_FALSE(load(writeFile("e.yaml", base + "include:\n  - [a, b]\n"),
                    &config, &error));
  EXPECT_NE(error.find("include"), std::string::npos);

  // An empty file is valid YAML; required options are then missing.
  EXPECT_FALSE(load(writeFile("f.yaml", ""), &config, &error));
  EXPECT_NE(error.find("supla_server"), std::string::npos);
}
