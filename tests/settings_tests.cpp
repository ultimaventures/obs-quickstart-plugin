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
#include <vector>

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
  bool saveFails = false; // when true, config_save() returns CONFIG_ERROR

  void clear() {
    strings.clear();
    uints.clear();
    bools.clear();
    saveCalled = false;
    saveFails = false;
  }
};

FakeConfig g_fakeConfig;
// When false, obs_frontend_get_profile_config() returns nullptr,
// mirroring OBS running without a loaded profile.
bool g_configAvailable = true;
// Profile directory returned by the obs_frontend_get_current_profile_path
// stub; set per-test to a sandbox dir.
std::string g_profilePath;

extern "C" {

void blog(int, const char *, ...) {}

config_t *obs_frontend_get_profile_config(void) {
  if (!g_configAvailable)
    return nullptr;
  return reinterpret_cast<config_t *>(&g_fakeConfig);
}

char *obs_frontend_get_current_profile_path(void) {
  // Real signature returns a heap-allocated char* the caller bfrees (see
  // profile-module.cpp); duplicate here so the module's bfree is safe.
  return bstrdup(g_profilePath.c_str());
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

bool config_get_bool(config_t *config, const char *section, const char *name) {
  const auto *fake = reinterpret_cast<const FakeConfig *>(config);
  const auto it =
      fake->bools.find(std::make_pair(std::string(section), std::string(name)));
  return it != fake->bools.end() ? it->second : false;
}

bool config_remove_value(config_t *config, const char *section,
                         const char *name) {
  auto *fake = reinterpret_cast<FakeConfig *>(config);
  const auto key = std::make_pair(std::string(section), std::string(name));
  fake->strings.erase(key);
  fake->uints.erase(key);
  fake->bools.erase(key);
  return true;
}

int config_save(config_t *config) {
  auto *fake = reinterpret_cast<FakeConfig *>(config);
  fake->saveCalled = true;
  return fake->saveFails ? CONFIG_ERROR : CONFIG_SUCCESS;
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
      {"obs_qsv11_v2", "QuickSync H.264"},
      {"obs_qsv11_av1", "QuickSync AV1"},
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

// ---- libobsEncoderIdForSimpleEncoder (pure) ----

TEST(LibobsEncoderIdForSimpleEncoder, MapsUiStrings) {
  // Mirrors OBS's get_simple_output_encoder() in
  // frontend/utility/SimpleOutput.cpp.
  EXPECT_EQ(settings::libobsEncoderIdForSimpleEncoder("x264"), "obs_x264");
  EXPECT_EQ(settings::libobsEncoderIdForSimpleEncoder("x264_lowcpu"),
            "obs_x264");
  EXPECT_EQ(settings::libobsEncoderIdForSimpleEncoder("nvenc"),
            "obs_nvenc_h264_tex");
  EXPECT_EQ(settings::libobsEncoderIdForSimpleEncoder("nvenc_hevc"),
            "obs_nvenc_hevc_tex");
  EXPECT_EQ(settings::libobsEncoderIdForSimpleEncoder("nvenc_av1"),
            "obs_nvenc_av1_tex");
  EXPECT_EQ(settings::libobsEncoderIdForSimpleEncoder("qsv"), "obs_qsv11_v2");
  EXPECT_EQ(settings::libobsEncoderIdForSimpleEncoder("amd"),
            "h264_texture_amf");
  EXPECT_EQ(settings::libobsEncoderIdForSimpleEncoder("apple_h264"),
            "com.apple.videotoolbox.videoencoder.ave.avc");
}

TEST(LibobsEncoderIdForSimpleEncoder, UnknownReturnsEmpty) {
  EXPECT_EQ(settings::libobsEncoderIdForSimpleEncoder("obs_x264"), "");
  EXPECT_EQ(settings::libobsEncoderIdForSimpleEncoder("bogus"), "");
  EXPECT_EQ(settings::libobsEncoderIdForSimpleEncoder(""), "");
}

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
  // Legacy FFmpeg NVENC uses a different preset property ("preset2"), so
  // it must not match as NVENC — fail safe.
  EXPECT_EQ(settings::presetKeyForEncoder("ffmpeg_nvenc"), "");
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

// ---- presetsFor / defaultPresetFor (pure) ----
// These lists are the single source of truth shared by SettingsManager's
// validation and the settings dialog's preset picker.

TEST(PresetsFor, Nvenc) {
  EXPECT_EQ(
      settings::presetsFor("obs_nvenc_h264_tex"),
      (std::vector<std::string>{"p1", "p2", "p3", "p4", "p5", "p6", "p7"}));
  EXPECT_EQ(settings::presetsFor("obs_nvenc_av1_tex"),
            settings::presetsFor("obs_nvenc_h264_tex"));
}

TEST(PresetsFor, X264) {
  EXPECT_EQ(settings::presetsFor("obs_x264"),
            (std::vector<std::string>{"ultrafast", "superfast", "veryfast",
                                      "faster", "fast", "medium", "slow",
                                      "slower", "veryslow", "placebo"}));
}

TEST(PresetsFor, UnverifiedEncodersEmpty) {
  // QSV/AMF/Apple vocabularies are unverified: empty, never guessed. This
  // is also what keeps the legacy FFmpeg NVENC ids out.
  EXPECT_TRUE(settings::presetsFor("obs_qsv11_v2").empty());
  EXPECT_TRUE(settings::presetsFor("h264_texture_amf").empty());
  EXPECT_TRUE(settings::presetsFor("ffmpeg_nvenc").empty());
  EXPECT_TRUE(settings::presetsFor("bogus_encoder").empty());
  EXPECT_TRUE(settings::presetsFor("").empty());
}

TEST(DefaultPresetFor, KnownFamilies) {
  EXPECT_EQ(settings::defaultPresetFor("obs_nvenc_h264_tex"), "p5");
  EXPECT_EQ(settings::defaultPresetFor("obs_x264"), "veryfast");
}

TEST(DefaultPresetFor, UnverifiedEncodersEmpty) {
  EXPECT_EQ(settings::defaultPresetFor("obs_qsv11_v2"), "");
  EXPECT_EQ(settings::defaultPresetFor("bogus_encoder"), "");
  EXPECT_EQ(settings::defaultPresetFor(""), "");
}

// ---- apply* methods (via FakeConfig) ----

class SettingsTest : public ::testing::Test {
protected:
  void SetUp() override {
    g_fakeConfig.clear();
    g_configAvailable = true;
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
  // SimpleOutput/StreamEncoder holds the UI string, not the libobs id.
  g_fakeConfig.strings[{"SimpleOutput", "StreamEncoder"}] = "nvenc";
  settings::SettingsManager mgr;
  EXPECT_TRUE(mgr.applyEncoderPreset("p5"));
  EXPECT_EQ((g_fakeConfig.strings[{"SimpleOutput", "NVENCPreset2"}]), "p5");
  EXPECT_TRUE(g_fakeConfig.saveCalled);
}

TEST_F(SettingsTest, ApplyEncoderPresetX264) {
  g_fakeConfig.strings[{"SimpleOutput", "StreamEncoder"}] = "x264";
  settings::SettingsManager mgr;
  EXPECT_TRUE(mgr.applyEncoderPreset("veryfast"));
  EXPECT_EQ((g_fakeConfig.strings[{"SimpleOutput", "Preset"}]), "veryfast");
  EXPECT_TRUE(g_fakeConfig.saveCalled);
}

TEST_F(SettingsTest, ApplyEncoderPresetRefusesMismatchedPreset) {
  // p5 is NVENC-only; writing it to x264's Preset key would be garbage.
  g_fakeConfig.strings[{"SimpleOutput", "StreamEncoder"}] = "x264";
  settings::SettingsManager mgr;
  EXPECT_FALSE(mgr.applyEncoderPreset("p5"));
  EXPECT_EQ(g_fakeConfig.strings.count({"SimpleOutput", "Preset"}), 0u);
  EXPECT_FALSE(g_fakeConfig.saveCalled);
}

TEST_F(SettingsTest, ApplyEncoderPresetRefusesQsvWithX264Preset) {
  // "veryfast" is a valid x264 preset but meaningless to QSV; the x264
  // list must not validate non-x264 encoders.
  g_fakeConfig.strings[{"SimpleOutput", "StreamEncoder"}] = "qsv";
  settings::SettingsManager mgr;
  EXPECT_FALSE(mgr.applyEncoderPreset("veryfast"));
  EXPECT_EQ(g_fakeConfig.strings.count({"SimpleOutput", "QSVPreset"}), 0u);
  EXPECT_FALSE(g_fakeConfig.saveCalled);
}

TEST_F(SettingsTest, ApplyEncoderPresetUnknownEncoderSkipped) {
  g_fakeConfig.strings[{"SimpleOutput", "StreamEncoder"}] = "bogus_encoder";
  settings::SettingsManager mgr;
  EXPECT_FALSE(mgr.applyEncoderPreset("p5"));
  // No preset key written and nothing persisted: never guess a key.
  EXPECT_EQ(g_fakeConfig.strings.count({"SimpleOutput", "NVENCPreset2"}), 0u);
  EXPECT_EQ(g_fakeConfig.strings.count({"SimpleOutput", "Preset"}), 0u);
  EXPECT_FALSE(g_fakeConfig.saveCalled);
}

TEST_F(SettingsTest, ApplyAudioSettings) {
  settings::SettingsManager mgr;
  EXPECT_TRUE(mgr.applyAudioSettings());
  EXPECT_EQ((g_fakeConfig.uints[{"Audio", "SampleRate"}]), 48000u);
  EXPECT_EQ((g_fakeConfig.strings[{"Audio", "ChannelSetup"}]), "Stereo");
  EXPECT_TRUE(g_fakeConfig.saveCalled);
}

TEST_F(SettingsTest, ApplyRecordingSettings) {
  settings::SettingsManager mgr;
  EXPECT_TRUE(mgr.applyRecordingSettings());
  // "HQ" is the stored value for the UI's "Indistinguishable" label.
  EXPECT_EQ((g_fakeConfig.strings[{"SimpleOutput", "RecQuality"}]), "HQ");
  EXPECT_EQ((g_fakeConfig.strings[{"SimpleOutput", "RecFormat2"}]), "mkv");
  EXPECT_TRUE((g_fakeConfig.bools[{"Video", "AutoRemux"}]));
  EXPECT_TRUE(g_fakeConfig.saveCalled);
}

// The apply* methods must report failure, not just log it — the dialog
// shows per-item results from these returns.

TEST_F(SettingsTest, ApplyReturnsFalseWhenSaveFails) {
  g_fakeConfig.strings[{"SimpleOutput", "StreamEncoder"}] = "nvenc";
  g_fakeConfig.saveFails = true;
  settings::SettingsManager mgr;
  EXPECT_FALSE(mgr.applyEncoderPreset("p5"));
  EXPECT_FALSE(mgr.applyAudioSettings());
  EXPECT_FALSE(mgr.applyRecordingSettings());
}

// On save failure the apply* methods must restore the previous in-memory
// values — otherwise OBS's next save would persist a value the UI just
// reported as FAILED.

TEST_F(SettingsTest, ApplyEncoderPresetSaveFailureRestoresOldValue) {
  g_fakeConfig.strings[{"SimpleOutput", "StreamEncoder"}] = "nvenc";
  g_fakeConfig.strings[{"SimpleOutput", "NVENCPreset2"}] = "p3";
  g_fakeConfig.saveFails = true;
  settings::SettingsManager mgr;
  EXPECT_FALSE(mgr.applyEncoderPreset("p5"));
  EXPECT_EQ((g_fakeConfig.strings[{"SimpleOutput", "NVENCPreset2"}]), "p3");
}

TEST_F(SettingsTest, ApplyEncoderPresetSaveFailureRemovesNewKey) {
  g_fakeConfig.strings[{"SimpleOutput", "StreamEncoder"}] = "x264";
  // No Preset key beforehand: a failed apply must not leave one behind.
  g_fakeConfig.saveFails = true;
  settings::SettingsManager mgr;
  EXPECT_FALSE(mgr.applyEncoderPreset("veryfast"));
  EXPECT_EQ(g_fakeConfig.strings.count({"SimpleOutput", "Preset"}), 0u);
}

TEST_F(SettingsTest, ApplyAudioSettingsSaveFailureRestores) {
  g_fakeConfig.uints[{"Audio", "SampleRate"}] = 44100;
  g_fakeConfig.strings[{"Audio", "ChannelSetup"}] = "Mono";
  g_fakeConfig.saveFails = true;
  settings::SettingsManager mgr;
  EXPECT_FALSE(mgr.applyAudioSettings());
  EXPECT_EQ((g_fakeConfig.uints[{"Audio", "SampleRate"}]), 44100u);
  EXPECT_EQ((g_fakeConfig.strings[{"Audio", "ChannelSetup"}]), "Mono");
}

TEST_F(SettingsTest, ApplyRecordingSettingsSaveFailureRestores) {
  g_fakeConfig.strings[{"SimpleOutput", "RecQuality"}] = "Stream";
  g_fakeConfig.strings[{"SimpleOutput", "RecFormat2"}] = "mp4";
  g_fakeConfig.bools[{"Video", "AutoRemux"}] = false;
  g_fakeConfig.saveFails = true;
  settings::SettingsManager mgr;
  EXPECT_FALSE(mgr.applyRecordingSettings());
  EXPECT_EQ((g_fakeConfig.strings[{"SimpleOutput", "RecQuality"}]), "Stream");
  EXPECT_EQ((g_fakeConfig.strings[{"SimpleOutput", "RecFormat2"}]), "mp4");
  EXPECT_FALSE((g_fakeConfig.bools[{"Video", "AutoRemux"}]));
}

TEST_F(SettingsTest, ApplyReturnsFalseWithNoProfileConfig) {
  g_configAvailable = false;
  settings::SettingsManager mgr;
  EXPECT_FALSE(mgr.applyEncoderPreset("p5"));
  EXPECT_FALSE(mgr.applyAudioSettings());
  EXPECT_FALSE(mgr.applyRecordingSettings());
}

TEST_F(SettingsTest, ApplyEncoderPresetMissingEncoderKey) {
  // No StreamEncoder key at all: nothing to resolve, report failure.
  settings::SettingsManager mgr;
  EXPECT_FALSE(mgr.applyEncoderPreset("p5"));
  EXPECT_FALSE(g_fakeConfig.saveCalled);
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
  g_fakeConfig.strings[{"SimpleOutput", "StreamEncoder"}] = "qsv";
  g_fakeConfig.strings[{"SimpleOutput", "QSVPreset"}] = "balanced";
  settings::SettingsManager mgr;
  EXPECT_EQ(mgr.migrateToAdvancedMode(),
            settings::MigrateResult::UnsupportedEncoder);
  EXPECT_FALSE(mgr.needsRecordingModal());
  EXPECT_FALSE(fs::exists(sandbox_ / "streamEncoder.json"));
}

TEST_F(SettingsTest, MigrateRejectsBadX264Preset) {
  g_fakeConfig.strings[{"Output", "Mode"}] = "Simple";
  g_fakeConfig.strings[{"SimpleOutput", "StreamEncoder"}] = "x264";
  g_fakeConfig.strings[{"SimpleOutput", "Preset"}] = "p5"; // NVENC value
  settings::SettingsManager mgr;
  EXPECT_EQ(mgr.migrateToAdvancedMode(),
            settings::MigrateResult::UnsupportedEncoder);
  EXPECT_FALSE(fs::exists(sandbox_ / "streamEncoder.json"));
}

TEST_F(SettingsTest, MigrateSuccessNvenc) {
  g_fakeConfig.strings[{"Output", "Mode"}] = "Simple";
  g_fakeConfig.strings[{"SimpleOutput", "StreamEncoder"}] = "nvenc";
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
  g_fakeConfig.strings[{"SimpleOutput", "StreamEncoder"}] = "x264";
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

TEST_F(SettingsTest, MigrateSaveFailureRestoresSimpleMode) {
  g_fakeConfig.strings[{"Output", "Mode"}] = "Simple";
  g_fakeConfig.strings[{"SimpleOutput", "StreamEncoder"}] = "x264";
  g_fakeConfig.strings[{"SimpleOutput", "Preset"}] = "veryfast";
  g_fakeConfig.saveFails = true;
  settings::SettingsManager mgr;
  EXPECT_EQ(mgr.migrateToAdvancedMode(), settings::MigrateResult::WriteFailed);
  EXPECT_FALSE(mgr.needsRecordingModal());
  // The in-memory mode is rolled back so a retry sees Simple, not a
  // phantom Advanced that was never committed to disk.
  EXPECT_EQ((g_fakeConfig.strings[{"Output", "Mode"}]), "Simple");
}

TEST_F(SettingsTest, MigrateSkipsMissingBitrate) {
  g_fakeConfig.strings[{"Output", "Mode"}] = "Simple";
  g_fakeConfig.strings[{"SimpleOutput", "StreamEncoder"}] = "nvenc";
  g_fakeConfig.strings[{"SimpleOutput", "NVENCPreset2"}] = "p5";
  // No VBitrate: migration still succeeds; encoder uses its default.
  settings::SettingsManager mgr;
  EXPECT_EQ(mgr.migrateToAdvancedMode(), settings::MigrateResult::Success);
  const std::string content = readFile(sandbox_ / "streamEncoder.json");
  EXPECT_NE(content.find("p5"), std::string::npos);
  EXPECT_EQ(content.find("bitrate"), std::string::npos);
}
