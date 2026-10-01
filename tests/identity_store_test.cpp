// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <string>

#include "z2s/identity_store.h"

using namespace z2s;

namespace {

RandomBytesFn counterRandom() {
  auto counter = std::make_shared<uint8_t>(0);
  return [counter](uint8_t *buf, size_t len) {
    for (size_t i = 0; i < len; i++) buf[i] = ++(*counter);
    return true;
  };
}

DeviceDescriptor makeDevice(std::vector<std::string> keys) {
  DeviceDescriptor d;
  d.id = "0x0011223344556677";
  d.name = "Test";
  for (auto &k : keys) {
    ChannelSpec spec;
    spec.kind = ChannelKind::Relay;
    spec.key = k;
    d.channels.push_back(spec);
  }
  return d;
}

class IdentityStoreTest : public ::testing::Test {
 protected:
  void SetUp() override {
    char tmpl[] = "/tmp/z2s_identity_XXXXXX";
    dir_ = mkdtemp(tmpl);
    path_ = dir_ + "/identities.json";
  }
  void TearDown() override {
    unlink(path_.c_str());
    rmdir(dir_.c_str());
  }
  std::string dir_;
  std::string path_;
};

}  // namespace

TEST_F(IdentityStoreTest, CreatesIdentityAndKeepsItAcrossRestarts) {
  std::array<uint8_t, 16> guid;
  {
    IdentityStore store(path_, counterRandom());
    ASSERT_TRUE(store.load());
    bool changed = false;
    auto *id = store.reconcile(makeDevice({"state_l1", "state_l2"}), &changed);
    ASSERT_NE(id, nullptr);
    EXPECT_TRUE(changed);
    EXPECT_NE(id->guid, id->authKey);
    guid = id->guid;
    ASSERT_TRUE(store.save());
  }

  struct stat st;
  ASSERT_EQ(stat(path_.c_str(), &st), 0);
  EXPECT_EQ(st.st_mode & 0777, 0600);

  IdentityStore store(path_, [](uint8_t *, size_t) { return false; });
  ASSERT_TRUE(store.load());
  bool changed = true;
  auto *id = store.reconcile(makeDevice({"state_l1", "state_l2"}), &changed);
  ASSERT_NE(id, nullptr);
  EXPECT_FALSE(changed);
  EXPECT_EQ(id->guid, guid);
  ASSERT_EQ(id->channels.size(), 2u);
  EXPECT_EQ(id->channels[0].key, "state_l1");
  EXPECT_EQ(id->channels[1].key, "state_l2");
}

TEST_F(IdentityStoreTest, ChannelLayoutIsAppendOnly) {
  IdentityStore store(path_, counterRandom());
  bool changed;
  store.reconcile(makeDevice({"a", "b"}), &changed);
  // "a" disappears, "c" is new: "a" keeps channel 0, "c" is appended.
  auto *id = store.reconcile(makeDevice({"c", "b"}), &changed);
  EXPECT_TRUE(changed);
  ASSERT_EQ(id->channels.size(), 3u);
  EXPECT_EQ(id->channels[0].key, "a");
  EXPECT_EQ(id->channels[1].key, "b");
  EXPECT_EQ(id->channels[2].key, "c");
}

TEST_F(IdentityStoreTest, CorruptedFileFailsToLoad) {
  FILE *f = fopen(path_.c_str(), "w");
  fputs("{not json", f);
  fclose(f);
  IdentityStore store(path_, counterRandom());
  EXPECT_FALSE(store.load());
}

TEST_F(IdentityStoreTest, RandomFailureReturnsNull) {
  IdentityStore store(path_, [](uint8_t *, size_t) { return false; });
  bool changed;
  EXPECT_EQ(store.reconcile(makeDevice({"a"}), &changed), nullptr);
}
