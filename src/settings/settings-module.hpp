#ifndef OBS_SETUP_SETTINGS_SETTINGS_MODULE_HPP
#define OBS_SETUP_SETTINGS_SETTINGS_MODULE_HPP

#include <string>

namespace obs_setup {
namespace settings {

/** Default NVENC preset (p1 = max performance, p7 = max quality). */
inline constexpr const char *DEFAULT_ENCODER_PRESET = "p5";
/** Target sample rate: 48 kHz industry standard. */
inline constexpr uint32_t AUDIO_SAMPLE_RATE_HZ = 48000;
/** Target channel setup. */
inline constexpr const char *AUDIO_CHANNEL_SETUP = "Stereo";
/** Recording quality shown in the UI. */
inline constexpr const char *RECORDING_QUALITY = "Indistinguishable";
/** Recording container format. */
inline constexpr const char *RECORDING_FORMAT = "mkv";

/**
 * @brief Simple-mode profile key holding the encoder preset for an encoder id.
 *
 * Pure function. Matches encoder families by id prefix:
 * NVENC (obs_nvenc_*) → "NVENCPreset2"; x264 (obs_x264) → "Preset";
 * QSV (obs_qsv*) → "QSVPreset"; AMD AMF (amd_amf_*) → "AMDPreset", or
 * "AMDAV1Preset" for AV1 variants.
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
  /// StreamEncoder id missing, or obs_encoder_get_display_name() returned
  /// null for it.
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
   * Reads the active encoder id from SimpleOutput/StreamEncoder, resolves
   * the exact profile key via presetKeyForEncoder(), and writes `preset`.
   * Unknown encoders are skipped (logged), never guessed.
   */
  void applyEncoderPreset(const std::string &preset);

  /** @brief Sets 48 kHz sample rate and stereo channels (Audio section). */
  void applyAudioSettings();

  /**
   * @brief Sets MKV format, Indistinguishable quality, auto-remux.
   *
   * Key names (SimpleOutput/RecQuality, SimpleOutput/RecFormat,
   * SimpleOutput/RecRemux) are best-effort: the planning docs do not pin
   * them against OBS 31.1.1 source. Writing an unknown key is harmless —
   * OBS ignores keys it does not recognize — but the remux key in
   * particular should be re-verified against source.
   */
  void applyRecordingSettings();

  /**
   * @brief Migrates Simple-mode streaming settings to Advanced mode.
   *
   * Only runs when Output/Mode == "Simple" (otherwise NotSimpleMode).
   * Copies encoder id → AdvOut/Encoder (validated via
   * obs_encoder_get_display_name), preset → streamEncoder.json `preset`,
   * and VBitrate → streamEncoder.json `bitrate`. streamEncoder.json is
   * written as obs_data JSON into the profile directory. Output/Mode is
   * written as "Advanced" LAST, after all targets are staged.
   *
   * Aborts the entire migration (no writes) on unknown/missing encoder or
   * unverified preset mapping — never half-migrates. Recording settings
   * are intentionally NOT migrated (no Advanced equivalent); on Success
   * the caller must show the modal directing the user to Settings →
   * Output, gated by needsRecordingModal(). The audio bitrate (ABitrate)
   * is intentionally not migrated: its Advanced-mode destination is
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
