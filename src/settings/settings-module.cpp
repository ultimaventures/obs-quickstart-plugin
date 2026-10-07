#include "settings-module.hpp"

#include <filesystem>

#include <obs-frontend-api.h> // obs_frontend_get_profile_config,
#include <obs.h>              // obs_data_*, obs_encoder_get_display_name
                              // obs_frontend_get_current_profile_path
#include <util/base.h>        // blog
#include <util/config-file.h> // config_*

namespace fs = std::filesystem;

namespace obs_setup {
namespace settings {

namespace {

// Standard x264 preset names. OBS's x264 encoder uses these same strings
// for its `preset` property, so a Simple-mode value from this list copies
// 1:1 into streamEncoder.json.
constexpr const char *kX264Presets[] = {
    "ultrafast", "superfast", "veryfast", "faster",   "fast",
    "medium",    "slow",      "slower",   "veryslow", "placebo",
};

// NVENC presets are p1 (max performance) through p7 (max quality); the
// strings are identical in Simple and Advanced mode (verified against
// OBS 31.1.1 plugins/obs-nvenc/nvenc-properties.c).
bool isValidNvencPreset(const std::string &preset) {
  return preset.size() == 2 && preset[0] == 'p' && preset[1] >= '1' &&
         preset[1] <= '7';
}

bool isNvencEncoder(const std::string &encoderId) {
  return encoderId.rfind("obs_nvenc", 0) == 0;
}

} // namespace

std::string presetKeyForEncoder(const std::string &encoderId) {
  if (isNvencEncoder(encoderId))
    return "NVENCPreset2";
  if (encoderId == "obs_x264")
    return "Preset";
  if (encoderId.rfind("obs_qsv", 0) == 0)
    return "QSVPreset";
  if (encoderId.rfind("amd_amf", 0) == 0) {
    if (encoderId.find("av1") != std::string::npos)
      return "AMDAV1Preset";
    return "AMDPreset";
  }
  return "";
}

bool canMigrateEncoderPreset(const std::string &encoderId) {
  // Only encoders whose Advanced-mode preset value strings are verified.
  return isNvencEncoder(encoderId) || encoderId == "obs_x264";
}

bool isValidX264Preset(const std::string &preset) {
  for (const char *p : kX264Presets) {
    if (preset == p)
      return true;
  }
  return false;
}

void SettingsManager::applyEncoderPreset(const std::string &preset) {
  config_t *config = obs_frontend_get_profile_config();
  if (!config) {
    blog(LOG_ERROR, "[Settings] applyEncoderPreset: no profile config");
    return;
  }
  const char *encoderId =
      config_get_string(config, "SimpleOutput", "StreamEncoder");
  if (!encoderId || !*encoderId) {
    blog(LOG_ERROR,
         "[Settings] applyEncoderPreset: SimpleOutput/StreamEncoder missing");
    return;
  }
  const std::string key = presetKeyForEncoder(encoderId);
  if (key.empty()) {
    blog(LOG_WARNING,
         "[Settings] applyEncoderPreset: unrecognized encoder '%s'; preset "
         "not applied",
         encoderId);
    return;
  }
  // NOTE: "preset"/"preset2" are encoder *property* names, not profile keys —
  // never write those. The key above is the verified Simple-mode profile key.
  config_set_string(config, "SimpleOutput", key.c_str(), preset.c_str());
  config_save(config);
  blog(LOG_INFO, "[Settings] Encoder preset: SimpleOutput/%s = %s", key.c_str(),
       preset.c_str());
}

void SettingsManager::applyAudioSettings() {
  config_t *config = obs_frontend_get_profile_config();
  if (!config) {
    blog(LOG_ERROR, "[Settings] applyAudioSettings: no profile config");
    return;
  }
  config_set_uint(config, "Audio", "SampleRate", AUDIO_SAMPLE_RATE_HZ);
  config_set_string(config, "Audio", "ChannelSetup", AUDIO_CHANNEL_SETUP);
  config_save(config);
  blog(LOG_INFO, "[Settings] Audio: 48 kHz, Stereo");
}

void SettingsManager::applyRecordingSettings() {
  config_t *config = obs_frontend_get_profile_config();
  if (!config) {
    blog(LOG_ERROR, "[Settings] applyRecordingSettings: no profile config");
    return;
  }
  // Key names are best-effort (see header): OBS ignores keys it does not
  // recognize, so a wrong guess is a silent no-op, not corruption.
  config_set_string(config, "SimpleOutput", "RecQuality", RECORDING_QUALITY);
  config_set_string(config, "SimpleOutput", "RecFormat", RECORDING_FORMAT);
  config_set_bool(config, "SimpleOutput", "RecRemux", true);
  config_save(config);
  blog(LOG_INFO, "[Settings] Recording: quality Indistinguishable, format MKV, "
                 "auto-remux on");
}

MigrateResult SettingsManager::migrateToAdvancedMode() {
  m_needsRecordingModal = false;

  config_t *config = obs_frontend_get_profile_config();
  if (!config) {
    blog(LOG_ERROR, "[Settings] migrateToAdvancedMode: no profile config");
    return MigrateResult::WriteFailed;
  }

  // 1. Only Simple mode has anything to migrate from.
  const char *mode = config_get_string(config, "Output", "Mode");
  if (!mode || std::string(mode) != "Simple") {
    return MigrateResult::NotSimpleMode;
  }

  // 2. Resolve and validate the encoder id. A null display name means OBS
  // does not know this encoder — abort rather than writing a bogus id.
  const char *encoderIdC =
      config_get_string(config, "SimpleOutput", "StreamEncoder");
  if (!encoderIdC || !*encoderIdC) {
    blog(LOG_ERROR,
         "[Settings] migrateToAdvancedMode: SimpleOutput/StreamEncoder "
         "missing; aborting");
    return MigrateResult::UnknownEncoder;
  }
  const std::string encoderId(encoderIdC);
  if (!obs_encoder_get_display_name(encoderId.c_str())) {
    blog(LOG_ERROR,
         "[Settings] migrateToAdvancedMode: unrecognized encoder '%s'; "
         "aborting",
         encoderId.c_str());
    return MigrateResult::UnknownEncoder;
  }

  // 3. Refuse encoders whose preset value mapping is unverified.
  if (!canMigrateEncoderPreset(encoderId)) {
    blog(LOG_ERROR,
         "[Settings] migrateToAdvancedMode: preset mapping unverified for "
         "encoder '%s' (QSV/AMF/Apple); refusing to guess",
         encoderId.c_str());
    return MigrateResult::UnsupportedEncoder;
  }

  // 4. Resolve and validate the preset value before copying it 1:1.
  const std::string presetKey = presetKeyForEncoder(encoderId);
  const char *presetC =
      config_get_string(config, "SimpleOutput", presetKey.c_str());
  if (!presetC || !*presetC) {
    blog(LOG_ERROR,
         "[Settings] migrateToAdvancedMode: preset value missing "
         "(SimpleOutput/%s); aborting",
         presetKey.c_str());
    return MigrateResult::UnsupportedEncoder;
  }
  const std::string preset(presetC);
  const bool presetOk = isNvencEncoder(encoderId) ? isValidNvencPreset(preset)
                                                  : isValidX264Preset(preset);
  if (!presetOk) {
    blog(LOG_ERROR,
         "[Settings] migrateToAdvancedMode: unrecognized preset value '%s' "
         "for encoder '%s'; aborting",
         preset.c_str(), encoderId.c_str());
    return MigrateResult::UnsupportedEncoder;
  }

  // 5. Bitrate is optional: missing → OBS default, logged, not fatal.
  const uint64_t bitrate = config_get_uint(config, "SimpleOutput", "VBitrate");
  if (bitrate == 0) {
    blog(LOG_WARNING,
         "[Settings] migrateToAdvancedMode: SimpleOutput/VBitrate missing; "
         "encoder will use its default bitrate");
  }

  // 6. Stage streamEncoder.json in the profile directory. Written as
  // obs_data JSON — the same shape OBS's settings dialog produces.
  // NOTE: obs_frontend_get_current_profile_path() ownership is unclear
  // (may be internal); copy immediately and never free it — a tiny
  // one-time leak beats a potential double-free.
  const char *profilePathC = obs_frontend_get_current_profile_path();
  if (!profilePathC || !*profilePathC) {
    blog(LOG_ERROR,
         "[Settings] migrateToAdvancedMode: could not resolve profile path");
    return MigrateResult::WriteFailed;
  }
  const std::string jsonPath =
      (fs::u8path(profilePathC) / "streamEncoder.json").u8string();

  obs_data_t *settings = obs_data_create();
  obs_data_set_string(settings, "preset", preset.c_str());
  if (bitrate > 0)
    obs_data_set_int(settings, "bitrate", static_cast<long long>(bitrate));
  const bool jsonOk = obs_data_save_json(settings, jsonPath.c_str());
  obs_data_release(settings);
  if (!jsonOk) {
    blog(LOG_ERROR, "[Settings] migrateToAdvancedMode: failed to write %s",
         jsonPath.c_str());
    return MigrateResult::WriteFailed;
  }

  // 7. Stage the config keys. Output/Mode goes LAST — only config_save()
  // commits, so any failure above leaves Simple mode intact on disk.
  // ABitrate is intentionally NOT migrated: its Advanced-mode destination
  // is unverified, and the encoder default is safe.
  config_set_string(config, "AdvOut", "Encoder", encoderId.c_str());
  config_set_string(config, "Output", "Mode", "Advanced");
  if (config_save(config) != CONFIG_SUCCESS) {
    blog(LOG_ERROR, "[Settings] migrateToAdvancedMode: config_save failed");
    return MigrateResult::WriteFailed;
  }

  blog(LOG_INFO, "[Settings] Migrated to Advanced mode (encoder %s, preset %s)",
       encoderId.c_str(), preset.c_str());
  m_needsRecordingModal = true;
  return MigrateResult::Success;
}

} // namespace settings
} // namespace obs_setup
