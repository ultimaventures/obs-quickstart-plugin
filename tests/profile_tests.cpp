// Unit tests for the pure (non-OBS) parts of src/profile/profile-module.cpp.
//
// The test binary compiles the real module TU and links the real Qt,
// libobs and obs-frontend-api, but there is no OBS runtime here, so the OBS
// C functions touched by the tested paths are stubbed below. The stubs are
// plain extern "C" definitions in this TU: a definition in a linked object
// takes precedence over the same symbol from a shared library (interposition)
// or a static archive (the archive member is simply not pulled), so the pure
// functions under test never touch real OBS. The DeduplicatedName exhaustion
// test proves the blog stub — not libobs's real blog(), which is unsafe
// without an initialized OBS logging system — is the definition linked.

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator> // istreambuf_iterator
#include <map>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <obs-frontend-api.h>
#include <obs-module.h>       // obs_current_module (stubbed below)
#include <util/base.h>        // blog
#include <util/config-file.h> // config_get_string, config_get_uint

#include "profile-module.hpp"

namespace fs = std::filesystem;
namespace profile = obs_setup::profile;

namespace {

// ---- OBS stubs ------------------------------------------------------------

// Set by the blog stub; the exhaustion test asserts it, proving the stub
// below — not libobs's real blog() — is what got linked into the binary.
bool g_blogCalled = false;

extern "C" {

void blog(int, const char *, ...) { g_blogCalled = true; }

// obs_module_config_path() (used by backupExistingProfile()) expands to
// obs_module_get_config_path(obs_current_module(), ...). In current OBS
// headers obs_current_module is a per-plugin function (defined by
// OBS_DECLARE_MODULE in a real plugin), not a global, so the test binary
// defines the function here. It is never called — backupExistingProfile()
// is not under test — so it returns null.
obs_module_t *obs_current_module(void) { return nullptr; }

// Minimal fake backing the config_get_* stubs: snapshotOutputConfig() reads
// the wizard-owned keys through these, so tests drive the "after" values
// here while constructing the "before" snapshot directly.
struct FakeConfig {
  std::map<std::pair<std::string, std::string>, std::string> strings;
  std::map<std::pair<std::string, std::string>, uint64_t> uints;
};

FakeConfig g_fakeConfig;

config_t *obs_frontend_get_profile_config(void) {
  return reinterpret_cast<config_t *>(&g_fakeConfig);
}

const char *config_get_string(config_t *config, const char *section,
                              const char *name) {
  const auto *fake = reinterpret_cast<const FakeConfig *>(config);
  const auto it = fake->strings.find(
      std::make_pair(std::string(section), std::string(name)));
  return it != fake->strings.end() ? it->second.c_str() : nullptr;
}

uint64_t config_get_uint(config_t *config, const char *section,
                         const char *name) {
  const auto *fake = reinterpret_cast<const FakeConfig *>(config);
  const auto it =
      fake->uints.find(std::make_pair(std::string(section), std::string(name)));
  return it != fake->uints.end() ? it->second : 0;
}

} // extern "C"

// ---- filesystem test helpers (ported from tests/manual/filesystem-repro.cpp)

class FilesystemTest : public ::testing::Test {
protected:
  void SetUp() override {
    sandbox_ = fs::temp_directory_path() / "quickstart-profile-tests";
    std::error_code ec;
    fs::remove_all(sandbox_, ec);
    fs::create_directories(sandbox_, ec);
    ASSERT_FALSE(ec) << "could not create sandbox: " << ec.message();
  }

  void TearDown() override {
    std::error_code ec;
    fs::remove_all(sandbox_, ec);
  }

  static void writeFile(const fs::path &p, const std::string &content) {
    // Open via the path object itself, never via .u8string(): the narrow
    // string overload would re-decode with the ANSI code page on Windows.
    std::ofstream out(p, std::ios::binary);
    out << content;
  }

  static std::string readFile(const fs::path &p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)),
                       std::istreambuf_iterator<char>());
  }

  fs::path sandbox_;
};

} // namespace

// ---- copyDirectoryTree (ported from filesystem-repro cases 1, 2, 4, 6) ----

TEST_F(FilesystemTest, CreatesMissingDestinationParents) {
  const fs::path src = sandbox_ / "profile";
  fs::create_directories(src / "sub");
  writeFile(src / "basic.ini", "name=Quickstart\n");

  // Missing destination parents (reviewer round 3): the copy must create
  // them instead of throwing "No such file or directory".
  const std::string dest =
      (sandbox_ / "quickstart-backups" / "deep" / "missing" / "profile_123")
          .u8string();
  EXPECT_TRUE(profile::copyDirectoryTree(src.u8string(), dest));
  EXPECT_EQ(readFile(fs::u8path(dest) / "basic.ini"), "name=Quickstart\n");
}

TEST_F(FilesystemTest, RecursiveCopyPreservesStructureAndBytes) {
  const fs::path src = sandbox_ / "profile2";
  fs::create_directories(src / "a" / "b");
  writeFile(src / "basic.ini", "name=Quickstart\n");
  writeFile(src / "a" / "b" / "deep.txt", "deep-content");

  const fs::path dest = sandbox_ / "backup2" / "profile2_456";
  ASSERT_TRUE(profile::copyDirectoryTree(src.u8string(), dest.u8string()));
  EXPECT_EQ(readFile(dest / "basic.ini"), "name=Quickstart\n");
  EXPECT_EQ(readFile(dest / "a" / "b" / "deep.txt"), "deep-content");
}

TEST_F(FilesystemTest, OverwriteExistingDestination) {
  const fs::path src = sandbox_ / "profile3";
  fs::create_directories(src);
  writeFile(src / "basic.ini", "version=2\n");

  const fs::path dest = sandbox_ / "backup3";
  ASSERT_TRUE(profile::copyDirectoryTree(src.u8string(), dest.u8string()));
  writeFile(dest / "basic.ini", "version=1-stale\n");
  EXPECT_TRUE(profile::copyDirectoryTree(src.u8string(), dest.u8string()));
  EXPECT_EQ(readFile(dest / "basic.ini"), "version=2\n");
}

TEST_F(FilesystemTest, NonAsciiPathsRoundTrip) {
  // u8 literal with \u escape: MSVC's default execution charset is ANSI
  // without /utf-8, so a literal "é" would be wrong; the repo builds with
  // /utf-8 regardless, but the escape keeps this robust.
  const fs::path cafe = sandbox_ / fs::u8path(u8"Caf\u00e9");
  std::error_code ec;
  fs::create_directories(cafe, ec);
  ASSERT_FALSE(ec);

  writeFile(cafe / "note.txt", "caf\u00e9-data");
  EXPECT_EQ(readFile(cafe / "note.txt"), "caf\u00e9-data");

  // End-to-end: copy a profile tree into a non-ASCII backup destination.
  const fs::path dest =
      sandbox_ / "quickstart-backups" / fs::u8path(u8"Caf\u00e9_789");
  writeFile(cafe / "basic.ini", "name=Caf\u00e9\n");
  ASSERT_TRUE(profile::copyDirectoryTree(cafe.u8string(), dest.u8string()));
  EXPECT_EQ(readFile(dest / "basic.ini"), "name=Caf\u00e9\n");
}

// ---- backupDestinationFor ----
// The test MUST call the production function (round-6 regression
// requirement). Rebuilding the destination in-test would be tautological:
// it would still pass if production reverted to operator/(profileName +
// "_" + stamp) with locale decoding.

TEST_F(FilesystemTest, BackupDestinationKeepsNonAsciiNameIntact) {
  const std::string backupsDir = (sandbox_ / "quickstart-backups").u8string();
  const fs::path dest =
      profile::backupDestinationFor(backupsDir, u8"Caf\u00e9", "123");
  const fs::path expect =
      sandbox_ / "quickstart-backups" / fs::u8path(u8"Caf\u00e9_123");
  EXPECT_EQ(dest.u8string(), expect.u8string());
  // The leaf directory name round-trips byte-identical: this is the
  // regression test for the Windows ANSI-code-page bug (reviewer round 5).
  // Only the Windows CI leg can catch it — on Linux/macOS,
  // path(std::string) already decodes as UTF-8, so this passes with or
  // without u8path there.
  EXPECT_EQ(dest.filename().u8string(),
            fs::u8path(u8"Caf\u00e9_123").u8string());
}

TEST_F(FilesystemTest, BackupDestinationAscii) {
  const std::string backupsDir = (sandbox_ / "quickstart-backups").u8string();
  EXPECT_EQ(
      profile::backupDestinationFor(backupsDir, "MyProfile", "456").u8string(),
      (sandbox_ / "quickstart-backups" / "MyProfile_456").u8string());
}

// ---- deduplicatedName ----

TEST(DeduplicatedName, FreeNameUnchanged) {
  EXPECT_EQ(profile::deduplicatedName("Quickstart", {"Other", "Stuff"}),
            "Quickstart");
}

TEST(DeduplicatedName, TakenNameGetsNumericSuffix) {
  EXPECT_EQ(profile::deduplicatedName("Quickstart", {"Quickstart"}),
            "Quickstart 2");
  EXPECT_EQ(
      profile::deduplicatedName("Quickstart", {"Quickstart", "Quickstart 2"}),
      "Quickstart 3");
}

TEST(DeduplicatedName, ExhaustionReturnsEmpty) {
  std::vector<std::string> taken;
  taken.reserve(1001);
  taken.push_back("Quickstart");
  for (int i = 2; i <= 1000; ++i)
    taken.push_back("Quickstart " + std::to_string(i));
  g_blogCalled = false;
  EXPECT_EQ(profile::deduplicatedName("Quickstart", taken), "");
  // The exhaustion path logs via blog(): asserting the flag proves the
  // no-op stub above — not libobs's real blog() — is what got linked.
  EXPECT_TRUE(g_blogCalled);
}

// ---- outputConfigChanged ----
// The "before" snapshot is constructed directly; the "after" side comes from
// the FakeConfig stubs above. No OBS calls.

namespace {

void setFakeOutputConfig(const profile::OutputConfigSnapshot &snap) {
  g_fakeConfig.strings.clear();
  g_fakeConfig.uints.clear();
  g_fakeConfig.strings[{"Output", "Mode"}] = snap.outputMode;
  g_fakeConfig.uints[{"Video", "BaseCX"}] = snap.baseWidth;
  g_fakeConfig.uints[{"Video", "BaseCY"}] = snap.baseHeight;
  g_fakeConfig.uints[{"Video", "OutputCX"}] = snap.outputWidth;
  g_fakeConfig.uints[{"Video", "OutputCY"}] = snap.outputHeight;
  g_fakeConfig.strings[{"Video", "FPSCommon"}] = snap.fpsCommon;
  g_fakeConfig.uints[{"SimpleOutput", "VBitrate"}] = snap.simpleBitrate;
  g_fakeConfig.strings[{"SimpleOutput", "StreamEncoder"}] = snap.simpleEncoder;
}

profile::OutputConfigSnapshot makeSnapshot() {
  profile::OutputConfigSnapshot snap;
  snap.outputMode = "Advanced";
  snap.baseWidth = 1920;
  snap.baseHeight = 1080;
  snap.outputWidth = 1280;
  snap.outputHeight = 720;
  snap.fpsCommon = "60";
  snap.simpleBitrate = 6000;
  snap.simpleEncoder = "nvenc";
  return snap;
}

} // namespace

TEST(OutputConfigChanged, IdenticalSnapshotsUnchanged) {
  profile::ProfileManager mgr;
  const profile::OutputConfigSnapshot before = makeSnapshot();
  setFakeOutputConfig(before); // "after" == before
  EXPECT_FALSE(mgr.outputConfigChanged(before));
}

TEST(OutputConfigChanged, EachDifferingFieldDetected) {
  profile::ProfileManager mgr;
  const profile::OutputConfigSnapshot before = makeSnapshot();

  const auto expectChangedWhen = [&](const char *field,
                                     profile::OutputConfigSnapshot modified) {
    setFakeOutputConfig(modified);
    EXPECT_TRUE(mgr.outputConfigChanged(before)) << "field: " << field;
  };

  {
    auto modified = before;
    modified.outputMode = "Simple";
    expectChangedWhen("outputMode", modified);
  }
  {
    auto modified = before;
    modified.baseWidth = 3840;
    expectChangedWhen("baseWidth", modified);
  }
  {
    auto modified = before;
    modified.baseHeight = 2160;
    expectChangedWhen("baseHeight", modified);
  }
  {
    auto modified = before;
    modified.outputWidth = 1920;
    expectChangedWhen("outputWidth", modified);
  }
  {
    auto modified = before;
    modified.outputHeight = 1080;
    expectChangedWhen("outputHeight", modified);
  }
  {
    auto modified = before;
    modified.fpsCommon = "30";
    expectChangedWhen("fpsCommon", modified);
  }
  {
    auto modified = before;
    modified.simpleBitrate = 8000;
    expectChangedWhen("simpleBitrate", modified);
  }
  {
    auto modified = before;
    modified.simpleEncoder = "x264";
    expectChangedWhen("simpleEncoder", modified);
  }
}
