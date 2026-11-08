#ifndef OBS_SETUP_MONITORING_MONITORING_MONITOR_HPP
#define OBS_SETUP_MONITORING_MONITORING_MONITOR_HPP

#include "monitoring-module.hpp"

#include <QElapsedTimer>
#include <QObject>
#include <map>
#include <string>
#include <vector>

struct os_cpu_usage_info;
typedef struct os_cpu_usage_info os_cpu_usage_info_t;

class QTimer;

namespace obs_setup {
namespace monitoring {

/** One completed attempt of the stability test. */
struct AttemptRecord {
  int attempt = 0;    ///< 1-based.
  std::string preset; ///< Encoder preset this attempt ran with.
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
  Kind kind = Kind::Error;
  /** Busy output names or an error message, when kind needs one. */
  std::string detail;
  /**
   * The encoder the test actually measured: the live recording
   * output's encoder id, read from the running output (not inferred
   * from the streaming configuration — under non-"Stream" recording
   * qualities the recording encoder is a separate instance, and for
   * x264 its preset is not the streaming preset at all).
   */
  std::string encoderId;
  EncoderFamily family = EncoderFamily::Unknown;
  /** SimpleOutput/RecQuality in effect for the test ("Stream", "HQ", …). */
  std::string recQuality;
  /**
   * Whether stepping the Simple-mode preset key would change the
   * measured encoder (see recordingPresetGovernedByConfig). When
   * false the test ran exactly one attempt and the dialog must not
   * imply retries happened.
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
   * preset was NOT kept: finalPreset == originalPreset again.
   */
  std::string finalPreset;
  std::vector<AttemptRecord> attempts;
  /**
   * Truthful cleanup report built from what deletion actually did:
   * what was deleted, what was left in place and where. The dialog
   * shows this verbatim instead of asserting cleanup happened.
   */
  std::string cleanupNote;
};

/**
 * @brief Runs the 30-second stability test asynchronously.
 *
 * Main-thread QObject implementing architecture §7's required pattern:
 * recording starts/stops on the main thread, a 1-second QTimer polls
 * metrics, and results arrive through signals — OBS never blocks. All
 * durations are wall-clock (QElapsedTimer), not tick counts: under
 * exactly the overload this test detects, GUI timers slip, and a
 * "30-second" test must mean 30 real seconds.
 *
 * Measurement window: all three frame counters (rendered, lagged,
 * encoder-skipped) are baselined at the same moment — the first poll
 * where the recording is actually active — and read again when the
 * recording stops, so the verdict's numerators and denominator always
 * describe the same window.
 *
 * Retries step only the encoder preset (via
 * settings::SettingsManager::applyEncoderPreset, so validation and
 * fail-closed saves are the settings module's own); each attempt is a
 * fresh recording.
 *
 * Cleanup follows planCleanup() + SettleTracker (both pure, both
 * unit-tested): only files proven test-created are deleted, only
 * after the recording is inactive and file sizes stayed quiet for the
 * required polls, and never on a settle timeout — leftover files are
 * reported by path instead. Deletion results are propagated into
 * TestOutcome::cleanupNote; the monitor never claims a deletion it did
 * not observe succeed.
 */
class StabilityMonitor : public QObject {
  Q_OBJECT
public:
  explicit StabilityMonitor(QObject *parent = nullptr);
  ~StabilityMonitor() override;

  bool running() const { return m_state != State::Idle; }

  /** Starts a check. No-op when one is already running. A busy OBS
   *  produces an immediate finished() with Kind::Busy. */
  void start();
  /**
   * Stops the in-flight attempt and finishes with Kind::Cancelled.
   * During recording, the recording stops and cleanup still runs
   * through the settle policy. During settling, the wait is cut short
   * but deletion still requires the settle tracker's full quiet-poll
   * evidence — otherwise the files are left and reported. No-op when
   * idle.
   */
  void cancel();

signals:
  /** Once per second during an attempt. */
  void progress(int secondsElapsed, double fps, double cpuPercent);
  void attemptStarted(int attempt, int maxAttempts);
  void finished(const obs_setup::monitoring::TestOutcome &outcome);

private:
  enum class State { Idle, Recording, Settling };

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
   *  deletes nothing (settle timeout / unproven cancel). */
  void performCleanup(bool mayDelete);
  /** Stops the recording and enters Settling with a pending verdict
   *  path: evaluate the attempt (normal), or finish directly with the
   *  given kind/detail (cancel / error). */
  void enterSettling();
  void finish(TestOutcome::Kind kind, const std::string &detail = "");
  /** The exact cleanup candidates for this attempt, path -> size. */
  std::map<std::string, uint64_t> trackedFiles() const;
  std::string remuxSiblingPath() const;

  State m_state = State::Idle;
  QTimer *m_timer = nullptr;
  os_cpu_usage_info_t *m_cpuInfo = nullptr;
  bool m_cancelRequested = false;

  TestOutcome m_outcome;
  std::string m_currentPreset;
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

  // Encoder identity, finalized from the live recording output at
  // the first baseline (the config-derived values from start() are a
  // preliminary estimate for the progress label only).
  std::string m_recQuality = "Stream";
  bool m_presetGoverned = false;
  bool m_identityFinalized = false;

  // Per-attempt file tracking.
  /** Exact path of the test recording, from the output's settings. */
  std::string m_testFilePath;
  /** True when the recording path could not be read from the output:
   *  file identity is unproven, so cleanup deletes nothing. */
  bool m_fallbackMode = false;
  bool m_recordingExistedAtStart = false;
  bool m_siblingExistedAtStart = false;
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
