#ifndef OBS_SETUP_MONITORING_MONITORING_MODULE_HPP
#define OBS_SETUP_MONITORING_MONITORING_MODULE_HPP

#include <cstdint>
#include <optional>
#include <string>

namespace obs_setup {
namespace monitoring {

// The stability test (docs/architecture.md §7, "Local Recording Test &
// Metrics"): a 30-second local recording that validates the configured
// setup under sustained load. On instability it retries, stepping ONLY
// the encoder's quality ladder down; resolution/FPS/bitrate belong to
// OBS's Auto-Configuration Wizard and are never rewritten by the test.

/** Seconds of recording per attempt. */
inline constexpr int TEST_DURATION_SECONDS = 30;
/**
 * Per-attempt watchdog, in seconds (architecture §7: the test dialog
 * has Cancel and a 45-second timeout). Covers recording start, the
 * 30-second record, stop, and file settling; an attempt that has not
 * produced a verdict within this budget is aborted as an error rather
 * than wedging the dialog.
 */
inline constexpr int ATTEMPT_TIMEOUT_SECONDS = 45;
/** Total attempts for encoders with a quality ladder (first run + 2
 *  stepped retries). */
inline constexpr int MAX_LADDER_ATTEMPTS = 3;
/**
 * Stability thresholds: an attempt is stable when neither rendering-lag
 * frames nor encoder-skipped frames exceed this percent of all frames
 * in the window. CPU is reported but deliberately NOT gated — the old
 * CPU gate was measured on a near-idle desktop and is not a reliable
 * signal (architecture §6 note).
 */
inline constexpr double MAX_LAGGED_FRAME_PERCENT = 1.0;
inline constexpr double MAX_SKIPPED_FRAME_PERCENT = 1.0;

/** Encoder families the stability test distinguishes. */
enum class EncoderFamily { NVENC, X264, QSV, AMF, VideoToolbox, Unknown };

/**
 * @brief Classifies a libobs encoder id (as returned by
 * settings::libobsEncoderIdForSimpleEncoder) into a family.
 * Pure function.
 */
EncoderFamily familyForEncoderId(const std::string &encoderId);

/** Metrics collected over one recording attempt. */
struct Metrics {
  double durationSeconds = 0.0;
  /** Rendered frames in the window (delta of obs_get_total_frames). */
  uint64_t totalFrames = 0;
  /** Frames that missed their render deadline (rendering lag). */
  uint64_t laggedFrames = 0;
  /** Frames the recording encoder could not encode in time. */
  uint64_t skippedFrames = 0;
  double averageFps = 0.0;
  double targetFps = 0.0;
  /** CPU is informational only; never part of the verdict. */
  double averageCpuPercent = 0.0;
  double peakCpuPercent = 0.0;
};

/** The stability verdict for one attempt, with the measured rates. */
struct Stability {
  bool stable = false;
  double laggedPercent = 0.0;
  double skippedPercent = 0.0;
};

/**
 * @brief Decides stability from one attempt's metrics.
 *
 * Stable when lagged and skipped frames are each at or under their
 * percent thresholds. An attempt with zero frames carries no evidence
 * and is never stable. Pure function.
 */
Stability evaluateStability(const Metrics &metrics);

/**
 * @brief The next quality-ladder step DOWN (toward performance) for an
 * encoder family, or std::nullopt when no step exists.
 *
 * NVENC: p7..p2 step one rung toward p1. x264: one rung toward
 * ultrafast in the standard preset order. QSV and AMF have ladders in
 * the product spec, but their Simple-mode preset vocabularies are
 * unverified (the settings module's presetsFor() returns empty for
 * them — never guess), so they report no step until that verification
 * lands; VideoToolbox has no ladder by design. Pure function.
 */
std::optional<std::string> nextLadderStep(EncoderFamily family,
                                          const std::string &currentPreset);

/**
 * @brief How many attempts an encoder family gets: MAX_LADDER_ATTEMPTS
 * when it has a usable ladder, otherwise a single attempt (the spec's
 * VideoToolbox rule, extended to families whose ladder is not yet
 * verified — repeating an identical test proves nothing). Pure
 * function.
 */
int maxAttempts(EncoderFamily family);

} // namespace monitoring
} // namespace obs_setup

#endif // OBS_SETUP_MONITORING_MONITORING_MODULE_HPP
