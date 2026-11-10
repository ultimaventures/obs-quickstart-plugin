#ifndef OBS_SETUP_MONITORING_MONITORING_MODULE_HPP
#define OBS_SETUP_MONITORING_MONITORING_MODULE_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

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
 * Per-attempt watchdog for the RECORDING phase, in seconds
 * (architecture §7: the test dialog has Cancel and a 45-second
 * timeout). Covers recording start and the 30-second window with
 * margin; an attempt whose recording phase exceeds it is aborted as
 * an error rather than wedging the dialog. File settling after the
 * recording stops has its own budget — SETTLE_MAX_POLLS, enforced by
 * SettleTracker — and is not bounded by this watchdog.
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
 * Which side of a check a failure is attributed to (dialog copy keys
 * off it). Combined mode measures the streaming+recording PAIR
 * through one pipeline counter, so a combined failure is Pair: the
 * combination was too heavy. It is deliberately NOT "both sides
 * failed" — the measurement cannot show that, and no Recording value
 * exists for combined mode because there is no separate recording
 * measurement to fail. Recording is for Advanced mode, which measures
 * the recording output alone; Streaming is for checks that measured
 * the streaming encode alone.
 */
enum class FailedSide { None, Streaming, Recording, Pair };

/** What the monitor should do with a completed attempt. */
struct AttemptDecision {
  enum class Action {
    FinishStable,   ///< The final attempt passed: finish Stable.
    FinishMarginal, ///< Borderline: finish Marginal (restoring the
                    ///< original preset first when restoreOriginal).
    FinishUnstable, ///< No step remains: finish Unstable with
                    ///< failedSide as the attribution.
    StepAndRetry    ///< Apply stepPreset and run another attempt.
  };
  Action action = Action::FinishUnstable;
  /** Attribution for FinishUnstable; None otherwise. */
  FailedSide failedSide = FailedSide::None;
  /** FinishMarginal only: the run stepped the preset, and marginal
   *  evidence does not justify keeping the step — put the original
   *  back before finishing. */
  bool restoreOriginal = false;
  /** StepAndRetry only: the preset to apply for the next attempt. */
  std::string stepPreset;
};

/**
 * @brief Decides the outcome of a completed attempt, purely.
 *
 * The verdict is the FINAL attempt's tier — full stop. Earlier
 * attempts inform the run only through the stepping history (which
 * preset is in force, whether steps remain); no earlier attempt's
 * numbers can downgrade a later pass. (An earlier design kept the
 * first attempt's combined-window numbers in a separate "recording
 * verdict" that vetoed a later Stable — reporting a fixed problem as
 * a failure, blamed on a side the shared pipeline counter cannot
 * distinguish. Removed 2026-11-08 on three reviewers' convergent
 * findings; the fields went with it so the shape cannot return.)
 *
 * Attribution for an exhausted Unstable run comes only from what was
 * measured: Advanced mode measured recording alone (Recording);
 * combined mode measured the pair (Pair); anything else measured the
 * streaming encode (Streaming). Stepping is offered only when the
 * preset is governed, attempts remain, and nextStep names a rung —
 * the caller computes nextStep with nextLadderStep() so this function
 * stays free of ladder tables. Pure function.
 */
AttemptDecision decideOutcome(Tier tier, bool combinedMode, bool advancedMode,
                              bool presetGoverned, bool stepsRemain,
                              const std::optional<std::string> &nextStep,
                              bool presetWasStepped);

/**
 * @brief The next recording-quality rung DOWN (toward performance)
 * in Simple mode, or "" when no lower rung exists.
 *
 * The stored vocabulary is exactly four strings (OBS 31.1.1,
 * frontend/settings/OBSBasicSettings.cpp, the ADD_QUALITY combo
 * entries): "Stream", "Small", "HQ", "Lossless". The cost ladder
 * (frontend/utility/SimpleOutput.cpp, UpdateRecordingSettings):
 * "HQ" encodes at CRF/CQP 16 (Apple 70) — the UI's "Indistinguishable
 * Quality"; "Small" at CRF/CQP 23 (Apple 50) — the UI's "High
 * Quality, Medium File Size"; "Stream" runs no second encode at all.
 * So the steps are HQ -> Small -> Stream. "Lossless" is a separate
 * uncompressed output type, not a lossy rung; stepping from it lands
 * on "HQ" (leaving the lossless output behind is itself the saving
 * that matters there — file size, not encode cost). Unknown strings
 * have no step: never guess at a value OBS didn't define. Pure
 * function.
 */
std::string nextRecordingQualityStep(const std::string &currentQuality);

/**
 * @brief Short user-facing name for a stored recording-quality
 * string ("HQ" -> "Indistinguishable Quality", "Small" -> "High
 * Quality", "Stream" -> "Same as stream", "Lossless" -> "Lossless").
 * Unknown strings are returned unchanged. Pure function.
 */
std::string recordingQualityDisplayName(const std::string &quality);

/**
 * @brief Whether a stored pending-retest record (see
 * monitoring-ladder-state.hpp) is well-formed enough to act on.
 *
 * The state file is plugin-owned, but it is still just a file: a
 * hand edit or a torn write must not produce a launch prompt with a
 * nonsense stage or an empty "( -> )" quality pair. The stage must
 * be one the ladder writes ("quality" or "fallback"), and a quality
 * step must name both qualities. Pure function.
 */
bool pendingRetestWellFormed(const std::string &stage,
                             const std::string &fromQuality,
                             const std::string &toQuality);

/** The ladder state file's profile index with profileName added
 *  (unchanged when already present). Pure function. */
std::vector<std::string> ladderIndexWith(const std::vector<std::string> &index,
                                         const std::string &profileName);

/** The ladder state file's profile index with profileName removed.
 *  Pure function. */
std::vector<std::string>
ladderIndexWithout(const std::vector<std::string> &index,
                   const std::string &profileName);

/** Indexed profiles that no longer exist (their records are stale
 *  and should be dropped). Pure function. */
std::vector<std::string>
ladderIndexStale(const std::vector<std::string> &index,
                 const std::vector<std::string> &existingProfiles);

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
  /**
   * Auto-remux is on for this recording, so a remux sibling (.mp4)
   * is expected to appear. While expected-but-absent, polls do not
   * count as quiet: no OBS API reports remux completion, and the
   * sibling's appearance is the only observable trace. Bounded like
   * everything else by SETTLE_MAX_POLLS — a remux that never
   * produces the file ends in LeaveFiles, never in an endless wait.
   */
  bool siblingExpected = false;
  /** The remux sibling exists at this poll. */
  bool siblingPresent = false;
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
 * consecutive quiet polls. A poll is quiet when the recording is
 * inactive, no tracked file changed, and — when auto-remux is on
 * and a remux sibling is therefore expected — the sibling has
 * appeared. The budget is checked BEFORE the quiet evidence: a poll
 * that arrives after SETTLE_MAX_POLLS ends the wait as LeaveFiles
 * even if it is quiet. A cancel short-circuits the wait but never
 * weakens the evidence: with the full quiet polls already counted
 * it still deletes, otherwise it leaves the files. The tracker
 * NEVER returns DeleteNow on an unquiet poll. Once a terminal
 * action (DeleteNow / LeaveFiles) is returned it is sticky.
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

/**
 * Decides when the frontend recording stop may be issued during
 * settling. obs_frontend_recording_stop() is NOT idempotent: the
 * frontend passes its "already stopping" state as a force flag
 * (OBS 31.1.1: OBSBasic::StopRecording() passes recordingStopping to
 * the output handler, and SimpleOutput::StopRecording(force) calls
 * obs_output_force_stop() when it is set), so a second call while the
 * recording is still stopping force-stops it and skips the graceful
 * muxer finish. The gate therefore issues exactly one stop per
 * activation: on the rising edge (inactive → active) when no stop
 * has been issued for this activation yet. Seeing the output fully
 * inactive re-arms it, so an output that only starts after settling
 * began (asynchronous starts) still gets its one stop. The probe's
 * own obs_output_stop() has no such issue — libobs makes a repeat
 * graceful stop a no-op — and is not gated here. Pure; unit-tested.
 */
class FrontendStopGate {
public:
  /** Feed the frontend recording's current active state; returns
   *  true exactly when a stop should be issued now. */
  bool observe(bool active);

private:
  bool m_wasActive = false;
  bool m_stopIssued = false;
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
