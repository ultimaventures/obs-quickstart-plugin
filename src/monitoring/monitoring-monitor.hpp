#ifndef OBS_SETUP_MONITORING_MONITORING_MONITOR_HPP
#define OBS_SETUP_MONITORING_MONITORING_MONITOR_HPP

#include "monitoring-module.hpp"

#include <QElapsedTimer>
#include <QObject>
#include <map>
#include <set>
#include <string>
#include <vector>

struct os_cpu_usage_info;
typedef struct os_cpu_usage_info os_cpu_usage_info_t;

class QTimer;

namespace obs_setup {
namespace monitoring {

/** One completed attempt of the stability test. */
struct AttemptRecord {
  int attempt = 0; ///< 1-based, within its pass.
  /**
   * The preset this attempt measurably ran at, best evidence: the
   * live encoder readback when the encoder reports one; the config
   * preset when the pass is governed (there the config key feeds the
   * very instance being measured, so it IS the measured preset);
   * empty when neither is available — the encoder ran at a preset
   * nobody reported, and the UI must say so rather than guess.
   */
  std::string preset;
  Metrics metrics;
  Stability stability;
};

/** Final result of a stability check, delivered via finished(). */
struct TestOutcome {
  enum class Kind {
    Stable,    ///< An attempt passed; finalPreset is the working preset.
    Marginal,  ///< Best attempt landed in the marginal band; the original
               ///< preset was restored — retest suggested, nothing kept.
    Unstable,  ///< All attempts failed; offers (wizard / fallback) apply.
    Cancelled, ///< The user cancelled.
    Busy,      ///< Refused to start: outputs were already active.
    Error      ///< The test itself failed (see detail).
  };
  /**
   * Headline verdict: the STREAMING pass's verdict (in a single-pass
   * check, that pass's verdict). The recording pass of a dual-pass
   * check reports separately in recordingKind.
   */
  Kind kind = Kind::Error;
  /** Busy output names or an error message, when kind needs one. */
  std::string detail;
  /**
   * The encoder the headline pass actually measured: the live
   * recording output's encoder id, read from the running output (not
   * inferred from configuration — under non-"Stream" recording
   * qualities the recording encoder is a separate instance).
   */
  std::string encoderId;
  EncoderFamily family = EncoderFamily::Unknown;
  /** The profile's SimpleOutput/RecQuality ("" when the profile has
   *  no user value for it). */
  std::string recQuality;
  /**
   * Whether stepping the Simple-mode preset key would change the
   * measured encoder in the headline pass (see
   * recordingPresetGovernedByConfig). When false the headline pass
   * ran exactly one attempt and the dialog must not imply retries
   * happened.
   */
  bool presetGoverned = false;
  /** Preset in effect before the check ("" = encoder default). */
  std::string originalPreset;
  /**
   * Preset in effect when the check ended. On Stable this is the
   * preset that passed (kept deliberately — it is the fix, and the
   * results dialog says so). On Unstable it is the fastest step
   * tried, still in effect — the dialog discloses the change from
   * originalPreset and offers to restore it. On Marginal the stepped
   * preset was NOT kept: finalPreset == originalPreset again —
   * unless presetRestoreFailed says otherwise.
   */
  std::string finalPreset;
  /**
   * A Marginal result tried to restore the original preset and the
   * restore FAILED: the preset is still stepped (finalPreset), and
   * the dialog must offer the restore action instead of reassuring
   * the user that nothing changed.
   */
  bool presetRestoreFailed = false;
  std::vector<AttemptRecord> attempts;
  /** True when the profile uses OBS's Advanced output mode: the
   *  check measured the recording output only, in a single pass,
   *  with no stepping and report-only cleanup. */
  bool advancedMode = false;
  /** Single-pass check where recordings reuse the streaming encoder
   *  (RecQuality "Stream"): the one verdict covers both. */
  bool recordingSharesStreamingEncoder = false;
  /** A separate recording-encoder pass ran after the streaming pass. */
  bool hasRecordingPass = false;
  /** The recording pass produced a measured verdict. */
  bool recordingMeasured = false;
  Kind recordingKind = Kind::Error;
  AttemptRecord recordingAttempt;
  std::string recordingEncoderId;
  /** The recording pass itself failed (recordingKind == Error). */
  std::string recordingError;
  /**
   * The delivered-frames evidence gate could not run because OBS
   * reported no target frame rate: verdicts rest on the
   * dropped-frame percentages alone, and the dialog says so.
   */
  bool frameRateGateInoperative = false;
  /**
   * Truthful cleanup report built from what deletion actually did:
   * what was deleted, what was left in place and where (one note per
   * attempt cleanup, newline-joined). The dialog shows this verbatim
   * instead of asserting cleanup happened.
   */
  std::string cleanupNote;
};

/**
 * @brief Runs the stability test asynchronously — two passes.
 *
 * Main-thread QObject implementing architecture §7's required pattern:
 * recording starts/stops on the main thread, a 1-second QTimer polls
 * metrics, and results arrive through signals — OBS never blocks. All
 * durations are wall-clock (QElapsedTimer), not tick counts: under
 * exactly the overload this test detects, GUI timers slip, and a
 * "30-second" test must mean 30 real seconds.
 *
 * The check measures BOTH encoders a streaming setup actually uses,
 * back to back:
 * - Streaming pass: the recording output temporarily runs the
 *   streaming encoder itself. That is exactly what RecQuality
 *   "Stream" means in OBS's SimpleOutput, so the pass sets
 *   SimpleOutput/RecQuality to "Stream" IN MEMORY ONLY — never
 *   saved (no config_save on any path) — and restores the profile's
 *   exact prior state (value, or absence of a value) when the pass
 *   ends, on every exit path including cancel, error and this
 *   object's destruction. A crash cannot persist the edit; it lives
 *   only in the running process's memory and dies with it. Preset
 *   stepping (the only retries) happens in this pass, where the
 *   preset key provably governs the measured encoder.
 * - Recording pass (skipped when the profile already records at
 *   "Stream" quality, or in Advanced mode): one measured attempt at
 *   the profile's real recording quality, against the separate
 *   recording encoder OBS builds for it. Never stepped — that
 *   encoder's settings are constructed by OBS's code, not by any
 *   profile preset key.
 *
 * Advanced output mode collapses the check to a single
 * recording-output measurement: no in-memory switch (the
 * SimpleOutput keys are dormant there), no stepping, and report-only
 * cleanup (the Simple-mode path evidence does not apply).
 *
 * Measurement window: all three frame counters (rendered, lagged,
 * encoder-skipped) are baselined at the same moment — the first poll
 * where the recording is actually active — and read again when the
 * recording stops, so the verdict's numerators and denominator always
 * describe the same window. Two preset values are kept apart on
 * purpose: the CONFIG preset is the stepping cursor and the restore
 * target; the LIVE readback from the running encoder is what gets
 * reported as measured.
 *
 * Cancel semantics: a cancel in ANY state finishes the check as
 * Cancelled after cleanup — it never evaluates a verdict and never
 * starts another attempt or pass. (Cleanup evidence rules are
 * separate and unchanged: a cancel during settling still needs the
 * settle tracker's full quiet evidence before anything is deleted.)
 *
 * Cleanup follows planCleanup() + SettleTracker (both pure, both
 * unit-tested): only files proven test-created are deleted, only
 * after the recording is inactive and file sizes stayed quiet for
 * the required polls, and never on a settle timeout — leftover files
 * are reported by path instead. Deletion results are propagated into
 * TestOutcome::cleanupNote; the monitor never claims a deletion it
 * did not observe succeed.
 */
class StabilityMonitor : public QObject {
  Q_OBJECT
public:
  explicit StabilityMonitor(QObject *parent = nullptr);
  ~StabilityMonitor() override;

  bool running() const { return m_state != State::Idle; }
  bool settling() const { return m_state == State::Settling; }
  /** 0-based index of the pass currently running. */
  int currentPassIndex() const { return m_passIndex; }
  int passCount() const { return m_passCount; }
  bool onRecordingPass() const { return m_pass == PassKind::Recording; }
  /**
   * Whole seconds left in the current step's measurement window, or
   * -1 when not in a measurement window (settling / idle): the
   * dialog shows a countdown for the former and a "finishing up"
   * state for the latter.
   */
  int stepSecondsRemaining() const;

  /** Starts a check. No-op when one is already running. A busy OBS
   *  produces an immediate finished() with Kind::Busy. */
  void start();
  /**
   * Finishes the check with Kind::Cancelled once the in-flight
   * attempt's cleanup completes. During recording, the recording
   * stops and cleanup runs through the full settle policy; during
   * settling, the wait is cut short but deletion still requires the
   * settle tracker's full quiet-poll evidence — otherwise the files
   * are left and reported. Either way the verdict is Cancelled: no
   * new attempt, no new pass. No-op when idle.
   */
  void cancel();

signals:
  /** Once per second during an attempt. */
  void progress(int secondsElapsed, double fps, double cpuPercent);
  void attemptStarted(int attempt, int maxAttempts);
  /** A pass began (before its first attemptStarted). */
  void passStarted(int passIndex, int passCount, bool recordingPass);
  void finished(const obs_setup::monitoring::TestOutcome &outcome);

private:
  enum class State { Idle, Recording, Settling };
  enum class PassKind { Streaming, Recording };

  /** Sets up a pass (identity, governed, attempt budget, quality
   *  switch for the streaming pass) and begins its first attempt. */
  void startPass(PassKind kind);
  /** The current pass reached a final verdict: advance to the
   *  recording pass when one remains, else finish the check. */
  void passFinished(TestOutcome::Kind verdict);
  void beginAttempt();
  void onTick();
  /** Takes the shared baseline for all frame counters (first active
   *  poll) and pins the test file's identity. */
  void takeBaseline();
  void sampleFinalMetrics();
  /** Runs the settle policy for one poll; returns true when settling
   *  reached a terminal action and the attempt was completed. */
  bool pollSettling();
  void completeAttempt();
  /** mayDelete=false reports the tracked files as left in place and
   *  deletes nothing (settle timeout / unproven cancel / Advanced
   *  mode's report-only cleanup). */
  void performCleanup(bool mayDelete);
  /** Stops the recording and enters Settling with a pending verdict
   *  path: evaluate the attempt (normal), or finish directly with the
   *  given kind/detail (cancel / error). */
  void enterSettling();
  void finish(TestOutcome::Kind kind, const std::string &detail = "");
  /** The exact cleanup candidates for this attempt, path -> size. */
  std::map<std::string, uint64_t> trackedFiles() const;
  std::string remuxSiblingPath() const;
  /** SimpleOutput/FilePath, or "" when the profile has none saved. */
  std::string recordingDirectory() const;
  /** Sets SimpleOutput/RecQuality to "Stream" in memory only (never
   *  saved); remembers the exact prior state for restoreQuality(). */
  void switchQualityToStream();
  /** Undoes switchQualityToStream() exactly (value or absence).
   *  Idempotent; called on pass transition, finish, and destruction. */
  void restoreQuality();

  State m_state = State::Idle;
  QTimer *m_timer = nullptr;
  os_cpu_usage_info_t *m_cpuInfo = nullptr;
  bool m_cancelRequested = false;
  /** A cancel that arrived DURING settling (fed to the tracker);
   *  distinct from a recording-cancel, which settles normally. The
   *  outcome is Cancelled either way — see cancel(). */
  bool m_settleCancelRequested = false;

  // Check-level configuration, captured in start().
  bool m_advancedMode = false;
  /** The profile's RecQuality user value ("" when none is set). */
  std::string m_profileRecQuality;
  bool m_recQualityKnown = false;
  PassKind m_pass = PassKind::Streaming;
  int m_passIndex = 0;
  int m_passCount = 1;
  bool m_hasRecordingPass = false;
  bool m_streamingDone = false;

  // In-memory RecQuality switch state (streaming pass).
  bool m_qualitySwitched = false;
  bool m_savedQualityHadUserValue = false;

  TestOutcome m_outcome;
  /** The CONFIG preset: stepping cursor and restore target. Never
   *  overwritten by live readback — the two are different things. */
  std::string m_currentPreset;
  /** The running encoder's own preset readback for this attempt
   *  ("" when the encoder reports none). Display/reporting only. */
  std::string m_measuredPreset;
  int m_attempt = 0;
  int m_maxAttempts = 1;

  // Per-attempt timing (wall-clock; see class comment).
  QElapsedTimer m_attemptClock;
  QElapsedTimer m_windowClock;
  bool m_baselineTaken = false;
  uint32_t m_baseTotalFrames = 0;
  uint32_t m_baseLaggedFrames = 0;
  uint32_t m_baseSkippedFrames = 0;
  double m_fpsSum = 0.0;
  int m_fpsSamples = 0;
  double m_cpuSum = 0.0;
  int m_cpuSamples = 0;
  double m_cpuPeak = 0.0;
  double m_targetFps = 0.0;
  Metrics m_attemptMetrics;

  // Per-pass identity. The preliminary values come from config at
  // pass start; takeBaseline() replaces them with the live recording
  // encoder's identity for reporting, and revokes m_presetGoverned
  // if the live encoder contradicts the stepping assumptions.
  bool m_presetGoverned = false;

  // Per-attempt file tracking.
  /** Exact path of the test recording, from the output's settings. */
  std::string m_testFilePath;
  /** True when the recording path could not be read from the output:
   *  file identity is unproven, so cleanup deletes nothing. */
  bool m_fallbackMode = false;
  /** Advanced mode: cleanup reports, never deletes (primary review:
   *  non-Simple mode means report-only cleanup). */
  bool m_forceReportOnly = false;
  bool m_recordingExistedAtStart = false;
  bool m_siblingExistedAtStart = false;
  /** Auto-remux was on for this attempt and the sibling is a
   *  different file than the recording: settle polls do not count
   *  as quiet until it appears (SettleObservation). */
  bool m_siblingExpected = false;
  bool m_autoRemux = false;
  /**
   * Entry NAMES (not sizes) in the recording directory, captured
   * before the attempt's recording starts: the ownership evidence
   * for the recording file itself. Names-only keeps the per-attempt
   * cost to one directory listing. The snapshot counts as evidence
   * only if the test file's actual parent directory matches
   * m_snapshotDir (checked at baseline) and the listing completed
   * without error; otherwise ownership is unprovable and the file
   * is treated as pre-existing — report, never delete.
   */
  std::set<std::string> m_dirNamesAtStart;
  std::string m_snapshotDir;
  bool m_dirSnapshotTaken = false;
  std::map<std::string, uint64_t> m_lastTrackedFiles;
  SettleTracker m_settleTracker;
  /** Pending finish for attempts that end through settling without a
   *  verdict evaluation (cancel / error); kEvaluate = normal path. */
  enum class Pending { Evaluate, FinishInstead } m_pending = Pending::Evaluate;
  TestOutcome::Kind m_pendingKind = TestOutcome::Kind::Error;
  std::string m_pendingDetail;
  std::vector<std::string> m_cleanupNotes;
};

} // namespace monitoring
} // namespace obs_setup

#endif // OBS_SETUP_MONITORING_MONITORING_MONITOR_HPP
