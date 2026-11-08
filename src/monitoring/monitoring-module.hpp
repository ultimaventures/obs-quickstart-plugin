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
/**
 * Marginal band: rates above the stable thresholds but at or under
 * these are "marginal" — borderline evidence from one noisy 30-second
 * window, not proof of a broken setup. A marginal result does NOT keep
 * a stepped preset (the original is restored) and suggests a retest,
 * instead of treating 1.1% exactly like a blowout failure.
 */
inline constexpr double MARGINAL_LAGGED_FRAME_PERCENT = 3.0;
inline constexpr double MARGINAL_SKIPPED_FRAME_PERCENT = 3.0;
/**
 * Evidence-quality gate: the PC must actually deliver the target frame
 * rate. An attempt whose rendered frames fall below this ratio of
 * (window duration × target FPS) cannot be called stable or marginal,
 * whatever the lag percentages say — a steady low-rate render produces
 * few formally "missed" deadlines while delivering a fraction of the
 * frames.
 */
inline constexpr double MIN_DELIVERED_FRAME_RATIO = 0.90;
/**
 * Settle policy (post-recording): the test file may be deleted only
 * after this many consecutive one-second polls in which the recording
 * is inactive AND no tracked file changed size — the guard against
 * deleting a file an auto-remux is still writing.
 */
inline constexpr int SETTLE_REQUIRED_QUIET_POLLS = 2;
/**
 * Settle budget, in one-second polls. When it runs out the files are
 * LEFT IN PLACE and reported — a settle timeout never deletes files
 * that may still be in use.
 */
inline constexpr int SETTLE_MAX_POLLS = 20;

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

/** Three-way verdict tier for one attempt. */
enum class Tier { Stable, Marginal, Unstable };

/** The stability verdict for one attempt, with the measured rates. */
struct Stability {
  Tier tier = Tier::Unstable;
  /** True exactly when tier is Tier::Stable. */
  bool stable = false;
  double laggedPercent = 0.0;
  double skippedPercent = 0.0;
  /**
   * Whether rendered frames reached MIN_DELIVERED_FRAME_RATIO of
   * (duration × target FPS). True when duration or target FPS is
   * unknown (no evidence either way — the gate only fires on measured
   * shortfall).
   */
  bool frameRateDelivered = true;
};

/**
 * @brief Decides stability from one attempt's metrics.
 *
 * Stable when lagged and skipped frames are each at or under their
 * percent thresholds AND the target frame rate was delivered.
 * Marginal when both rates are within the wider marginal band and the
 * frame rate was delivered. An attempt with zero frames carries no
 * evidence and is unstable. Pure function.
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

/**
 * @brief How many attempts are actually available starting from a
 * specific preset: the first attempt plus one per remaining ladder
 * rung, capped by maxAttempts(). A preset already at the bottom rung
 * gets exactly one attempt — the dialog announces this number, so it
 * must never promise retries that cannot happen. Pure function.
 */
int attemptsAvailable(EncoderFamily family, const std::string &currentPreset);

/**
 * @brief Whether the recording encoder's preset is governed by the
 * Simple-mode preset config key — i.e. whether stepping that key
 * (settings::applyEncoderPreset) would actually change the encoder
 * the recording test measures.
 *
 * Established from OBS's SimpleOutput (frontend/utility/
 * SimpleOutput.cpp at 31.1.1: LoadRecordingPreset,
 * UpdateRecordingSettings and its per-family helpers):
 * - RecQuality "Stream": the file output reuses the streaming
 *   encoder instance itself (videoRecording = videoStreaming), so
 *   the streaming preset key governs — for the families that HAVE
 *   a verified key (NVENC, x264; settings::presetsFor is empty for
 *   the others).
 * - Every other quality ("HQ" and the other lossy tiers,
 *   "Lossless"): the recording encoder is a SEPARATE instance
 *   (built from SimpleOutput/RecEncoder, which can even differ
 *   from StreamEncoder) whose settings are constructed in code —
 *   x264 gets a hardcoded "veryfast" preset, AMF a hardcoded
 *   "quality", and NVENC/QSV/Apple get NO preset written at all
 *   (the encoder runs at its own internal default). No profile
 *   preset key reaches any of them, so stepping the streaming key
 *   would change an encoder the test is not measuring while the
 *   measured one stays exactly as it was.
 *
 * When this returns false the test gets exactly one attempt: it
 * measures the encoder that actually records, and reports a verdict
 * about THAT encoder, instead of stepping a dormant key. Pure
 * function.
 */
bool recordingPresetGovernedByConfig(EncoderFamily family,
                                     const std::string &recQuality);

/** One settle poll's observations, fed to SettleTracker. */
struct SettleObservation {
  /** OBS still reports the recording output active. */
  bool recordingActive = false;
  /** Any tracked file changed size (or appeared) since the last poll. */
  bool filesChanged = false;
  /** The user asked to cancel during settling. */
  bool cancelRequested = false;
};

/** What the settle tracker decides at a poll. */
enum class SettleAction {
  /** Keep waiting. */
  Wait,
  /** Recording inactive and files quiet long enough: safe to delete. */
  DeleteNow,
  /**
   * Do not delete: the budget ran out, or a cancel arrived while files
   * were still in flight. The files stay where they are and are
   * reported to the user.
   */
  LeaveFiles
};

/**
 * @brief Pure settle policy for post-recording cleanup.
 *
 * Deletion is permitted only after SETTLE_REQUIRED_QUIET_POLLS
 * consecutive polls with the recording inactive and no file changes.
 * The tracker NEVER returns DeleteNow while the recording is active or
 * files are changing — including on budget expiry and on cancel (a
 * cancel short-circuits the wait, but DeleteNow still requires the
 * full quiet-poll evidence; anything less is LeaveFiles). Once a
 * terminal action (DeleteNow / LeaveFiles) is returned it is sticky.
 */
class SettleTracker {
public:
  SettleAction observe(const SettleObservation &observation);

private:
  int m_polls = 0;
  int m_quietPolls = 0;
  bool m_done = false;
  SettleAction m_final = SettleAction::Wait;
};

/** Which of the test's files cleanup may delete. */
struct CleanupPlan {
  bool deleteRecording = false;
  bool deleteRemuxSibling = false;
};

/**
 * @brief Decides which files cleanup may delete, from ownership
 * evidence gathered at attempt start.
 *
 * Rules: in fallback mode (the recording path could not be read from
 * the output, so file identity is guesswork) nothing is deleted — the
 * paths are reported instead. The recording itself is deleted only
 * when no file existed at its exact path before the attempt. The
 * remux sibling (.mp4, same stem) is deleted only when it did NOT
 * exist before the attempt — a same-stem .mp4 that was already there
 * is the user's recording, never cleanup. Pure function.
 */
CleanupPlan planCleanup(bool fallbackMode, bool haveRecordingPath,
                        bool recordingExistedAtStart,
                        bool remuxSiblingExistedAtStart);

} // namespace monitoring
} // namespace obs_setup

#endif // OBS_SETUP_MONITORING_MONITORING_MODULE_HPP
