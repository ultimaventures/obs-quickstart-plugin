#ifndef OBS_SETUP_MONITORING_MONITORING_MONITOR_HPP
#define OBS_SETUP_MONITORING_MONITORING_MONITOR_HPP

#include "monitoring-module.hpp"
#include "monitoring-probe.hpp"

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
  int attempt = 0; ///< 1-based.
  /**
   * The preset this attempt measurably ran at, best evidence: the
   * live encoder readback when the encoder reports one; the config
   * preset when stepping is governed (there the config key feeds
   * the very settings the measured encoder was built with); empty
   * when neither is available — the encoder ran at a preset nobody
   * reported, and the UI must say so rather than guess.
   */
  std::string preset;
  Metrics metrics;
  Stability stability;
};

/** Final result of a stability check, delivered via finished(). */
struct TestOutcome {
  enum class Kind {
    Stable,    ///< Everything measured passed.
    Marginal,  ///< Best evidence landed in the marginal band; the
               ///< original preset was restored when stepping ran.
    Unstable,  ///< Something measured failed (see failedSide).
    Cancelled, ///< The user cancelled.
    Busy,      ///< Refused to start: outputs were already active.
    Error      ///< The test itself failed (see detail).
  };
  Kind kind = Kind::Error;
  /** Busy output names or an error message, when kind needs one. */
  std::string detail;
  FailedSide failedSide = FailedSide::None;
  /**
   * The streaming encoder the check measured: in probe modes, the
   * probe's encoder id; when recordings share the streaming encoder
   * ("Stream" quality), the frontend recording output's live id; in
   * Advanced mode, the recording encoder's id.
   */
  std::string encoderId;
  EncoderFamily family = EncoderFamily::Unknown;
  /** The profile's SimpleOutput/RecQuality ("" when the profile has
   *  no user value for it). */
  std::string recQuality;
  /**
   * Whether stepping the streaming preset changes the measured
   * streaming encode in this check (probe modes: the probe is built
   * from the config key; shared mode: the key feeds the shared
   * encoder). When false the check ran exactly one attempt at the
   * streaming settings and the dialog must not imply retries.
   */
  bool presetGoverned = false;
  /** Streaming preset in effect before the check ("" = default). */
  std::string originalPreset;
  /**
   * Streaming preset in effect when the check ended. On Stable this
   * is the preset that passed (kept deliberately — it is the fix,
   * and the results dialog says so). On Unstable it is the fastest
   * step tried, still in effect — the dialog discloses the change
   * from originalPreset and offers to restore it. On Marginal the
   * stepped preset was NOT kept: finalPreset == originalPreset
   * again — unless presetRestoreFailed says otherwise.
   */
  std::string finalPreset;
  /**
   * A Marginal result tried to restore the original preset and the
   * restore FAILED: the preset is still stepped (finalPreset), and
   * the dialog must offer the restore action instead of reassuring
   * the user that nothing changed.
   */
  bool presetRestoreFailed = false;
  /** Streaming-side attempt records (in Advanced mode: the
   *  recording-side records — that mode measures recording only). */
  std::vector<AttemptRecord> attempts;
  /** True when the profile uses OBS's Advanced output mode: the
   *  check measured the recording output only, with no probe, no
   *  stepping, and report-only cleanup. */
  bool advancedMode = false;
  /** The check ran streaming + recording at the same time. */
  bool combinedMode = false;
  /** Recordings reuse the streaming encoder (RecQuality "Stream"),
   *  so the frontend recording measured both sides at once and no
   *  separate recording verdict exists. */
  bool recordingSharesStreamingEncoder = false;
  /** The recording encoder's live id (combined/Advanced modes), for
   *  reporting; the recording side has no separate verdict — see the
   *  class comment on the shared pipeline counter. */
  std::string recordingEncoderId;
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
 * @brief Runs the stability test asynchronously.
 *
 * Main-thread QObject implementing architecture §7's required pattern:
 * outputs start/stop on the main thread, a 1-second QTimer polls
 * metrics, and results arrive through signals — OBS never blocks. All
 * durations are wall-clock (QElapsedTimer), not tick counts: under
 * exactly the overload this test detects, GUI timers slip, and a
 * "30-second" test must mean 30 real seconds.
 *
 * What runs depends on the profile and the user's pre-test choice:
 * - Combined (default, Simple mode, recording quality not "Stream"):
 *   OBS's own recording output (the profile's real recording
 *   encoder) AND a StreamingProbe — the plugin's own encoder built
 *   with the profile's streaming settings — record SIMULTANEOUSLY
 *   for 30 seconds, the load a stream-and-record session actually
 *   imposes, and the verdict is a verdict on the PAIR: OBS counts
 *   encoding-skipped frames once for the whole video pipeline, not
 *   per encoder, so no per-side split exists to report. The verdict
 *   is the final attempt's tier (decideOutcome): stepping the
 *   streaming preset is the one adjustment the test can make live,
 *   so that is what retries do (the probe is rebuilt with the
 *   stepped preset) — a pass after a step proves the combination
 *   works at the new preset, nothing more.
 * - Streaming-only (user's pre-test choice): the probe alone.
 * - "Stream" recording quality: the frontend recording output IS
 *   the streaming encoder — it is measured directly (governed, as
 *   before), and its one verdict covers recording too.
 * - Advanced mode: the frontend recording output alone, ungoverned,
 *   report-only cleanup; the Simple-mode probe construction does not
 *   apply there.
 *
 * Two preset values are kept apart on purpose: the CONFIG preset is
 * the stepping cursor and the restore target; the LIVE readback from
 * the running encoder is what gets reported as measured.
 *
 * Cancel semantics: a cancel in ANY state finishes the check as
 * Cancelled after cleanup — it never evaluates a verdict and never
 * starts another attempt. (Cleanup evidence rules are separate and
 * unchanged: a cancel during settling still needs the settle
 * tracker's full quiet evidence before anything is deleted.)
 *
 * Cleanup follows planCleanup() + SettleTracker (both pure, both
 * unit-tested): only files proven test-created are deleted (the
 * probe's file is test-created by construction — the plugin named
 * it), only after every output is inactive and file sizes stayed
 * quiet for the required polls, and never on a settle timeout —
 * leftover files are reported by path instead. Deletion results are
 * propagated into TestOutcome::cleanupNote; the monitor never claims
 * a deletion it did not observe succeed.
 */
class StabilityMonitor : public QObject {
  Q_OBJECT
public:
  explicit StabilityMonitor(QObject *parent = nullptr);
  ~StabilityMonitor() override;

  bool running() const { return m_state != State::Idle; }
  bool settling() const { return m_state == State::Settling; }
  bool combinedMode() const { return m_mode == Mode::Combined; }
  bool streamingOnlyMode() const { return m_mode == Mode::StreamingOnly; }
  bool advancedMode() const { return m_mode == Mode::AdvancedRecording; }
  bool sharedEncoderMode() const { return m_mode == Mode::SharedEncoder; }
  /**
   * Whole seconds left in the current step's measurement window, or
   * -1 when not in a measurement window (settling / idle): the
   * dialog shows a countdown for the former and a "finishing up"
   * state for the latter.
   */
  int stepSecondsRemaining() const;

  /** Starts a check. No-op when one is already running. A busy OBS
   *  produces an immediate finished() with Kind::Busy.
   *  @param streamingOnly Measure only the streaming encode (the
   *  user's pre-test choice); ignored in Advanced mode. */
  void start(bool streamingOnly = false);
  /**
   * Finishes the check with Kind::Cancelled once the in-flight
   * attempt's cleanup completes. During recording, the outputs stop
   * and cleanup runs through the full settle policy; during
   * settling, the wait is cut short but deletion still requires the
   * settle tracker's full quiet-poll evidence — otherwise the files
   * are left and reported. Either way the verdict is Cancelled: no
   * new attempt. No-op when idle.
   */
  void cancel();

signals:
  /** Once per second during an attempt. */
  void progress(int secondsElapsed, double fps, double cpuPercent);
  void attemptStarted(int attempt, int maxAttempts);
  void finished(const obs_setup::monitoring::TestOutcome &outcome);

private:
  enum class State { Idle, Recording, Settling };
  enum class Mode {
    Combined,         ///< Frontend recording + probe, simultaneously.
    StreamingOnly,    ///< Probe alone.
    SharedEncoder,    ///< "Stream" quality: frontend recording is the
                      ///< streaming encode.
    AdvancedRecording ///< Advanced mode: frontend recording alone.
  };

  void beginAttempt();
  void onTick();
  /** Takes the shared baseline for all frame counters (first poll
   *  where every expected output is active) and pins the test
   *  files' identities. */
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
  /** Stops every running output and enters Settling with a pending
   *  verdict path: evaluate the attempt (normal), or finish directly
   *  with the given kind/detail (cancel / error). */
  void enterSettling();
  void finish(TestOutcome::Kind kind, const std::string &detail = "");
  /** The exact cleanup candidates for this attempt, path -> size. */
  std::map<std::string, uint64_t> trackedFiles() const;
  std::string remuxSiblingPath() const;
  /** SimpleOutput/FilePath, or "" when the profile has none saved. */
  std::string recordingDirectory() const;
  bool frontendExpected() const { return m_mode != Mode::StreamingOnly; }
  bool probeExpected() const {
    return m_mode == Mode::Combined || m_mode == Mode::StreamingOnly;
  }

  State m_state = State::Idle;
  Mode m_mode = Mode::Combined;
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
  bool m_forceReportOnly = false;

  TestOutcome m_outcome;
  /** The CONFIG streaming preset: stepping cursor and restore
   *  target. Never overwritten by live readback. */
  std::string m_currentPreset;
  /** The running streaming encoder's own preset readback for this
   *  attempt ("" when the encoder reports none). Reporting only. */
  std::string m_measuredPreset;
  bool m_presetGoverned = false;
  int m_attempt = 0;
  int m_maxAttempts = 1;

  StreamingProbe m_probe;
  /** Full path of this attempt's probe recording ("" when the probe
   *  is not part of the check). Plugin-named, in the recording
   *  directory when one is known. */
  std::string m_probeFilePath;
  bool m_probeStarted = false;

  // Per-attempt timing (wall-clock; see class comment).
  QElapsedTimer m_attemptClock;
  QElapsedTimer m_windowClock;
  bool m_baselineTaken = false;
  uint32_t m_baseTotalFrames = 0;
  uint32_t m_baseLaggedFrames = 0;
  uint32_t m_baseSkippedFrames = 0;
  uint32_t m_baseProbeSkippedFrames = 0;
  double m_fpsSum = 0.0;
  int m_fpsSamples = 0;
  double m_cpuSum = 0.0;
  int m_cpuSamples = 0;
  double m_cpuPeak = 0.0;
  double m_targetFps = 0.0;
  Metrics m_streamingMetrics;

  // Per-attempt file tracking (frontend recording).
  /** Exact path of the test recording, from the output's settings. */
  std::string m_testFilePath;
  /** True when the recording path could not be read from the output:
   *  file identity is unproven, so cleanup deletes nothing. */
  bool m_fallbackMode = false;
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
