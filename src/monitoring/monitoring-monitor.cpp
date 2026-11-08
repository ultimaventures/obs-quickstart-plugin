#include "monitoring-monitor.hpp"

#include "../profile/profile-module.hpp"
#include "../settings/settings-module.hpp"

#include <QTimer>

#include <media-io/video-io.h>
#include <obs-frontend-api.h>
#include <obs.h>
#include <util/config-file.h>
#include <util/platform.h>

#include <filesystem>

namespace obs_setup {
namespace monitoring {
namespace fs = std::filesystem;
namespace {

uint32_t recordingSkippedFrames() {
  obs_output_t *output = obs_frontend_get_recording_output();
  if (!output)
    return 0;
  obs_encoder_t *encoder = obs_output_get_video_encoder(output);
  if (!encoder)
    return 0;
  video_t *video = obs_encoder_video(encoder);
  if (!video)
    return 0;
  return video_output_get_skipped_frames(video);
}

std::string joinNames(const std::vector<std::string> &names) {
  std::string out;
  for (size_t i = 0; i < names.size(); ++i) {
    if (i)
      out += ", ";
    out += names[i];
  }
  return out;
}

/** File size, or 0 when the file is absent/unreadable. All path
 *  construction from OBS strings goes through fs::u8path — OBS hands
 *  out UTF-8, and path(std::string) would decode via the Windows ANSI
 *  code page (docs/conventions.md §12). */
uint64_t fileSizeOrZero(const std::string &utf8Path) {
  std::error_code ec;
  const auto size = fs::file_size(fs::u8path(utf8Path), ec);
  return ec ? 0 : static_cast<uint64_t>(size);
}

bool fileExists(const std::string &utf8Path) {
  std::error_code ec;
  return fs::is_regular_file(fs::u8path(utf8Path), ec);
}

} // namespace

StabilityMonitor::StabilityMonitor(QObject *parent)
    : QObject(parent), m_timer(new QTimer(this)) {
  m_timer->setInterval(1000);
  connect(m_timer, &QTimer::timeout, this, &StabilityMonitor::onTick);
}

StabilityMonitor::~StabilityMonitor() {
  m_timer->stop();
  if (m_state == State::Recording && obs_frontend_recording_active())
    obs_frontend_recording_stop();
  if (!m_testFilePath.empty() && m_state != State::Idle) {
    // No event loop remains to settle safely; never guess-delete from
    // a destructor. Name the file so it can be found, and leave it.
    blog(LOG_WARNING,
         "[Monitoring] Monitor destroyed mid-test; test recording left "
         "at %s",
         m_testFilePath.c_str());
  }
  if (m_cpuInfo) {
    os_cpu_usage_info_destroy(m_cpuInfo);
    m_cpuInfo = nullptr;
  }
}

std::string StabilityMonitor::remuxSiblingPath() const {
  if (m_testFilePath.empty())
    return "";
  return fs::u8path(m_testFilePath).replace_extension(".mp4").u8string();
}

std::map<std::string, uint64_t> StabilityMonitor::trackedFiles() const {
  std::map<std::string, uint64_t> files;
  if (m_testFilePath.empty())
    return files;
  if (fileExists(m_testFilePath))
    files[m_testFilePath] = fileSizeOrZero(m_testFilePath);
  const std::string sibling = remuxSiblingPath();
  if (!sibling.empty() && sibling != m_testFilePath && fileExists(sibling))
    files[sibling] = fileSizeOrZero(sibling);
  return files;
}

void StabilityMonitor::start() {
  if (running())
    return;
  m_cancelRequested = false;
  m_outcome = TestOutcome{};
  m_cleanupNotes.clear();

  profile::ProfileManager profiles;
  if (profiles.isBusy()) {
    m_outcome.kind = TestOutcome::Kind::Busy;
    m_outcome.detail = joinNames(profiles.activeOutputs());
    emit finished(m_outcome);
    return;
  }

  config_t *config = obs_frontend_get_profile_config();
  if (!config) {
    finish(TestOutcome::Kind::Error, "No active profile configuration.");
    return;
  }
  // Recording quality decides which encoder the test will actually
  // measure (see recordingPresetGovernedByConfig): only "Stream"
  // shares the streaming encoder instance. A missing value behaves
  // as "Stream" (the frontend's own default).
  const char *recQuality =
      config_get_string(config, "SimpleOutput", "RecQuality");
  m_recQuality = recQuality && *recQuality ? recQuality : "Stream";
  m_outcome.recQuality = m_recQuality;
  m_identityFinalized = false;
  m_presetGoverned = false;

  // Preliminary identity from the streaming configuration — used for
  // the progress label until takeBaseline() replaces it with the live
  // recording encoder's identity. Absent/unrecognized streaming
  // configuration is NOT an error anymore: the recording output may
  // still run (e.g. a separate recording encoder under "HQ"), and
  // the baseline reads the truth from it.
  const char *simpleEncoder =
      config_get_string(config, "SimpleOutput", "StreamEncoder");
  m_outcome.encoderId =
      simpleEncoder ? settings::libobsEncoderIdForSimpleEncoder(simpleEncoder)
                    : "";
  m_outcome.family = familyForEncoderId(m_outcome.encoderId);
  const std::string key = settings::presetKeyForEncoder(m_outcome.encoderId);
  const char *preset =
      key.empty() ? nullptr
                  : config_get_string(config, "SimpleOutput", key.c_str());
  m_currentPreset = preset && *preset
                        ? preset
                        : settings::defaultPresetFor(m_outcome.encoderId);
  m_outcome.originalPreset = m_currentPreset;
  m_presetGoverned =
      recordingPresetGovernedByConfig(m_outcome.family, m_recQuality);
  m_outcome.presetGoverned = m_presetGoverned;
  // Honest attempt budget: remaining ladder rungs from THIS preset —
  // and exactly one attempt when stepping could not change the
  // encoder being measured.
  m_maxAttempts = m_presetGoverned
                      ? attemptsAvailable(m_outcome.family, m_currentPreset)
                      : 1;
  m_attempt = 0;

  // Note: a fresh profile may have no SimpleOutput/FilePath saved at
  // all — OBS records to a default location from in-memory settings.
  // That is not an error: the test file's exact path is read back from
  // the recording output's own settings once recording starts.

  obs_video_info ovi{};
  if (obs_get_video_info(&ovi) && ovi.fps_den != 0)
    m_targetFps = static_cast<double>(ovi.fps_num) / ovi.fps_den;

  m_cpuInfo = os_cpu_usage_info_start();
  beginAttempt();
}

void StabilityMonitor::beginAttempt() {
  ++m_attempt;
  m_baselineTaken = false;
  m_fpsSum = 0.0;
  m_fpsSamples = 0;
  m_cpuSum = 0.0;
  m_cpuSamples = 0;
  m_cpuPeak = 0.0;
  m_attemptMetrics = Metrics{};
  m_testFilePath.clear();
  m_fallbackMode = false;
  m_recordingExistedAtStart = false;
  m_siblingExistedAtStart = false;
  m_lastTrackedFiles.clear();
  m_settleTracker = SettleTracker{};
  m_pending = Pending::Evaluate;
  m_attemptClock.restart();

  blog(LOG_INFO,
       "[Monitoring] Stability attempt %d/%d: encoder %s at preset "
       "'%s'",
       m_attempt, m_maxAttempts, m_outcome.encoderId.c_str(),
       m_currentPreset.c_str());
  emit attemptStarted(m_attempt, m_maxAttempts);
  obs_frontend_recording_start();
  m_state = State::Recording;
  m_timer->start();
}

void StabilityMonitor::takeBaseline() {
  // One shared baseline moment for all three verdict counters: the
  // first poll where the recording output actually exists and is
  // active. Baselining the skipped counter ~1s after the others (the
  // old shape) silently excluded the startup second — plausibly the
  // worst second for encoder skips — from exactly one numerator.
  m_baseTotalFrames = obs_get_total_frames();
  m_baseLaggedFrames = obs_get_lagged_frames();
  m_baseSkippedFrames = recordingSkippedFrames();
  m_baselineTaken = true;
  m_windowClock.restart();

  obs_output_t *output = obs_frontend_get_recording_output();
  if (output) {
    obs_data_t *settings = obs_output_get_settings(output);
    if (settings) {
      const char *path = obs_data_get_string(settings, "path");
      if (path && *path)
        m_testFilePath = path;
      obs_data_release(settings);
    }
    // Finalize WHO is being measured from the live output: the
    // recording encoder instance that is actually running, its id
    // and its live preset. Under "HQ"-family qualities this encoder
    // is a separate instance from the streaming encoder, and for
    // x264 its preset is hardcoded by OBS — the config-derived
    // identity from start() is only an estimate until this runs.
    obs_encoder_t *encoder = obs_output_get_video_encoder(output);
    if (encoder) {
      const char *liveId = obs_encoder_get_id(encoder);
      obs_data_t *encSettings = obs_encoder_get_settings(encoder);
      std::string livePreset;
      if (encSettings) {
        const char *p = obs_data_get_string(encSettings, "preset");
        if (p && *p)
          livePreset = p;
        obs_data_release(encSettings);
      }
      if (liveId && *liveId) {
        m_outcome.encoderId = liveId;
        m_outcome.family = familyForEncoderId(liveId);
        if (!livePreset.empty() && !m_identityFinalized) {
          m_currentPreset = livePreset;
          m_outcome.originalPreset = livePreset;
        }
        m_presetGoverned =
            recordingPresetGovernedByConfig(m_outcome.family, m_recQuality);
        m_outcome.presetGoverned = m_presetGoverned;
        const int honest =
            m_presetGoverned
                ? attemptsAvailable(m_outcome.family, m_currentPreset)
                : 1;
        if (honest != m_maxAttempts) {
          m_maxAttempts = honest;
          emit attemptStarted(m_attempt, m_maxAttempts);
        }
        m_identityFinalized = true;
        blog(LOG_INFO,
             "[Monitoring] Measuring recording encoder %s at preset "
             "'%s' (RecQuality %s, preset stepping %s)",
             m_outcome.encoderId.c_str(), m_currentPreset.c_str(),
             m_recQuality.c_str(),
             m_presetGoverned ? "available" : "not applicable");
      }
    }
  }
  if (m_testFilePath.empty()) {
    // File identity unproven: cleanup will report, never delete.
    m_fallbackMode = true;
    blog(LOG_WARNING, "[Monitoring] Could not read the recording path "
                      "from the output; cleanup will be report-only");
    return;
  }
  // Ownership evidence, taken while the test recording is running:
  // the remux sibling cannot have been produced by THIS recording yet
  // (remux runs after stop), so a sibling present now predates the
  // test and is the user's file. The recording itself is always a
  // fresh file created for this recording: the frontend generates
  // collision-safe filenames (OBSBasic::GenerateSpecifiedFilename
  // appends " (n)" when a name is taken, and file overwrite is off by
  // default), so the exact path the output reports did not hold a
  // user file a moment ago.
  m_siblingExistedAtStart = fileExists(remuxSiblingPath());
  m_recordingExistedAtStart = false;
  m_lastTrackedFiles = trackedFiles();
}

void StabilityMonitor::cancel() {
  if (!running())
    return;
  if (m_state == State::Settling) {
    // A cancel during settling is the impatient one: it ends the
    // wait via the tracker, without weakening deletion evidence.
    m_settleCancelRequested = true;
    return;
  }
  if (m_cancelRequested)
    return;
  m_cancelRequested = true;
  if (m_state == State::Recording) {
    sampleFinalMetrics();
    m_pending = Pending::FinishInstead;
    m_pendingKind = TestOutcome::Kind::Cancelled;
    m_pendingDetail.clear();
    enterSettling();
  }
}

void StabilityMonitor::enterSettling() {
  if (obs_frontend_recording_active())
    obs_frontend_recording_stop();
  m_state = State::Settling;
  m_settleTracker = SettleTracker{};
  m_settleCancelRequested = false;
  m_lastTrackedFiles = trackedFiles();
}

void StabilityMonitor::onTick() {
  const double attemptElapsed = m_attemptClock.elapsed() / 1000.0;

  if (m_state == State::Recording) {
    if (!m_baselineTaken && obs_frontend_recording_active())
      takeBaseline();

    if (m_baselineTaken && attemptElapsed >= 2.0 &&
        !obs_frontend_recording_active()) {
      // Started, then died on its own: an error — but the partial
      // file still goes through the settle policy before finishing.
      m_pending = Pending::FinishInstead;
      m_pendingKind = TestOutcome::Kind::Error;
      m_pendingDetail = "The test recording stopped early.";
      enterSettling();
      return;
    }
    if (!m_baselineTaken && attemptElapsed >= 5.0) {
      m_pending = Pending::FinishInstead;
      m_pendingKind = TestOutcome::Kind::Error;
      m_pendingDetail = "The test recording did not start.";
      enterSettling();
      return;
    }

    const double fps = obs_get_active_fps();
    const double cpu = m_cpuInfo ? os_cpu_usage_info_query(m_cpuInfo) : 0.0;
    if (m_baselineTaken && m_windowClock.elapsed() >= 1000) {
      // Skip the first window second (warm-up), as before — but now
      // the verdict counters share that exclusion via the baseline.
      m_fpsSum += fps;
      ++m_fpsSamples;
      m_cpuSum += cpu;
      ++m_cpuSamples;
      if (cpu > m_cpuPeak)
        m_cpuPeak = cpu;
    }
    emit progress(static_cast<int>(attemptElapsed), fps, cpu);

    if (m_baselineTaken &&
        m_windowClock.elapsed() / 1000.0 >= TEST_DURATION_SECONDS) {
      sampleFinalMetrics();
      enterSettling();
      return;
    }
    if (attemptElapsed >= ATTEMPT_TIMEOUT_SECONDS) {
      sampleFinalMetrics();
      m_pending = Pending::FinishInstead;
      m_pendingKind = TestOutcome::Kind::Error;
      m_pendingDetail = "The test recording ran past its time budget.";
      enterSettling();
    }
    return;
  }

  if (m_state == State::Settling) {
    if (pollSettling())
      return;
    if (attemptElapsed >= ATTEMPT_TIMEOUT_SECONDS + SETTLE_MAX_POLLS) {
      // Hard bound beyond the tracker's own budget: finish, leaving
      // whatever files remain, and report them.
      performCleanup(false);
      completeAttempt();
    }
    return;
  }
}

bool StabilityMonitor::pollSettling() {
  const auto now = trackedFiles();
  const bool changed = now != m_lastTrackedFiles;
  m_lastTrackedFiles = now;
  SettleObservation observation;
  observation.recordingActive = obs_frontend_recording_active();
  observation.filesChanged = changed;
  observation.cancelRequested = m_settleCancelRequested;
  const SettleAction action = m_settleTracker.observe(observation);
  if (action == SettleAction::Wait)
    return false;
  performCleanup(action == SettleAction::DeleteNow);
  completeAttempt();
  return true;
}

void StabilityMonitor::performCleanup(bool mayDelete) {
  if (m_fallbackMode || m_testFilePath.empty()) {
    m_cleanupNotes.push_back(
        "The test recording could not be identified for cleanup, so "
        "nothing was deleted automatically. Check your recording "
        "folder for a short test recording from just now.");
    return;
  }
  const CleanupPlan plan =
      planCleanup(m_fallbackMode, !m_testFilePath.empty(),
                  m_recordingExistedAtStart, m_siblingExistedAtStart);
  const std::string sibling = remuxSiblingPath();
  auto handle = [&](const std::string &path, bool planAllows) {
    if (path.empty() || !fileExists(path))
      return;
    if (!mayDelete) {
      // Settle policy said leave everything (timeout, or a cancel
      // without full quiet evidence): report, touch nothing.
      m_cleanupNotes.push_back("Left in place: " + path);
      return;
    }
    if (!planAllows) {
      m_cleanupNotes.push_back("Left in place because it was already "
                               "there before the test: " +
                               path);
      return;
    }
    std::error_code ec;
    if (fs::remove(fs::u8path(path), ec))
      blog(LOG_INFO, "[Monitoring] Deleted test recording %s", path.c_str());
    else
      m_cleanupNotes.push_back("Could not delete the test recording; "
                               "it is still at: " +
                               path);
  };
  handle(m_testFilePath, plan.deleteRecording);
  if (!sibling.empty() && sibling != m_testFilePath)
    handle(sibling, plan.deleteRemuxSibling);
  if (mayDelete && plan.deleteRecording && !fileExists(m_testFilePath))
    m_cleanupNotes.push_back("The test recording was deleted.");
}

void StabilityMonitor::sampleFinalMetrics() {
  Metrics &m = m_attemptMetrics;
  if (!m_baselineTaken) {
    m = Metrics{};
    return;
  }
  m.durationSeconds = m_windowClock.elapsed() / 1000.0;
  m.totalFrames =
      static_cast<uint32_t>(obs_get_total_frames() - m_baseTotalFrames);
  m.laggedFrames =
      static_cast<uint32_t>(obs_get_lagged_frames() - m_baseLaggedFrames);
  m.skippedFrames =
      static_cast<uint32_t>(recordingSkippedFrames() - m_baseSkippedFrames);
  m.averageFps = m_fpsSamples > 0 ? m_fpsSum / m_fpsSamples : 0.0;
  m.targetFps = m_targetFps;
  m.averageCpuPercent = m_cpuSamples > 0 ? m_cpuSum / m_cpuSamples : 0.0;
  m.peakCpuPercent = m_cpuPeak;
}

void StabilityMonitor::completeAttempt() {
  m_timer->stop();

  if (m_pending == Pending::FinishInstead) {
    finish(m_pendingKind, m_pendingDetail);
    return;
  }
  if (m_cancelRequested) {
    finish(TestOutcome::Kind::Cancelled);
    return;
  }

  AttemptRecord record;
  record.attempt = m_attempt;
  record.preset = m_currentPreset;
  record.metrics = m_attemptMetrics;
  record.stability = evaluateStability(m_attemptMetrics);
  m_outcome.attempts.push_back(record);

  blog(LOG_INFO,
       "[Monitoring] Attempt %d: %llu frames, lagged %.2f%%, skipped %.2f%%, "
       "fps %.1f/%.1f, cpu avg %.0f%% -> %s",
       m_attempt, static_cast<unsigned long long>(m_attemptMetrics.totalFrames),
       record.stability.laggedPercent, record.stability.skippedPercent,
       m_attemptMetrics.averageFps, m_attemptMetrics.targetFps,
       m_attemptMetrics.averageCpuPercent,
       record.stability.tier == Tier::Stable
           ? "STABLE"
           : (record.stability.tier == Tier::Marginal ? "MARGINAL"
                                                      : "UNSTABLE"));

  if (record.stability.tier == Tier::Stable) {
    finish(TestOutcome::Kind::Stable);
    return;
  }
  if (record.stability.tier == Tier::Marginal) {
    // Marginal evidence does not justify keeping a stepped preset:
    // restore the original and let the user retest, rather than
    // silently locking in a quality reduction on borderline data.
    if (m_currentPreset != m_outcome.originalPreset) {
      settings::SettingsManager settingsManager;
      if (settingsManager.applyEncoderPreset(m_outcome.originalPreset)) {
        blog(LOG_INFO,
             "[Monitoring] Marginal result; restored original "
             "preset '%s'",
             m_outcome.originalPreset.c_str());
        m_currentPreset = m_outcome.originalPreset;
      } else {
        blog(LOG_WARNING,
             "[Monitoring] Marginal result; could NOT restore "
             "original preset '%s'",
             m_outcome.originalPreset.c_str());
      }
    }
    finish(TestOutcome::Kind::Marginal);
    return;
  }
  if (m_presetGoverned && m_attempt < m_maxAttempts) {
    // The governed check is the M4 guard: never step a config key
    // that does not control the encoder being measured.
    const auto step = nextLadderStep(m_outcome.family, m_currentPreset);
    if (step.has_value()) {
      settings::SettingsManager settingsManager;
      if (settingsManager.applyEncoderPreset(*step)) {
        blog(LOG_INFO,
             "[Monitoring] Stepping encoder preset '%s' -> '%s' "
             "and retrying",
             m_currentPreset.c_str(), step->c_str());
        m_currentPreset = *step;
        beginAttempt();
        return;
      }
      finish(TestOutcome::Kind::Error,
             "Could not apply the stepped encoder preset '" + *step + "'.");
      return;
    }
  }
  finish(TestOutcome::Kind::Unstable);
}

void StabilityMonitor::finish(TestOutcome::Kind kind,
                              const std::string &detail) {
  m_timer->stop();
  m_state = State::Idle;
  if (m_cpuInfo) {
    os_cpu_usage_info_destroy(m_cpuInfo);
    m_cpuInfo = nullptr;
  }
  m_outcome.kind = kind;
  if (!detail.empty())
    m_outcome.detail = detail;
  m_outcome.finalPreset = m_currentPreset;
  std::string note;
  for (size_t i = 0; i < m_cleanupNotes.size(); ++i) {
    if (i)
      note += " ";
    note += m_cleanupNotes[i];
  }
  m_outcome.cleanupNote = note;
  emit finished(m_outcome);
}

} // namespace monitoring
} // namespace obs_setup
