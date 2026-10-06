// Unit tests for the pure-logic parts of src/settings/settings-module.cpp.
//
// Same stub strategy as tests/profile_tests.cpp: the test binary compiles
// the real module TU and links the real Qt, libobs and obs-frontend-api,
// but there is no OBS runtime here, so the OBS C functions touched by the
// tested paths are stubbed below as plain extern "C" definitions in this TU.
// The config_get_*/config_set_* stubs are backed by an in-memory FakeConfig,
// so the apply* methods are verified end-to-end against the fake (keys and
// values asserted, save observed). obs_data_* (used for streamEncoder.json)
// are the real libobs functions — they are pure data-container operations.

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator> // istreambuf_iterator
#include <map>
#include <string>
#include <system_error>
#include <utility>

#include <obs-frontend-api.h>
#include <obs.h>              // obs_data_*, obs_encoder_get_display_name
#include <util/base.h>        // blog
#include <util/config-file.h> // config_*

#include "settings-module.hpp"

namespace fs = std::filesystem;
namespace settings = obs_setup::settings;

namespace {

// ---- OBS stubs ------------------------------------------------------------

struct FakeConfig {
  std::map<std::pair<std::string, std::string>, std::string> strings;
  std::map<std::pair<std::string, std::string>, uint64_t> uints;
  std::map<std::pair<std::string, std::string>, bool> bools;
  bool saveCalled = false;

  void clear() {
    strings.clear();
    uints.clear();
    bools.clear();
    saveCalled = false;
  }
};

FakeConfig g_fakeConfig;
// Profile directory returned by the obs_frontend_get_current_profile_path
// stub; set per-test to a sandbox dir.
std::string g_profilePath;

extern "C" {

void blog(int, const char *, ...) {}

config_t *obs_frontend_get_profile_config(void) {
  return reinterpret_cast<config_t *>(&g_fakeConfig);
}

const char *obs_frontend_get_current_profile_path(void) {
  return g_profilePath.c_str();
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

void config_set_string(config_t *config, const char *section, const char *name,
                       const char *value) {
  auto *fake = reinterpret_cast<FakeConfig *>(config);
  fake->strings[std::make_pair(std::string(section), std::string(name))] =
      value ? value : "";
}

void config_set_uint(config_t *config, const char *section, const char *name,
                     uint64_t value) {
  auto *fake = reinterpret_cast<FakeConfig *>(config);
  fake->uints[std::make_pair(std::string(section), std::string(name))] = value;
}

void config_set_bool(config_t *config, const char *section, const char *name,
                     bool value) {
  auto *fake = reinterpret_cast<FakeConfig *>(config);
  fake->bools[std::make_pair(std::string(section), std::string(name))] = value;
}

bool config_save(config_t *config) {
  reinterpret_cast<FakeConfig *>(config)->saveCalled = true;
  return true;
}

// Known encoder ids for the migrate validation. Unknown ids return null,
// mirroring the real function.
const char *obs_encoder_get_display_name(const char *id) {
  static const std::map<std::string, const char *> kKnown = {
      {"obs_nvenc_h264_tex", "NVIDIA NVENC H.264"},
      {"obs_nvenc_hevc_tex", "NVIDIA NVENC HEVC"},
      {"obs_nvenc_av1_tex", "NVIDIA NVENC AV1"},
      {"obs_x264", "x264"},
      {"obs_qsv11", "QuickSync H.264"},
      {"amd_amf_h264", "AMD AMF H.264"},
  };
  const auto it = kKnown.find(id ? id : "");
  return it != kKnown.end() ? it->second : nullptr;
}

} // extern "C"

std::string readFile(const fs::path &p) {
  std::ifstream in(p, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
}

} // namespace

// ---- presetKeyForEncoder (pure) ----

TEST(PresetKeyForEncoder, NvencVariants) {
  EXPECT_EQ(settings::presetKeyForEncoder("obs_nvenc_h264_tex"),
            "NVENCPreset2");
  EXPECT_EQ(settings::presetKeyForEncoder("obs_nvenc_hevc_tex"),
            "NVENCPreset2");
  EXPECT_EQ(settings::presetKeyForEncoder("obs_nvenc_av1_tex"), "NVENCPreset2");
}

TEST(PresetKeyForEncoder, X264) {
  EXPECT_EQ(settings::presetKeyForEncoder("obs_x264"), "Preset");
}

TEST(PresetKeyForEncoder, Qsv) {
  EXPECT_EQ(settings::presetKeyForEncoder("obs_qsv11"), "QSVPreset");
  EXPECT_EQ(settings::presetKeyForEncoder("obs_qsv11_hevc"), "QSVPreset");
}

TEST(PresetKeyForEncoder, AmdAmf) {
  EXPECT_EQ(settings::presetKeyForEncoder("amd_amf_h264"), "AMDPreset");
  EXPECT_EQ(settings::presetKeyForEncoder("amd_amf_hevc"), "AMDPreset");
  EXPECT_EQ(settings::presetKeyForEncoder("amd_amf_av1"), "AMDAV1Preset");
}

TEST(PresetKeyForEncoder, UnknownReturnsEmpty) {
  // Never guess: an empty key means the caller must skip the write.
  EXPECT_EQ(settings::presetKeyForEncoder("obs_videotoolbox"), "");
  EXPECT_EQ(settings::presetKeyForEncoder("bogus_encoder"), "");
  EXPECT_EQ(settings::presetKeyForEncoder(""), "");
}

// ---- canMigrateEncoderPreset (pure) ----

TEST(CanMigrateEncoderPreset, NvencAndX264) {
  EXPECT_TRUE(settings::canMigrateEncoderPreset("obs_nvenc_h264_tex"));
  EXPECT_TRUE(settings::canMigrateEncoderPreset("obs_nvenc_hevc_tex"));
  EXPECT_TRUE(settings::canMigrateEncoderPreset("obs_x264"));
}

TEST(CanMigrateEncoderPreset, UnverifiedMappingsRefused) {
  // QSV/AMF/Apple preset value strings are unverified — migration refuses
  // rather than guessing.
  EXPECT_FALSE(settings::canMigrateEncoderPreset("obs_qsv11"));
  EXPECT_FALSE(settings::canMigrateEncoderPreset("amd_amf_h264"));
  EXPECT_FALSE(settings::canMigrateEncoderPreset("bogus_encoder"));
  EXPECT_FALSE(settings::canMigrateEncoderPreset(""));
}

// ---- isValidX264Preset (pure) ----

TEST(IsValidX264Preset, StandardNames) {
  for (const char *p : {"ultrafast", "superfast", "veryfast", "faster", "fast",
                        "medium", "slow", "slower", "veryslow", "placebo"}) {
    EXPECT_TRUE(settings::isValidX264Preset(p)) << p;
  }
}

TEST(IsValidX264Preset, RejectsUnknown) {
  EXPECT_FALSE(settings::isValidX264Preset("p5")); // NVENC value on x264
  EXPECT_FALSE(settings::isValidX264Preset(""));
  EXPECT_FALSE(settings::isValidX264Preset("VeryFast")); // case-sensitive
}

// ---- apply* methods (via FakeConfig) ----

class SettingsTest : public ::testing::Test {
protected:
  void SetUp() override {
    g_fakeConfig.clear();
    sandbox_ = fs::temp_directory_path() / "quickstart-settings-tests";
    std::error_code ec;
    fs::remove_all(sandbox_, ec);
    fs::create_directories(sandbox_, ec);
    ASSERT_FALSE(ec) << "could not create sandbox: " << ec.message();
    g_profilePath = sandbox_.u8string();
  }

  void TearDown() override {
    std::error_code ec;
    fs::remove_all(sandbox_, ec);
    g_profilePath.clear();
  }

  fs::path sandbox_;
};

TEST_F(SettingsTest, ApplyEncoderPresetNvenc) {
  g_fakeConfig.strings[{"SimpleOutput", "StreamEncoder"}] =
      "obs_nvenc_h264_tex";
  settings::SettingsManager mgr;
  mgr.applyEncoderPreset("p5");
  EXPECT_EQ((g_fakeConfig.strings[{"SimpleOutput", "NVENCPreset2"}]), "p5");
  EXPECT_TRUE(g_fakeConfig.saveCalled);
}

TEST_F(SettingsTest, ApplyEncoderPresetX264) {
  g_fakeConfig.strings[{"SimpleOutput", "StreamEncoder"}] = "obs_x264";
  settings::SettingsManager mgr;
  mgr.applyEncoderPreset("veryfast");
  EXPECT_EQ((g_fakeConfig.strings[{"SimpleOutput", "Preset"}]), "veryfast");
  EXPECT_TRUE(g_fakeConfig.saveCalled);
}

TEST_F(SettingsTest, ApplyEncoderPresetUnknownEncoderSkipped) {
  g_fakeConfig.strings[{"SimpleOutput", "StreamEncoder"}] = "bogus_encoder";
  settings::SettingsManager mgr;
  mgr.applyEncoderPreset("p5");
  // No preset key written and nothing persisted: never guess a key.
  EXPECT_EQ(g_fakeConfig.strings.count({"SimpleOutput", "NVENCPreset2"}), 0u);
  EXPECT_EQ(g_fakeConfig.strings.count({"SimpleOutput", "Preset"}), 0u);
  EXPECT_FALSE(g_fakeConfig.saveCalled);
}

TEST_F(SettingsTest, ApplyAudioSettings) {
  settings::SettingsManager mgr;
  mgr.applyAudioSettings();
  EXPECT_EQ((g_fakeConfig.uints[{"Audio", "SampleRate"}]), 48000u);
  EXPECT_EQ((g_fakeConfig.strings[{"Audio", "ChannelSetup"}]), "Stereo");
  EXPECT_TRUE(g_fakeConfig.saveCalled);
}

TEST_F(SettingsTest, ApplyRecordingSettings) {
  settings::SettingsManager mgr;
  mgr.applyRecordingSettings();
  EXPECT_EQ((g_fakeConfig.strings[{"SimpleOutput", "RecQuality"}],
            "Indistinguishable");
  EXPECT_EQ((g_fakeConfig.strings[{"SimpleOutput", "RecFormat"}]), "mkv");
  EXPECT_TRUE((g_fakeConfig.bools[{"SimpleOutput", "RecRemux"}])]);
  EXPECT_TRUE(g_fakeConfig.saveCalled);
}

// ---- migrateToAdvancedMode ----

TEST_F(SettingsTest, MigrateNotSimpleMode) {
  g_fakeConfig.strings[{"Output", "Mode"}] = "Advanced";
  settings::SettingsManager mgr;
  EXPECT_EQ(mgr.migrateToAdvancedMode(),
            settings::MigrateResult::NotSimpleMode);
  EXPECT_FALSE(mgr.needsRecordingModal());
  EXPECT_FALSE(g_fakeConfig.saveCalled);
  EXPECT_FALSE(fs::exists(sandbox_ / "streamEncoder.json"));
}

TEST_F(SettingsTest, MigrateMissingEncoder) {
  g_fakeConfig.strings[{"Output", "Mode"}] = "Simple";
  // No StreamEncoder key at all.
  settings::SettingsManager mgr;
  EXPECT_EQ(mgr.migrateToAdvancedMode(),
            settings::MigrateResult::UnknownEncoder);
  EXPECT_FALSE(mgr.needsRecordingModal());
}

TEST_F(SettingsTest, MigrateUnrecognizedEncoder) {
  g_fakeConfig.strings[{"Output", "Mode"}] = "Simple";
  // Present in config but unknown to OBS (stub returns null).
  g_fakeConfig.strings[{"SimpleOutput", "StreamEncoder"}] = "bogus_encoder";
  settings::SettingsManager mgr;
  EXPECT_EQ(mgr.migrateToAdvancedMode(),
            settings::MigrateResult::UnknownEncoder);
  EXPECT_FALSE(fs::exists(sandbox_ / "streamEncoder.json"));
}

TEST_F(SettingsTest, MigrateUnsupportedEncoder) {
  g_fakeConfig.strings[{"Output", "Mode"}] = "Simple";
  g_fakeConfig.strings[{"SimpleOutput", "StreamEncoder"}] = "obs_qsv11";
  g_fakeConfig.strings[{"SimpleOutput", "QSVPreset"}] = "balanced";
  settings::SettingsManager mgr;
  EXPECT_EQ(mgr.migrateToAdvancedMode(),
            settings::MigrateResult::UnsupportedEncoder);
  EXPECT_FALSE(mgr.needsRecordingModal());
  EXPECT_FALSE(fs::exists(sandbox_ / "streamEncoder.json"));
}

TEST_F(SettingsTest, MigrateRejectsBadX264Preset) {
  g_fakeConfig.strings[{"Output", "Mode"}] = "Simple";
  g_fakeConfig.strings[{"SimpleOutput", "StreamEncoder"}] = "obs_x264";
  g_fakeConfig.strings[{"SimpleOutput", "Preset"}] = "p5"; // NVENC value
  settings::SettingsManager mgr;
  EXPECT_EQ(mgr.migrateToAdvancedMode(),
            settings::MigrateResult::UnsupportedEncoder);
  EXPECT_FALSE(fs::exists(sandbox_ / "streamEncoder.json"));
}

TEST_F(SettingsTest, MigrateSuccessNvenc) {
  g_fakeConfig.strings[{"Output", "Mode"}] = "Simple";
  g_fakeConfig.strings[{"SimpleOutput", "StreamEncoder"}] =
      "obs_nvenc_h264_tex";
  g_fakeConfig.strings[{"SimpleOutput", "NVENCPreset2"}] = "p5";
  g_fakeConfig.uints[{"SimpleOutput", "VBitrate"}] = 6000;
  settings::SettingsManager mgr;
  EXPECT_EQ(mgr.migrateToAdvancedMode(), settings::MigrateResult::Success);
  EXPECT_TRUE(mgr.needsRecordingModal());
  // Encoder id migrated to AdvOut.
  EXPECT_EQ((g_fakeConfig.strings[{"AdvOut", "Encoder"}]),
            "obs_nvenc_h264_tex");
  // Mode is Advanced only after staging; save persisted everything.
  EXPECT_EQ((g_fakeConfig.strings[{"Output", "Mode"}]), "Advanced");
  EXPECT_TRUE(g_fakeConfig.saveCalled);
  // streamEncoder.json holds the 1:1 preset and bitrate.
  const fs::path jsonPath = sandbox_ / "streamEncoder.json";
  ASSERT_TRUE(fs::exists(jsonPath));
  const std::string content = readFile(jsonPath);
  EXPECT_NE(content.find("\"preset\""), std::string::npos);
  EXPECT_NE(content.find("p5"), std::string::npos);
  EXPECT_NE(content.find("6000"), std::string::npos);
}

TEST_F(SettingsTest, MigrateSuccessX264) {
  g_fakeConfig.strings[{"Output", "Mode"}] = "Simple";
  g_fakeConfig.strings[{"SimpleOutput", "StreamEncoder"}] = "obs_x264";
  g_fakeConfig.strings[{"SimpleOutput", "Preset"}] = "veryfast";
  g_fakeConfig.uints[{"SimpleOutput", "VBitrate"}] = 8000;
  settings::SettingsManager mgr;
  EXPECT_EQ(mgr.migrateToAdvancedMode(), settings::MigrateResult::Success);
  EXPECT_TRUE(mgr.needsRecordingModal());
  EXPECT_EQ((g_fakeConfig.strings[{"AdvOut", "Encoder"}]), "obs_x264");
  const std::string content = readFile(sandbox_ / "streamEncoder.json");
  EXPECT_NE(content.find("veryfast"), std::string::npos);
  EXPECT_NE(content.find("8000"), std::string::npos);
}

TEST_F(SettingsTest, MigrateSkipsMissingBitrate) {
  g_fakeConfig.strings[{"Output", "Mode"}] = "Simple";
  g_fakeConfig.strings[{"SimpleOutput", "StreamEncoder"}] =
      "obs_nvenc_h264_tex";
  g_fakeConfig.strings[{"SimpleOutput", "NVENCPreset2"}] = "p5";
  // No VBitrate: migration still succeeds; encoder uses its default.
  settings::SettingsManager mgr;
  EXPECT_EQ(mgr.migrateToAdvancedMode(), settings::MigrateResult::Success);
  const std::string content = readFile(sandbox_ / "streamEncoder.json");
  EXPECT_NE(content.find("p5"), std::string::npos);
  EXPECT_EQ(content.find("bitrate"), std::string::npos);
}
