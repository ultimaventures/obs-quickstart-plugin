#include "monitoring-monitor.hpp"

#include "../profile/profile-module.hpp"
#include "../settings/settings-module.hpp"

#include <QDateTime>
#include <QTimer>

#include <media-io/video-io.h>
#include <obs-frontend-api.h>
#include <obs.h>
#include <util/config-file.h>
#include <util/platform.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>

namespace obs_setup {
namespace monitoring {
namespace fs = std::filesystem;
namespace {

uint32_t frontendSkippedFrames() {
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

bool equalsIgnoreCase(const std::string &a, const std::string &b) {
  if (a.size() != b.size())
    return false;
  for (size_t i = 0; i < a.size(); ++i)
    if (std::tolower(static_cast<unsigned char>(a[i])) !=
        std::tolower(static_cast<unsigned char>(b[i])))
      return false;
  return true;
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
  m_probe.stop();
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

std::string StabilityMonitor::recordingDirectory() const {
  config_t *config = obs_frontend_get_profile_config();
  if (!config)
    return "";
  const char *path = config_get_string(config, "SimpleOutput", "FilePath");
  return path ? path : "";
}

std::string StabilityMonitor::remuxSiblingPath() const {
  if (m_testFilePath.empty())
    return "";
  return fs::u8path(m_testFilePath).replace_extension(".mp4").u8string();
}

std::map<std::string, uint64_t> StabilityMonitor::trackedFiles() const {
  std::map<std::string, uint64_t> files;
  if (!m_testFilePath.empty() && fileExists(m_testFilePath))
    files[m_testFilePath] = fileSizeOrZero(m_testFilePath);
  const std::string sibling = remuxSiblingPath();
  if (!sibling.empty() && sibling != m_testFilePath && fileExists(sibling))
    files[sibling] = fileSizeOrZero(sibling);
  if (!m_probeFilePath.empty() && fileExists(m_probeFilePath))
    files[m_probeFilePath] = fileSizeOrZero(m_probeFilePath);
  return files;
}

int StabilityMonitor::stepSecondsRemaining() const {
  if (m_state != State::Recording)
    return -1;
  if (!m_baselineTaken)
    return TEST_DURATION_SECONDS;
  const double left = TEST_DURATION_SECONDS - m_windowClock.elapsed() / 1000.0;
  if (left <= 0.0)
    return 0;
  return static_cast<int>(std::ceil(left));
}

void StabilityMonitor::start(bool streamingOnly) {
  if (running())
    return;
  m_cancelRequested = false;
  m_settleCancelRequested = false;
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

  // Output mode, read through the live profile config (the frontend
  // registers "Simple" as the default, so a missing value reads as
  // Simple — the same effective-mode rule OBS itself applies).
  const char *mode = config_get_string(config, "Output", "Mode");
  m_advancedMode = mode && equalsIgnoreCase(mode, "Advanced");
  m_outcome.advancedMode = m_advancedMode;
  m_forceReportOnly = m_advancedMode;

  // The profile's recording quality: decides the check's shape, and
  // is NEVER assumed when absent — a profile with no RecQuality
  // value is measured as-is (the "unproven means conservative" rule
  // the rest of this module follows).
  const bool recQualityKnown =
      config_has_user_value(config, "SimpleOutput", "RecQuality");
  const char *recQuality =
      config_get_string(config, "SimpleOutput", "RecQuality");
  m_profileRecQuality =
      recQualityKnown && recQuality && *recQuality ? recQuality : "";
  m_outcome.recQuality = m_profileRecQuality;

  // Mode selection.
  if (m_advancedMode) {
    m_mode = Mode::AdvancedRecording;
  } else if (streamingOnly) {
    m_mode = Mode::StreamingOnly;
  } else if (m_profileRecQuality == "Stream") {
    // The frontend recording output IS the streaming encoder.
    m_mode = Mode::SharedEncoder;
    m_outcome.recordingSharesStreamingEncoder = true;
  } else {
    m_mode = Mode::Combined;
    m_outcome.combinedMode = true;
  }

  // Preliminary identity + stepping cursor.
  m_presetGoverned = false;
  if (m_mode == Mode::AdvancedRecording) {
    const char *recEncoder = config_get_string(config, "AdvOut", "RecEncoder");
    m_currentPreset.clear();
    m_outcome.encoderId = recEncoder ? recEncoder : "";
    m_outcome.family = familyForEncoderId(m_outcome.encoderId);
    m_outcome.originalPreset.clear();
    m_outcome.presetGoverned = false;
  } else {
    const char *simpleEncoder =
        config_get_string(config, "SimpleOutput", "StreamEncoder");
    const std::string encoderId =
        simpleEncoder ? settings::libobsEncoderIdForSimpleEncoder(simpleEncoder)
                      : "";
    const std::string key = settings::presetKeyForEncoder(encoderId);
    const char *preset =
        !key.empty() ? config_get_string(config, "SimpleOutput", key.c_str())
                     : nullptr;
    m_currentPreset =
        preset && *preset ? preset : settings::defaultPresetFor(encoderId);
    m_outcome.encoderId = encoderId;
    m_outcome.family = familyForEncoderId(encoderId);
    m_outcome.originalPreset = m_currentPreset;
    // Governed: the probe is built from this very key (probe modes),
    // or the key feeds the shared encoder directly (shared mode).
    m_presetGoverned =
        recordingPresetGovernedByConfig(m_outcome.family, "Stream");
    m_outcome.presetGoverned = m_presetGoverned;
  }
  m_maxAttempts = m_presetGoverned
                      ? attemptsAvailable(m_outcome.family, m_currentPreset)
                      : 1;
  m_attempt = 0;

  obs_video_info ovi{};
  if (obs_get_video_info(&ovi) && ovi.fps_den != 0) {
    m_targetFps = static_cast<double>(ovi.fps_num) / ovi.fps_den;
  } else {
    m_targetFps = 0.0;
    m_outcome.frameRateGateInoperative = true;
  }

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
  m_streamingMetrics = Metrics{};
  m_measuredPreset.clear();
  m_testFilePath.clear();
  m_probeFilePath.clear();
  m_probeStarted = false;
  m_fallbackMode = false;
  m_recordingExistedAtStart = false;
  m_siblingExistedAtStart = false;
  m_siblingExpected = false;
  m_lastTrackedFiles.clear();
  m_settleTracker = SettleTracker{};
  m_pending = Pending::Evaluate;
  config_t *config = obs_frontend_get_profile_config();
  m_autoRemux = config && config_get_bool(config, "Video", "AutoRemux");
  // Ownership evidence for the recording file: which entry names
  // already exist in the recording directory BEFORE this recording
  // starts (names only — see the header note). If the directory is
  // unknown (profile has no FilePath saved), or the listing fails
  // partway, no snapshot is taken and ownership stays unprovable
  // for this attempt.
  m_dirNamesAtStart.clear();
  m_snapshotDir.clear();
  m_dirSnapshotTaken = false;
  const std::string dir = recordingDirectory();
  if (!dir.empty()) {
    m_snapshotDir = dir;
    std::error_code ec;
    fs::directory_iterator it(fs::u8path(dir), ec);
    const fs::directory_iterator end;
    for (; !ec && it != end; it.increment(ec)) {
      std::error_code ec2;
      if (it->is_regular_file(ec2))
        m_dirNamesAtStart.insert(it->path().filename().u8string());
    }
    m_dirSnapshotTaken = !ec;
  }
  m_attemptClock.restart();

  blog(LOG_INFO,
       "[Monitoring] Stability attempt %d/%d (mode %d): streaming "
       "encoder %s at preset '%s'",
       m_attempt, m_maxAttempts, static_cast<int>(m_mode),
       m_outcome.encoderId.c_str(), m_currentPreset.c_str());
  emit attemptStarted(m_attempt, m_maxAttempts);

  // Start the probe first: its start is synchronous, so a failure is
  // known before the frontend recording begins, and the attempt can
  // error without a stray recording to clean up beyond the probe's
  // own file.
  if (probeExpected()) {
    std::string probeDir = dir;
    if (probeDir.empty()) {
      std::error_code ec;
      probeDir = fs::temp_directory_path(ec).u8string();
    }
    const QString stamp =
        QDateTime::currentDateTime().toString("yyyyMMdd-hhmmss");
    m_probeFilePath = (fs::u8path(probeDir) /
                       fs::u8path("quickstart-probe-" + stamp.toStdString() +
                                  "-a" + std::to_string(m_attempt) + ".mkv"))
                          .u8string();
    std::string error;
    if (!m_probe.start(config, m_currentPreset, m_probeFilePath, error)) {
      blog(LOG_WARNING, "[Monitoring] Streaming probe failed: %s",
           error.c_str());
      m_pending = Pending::FinishInstead;
      m_pendingKind = TestOutcome::Kind::Error;
      m_pendingDetail = error;
      m_state = State::Recording; // enterSettling expects a live attempt
      m_timer->start();
      enterSettling();
      return;
    }
    m_probeStarted = true;
  }
  if (frontendExpected())
    obs_frontend_recording_start();
  m_state = State::Recording;
  m_timer->start();
}

void StabilityMonitor::takeBaseline() {
  // One shared baseline moment for all verdict counters: the first
  // poll where every expected output is actually active. Baselining
  // a counter ~1s after the others silently excludes the startup
  // second — plausibly the worst second for encoder skips — from
  // exactly one numerator.
  m_baseTotalFrames = obs_get_total_frames();
  m_baseLaggedFrames = obs_get_lagged_frames();
  m_baseSkippedFrames = frontendExpected() ? frontendSkippedFrames() : 0;
  m_baseProbeSkippedFrames = probeExpected() ? m_probe.skippedFrames() : 0;
  m_baselineTaken = true;
  m_windowClock.restart();

  if (probeExpected()) {
    // The probe was created from the config, so its family cannot
    // contradict the stepping assumptions — but its live preset can
    // (an encoder may normalize what it was given). Verify it the
    // same way the frontend path does.
    m_measuredPreset = m_probe.livePreset();
    if (m_presetGoverned && !m_measuredPreset.empty() &&
        m_measuredPreset != m_currentPreset) {
      blog(LOG_WARNING,
           "[Monitoring] Probe encoder runs at preset '%s', not the "
           "configured '%s'; disabling preset stepping",
           m_measuredPreset.c_str(), m_currentPreset.c_str());
      m_presetGoverned = false;
      m_outcome.presetGoverned = false;
      m_maxAttempts = m_attempt;
    }
    blog(LOG_INFO,
         "[Monitoring] Measuring streaming encoder %s at preset '%s' "
         "(probe, stepping %s)",
         m_probe.encoderId().c_str(),
         !m_measuredPreset.empty()
             ? m_measuredPreset.c_str()
             : (m_presetGoverned ? m_currentPreset.c_str() : "not reported"),
         m_presetGoverned ? "available" : "not applicable");
  }

  if (!frontendExpected())
    return;
  obs_output_t *output = obs_frontend_get_recording_output();
  if (output) {
    obs_data_t *settings = obs_output_get_settings(output);
    if (settings) {
      const char *path = obs_data_get_string(settings, "path");
      if (path && *path)
        m_testFilePath = path;
      obs_data_release(settings);
    }
    // Finalize the frontend encoder's identity from the live output.
    // In combined mode this is the RECORDING side; in shared and
    // Advanced modes it is the headline measurement itself.
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
        if (m_mode == Mode::Combined) {
          m_outcome.recordingEncoderId = liveId;
          blog(LOG_INFO, "[Monitoring] Measuring recording encoder %s", liveId);
        } else {
          // Headline = the frontend encoder (shared / Advanced).
          const EncoderFamily prelimFamily = m_outcome.family;
          m_outcome.encoderId = liveId;
          m_outcome.family = familyForEncoderId(liveId);
          m_measuredPreset = livePreset;
          if (m_presetGoverned &&
              (m_outcome.family != prelimFamily ||
               (!livePreset.empty() && livePreset != m_currentPreset))) {
            blog(LOG_WARNING,
                 "[Monitoring] Live encoder %s at preset '%s' does not "
                 "match the configured stepping target at '%s'; "
                 "disabling preset stepping",
                 liveId, livePreset.c_str(), m_currentPreset.c_str());
            m_presetGoverned = false;
            m_outcome.presetGoverned = false;
            m_maxAttempts = m_attempt;
          }
        }
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
  // The directory snapshot is evidence about ONE directory: the one
  // SimpleOutput/FilePath named at attempt start. If the recording
  // actually landed somewhere else (Advanced-mode paths, an unsaved
  // path change, a filename format with folders), the snapshot says
  // nothing about the real folder — treat ownership as unprovable.
  if (m_dirSnapshotTaken) {
    std::error_code ec1, ec2;
    const auto parent =
        fs::weakly_canonical(fs::u8path(m_testFilePath).parent_path(), ec1);
    const auto snap = fs::weakly_canonical(fs::u8path(m_snapshotDir), ec2);
    if (ec1 || ec2 || parent != snap) {
      m_dirSnapshotTaken = false;
      blog(LOG_WARNING,
           "[Monitoring] Recording landed in %s, not the snapshot "
           "directory %s; ownership unprovable, cleanup report-only",
           fs::u8path(m_testFilePath).parent_path().u8string().c_str(),
           m_snapshotDir.c_str());
    }
  }
  // Ownership evidence, taken while the test recording is running:
  // the remux sibling cannot have been produced by THIS recording yet
  // (remux runs after stop), so a sibling present now predates the
  // test and is the user's file. For the recording itself, the
  // attempt-start directory snapshot decides (see the header note).
  m_siblingExistedAtStart = fileExists(remuxSiblingPath());
  if (m_dirSnapshotTaken) {
    const std::string name = fs::u8path(m_testFilePath).filename().u8string();
    m_recordingExistedAtStart = m_dirNamesAtStart.count(name) > 0;
  } else {
    m_recordingExistedAtStart = true;
  }
  // Settle expectation: with auto-remux on, the .mp4 sibling is this
  // recording's remux product and must appear before polls count as
  // quiet — unless the recording IS the .mp4 (no sibling exists).
  const std::string sibling = remuxSiblingPath();
  m_siblingExpected =
      m_autoRemux && !sibling.empty() && sibling != m_testFilePath;
  m_lastTrackedFiles = trackedFiles();
}

void StabilityMonitor::cancel() {
  if (!running())
    return;
  // The outcome is Cancelled no matter which state the cancel lands
  // in — including settling. (An earlier shape let a settle-time
  // cancel fall through to verdict evaluation, which could step the
  // preset and start another attempt after the user had cancelled.)
  m_cancelRequested = true;
  if (m_state == State::Settling) {
    // Cuts the settle wait short via the tracker, without weakening
    // deletion evidence. completeAttempt() will finish Cancelled.
    m_settleCancelRequested = true;
    return;
  }
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
  if (m_probeStarted)
    m_probe.stopOutput();
  m_state = State::Settling;
  m_settleTracker = SettleTracker{};
  m_settleCancelRequested = false;
  m_lastTrackedFiles = trackedFiles();
}

void StabilityMonitor::onTick() {
  const double attemptElapsed = m_attemptClock.elapsed() / 1000.0;

  if (m_state == State::Recording) {
    const bool frontendActive =
        !frontendExpected() || obs_frontend_recording_active();
    const bool probeActive = !probeExpected() || m_probe.active();
    if (!m_baselineTaken && frontendActive && probeActive)
      takeBaseline();

    if (m_baselineTaken && attemptElapsed >= 2.0 &&
        (!frontendActive || !probeActive)) {
      // Started, then died on its own: an error — but the partial
      // files still go through the settle policy before finishing.
      m_pending = Pending::FinishInstead;
      m_pendingKind = TestOutcome::Kind::Error;
      m_pendingDetail = !frontendActive
                            ? "The test recording stopped early."
                            : "The streaming test encoder stopped early.";
      enterSettling();
      return;
    }
    if (!m_baselineTaken && attemptElapsed >= 5.0) {
      m_pending = Pending::FinishInstead;
      m_pendingKind = TestOutcome::Kind::Error;
      m_pendingDetail = !frontendActive
                            ? "The test recording did not start."
                            : "The streaming test encoder did not start.";
      enterSettling();
      return;
    }

    const double fps = obs_get_active_fps();
    const double cpu = m_cpuInfo ? os_cpu_usage_info_query(m_cpuInfo) : 0.0;
    if (m_baselineTaken && m_windowClock.elapsed() >= 1000) {
      // Skip the first window second (warm-up) — the verdict
      // counters share that exclusion via the baseline.
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
      m_pendingDetail = "The test ran past its time budget.";
      enterSettling();
    }
    return;
  }

  if (m_state == State::Settling) {
    // The settle tracker always terminates on its own budget
    // (SETTLE_MAX_POLLS polls), so no second watchdog is needed here.
    pollSettling();
    return;
  }
}

bool StabilityMonitor::pollSettling() {
  // Output starts are asynchronous: a cancel (or the did-not-start
  // timeout) can enter settling before an output ever became active,
  // and a stop issued then would have nothing to stop. Keep issuing
  // the stops while finishing so a late-starting output is stopped
  // instead of being left running.
  if (obs_frontend_recording_active())
    obs_frontend_recording_stop();
  if (m_probeStarted)
    m_probe.stopOutput();
  const auto now = trackedFiles();
  const bool changed = now != m_lastTrackedFiles;
  m_lastTrackedFiles = now;
  SettleObservation observation;
  observation.recordingActive =
      obs_frontend_recording_active() || (m_probeStarted && m_probe.active());
  observation.filesChanged = changed;
  observation.cancelRequested = m_settleCancelRequested;
  const std::string sibling = remuxSiblingPath();
  observation.siblingExpected = m_siblingExpected;
  observation.siblingPresent = !sibling.empty() && fileExists(sibling);
  const SettleAction action = m_settleTracker.observe(observation);
  if (action == SettleAction::Wait)
    return false;
  performCleanup(action == SettleAction::DeleteNow && !m_forceReportOnly);
  completeAttempt();
  return true;
}

void StabilityMonitor::performCleanup(bool mayDelete) {
  bool deletedRecording = false;
  bool deletedSibling = false;
  bool deletedProbe = false;
  auto handle = [&](const std::string &path, bool planAllows, bool isPrimary,
                    bool &deletedFlag) {
    if (path.empty())
      return;
    if (!fileExists(path)) {
      // Absence is not deletion: the file may never have been
      // created. Say exactly that — but only for the recording
      // itself, and only when cleanup would otherwise have deleted
      // it (a missing remux sibling or probe file is unremarkable).
      if (isPrimary && mayDelete && planAllows)
        m_cleanupNotes.push_back("The test recording was not found "
                                 "afterwards, so there was nothing "
                                 "to delete.");
      return;
    }
    if (!mayDelete) {
      // Settle policy said leave everything (timeout, or a cancel
      // without full quiet evidence), or this mode is report-only:
      // report, touch nothing.
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
    if (fs::remove(fs::u8path(path), ec)) {
      blog(LOG_INFO, "[Monitoring] Deleted test file %s", path.c_str());
      deletedFlag = true;
    } else {
      m_cleanupNotes.push_back("Could not delete a test recording; "
                               "it is still at: " +
                               path);
    }
  };

  if (frontendExpected()) {
    if (m_fallbackMode || m_testFilePath.empty()) {
      m_cleanupNotes.push_back(
          "The test recording could not be identified for cleanup, so "
          "nothing was deleted automatically. Check your recording "
          "folder for a short test recording from just now.");
    } else {
      const CleanupPlan plan =
          planCleanup(m_fallbackMode, !m_testFilePath.empty(),
                      m_recordingExistedAtStart, m_siblingExistedAtStart);
      handle(m_testFilePath, plan.deleteRecording, true, deletedRecording);
      const std::string sibling = remuxSiblingPath();
      if (!sibling.empty() && sibling != m_testFilePath)
        handle(sibling, plan.deleteRemuxSibling, false, deletedSibling);
    }
  }
  // The probe file is test-created by construction (the plugin named
  // and started it): the plan always allows its deletion; the settle
  // decision (mayDelete) still applies in full.
  if (!m_probeFilePath.empty())
    handle(m_probeFilePath, true, false, deletedProbe);
  // Deletion claims are made only for files that existed at cleanup
  // time and were actually removed above.
  if (deletedRecording && deletedProbe) {
    m_cleanupNotes.push_back("Both test recordings were deleted.");
  } else if (deletedRecording) {
    std::string note = "The test recording was deleted.";
    if (deletedSibling)
      note += " Its remuxed copy was deleted too.";
    m_cleanupNotes.push_back(note);
  } else if (deletedProbe) {
    m_cleanupNotes.push_back("The streaming test recording was deleted.");
  } else if (deletedSibling) {
    m_cleanupNotes.push_back("The remuxed copy of the test recording "
                             "was deleted.");
  }
}

void StabilityMonitor::sampleFinalMetrics() {
  if (!m_baselineTaken) {
    m_streamingMetrics = Metrics{};
    return;
  }
  Metrics shared;
  shared.durationSeconds = m_windowClock.elapsed() / 1000.0;
  shared.totalFrames =
      static_cast<uint32_t>(obs_get_total_frames() - m_baseTotalFrames);
  shared.laggedFrames =
      static_cast<uint32_t>(obs_get_lagged_frames() - m_baseLaggedFrames);
  shared.averageFps = m_fpsSamples > 0 ? m_fpsSum / m_fpsSamples : 0.0;
  shared.targetFps = m_targetFps;
  shared.averageCpuPercent = m_cpuSamples > 0 ? m_cpuSum / m_cpuSamples : 0.0;
  shared.peakCpuPercent = m_cpuPeak;

  const uint32_t frontendDelta =
      static_cast<uint32_t>(frontendSkippedFrames() - m_baseSkippedFrames);
  // One metrics record for the attempt: in combined mode these ARE
  // the pair's numbers (the pipeline counters are shared), read from
  // the probe's counter when the probe ran — it is the encoder whose
  // preset the stepping ladder controls — and from the frontend
  // counter otherwise (shared-encoder / Advanced modes).
  m_streamingMetrics = shared;
  m_streamingMetrics.skippedFrames =
      probeExpected() ? static_cast<uint32_t>(m_probe.skippedFrames() -
                                              m_baseProbeSkippedFrames)
                      : frontendDelta;
}

void StabilityMonitor::completeAttempt() {
  m_timer->stop();
  m_probe.stop(); // release this attempt's probe; metrics are sampled

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
  // Reported preset = the measured one (live readback); fall back to
  // the config cursor only when governed, where the config key feeds
  // the measured encoder's settings by construction. Otherwise
  // empty: the encoder ran at a preset nobody reported.
  record.preset = !m_measuredPreset.empty()
                      ? m_measuredPreset
                      : (m_presetGoverned ? m_currentPreset : std::string());
  record.metrics = m_streamingMetrics;
  record.stability = evaluateStability(m_streamingMetrics);
  m_outcome.attempts.push_back(record);

  blog(LOG_INFO,
       "[Monitoring] Attempt %d: streaming %llu frames, lagged %.2f%%, "
       "skipped %.2f%% -> %s",
       m_attempt,
       static_cast<unsigned long long>(m_streamingMetrics.totalFrames),
       record.stability.laggedPercent, record.stability.skippedPercent,
       record.stability.tier == Tier::Stable
           ? "STABLE"
           : (record.stability.tier == Tier::Marginal ? "MARGINAL"
                                                      : "UNSTABLE"));

  // The verdict is the FINAL attempt's tier. decideOutcome() is the
  // pure policy (unit-tested); this function only performs the side
  // effects the decision names: apply a step, restore on Marginal,
  // finish. The M4 guard lives in the policy: a step is offered only
  // when the preset is governed, attempts remain, and a rung exists
  // — never step a config key that does not control the encoder
  // being measured.
  const auto nextStep = nextLadderStep(m_outcome.family, m_currentPreset);
  const AttemptDecision decision =
      decideOutcome(record.stability.tier, m_mode == Mode::Combined,
                    m_mode == Mode::AdvancedRecording, m_presetGoverned,
                    m_attempt < m_maxAttempts, nextStep,
                    m_currentPreset != m_outcome.originalPreset);
  switch (decision.action) {
  case AttemptDecision::Action::FinishStable:
    finish(TestOutcome::Kind::Stable);
    return;
  case AttemptDecision::Action::FinishMarginal: {
    if (decision.restoreOriginal) {
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
        m_outcome.presetRestoreFailed = true;
      }
    }
    finish(TestOutcome::Kind::Marginal);
    return;
  }
  case AttemptDecision::Action::StepAndRetry: {
    settings::SettingsManager settingsManager;
    if (settingsManager.applyEncoderPreset(decision.stepPreset)) {
      blog(LOG_INFO,
           "[Monitoring] Stepping encoder preset '%s' -> '%s' "
           "and retrying",
           m_currentPreset.c_str(), decision.stepPreset.c_str());
      m_currentPreset = decision.stepPreset;
      beginAttempt();
      return;
    }
    finish(TestOutcome::Kind::Error,
           "Could not apply the stepped encoder preset '" +
               decision.stepPreset + "'.");
    return;
  }
  case AttemptDecision::Action::FinishUnstable:
    m_outcome.failedSide = decision.failedSide;
    finish(TestOutcome::Kind::Unstable);
    return;
  }
}

void StabilityMonitor::finish(TestOutcome::Kind kind,
                              const std::string &detail) {
  m_probe.stop();
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
  // One line per DISTINCT cleanup outcome: a three-attempt run
  // otherwise repeats "Both test recordings were deleted." three
  // times. Distinct notes (different left-in-place paths) all stay.
  std::string note;
  std::vector<std::string> seen;
  for (const std::string &line : m_cleanupNotes) {
    if (std::find(seen.begin(), seen.end(), line) == seen.end()) {
      seen.push_back(line);
      if (!note.empty())
        note += "\n";
      note += line;
    }
  }
  m_outcome.cleanupNote = note;
  emit finished(m_outcome);
}

} // namespace monitoring
} // namespace obs_setup
