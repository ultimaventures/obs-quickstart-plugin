#ifndef OBS_SETUP_SETTINGS_SETTINGS_MODULE_HPP
#define OBS_SETUP_SETTINGS_SETTINGS_MODULE_HPP

#include <cstdint>
#include <string>
#include <vector>

namespace obs_setup {
namespace settings {

/** Default NVENC preset (p1 = max performance, p7 = max quality). */
inline constexpr const char *DEFAULT_ENCODER_PRESET = "p5";
/** Default x264 preset. */
inline constexpr const char *DEFAULT_X264_PRESET = "veryfast";
/** Target sample rate: 48 kHz industry standard. */
inline constexpr uint32_t AUDIO_SAMPLE_RATE_HZ = 48000;
/** Target channel setup. */
inline constexpr const char *AUDIO_CHANNEL_SETUP = "Stereo";
/** Recording quality value as stored by OBS (UI label "Indistinguishable"). */
inline constexpr const char *RECORDING_QUALITY = "HQ";
/** Recording container format. */
inline constexpr const char *RECORDING_FORMAT = "mkv";
/** Conservative fallback (architecture §7): 720p30 at 2500 kbps with
 *  x264 ultrafast — offered, never auto-applied, when the stability
 *  test still fails after its ladder retries. */
inline constexpr uint32_t FALLBACK_OUTPUT_WIDTH = 1280;
inline constexpr uint32_t FALLBACK_OUTPUT_HEIGHT = 720;
inline constexpr const char *FALLBACK_FPS_COMMON = "30";
inline constexpr int FALLBACK_VIDEO_BITRATE_KBPS = 2500;

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

/**
 * @brief The verified preset values for a libobs encoder id.
 *
 * Pure function. Single source of truth for the preset vocabulary:
 * SettingsManager validates against this list and the settings dialog
 * populates its preset picker from it, so the two can never drift.
 * NVENC (obs_nvenc_*) → p1–p7; x264 (obs_x264) → the standard preset
 * names. Every other encoder (QSV/AMF/Apple/unknown) returns an empty
 * list: their preset vocabularies are unverified, so callers must
 * refuse rather than guess.
 */
std::vector<std::string> presetsFor(const std::string &encoderId);

/**
 * @brief The recommended default preset for a libobs encoder id.
 *
 * Pure function. NVENC → "p5", x264 → "veryfast". Returns an empty
 * string for encoders without a verified preset list (see presetsFor()).
 */
std::string defaultPresetFor(const std::string &encoderId);

/** Outcome of SettingsManager::stepRecordingQualityDown(). */
enum class QualityStepResult {
  Stepped,       ///< One rung written and saved; from/to are set.
  NoStepExists,  ///< Already at "Stream", or the stored quality is
                 ///< not one of OBS's four values; nothing changed.
  NotSimpleMode, ///< Output/Mode is not "Simple"; nothing changed.
  WriteFailed,   ///< The save failed; the old value was restored.
};

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
 * requirement). Failures are logged via blog() AND reported via the
 * apply* methods' bool returns, so the UI never has to claim a success
 * it could not verify.
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
   *
   * @return True only when the value was written and config_save_safe()
   * succeeded; false on missing profile config, missing/unrecognized
   * encoder, invalid preset, or save failure.
   */
  bool applyEncoderPreset(const std::string &preset);

  /**
   * @brief Sets 48 kHz sample rate and stereo channels (Audio section).
   *
   * @return True only when the values were written and config_save_safe()
   * succeeded. Note the running OBS session keeps its startup sample
   * rate until OBS restarts — a no-edit Settings > OK does not reset
   * the audio engine either (verified on OBS 30). The write landing is
   * what this return value vouches for. On save failure the previous
   * in-memory values are restored before returning false, so a later
   * OBS save cannot persist values reported as FAILED (the same
   * restore applies to all three apply* methods).
   */
  bool applyAudioSettings();

  /**
   * @brief Sets MKV format, HQ ("Indistinguishable") quality, auto-remux.
   *
   * Writes SimpleOutput/RecQuality="HQ", SimpleOutput/RecFormat2="mkv",
   * Video/AutoRemux=true — the exact keys OBS 31 reads (verified against
   * frontend/utility/SimpleOutput.cpp and real basic.ini fixtures).
   *
   * @return True only when the values were written and config_save_safe()
   * succeeded.
   */
  bool applyRecordingSettings();

  /**
   * @brief Applies the conservative fallback — the stability ladder's
   * last rung: a 720p canvas AND output at 30 fps, recording quality
   * "Same as stream", 2500 kbps, x264 at ultrafast.
   *
   * The bundle spends the two biggest savings a struggling machine
   * has (user ruling, 2026-11-08): RecQuality="Stream" collapses the
   * two encodes into one, and BaseCX/BaseCY at 1280x720 removes the
   * 1080p scene-rendering cost entirely (~2.25x the pixels — the
   * dominant strain on weak integrated GPUs; merely scaling the
   * output down would leave it in place, which is also why OBS's own
   * wizard lowers the canvas, not just the output).
   *
   * Writes Video/BaseCX, Video/BaseCY, Video/OutputCX, Video/OutputCY,
   * Video/FPSCommon, SimpleOutput/VBitrate,
   * SimpleOutput/StreamEncoder="x264", SimpleOutput/RecQuality="Stream"
   * and the x264 preset key — in ONE config_save_safe() with the same
   * capture-and-restore discipline as the other apply* methods, so the
   * fallback lands whole or not at all. Simple mode only: a profile
   * whose Output/Mode is missing or not "Simple" is refused (missing
   * is not treated as Simple — the same rule migrateToAdvancedMode
   * uses). Profiles whose FPS selector (Video/FPSType) is Integer or
   * Fraction are detected and refused: FPSCommon does not govern
   * their frame rate, so the fallback could not honestly call the
   * result 720p30. Scope the code cannot detect is scope it must
   * not silently claim.
   *
   * Effect timing: the preset and bitrate apply at the next output
   * start; the canvas, frame rate and recording-quality changes are
   * read when OBS builds its video/outputs, so they land after an
   * OBS restart — the caller says so, and the stability check's
   * pending-retest flow verifies the result on the next launch.
   *
   * This is an OFFER, never an automatic action — the caller must
   * have the user's explicit confirmation before calling. On a
   * profile that is not plugin-managed, the caller must first warn
   * that a smaller canvas changes scene shapes and recommend
   * duplicating the profile (user ruling, 2026-11-08).
   *
   * @return True only when every value was written and saved.
   */
  bool applyConservativeFallback();

  /**
   * @brief Lowers SimpleOutput/RecQuality by ONE rung (the stability
   * ladder's restart step): HQ -> Small -> Stream (monitoring::
   * nextRecordingQualityStep holds the table).
   *
   * One rung, never a jump to the bottom: each step is the smallest
   * change that could work, and the pending-retest flow verifies it
   * after the restart this change needs — RecQuality is read when
   * OBS builds its outputs (SimpleOutput's constructor), so the new
   * value takes effect on the next OBS launch, not mid-session. The
   * caller owns communicating that, and recording the pending
   * retest (monitoring::savePendingRetest).
   *
   * Reads the EFFECTIVE quality (config_get_string, so a registered
   * default counts); writes with the capture-and-restore discipline:
   * on a failed save the previous state is restored exactly (a key
   * that had no user value is removed again). Simple mode only, by
   * the same user-value rule as applyConservativeFallback.
   *
   * @param fromQuality  Set to the previous quality on Stepped.
   * @param toQuality    Set to the new quality on Stepped.
   */
  QualityStepResult stepRecordingQualityDown(std::string &fromQuality,
                                             std::string &toQuality);

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
   * unverified preset mapping — never half-migrates. If the atomic
   * config_save_safe() fails after Output/Mode was staged in memory, the old
   * mode value is restored before returning WriteFailed. Recording settings are
   * intentionally NOT migrated (no Advanced equivalent); on Success the
   * caller must show the modal directing the user to Settings → Output,
   * gated by needsRecordingModal(). The modal must also say the change
   * takes effect only after an OBS restart: the running session keeps
   * its Simple output handler until then — verified empirically on
   * OBS 30 (post-migrate recordings still used the Simple handler, and
   * a no-edit Settings > OK did not apply it either; OBS rebuilds its
   * outputs only for categories the user actually changed). The audio
   * bitrate (ABitrate) is
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
