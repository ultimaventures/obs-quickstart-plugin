#ifndef OBS_SETUP_MONITORING_MONITORING_MONITOR_HPP
#define OBS_SETUP_MONITORING_MONITORING_MONITOR_HPP

#include "monitoring-module.hpp"

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
    Unstable,  ///< All attempts failed; offers (wizard / fallback) apply.
    Cancelled, ///< The user cancelled; any test file was cleaned up.
    Busy,      ///< Refused to start: outputs were already active.
    Error      ///< The test itself failed (see detail).
  };
  Kind kind = Kind::Error;
  /** Busy output names or an error message, when kind needs one. */
  std::string detail;
  std::string encoderId;
  EncoderFamily family = EncoderFamily::Unknown;
  /** Preset in effect before the check ("" = encoder default). */
  std::string originalPreset;
  /**
   * Preset in effect when the check ended. On Stable this is the
   * preset that passed (kept deliberately — it is the fix). On
   * Unstable it is the last, fastest step tried; the results dialog
   * says so rather than silently restoring the failing preset.
   */
  std::string finalPreset;
  std::vector<AttemptRecord> attempts;
};

/**
 * @brief Runs the 30-second stability test asynchronously.
 *
 * Main-thread QObject implementing architecture §7's required pattern:
 * recording starts/stops on the main thread, a 1-second QTimer polls
 * metrics, and results arrive through signals — OBS never blocks.
 * Retries step only the encoder preset (via
 * settings::SettingsManager::applyEncoderPreset, so validation and
 * fail-closed saves are the settings module's own); each attempt is a
 * fresh recording. Cleanup deletes only the exact files the test
 * created in the recording directory, after the recording stopped and
 * any auto-remux settled (file sizes stable across a poll).
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
  /** Stops the in-flight attempt, cleans up its file, and finishes
   *  with Kind::Cancelled. No-op when idle. */
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
  void sampleFinalMetrics();
  void finishAttempt();
  void deleteTestFiles();
  void finish(TestOutcome::Kind kind, const std::string &detail = "");
  std::string recordingDirectory() const;
  /** full path -> size map of the recording directory. */
  std::map<std::string, uint64_t> snapshotDirectory() const;
  /**
   * The files the current attempt owns: the exact recording path read
   * from the recording output's own settings (plus its auto-remux
   * sibling when one appears), or — when that path could not be read —
   * the new files in the recording directory since the attempt began.
   */
  std::map<std::string, uint64_t> currentTestFiles() const;

  State m_state = State::Idle;
  QTimer *m_timer = nullptr;
  os_cpu_usage_info_t *m_cpuInfo = nullptr;
  bool m_cancelled = false;

  TestOutcome m_outcome;
  std::string m_currentPreset;
  int m_attempt = 0;
  int m_maxAttempts = 1;

  // Per-attempt sampling state.
  int m_elapsedSeconds = 0;
  int m_settleTicks = 0;
  uint32_t m_baseTotalFrames = 0;
  uint32_t m_baseLaggedFrames = 0;
  uint32_t m_baseSkippedFrames = 0;
  bool m_skippedBaselineTaken = false;
  double m_fpsSum = 0.0;
  int m_fpsSamples = 0;
  double m_cpuSum = 0.0;
  int m_cpuSamples = 0;
  double m_cpuPeak = 0.0;
  double m_targetFps = 0.0;
  Metrics m_attemptMetrics;

  std::map<std::string, uint64_t> m_dirSnapshot;
  std::map<std::string, uint64_t> m_lastNewFiles;
  /** Exact path of the test recording, from the output's settings. */
  std::string m_testFilePath;
};

} // namespace monitoring
} // namespace obs_setup

#endif // OBS_SETUP_MONITORING_MONITORING_MONITOR_HPP
