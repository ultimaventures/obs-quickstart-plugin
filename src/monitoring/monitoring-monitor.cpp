#include "monitoring-monitor.hpp"

#include "../profile/profile-module.hpp"
#include "../settings/settings-module.hpp"

#include <QTimer>

#include <media-io/video-io.h>
#include <obs-frontend-api.h>
#include <obs.h>
#include <util/config-file.h>
#include <util/platform.h>

#include <cctype>
#include <cmath>
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
  // Undo the in-memory quality switch even on destruction mid-test:
  // the edit must not outlive the check in the running process.
  restoreQuality();
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
  if (m_testFilePath.empty())
    return files;
  if (fileExists(m_testFilePath))
    files[m_testFilePath] = fileSizeOrZero(m_testFilePath);
  const std::string sibling = remuxSiblingPath();
  if (!sibling.empty() && sibling != m_testFilePath && fileExists(sibling))
    files[sibling] = fileSizeOrZero(sibling);
  return files;
}

void StabilityMonitor::switchQualityToStream() {
  if (m_qualitySwitched)
    return;
  config_t *config = obs_frontend_get_profile_config();
  if (!config)
    return;
  m_savedQualityHadUserValue =
      config_has_user_value(config, "SimpleOutput", "RecQuality");
  // IN MEMORY ONLY. No config_save* is called anywhere in this
  // monitor: the value lives in the running process's profile config
  // and is restored by restoreQuality() on every exit path. A crash
  // loses the edit with the process; the profile on disk is never
  // touched by the switch.
  config_set_string(config, "SimpleOutput", "RecQuality", "Stream");
  m_qualitySwitched = true;
  blog(LOG_INFO,
       "[Monitoring] Streaming pass: RecQuality set to "
       "'Stream' in memory (profile value %s, not saved)",
       m_recQualityKnown ? m_profileRecQuality.c_str() : "<unset>");
}

void StabilityMonitor::restoreQuality() {
  if (!m_qualitySwitched)
    return;
  m_qualitySwitched = false;
  config_t *config = obs_frontend_get_profile_config();
  if (!config)
    return;
  if (m_savedQualityHadUserValue)
    config_set_string(config, "SimpleOutput", "RecQuality",
                      m_profileRecQuality.c_str());
  else
    config_remove_value(config, "SimpleOutput", "RecQuality");
  blog(LOG_INFO, "[Monitoring] RecQuality restored in memory to %s",
       m_recQualityKnown ? m_profileRecQuality.c_str() : "<unset>");
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

void StabilityMonitor::start() {
  if (running())
    return;
  m_cancelRequested = false;
  m_settleCancelRequested = false;
  m_outcome = TestOutcome{};
  m_cleanupNotes.clear();
  m_qualitySwitched = false;
  m_streamingDone = false;

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

  // The profile's recording quality: decides whether a separate
  // recording pass is needed, and is NEVER assumed when absent — a
  // profile with no RecQuality value gets an ungoverned recording
  // pass measured as-is (the "unproven means conservative" rule the
  // rest of this module follows).
  m_recQualityKnown =
      config_has_user_value(config, "SimpleOutput", "RecQuality");
  const char *recQuality =
      config_get_string(config, "SimpleOutput", "RecQuality");
  m_profileRecQuality =
      m_recQualityKnown && recQuality && *recQuality ? recQuality : "";
  m_outcome.recQuality = m_profileRecQuality;

  obs_video_info ovi{};
  if (obs_get_video_info(&ovi) && ovi.fps_den != 0) {
    m_targetFps = static_cast<double>(ovi.fps_num) / ovi.fps_den;
  } else {
    m_targetFps = 0.0;
    m_outcome.frameRateGateInoperative = true;
  }

  m_cpuInfo = os_cpu_usage_info_start();

  if (m_advancedMode) {
    // Single pass over the recording output as it is: the
    // SimpleOutput keys (and the in-memory switch) are dormant in
    // Advanced mode, and its cleanup evidence rules do not transfer.
    m_hasRecordingPass = false;
    m_passCount = 1;
    startPass(PassKind::Recording);
    return;
  }
  m_hasRecordingPass = m_profileRecQuality != "Stream";
  m_passCount = m_hasRecordingPass ? 2 : 1;
  m_outcome.recordingSharesStreamingEncoder = !m_hasRecordingPass;
  startPass(PassKind::Streaming);
}

void StabilityMonitor::startPass(PassKind kind) {
  m_pass = kind;
  m_passIndex = (kind == PassKind::Recording && m_hasRecordingPass) ? 1 : 0;
  m_presetGoverned = false;
  m_measuredPreset.clear();

  config_t *config = obs_frontend_get_profile_config();

  if (kind == PassKind::Streaming) {
    // The streaming pass runs the recording output AS the streaming
    // encoder: RecQuality "Stream" is precisely the frontend mode
    // where the file output reuses the streaming encoder instance.
    switchQualityToStream();
    const char *simpleEncoder =
        config ? config_get_string(config, "SimpleOutput", "StreamEncoder")
               : nullptr;
    const std::string encoderId =
        simpleEncoder ? settings::libobsEncoderIdForSimpleEncoder(simpleEncoder)
                      : "";
    const std::string key = settings::presetKeyForEncoder(encoderId);
    const char *preset =
        config && !key.empty()
            ? config_get_string(config, "SimpleOutput", key.c_str())
            : nullptr;
    m_currentPreset =
        preset && *preset ? preset : settings::defaultPresetFor(encoderId);
    m_outcome.encoderId = encoderId;
    m_outcome.family = familyForEncoderId(encoderId);
    m_outcome.originalPreset = m_currentPreset;
    // In force for this pass, the quality IS "Stream" (set above) —
    // governed is decided against that fact, not against the
    // profile's stored quality.
    m_presetGoverned =
        recordingPresetGovernedByConfig(m_outcome.family, "Stream");
    m_outcome.presetGoverned = m_presetGoverned;
  } else if (m_advancedMode) {
    // Advanced profiles name the recording encoder by libobs id in
    // AdvOut/RecEncoder. No cursor: nothing is stepped or restored.
    const char *recEncoder =
        config ? config_get_string(config, "AdvOut", "RecEncoder") : nullptr;
    m_currentPreset.clear();
    m_outcome.encoderId = recEncoder ? recEncoder : "";
    m_outcome.family = familyForEncoderId(m_outcome.encoderId);
    m_outcome.originalPreset.clear();
    m_outcome.presetGoverned = false;
  } else {
    // Dual-pass recording pass: the profile's separate recording
    // encoder, named by SimpleOutput/RecEncoder. Its settings are
    // constructed by OBS's code, so there is no preset cursor and no
    // stepping — one measured attempt.
    restoreQuality(); // already restored at pass transition; idempotent
    const char *recEncoder =
        config ? config_get_string(config, "SimpleOutput", "RecEncoder")
               : nullptr;
    const std::string encoderId =
        recEncoder ? settings::libobsEncoderIdForSimpleEncoder(recEncoder) : "";
    m_outcome.recordingEncoderId = encoderId;
  }

  // Honest attempt budget: remaining ladder rungs from THIS preset —
  // and exactly one attempt when stepping could not change the
  // encoder being measured.
  m_maxAttempts = m_presetGoverned
                      ? attemptsAvailable(m_outcome.family, m_currentPreset)
                      : 1;
  m_attempt = 0;
  emit passStarted(m_passIndex, m_passCount, kind == PassKind::Recording);
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
  m_measuredPreset.clear();
  m_testFilePath.clear();
  m_fallbackMode = false;
  m_recordingExistedAtStart = false;
  m_siblingExistedAtStart = false;
  m_siblingExpected = false;
  m_lastTrackedFiles.clear();
  m_settleTracker = SettleTracker{};
  m_pending = Pending::Evaluate;
  // Auto-remux state for this attempt (drives the settle sibling
  // expectation once the recording's path is known).
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

  const std::string &prelimId =
      (m_pass == PassKind::Recording && m_hasRecordingPass)
          ? m_outcome.recordingEncoderId
          : m_outcome.encoderId;
  blog(LOG_INFO,
       "[Monitoring] Stability attempt %d/%d (%s pass): encoder %s at "
       "preset '%s'",
       m_attempt, m_maxAttempts,
       m_pass == PassKind::Streaming ? "streaming" : "recording",
       prelimId.c_str(), m_currentPreset.c_str());
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
    // recording encoder instance that is actually running. The
    // config-derived identity from startPass() is only an estimate
    // until this runs. The live preset readback is captured as the
    // MEASURED preset only — it never overwrites the config preset
    // cursor (stepping / restore), which is a different value.
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
      m_measuredPreset = livePreset;
      if (liveId && *liveId) {
        const bool headline =
            m_pass == PassKind::Streaming || !m_hasRecordingPass;
        const EncoderFamily prelimFamily = m_outcome.family;
        const std::string prelimId = m_outcome.encoderId;
        const EncoderFamily liveFamily = familyForEncoderId(liveId);
        if (headline) {
          m_outcome.encoderId = liveId;
          m_outcome.family = liveFamily;
        } else {
          m_outcome.recordingEncoderId = liveId;
        }
        if (m_presetGoverned &&
            (liveFamily != prelimFamily ||
             (!livePreset.empty() && livePreset != m_currentPreset))) {
          // The live encoder contradicts the stepping assumptions
          // (different family than configured, or running a preset
          // other than the one the config key should have produced):
          // stop stepping. The measurement itself stays valid — the
          // verdict describes the encoder that actually ran.
          blog(LOG_WARNING,
               "[Monitoring] Live encoder %s at preset '%s' does not "
               "match the configured stepping target %s at '%s'; "
               "disabling preset stepping for this pass",
               liveId, livePreset.c_str(), prelimId.c_str(),
               m_currentPreset.c_str());
          m_presetGoverned = false;
          if (headline)
            m_outcome.presetGoverned = false;
          m_maxAttempts = m_attempt;
        }
        blog(LOG_INFO,
             "[Monitoring] Measuring %s encoder %s at preset '%s' "
             "(stepping %s)",
             m_pass == PassKind::Streaming ? "streaming" : "recording", liveId,
             !livePreset.empty() ? livePreset.c_str()
                                 : (m_presetGoverned ? m_currentPreset.c_str()
                                                     : "not reported"),
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
  // attempt-start directory snapshot decides: OBS computes the exact
  // filename before starting (the output's "path" setting), names
  // are timestamped by default but a user filename format can be
  // constant, and what happens on a name collision depends on the
  // profile's Output/OverwriteIfExists — so if a file with this name
  // was already in the directory snapshot, it is treated as the
  // user's. With no (valid) snapshot, existence is unprovable and
  // the answer is likewise "pre-existing": report, never delete.
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
    // The settle tracker always terminates on its own budget
    // (SETTLE_MAX_POLLS polls), so no second watchdog is needed here.
    pollSettling();
    return;
  }
}

bool StabilityMonitor::pollSettling() {
  // Recording start is asynchronous: a cancel (or the did-not-start
  // timeout) can enter settling before the output ever became
  // active, and a stop issued then would have nothing to stop. Keep
  // issuing the stop while finishing so a late-starting recording is
  // stopped instead of being left running.
  if (obs_frontend_recording_active())
    obs_frontend_recording_stop();
  const auto now = trackedFiles();
  const bool changed = now != m_lastTrackedFiles;
  m_lastTrackedFiles = now;
  SettleObservation observation;
  observation.recordingActive = obs_frontend_recording_active();
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
  bool deletedRecording = false;
  bool deletedSibling = false;
  auto handle = [&](const std::string &path, bool planAllows, bool isPrimary) {
    if (path.empty())
      return;
    if (!fileExists(path)) {
      // Absence is not deletion: the file may never have been
      // created. Say exactly that — but only for the recording
      // itself, and only when cleanup would otherwise have deleted
      // it (a missing remux sibling is unremarkable).
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
      blog(LOG_INFO, "[Monitoring] Deleted test recording %s", path.c_str());
      if (isPrimary)
        deletedRecording = true;
      else
        deletedSibling = true;
    } else {
      m_cleanupNotes.push_back("Could not delete the test recording; "
                               "it is still at: " +
                               path);
    }
  };
  handle(m_testFilePath, plan.deleteRecording, true);
  if (!sibling.empty() && sibling != m_testFilePath)
    handle(sibling, plan.deleteRemuxSibling, false);
  // The deletion claim is made only for files that existed at
  // cleanup time and were actually removed above.
  if (deletedRecording) {
    std::string note = "The test recording was deleted.";
    if (deletedSibling)
      note += " Its remuxed copy was deleted too.";
    m_cleanupNotes.push_back(note);
  } else if (deletedSibling) {
    m_cleanupNotes.push_back("The remuxed copy of the test recording "
                             "was deleted.");
  }
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
    if (m_pass == PassKind::Recording && m_hasRecordingPass &&
        m_streamingDone && m_pendingKind == TestOutcome::Kind::Error) {
      // The recording pass failed mechanically AFTER the streaming
      // pass produced a verdict: keep the headline, record the
      // recording failure alongside it.
      m_outcome.recordingKind = TestOutcome::Kind::Error;
      m_outcome.recordingError = m_pendingDetail;
      finish(m_outcome.kind);
      return;
    }
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
  // the measured instance by construction. Otherwise empty: the
  // encoder ran at a preset nobody reported.
  record.preset = !m_measuredPreset.empty()
                      ? m_measuredPreset
                      : (m_presetGoverned ? m_currentPreset : std::string());
  record.metrics = m_attemptMetrics;
  record.stability = evaluateStability(m_attemptMetrics);
  const bool dualRecording =
      m_pass == PassKind::Recording && m_hasRecordingPass;
  if (dualRecording)
    m_outcome.recordingAttempt = record;
  else
    m_outcome.attempts.push_back(record);

  blog(LOG_INFO,
       "[Monitoring] %s attempt %d: %llu frames, lagged %.2f%%, skipped "
       "%.2f%%, fps %.1f/%.1f, cpu avg %.0f%% -> %s",
       m_pass == PassKind::Streaming ? "Streaming" : "Recording", m_attempt,
       static_cast<unsigned long long>(m_attemptMetrics.totalFrames),
       record.stability.laggedPercent, record.stability.skippedPercent,
       m_attemptMetrics.averageFps, m_attemptMetrics.targetFps,
       m_attemptMetrics.averageCpuPercent,
       record.stability.tier == Tier::Stable
           ? "STABLE"
           : (record.stability.tier == Tier::Marginal ? "MARGINAL"
                                                      : "UNSTABLE"));

  if (record.stability.tier == Tier::Stable) {
    passFinished(TestOutcome::Kind::Stable);
    return;
  }
  if (record.stability.tier == Tier::Marginal) {
    // Marginal evidence does not justify keeping a stepped preset:
    // restore the original and let the user retest, rather than
    // silently locking in a quality reduction on borderline data.
    // Only the streaming pass can have stepped. A FAILED restore is
    // part of the outcome — never a reassurance.
    if (m_pass == PassKind::Streaming &&
        m_currentPreset != m_outcome.originalPreset) {
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
    passFinished(TestOutcome::Kind::Marginal);
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
  passFinished(TestOutcome::Kind::Unstable);
}

void StabilityMonitor::passFinished(TestOutcome::Kind verdict) {
  if (m_pass == PassKind::Streaming && m_hasRecordingPass) {
    // Headline verdict banked; the recording pass now measures the
    // profile's real recording setup (quality switch undone first,
    // inside startPass via restoreQuality()).
    m_outcome.kind = verdict;
    m_streamingDone = true;
    startPass(PassKind::Recording);
    return;
  }
  if (m_pass == PassKind::Recording && m_hasRecordingPass) {
    m_outcome.recordingKind = verdict;
    m_outcome.recordingMeasured = true;
    finish(m_outcome.kind);
    return;
  }
  finish(verdict);
}

void StabilityMonitor::finish(TestOutcome::Kind kind,
                              const std::string &detail) {
  // The in-memory quality switch never outlives the check.
  restoreQuality();
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
      note += "\n";
    note += m_cleanupNotes[i];
  }
  m_outcome.cleanupNote = note;
  emit finished(m_outcome);
}

} // namespace monitoring
} // namespace obs_setup
