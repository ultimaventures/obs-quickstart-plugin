#ifndef OBS_SETUP_SETTINGS_SETTINGS_MODULE_HPP
#define OBS_SETUP_SETTINGS_SETTINGS_MODULE_HPP

#include <cstdint>
#include <string>

namespace obs_setup {
namespace settings {

/** Default NVENC preset (p1 = max performance, p7 = max quality). */
inline constexpr const char *DEFAULT_ENCODER_PRESET = "p5";
/** Target sample rate: 48 kHz industry standard. */
inline constexpr uint32_t AUDIO_SAMPLE_RATE_HZ = 48000;
/** Target channel setup. */
inline constexpr const char *AUDIO_CHANNEL_SETUP = "Stereo";
/** Recording quality value as stored by OBS (UI label "Indistinguishable"). */
inline constexpr const char *RECORDING_QUALITY = "HQ";
/** Recording container format. */
inline constexpr const char *RECORDING_FORMAT = "mkv";

/**
 * @brief Maps a Simple-mode encoder UI string to its libobs encoder id.
 *
 * Pure function. SimpleOutput/StreamEncoder stores short UI strings
 * ("x264", "nvenc", "qsv", "amd", "apple_h264", ...), not libobs ids.
 * This mirrors OBS's get_simple_output_encoder() in
 * frontend/utility/SimpleOutput.cpp.
 *
 * @return The libobs encoder id, or an empty string for unrecognized
 * input (callers must not guess).
 */
std::string libobsEncoderIdForSimpleEncoder(const std::string &simpleEncoder);

/**
 * @brief Simple-mode profile key holding the encoder preset for a libobs
 * encoder id.
 *
 * Pure function. Takes a libobs encoder id (see
 * libobsEncoderIdForSimpleEncoder() to convert from the SimpleOutput/
 * StreamEncoder UI string). Matches encoder families by id prefix:
 * NVENC (obs_nvenc_*, ffmpeg_nvenc) → "NVENCPreset2"; x264 (obs_x264) →
 * "Preset"; QSV (obs_qsv*) → "QSVPreset"; AMD AMF (amd_amf_*, *_texture_amf)
 * → "AMDPreset", or "AMDAV1Preset" for AV1 variants.
 *
 * @return The key name, or an empty string for unrecognized encoders
 * (callers must not guess — "preset"/"preset2" are encoder *property*
 * names, not profile keys).
 */
std::string presetKeyForEncoder(const std::string &encoderId);

/**
 * @brief True when migrateToAdvancedMode() supports this encoder id.
 *
 * Pure function. Only NVENC and x264 are supported: their Advanced-mode
 * preset value strings are verified (NVENC: identical p1–p7; x264: standard
 * preset names). QSV/AMF/Apple preset mappings are unverified, so migration
 * refuses them rather than guessing.
 */
bool canMigrateEncoderPreset(const std::string &encoderId);

/**
 * @brief True when `preset` is a standard x264 preset name.
 *
 * Pure function. Used to validate the Simple-mode preset value before
 * copying it 1:1 into streamEncoder.json.
 */
bool isValidX264Preset(const std::string &preset);

/** Outcome of SettingsManager::migrateToAdvancedMode(). */
enum class MigrateResult {
  Success,       ///< Migrated; caller should show the recording-settings modal.
  NotSimpleMode, ///< Output/Mode is not "Simple"; nothing was changed.
  /// StreamEncoder UI string missing/unrecognized, or the resolved libobs
  /// id's obs_encoder_get_display_name() returned null.
  UnknownEncoder,
  UnsupportedEncoder, ///< Encoder recognized but its preset value mapping is
                      ///< unverified (QSV/AMF/Apple), or the preset value is
                      ///< missing/unrecognized. Migration refused.
  WriteFailed, ///< Could not write streamEncoder.json or save the profile
               ///< config.
};

/**
 * @brief Applies the plugin-owned settings OBS's wizard does not set.
 *
 * All methods must be called on the GUI thread (OBS frontend API
 * requirement). Failures are logged via blog(); apply* methods report via
 * return values where the caller can act, otherwise they log and no-op
 * rather than throwing.
 *
 * Explicitly NOT handled here: keyframe interval. In Simple output mode
 * (which the wizard forces) there is no keyframe-interval config path —
 * the streaming service applies its recommendation automatically.
 */
class SettingsManager {
public:
  SettingsManager() = default;

  /**
   * @brief Writes the encoder preset for the active Simple-mode encoder.
   *
   * Reads the active encoder UI string from SimpleOutput/StreamEncoder
   * ("nvenc", "x264", ...), resolves it to a libobs id via
   * libobsEncoderIdForSimpleEncoder(), then writes `preset` to the exact
   * profile key from presetKeyForEncoder(). The preset value is validated
   * against the encoder family (NVENC: p1-p7; x264: standard preset names)
   * — a mismatch is refused, never written. Unknown encoders are skipped
   * (logged), never guessed.
   */
  void applyEncoderPreset(const std::string &preset);

  /** @brief Sets 48 kHz sample rate and stereo channels (Audio section). */
  void applyAudioSettings();

  /**
   * @brief Sets MKV format, HQ ("Indistinguishable") quality, auto-remux.
   *
   * Writes SimpleOutput/RecQuality="HQ", SimpleOutput/RecFormat2="mkv",
   * Video/AutoRemux=true — the exact keys OBS 31 reads (verified against
   * frontend/utility/SimpleOutput.cpp and real basic.ini fixtures).
   */
  void applyRecordingSettings();

  /**
   * @brief Migrates Simple-mode streaming settings to Advanced mode.
   *
   * Only runs when Output/Mode == "Simple" (otherwise NotSimpleMode).
   * Reads the Simple-mode encoder UI string from SimpleOutput/
   * StreamEncoder, resolves it to a libobs id via
   * libobsEncoderIdForSimpleEncoder(), copies that id → AdvOut/Encoder
   * (validated via obs_encoder_get_display_name), preset →
   * streamEncoder.json `preset`, and VBitrate → streamEncoder.json
   * `bitrate`. streamEncoder.json is written with obs_data_save_json_safe
   * into the profile directory. Output/Mode is written as "Advanced"
   * LAST, after all targets are staged.
   *
   * Aborts the entire migration (no writes) on unknown/missing encoder or
   * unverified preset mapping — never half-migrates. If config_save()
   * fails after Output/Mode was staged in memory, the old mode value is
   * restored before returning WriteFailed. Recording settings are
   * intentionally NOT migrated (no Advanced equivalent); on Success the
   * caller must show the modal directing the user to Settings → Output,
   * gated by needsRecordingModal(). The audio bitrate (ABitrate) is
   * intentionally not migrated: its Advanced-mode destination is
   * unverified, and the encoder default is safe.
   */
  MigrateResult migrateToAdvancedMode();

  /**
   * @brief True when the last migrateToAdvancedMode() succeeded and the UI
   * should show the "set recording settings manually" modal.
   */
  bool needsRecordingModal() const { return m_needsRecordingModal; }

private:
  bool m_needsRecordingModal = false;
};

} // namespace settings
} // namespace obs_setup

#endif // OBS_SETUP_SETTINGS_SETTINGS_MODULE_HPP
