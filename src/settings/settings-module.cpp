#include "settings-module.hpp"

#include <algorithm>
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

bool isNvencEncoder(const std::string &encoderId) {
  // Native OBS NVENC only. The legacy FFmpeg NVENC ids (ffmpeg_nvenc,
  // ffmpeg_hevc_nvenc) use a different preset property ("preset2", not
  // "preset" — see frontend/utility/SimpleOutput.cpp), so they must NOT
  // match here. libobsEncoderIdForSimpleEncoder() never returns ffmpeg_*
  // ids anyway; excluding them makes any future direct caller fail safe.
  return encoderId.rfind("obs_nvenc", 0) == 0;
}

// Maps Simple-mode encoder UI strings (as stored in
// SimpleOutput/StreamEncoder) to libobs encoder ids. Mirrors OBS's
// get_simple_output_encoder() in frontend/utility/SimpleOutput.cpp;
// the SIMPLE_ENCODER_* constants live in frontend/widgets/OBSBasic.hpp.

// True when `preset` is in the verified list for `encoderId`
// (presetsFor() is the single source of truth for that list).
bool isVerifiedPreset(const std::string &encoderId, const std::string &preset) {
  const auto presets = presetsFor(encoderId);
  return std::find(presets.begin(), presets.end(), preset) != presets.end();
}
} // namespace

// Matches the declaration in settings-module.hpp; the tests call this
// function via settings::, so it must NOT live in an anonymous namespace.
std::string libobsEncoderIdForSimpleEncoder(const std::string &simpleEncoder) {
  if (simpleEncoder == "x264" || simpleEncoder == "x264_lowcpu")
    return "obs_x264";
  if (simpleEncoder == "qsv")
    return "obs_qsv11_v2";
  if (simpleEncoder == "qsv_av1")
    return "obs_qsv11_av1";
  if (simpleEncoder == "amd")
    return "h264_texture_amf";
  if (simpleEncoder == "amd_hevc")
    return "h265_texture_amf";
  if (simpleEncoder == "amd_av1")
    return "av1_texture_amf";
  if (simpleEncoder == "nvenc")
    return "obs_nvenc_h264_tex";
  if (simpleEncoder == "nvenc_hevc")
    return "obs_nvenc_hevc_tex";
  if (simpleEncoder == "nvenc_av1")
    return "obs_nvenc_av1_tex";
  if (simpleEncoder == "apple_h264")
    return "com.apple.videotoolbox.videoencoder.ave.avc";
  if (simpleEncoder == "apple_hevc")
    return "com.apple.videotoolbox.videoencoder.ave.hevc";
  return "";
}

std::string presetKeyForEncoder(const std::string &encoderId) {
  if (isNvencEncoder(encoderId))
    return "NVENCPreset2";
  if (encoderId == "obs_x264")
    return "Preset";
  if (encoderId.rfind("obs_qsv", 0) == 0)
    return "QSVPreset";
  if (encoderId.rfind("amd_amf", 0) == 0 ||
      encoderId.rfind("h264_texture_amf", 0) == 0 ||
      encoderId.rfind("h265_texture_amf", 0) == 0 ||
      encoderId.rfind("av1_texture_amf", 0) == 0) {
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
  return isVerifiedPreset("obs_x264", preset);
}

std::vector<std::string> presetsFor(const std::string &encoderId) {
  if (isNvencEncoder(encoderId)) {
    // NVENC presets are p1 (max performance) through p7 (max quality);
    // the strings are identical in Simple and Advanced mode (verified
    // against OBS 31.1.1 plugins/obs-nvenc/nvenc-properties.c).
    std::vector<std::string> presets;
    for (int i = 1; i <= 7; ++i)
      presets.push_back("p" + std::to_string(i));
    return presets;
  }
  if (encoderId == "obs_x264")
    return {std::begin(kX264Presets), std::end(kX264Presets)};
  // QSV/AMF/Apple preset vocabularies are unverified — empty, never guess.
  return {};
}

std::string defaultPresetFor(const std::string &encoderId) {
  if (isNvencEncoder(encoderId))
    return DEFAULT_ENCODER_PRESET;
  if (encoderId == "obs_x264")
    return DEFAULT_X264_PRESET;
  return "";
}

bool SettingsManager::applyEncoderPreset(const std::string &preset) {
  config_t *config = obs_frontend_get_profile_config();
  if (!config) {
    blog(LOG_ERROR, "[Settings] applyEncoderPreset: no profile config");
    return false;
  }
  // The preset keys this writes are Simple-mode keys. On an Advanced
  // profile they are dormant — the write would "succeed" while the
  // active encoder never sees it, and the caller would report a preset
  // change that did not happen. Verify the mode the same way
  // migrateToAdvancedMode does: a missing mode is NOT Simple.
  const char *modeC = config_get_string(config, "Output", "Mode");
  if (!modeC || std::string(modeC) != "Simple") {
    blog(LOG_ERROR, "[Settings] applyEncoderPreset: profile is not in "
                    "Simple mode; preset not applied");
    return false;
  }
  const char *simpleEncoderC =
      config_get_string(config, "SimpleOutput", "StreamEncoder");
  if (!simpleEncoderC || !*simpleEncoderC) {
    blog(LOG_ERROR,
         "[Settings] applyEncoderPreset: SimpleOutput/StreamEncoder missing");
    return false;
  }
  // SimpleOutput/StreamEncoder holds a UI string ("nvenc", "x264", ...),
  // not a libobs id — resolve it first.
  const std::string encoderId = libobsEncoderIdForSimpleEncoder(simpleEncoderC);
  if (encoderId.empty()) {
    blog(LOG_WARNING,
         "[Settings] applyEncoderPreset: unrecognized encoder '%s'; preset "
         "not applied",
         simpleEncoderC);
    return false;
  }
  const std::string key = presetKeyForEncoder(encoderId);
  if (key.empty()) {
    blog(LOG_WARNING,
         "[Settings] applyEncoderPreset: no preset key for encoder '%s'; "
         "preset not applied",
         encoderId.c_str());
    return false;
  }
  // Validate the preset value against the encoder family. Only NVENC
  // (p1-p7) and x264 (standard names) have verified value lists; QSV/AMF/
  // Apple presets use different vocabularies, so refuse rather than
  // writing a value that's invalid for that family.
  if (!isVerifiedPreset(encoderId, preset)) {
    blog(LOG_WARNING,
         "[Settings] applyEncoderPreset: preset '%s' invalid for encoder "
         "'%s'; not applied",
         preset.c_str(), encoderId.c_str());
    return false;
  }
  // NOTE: "preset"/"preset2" are encoder *property* names, not profile keys —
  // never write those. The key above is the verified Simple-mode profile key.
  // Capture the old value first: if the save fails, restore it so the
  // in-memory config is not left dirty (OBS's next save would otherwise
  // persist a value this method reported as FAILED). "Had a value" means
  // a user value specifically (config_has_user_value): config_get_string
  // also returns registered defaults, and restoring a default would write
  // an explicit key where the profile deliberately had none.
  const bool hadOldPreset =
      config_has_user_value(config, "SimpleOutput", key.c_str());
  const char *oldPresetC =
      config_get_string(config, "SimpleOutput", key.c_str());
  const std::string oldPreset = oldPresetC ? oldPresetC : "";
  config_set_string(config, "SimpleOutput", key.c_str(), preset.c_str());
  if (config_save_safe(config, "tmp", nullptr) != CONFIG_SUCCESS) {
    if (hadOldPreset)
      config_set_string(config, "SimpleOutput", key.c_str(), oldPreset.c_str());
    else
      config_remove_value(config, "SimpleOutput", key.c_str());
    blog(LOG_ERROR, "[Settings] applyEncoderPreset: config_save_safe failed; "
                    "previous value restored");
    return false;
  }
  blog(LOG_INFO, "[Settings] Encoder preset: SimpleOutput/%s = %s", key.c_str(),
       preset.c_str());
  return true;
}

bool SettingsManager::applyAudioSettings() {
  config_t *config = obs_frontend_get_profile_config();
  if (!config) {
    blog(LOG_ERROR, "[Settings] applyAudioSettings: no profile config");
    return false;
  }
  // Capture old values first; restore them if the save fails (see
  // applyEncoderPreset for why a dirty in-memory value is a real bug, and
  // for why "had a value" is config_has_user_value, not a null check —
  // without it, an absent SampleRate would "restore" as an explicit 0).
  const uint64_t oldRate = config_get_uint(config, "Audio", "SampleRate");
  const bool hadOldRate = config_has_user_value(config, "Audio", "SampleRate");
  const bool hadOldSetup =
      config_has_user_value(config, "Audio", "ChannelSetup");
  const char *oldSetupC = config_get_string(config, "Audio", "ChannelSetup");
  const std::string oldSetup = oldSetupC ? oldSetupC : "";
  config_set_uint(config, "Audio", "SampleRate", AUDIO_SAMPLE_RATE_HZ);
  config_set_string(config, "Audio", "ChannelSetup", AUDIO_CHANNEL_SETUP);
  if (config_save_safe(config, "tmp", nullptr) != CONFIG_SUCCESS) {
    if (hadOldRate)
      config_set_uint(config, "Audio", "SampleRate", oldRate);
    else
      config_remove_value(config, "Audio", "SampleRate");
    if (hadOldSetup)
      config_set_string(config, "Audio", "ChannelSetup", oldSetup.c_str());
    else
      config_remove_value(config, "Audio", "ChannelSetup");
    blog(LOG_ERROR, "[Settings] applyAudioSettings: config_save_safe failed; "
                    "previous values restored");
    return false;
  }
  blog(LOG_INFO, "[Settings] Audio: 48 kHz, Stereo");
  return true;
}

bool SettingsManager::applyRecordingSettings() {
  config_t *config = obs_frontend_get_profile_config();
  if (!config) {
    blog(LOG_ERROR, "[Settings] applyRecordingSettings: no profile config");
    return false;
  }
  // Exact keys OBS 31 reads (verified against
  // frontend/utility/SimpleOutput.cpp): RecQuality="HQ" is the stored value
  // for the UI's "Indistinguishable" label; RecFormat2 (not RecFormat);
  // Video/AutoRemux (not SimpleOutput/RecRemux).
  // Capture old values first; restore them if the save fails (see
  // applyEncoderPreset for why a dirty in-memory value is a real bug).
  const bool hadOldQuality =
      config_has_user_value(config, "SimpleOutput", "RecQuality");
  const char *oldQualityC =
      config_get_string(config, "SimpleOutput", "RecQuality");
  const std::string oldQuality = oldQualityC ? oldQualityC : "";
  const bool hadOldFormat =
      config_has_user_value(config, "SimpleOutput", "RecFormat2");
  const char *oldFormatC =
      config_get_string(config, "SimpleOutput", "RecFormat2");
  const std::string oldFormat = oldFormatC ? oldFormatC : "";
  const bool hadOldRemux = config_has_user_value(config, "Video", "AutoRemux");
  const bool oldRemux = config_get_bool(config, "Video", "AutoRemux");
  config_set_string(config, "SimpleOutput", "RecQuality", RECORDING_QUALITY);
  config_set_string(config, "SimpleOutput", "RecFormat2", RECORDING_FORMAT);
  config_set_bool(config, "Video", "AutoRemux", true);
  if (config_save_safe(config, "tmp", nullptr) != CONFIG_SUCCESS) {
    if (hadOldQuality)
      config_set_string(config, "SimpleOutput", "RecQuality",
                        oldQuality.c_str());
    else
      config_remove_value(config, "SimpleOutput", "RecQuality");
    if (hadOldFormat)
      config_set_string(config, "SimpleOutput", "RecFormat2",
                        oldFormat.c_str());
    else
      config_remove_value(config, "SimpleOutput", "RecFormat2");
    if (hadOldRemux)
      config_set_bool(config, "Video", "AutoRemux", oldRemux);
    else
      config_remove_value(config, "Video", "AutoRemux");
    blog(LOG_ERROR, "[Settings] applyRecordingSettings: config_save_safe "
                    "failed; previous values restored");
    return false;
  }
  blog(LOG_INFO, "[Settings] Recording: quality HQ (Indistinguishable), "
                 "format MKV, auto-remux on");
  return true;
}

bool SettingsManager::applyConservativeFallback() {
  config_t *config = obs_frontend_get_profile_config();
  if (!config) {
    blog(LOG_ERROR, "[Settings] applyConservativeFallback: no profile config");
    return false;
  }
  const char *modeC = config_get_string(config, "Output", "Mode");
  if (!modeC || std::string(modeC) != "Simple") {
    blog(LOG_ERROR, "[Settings] applyConservativeFallback: profile is not in "
                    "Simple mode (or mode is unset); fallback refused");
    return false;
  }
  // The fallback expresses frame rate through FPSCommon only, and
  // FPSCommon governs ONLY when the profile's FPS selector says so:
  // Video/FPSType 0 (or absent) = Common, 1 = Integer (FPSInt),
  // 2 = Fraction (FPSNum/FPSDen) — OBSBasic::GetConfigFPS. On a
  // profile whose selector is Integer or Fraction, writing
  // FPSCommon would change nothing while this function reported
  // 720p30. Detect the selector and refuse; stale FPSNum/FPSDen
  // values under a Common selector are inert and do NOT block.
  if (config_has_user_value(config, "Video", "FPSType") &&
      config_get_int(config, "Video", "FPSType") != 0) {
    blog(LOG_ERROR,
         "[Settings] applyConservativeFallback: profile uses "
         "a non-Common FPS type (FPSType=%lld); fallback refused",
         static_cast<long long>(config_get_int(config, "Video", "FPSType")));
    return false;
  }

  // Capture every value this write touches; restore all of them if
  // the save fails (the apply* discipline: a failed save must not
  // leave new values live in memory for a later OBS save to persist).
  struct Captured {
    const char *section;
    const char *key;
    bool hadValue;
    std::string oldString;
    long long oldNumber = 0;
    uint64_t oldUint = 0;
    enum class Kind { String, Int, Uint } kind = Kind::String;
  };
  const std::string x264PresetKey = presetKeyForEncoder("obs_x264");
  std::vector<Captured> captured;
  auto captureString = [&](const char *section, const char *key) {
    const char *old = config_get_string(config, section, key);
    captured.push_back({section, key,
                        config_has_user_value(config, section, key),
                        old ? old : "", 0, 0, Captured::Kind::String});
  };
  auto captureNumber = [&](const char *section, const char *key) {
    Captured c{section,
               key,
               config_has_user_value(config, section, key),
               "",
               0,
               0,
               Captured::Kind::Int};
    c.oldNumber = config_get_int(config, section, key);
    captured.push_back(c);
  };
  auto captureUint = [&](const char *section, const char *key) {
    Captured c{section,
               key,
               config_has_user_value(config, section, key),
               "",
               0,
               0,
               Captured::Kind::Uint};
    c.oldUint = config_get_uint(config, section, key);
    captured.push_back(c);
  };
  // Capture with the same accessor type the write uses: OutputCX/CY
  // are written with config_set_uint, so they are captured and
  // restored as uints (an int round-trip would be the wrong discipline
  // for a function whose contract is exact restore).
  captureUint("Video", "OutputCX");
  captureUint("Video", "OutputCY");
  captureString("Video", "FPSCommon");
  captureNumber("SimpleOutput", "VBitrate");
  captureString("SimpleOutput", "StreamEncoder");
  captureString("SimpleOutput", x264PresetKey.c_str());

  config_set_uint(config, "Video", "OutputCX", FALLBACK_OUTPUT_WIDTH);
  config_set_uint(config, "Video", "OutputCY", FALLBACK_OUTPUT_HEIGHT);
  config_set_string(config, "Video", "FPSCommon", FALLBACK_FPS_COMMON);
  config_set_int(config, "SimpleOutput", "VBitrate",
                 FALLBACK_VIDEO_BITRATE_KBPS);
  config_set_string(config, "SimpleOutput", "StreamEncoder", "x264");
  // "ultrafast" is a verified x264 preset (presetsFor) and the bottom
  // of the stability ladder — the fallback pairs it with the encoder
  // switch above so the two always land together.
  config_set_string(config, "SimpleOutput", x264PresetKey.c_str(), "ultrafast");

  if (config_save_safe(config, "tmp", nullptr) != CONFIG_SUCCESS) {
    for (const Captured &c : captured) {
      if (!c.hadValue)
        config_remove_value(config, c.section, c.key);
      else if (c.kind == Captured::Kind::Int)
        config_set_int(config, c.section, c.key, c.oldNumber);
      else if (c.kind == Captured::Kind::Uint)
        config_set_uint(config, c.section, c.key, c.oldUint);
      else
        config_set_string(config, c.section, c.key, c.oldString.c_str());
    }
    blog(LOG_ERROR, "[Settings] applyConservativeFallback: config_save_safe "
                    "failed; previous values restored");
    return false;
  }
  blog(LOG_INFO, "[Settings] Conservative fallback applied: 720p30, 2500 "
                 "kbps, x264 ultrafast");
  return true;
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

  // 2. Resolve the Simple-mode encoder UI string to a libobs id. A null
  // display name means OBS does not know this encoder — abort rather than
  // writing a bogus id.
  const char *simpleEncoderC =
      config_get_string(config, "SimpleOutput", "StreamEncoder");
  if (!simpleEncoderC || !*simpleEncoderC) {
    blog(LOG_ERROR,
         "[Settings] migrateToAdvancedMode: SimpleOutput/StreamEncoder "
         "missing; aborting");
    return MigrateResult::UnknownEncoder;
  }
  const std::string encoderId = libobsEncoderIdForSimpleEncoder(simpleEncoderC);
  if (encoderId.empty()) {
    blog(LOG_ERROR,
         "[Settings] migrateToAdvancedMode: unrecognized Simple-mode "
         "encoder '%s'; aborting",
         simpleEncoderC);
    return MigrateResult::UnknownEncoder;
  }
  if (!obs_encoder_get_display_name(encoderId.c_str())) {
    blog(LOG_ERROR,
         "[Settings] migrateToAdvancedMode: libobs encoder '%s' (from '%s') "
         "not available; aborting",
         encoderId.c_str(), simpleEncoderC);
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
  const bool presetOk = isVerifiedPreset(encoderId, preset);
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

  // 6. Stage streamEncoder.json in the profile directory, using the safe
  // writer (atomic rename). Matches profile-module: the frontend API
  // returns a bstr that the caller frees.
  char *profilePathC = obs_frontend_get_current_profile_path();
  if (!profilePathC || !*profilePathC) {
    blog(LOG_ERROR,
         "[Settings] migrateToAdvancedMode: could not resolve profile path");
    bfree(profilePathC);
    return MigrateResult::WriteFailed;
  }
  const std::string jsonPath =
      (fs::u8path(profilePathC) / "streamEncoder.json").u8string();
  bfree(profilePathC);

  obs_data_t *settings = obs_data_create();
  // "preset" is correct for native obs_nvenc_* and obs_x264. Do NOT change
  // to "preset2": OBS uses preset2 only for legacy ffmpeg_nvenc encoders
  // (frontend/utility/SimpleOutput.cpp: "Only use preset2 for legacy/FFmpeg
  // NVENC Encoder"), which this module never produces.
  obs_data_set_string(settings, "preset", preset.c_str());
  if (bitrate > 0)
    obs_data_set_int(settings, "bitrate", static_cast<long long>(bitrate));
  const bool jsonOk =
      obs_data_save_json_safe(settings, jsonPath.c_str(), "tmp", "bak");
  obs_data_release(settings);
  if (!jsonOk) {
    blog(LOG_ERROR, "[Settings] migrateToAdvancedMode: failed to write %s",
         jsonPath.c_str());
    return MigrateResult::WriteFailed;
  }

  // 7. Stage the config keys. Output/Mode goes LAST — only the save
  // commits, so any failure above leaves Simple mode intact. The commit
  // itself uses config_save_safe (write to basic.ini.tmp, rename over
  // the real file only on success — the same call OBS's own UI makes for
  // this config), so even a mid-write failure cannot truncate basic.ini.
  // ABitrate is intentionally NOT migrated: its Advanced-mode destination
  // is unverified, and the encoder default is safe.
  config_set_string(config, "AdvOut", "Encoder", encoderId.c_str());
  config_set_string(config, "Output", "Mode", "Advanced");
  if (config_save_safe(config, "tmp", nullptr) != CONFIG_SUCCESS) {
    // Restore the in-memory mode so a retry sees Simple, not a phantom
    // Advanced that was never committed to disk.
    config_set_string(config, "Output", "Mode", "Simple");
    blog(LOG_ERROR, "[Settings] migrateToAdvancedMode: config_save_safe "
                    "failed; mode restored to Simple");
    return MigrateResult::WriteFailed;
  }

  blog(LOG_INFO, "[Settings] Migrated to Advanced mode (encoder %s, preset %s)",
       encoderId.c_str(), preset.c_str());
  m_needsRecordingModal = true;
  return MigrateResult::Success;
}

} // namespace settings
} // namespace obs_setup
