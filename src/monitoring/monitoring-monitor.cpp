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

std::map<std::string, uint64_t> StabilityMonitor::snapshotDirectory() const {
  std::map<std::string, uint64_t> files;
  const std::string dir = recordingDirectory();
  if (dir.empty())
    return files;
  std::error_code ec;
  for (const auto &entry : std::filesystem::directory_iterator(dir, ec)) {
    if (entry.is_regular_file(ec))
      files[entry.path().string()] = entry.file_size(ec);
  }
  return files;
}

std::map<std::string, uint64_t> StabilityMonitor::currentTestFiles() const {
  std::map<std::string, uint64_t> files;
  std::error_code ec;
  if (!m_testFilePath.empty()) {
    // Authoritative: the exact file the recording output is writing,
    // plus the auto-remux sibling (same stem, .mp4) when it appears.
    if (std::filesystem::is_regular_file(m_testFilePath, ec))
      files[m_testFilePath] = std::filesystem::file_size(m_testFilePath, ec);
    const std::string remuxed = std::filesystem::path(m_testFilePath)
                                    .replace_extension(".mp4")
                                    .string();
    if (remuxed != m_testFilePath &&
        std::filesystem::is_regular_file(remuxed, ec))
      files[remuxed] = std::filesystem::file_size(remuxed, ec);
    return files;
  }
  // Fallback: whatever appeared in the recording directory.
  for (const auto &[path, size] : snapshotDirectory()) {
    if (m_dirSnapshot.find(path) == m_dirSnapshot.end())
      files[path] = size;
  }
  return files;
}

void StabilityMonitor::start() {
  if (running())
    return;
  m_cancelled = false;
  m_outcome = TestOutcome{};

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
  const char *simpleEncoder =
      config_get_string(config, "SimpleOutput", "StreamEncoder");
  if (!simpleEncoder || !*simpleEncoder) {
    finish(TestOutcome::Kind::Error,
           "The active profile has no streaming encoder configured.");
    return;
  }
  m_outcome.encoderId =
      settings::libobsEncoderIdForSimpleEncoder(simpleEncoder);
  if (m_outcome.encoderId.empty()) {
    finish(TestOutcome::Kind::Error,
           std::string("Unrecognized encoder '") + simpleEncoder + "'.");
    return;
  }
  m_outcome.family = familyForEncoderId(m_outcome.encoderId);

  const std::string key = settings::presetKeyForEncoder(m_outcome.encoderId);
  const char *preset =
      key.empty() ? nullptr
                  : config_get_string(config, "SimpleOutput", key.c_str());
  m_currentPreset = preset && *preset
                        ? preset
                        : settings::defaultPresetFor(m_outcome.encoderId);
  m_outcome.originalPreset = m_currentPreset;
  m_maxAttempts = maxAttempts(m_outcome.family);
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
  m_elapsedSeconds = 0;
  m_settleTicks = 0;
  m_baseTotalFrames = obs_get_total_frames();
  m_baseLaggedFrames = obs_get_lagged_frames();
  m_skippedBaselineTaken = false;
  m_fpsSum = 0.0;
  m_fpsSamples = 0;
  m_cpuSum = 0.0;
  m_cpuSamples = 0;
  m_cpuPeak = 0.0;
  m_attemptMetrics = Metrics{};
  m_dirSnapshot = snapshotDirectory();
  m_lastNewFiles.clear();
  m_testFilePath.clear();

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

void StabilityMonitor::cancel() {
  if (!running() || m_cancelled)
    return;
  m_cancelled = true;
  if (m_state == State::Recording) {
    // Stop the recording; the settling path still cleans up the test
    // file before finishing as Cancelled.
    sampleFinalMetrics();
    obs_frontend_recording_stop();
    m_state = State::Settling;
    m_settleTicks = 0;
  }
}

void StabilityMonitor::onTick() {
  if (m_state == State::Recording) {
    ++m_elapsedSeconds;

    if (m_elapsedSeconds >= 2 && !obs_frontend_recording_active()) {
      finish(TestOutcome::Kind::Error,
             "The test recording did not start (or stopped early).");
      return;
    }

    if (!m_skippedBaselineTaken && obs_frontend_recording_active()) {
      // The recording output exists only once recording is running;
      // take the encoder-skipped baseline at the first opportunity.
      m_baseSkippedFrames = recordingSkippedFrames();
      m_skippedBaselineTaken = true;
      // Same moment: pin down the exact file this test owns, from the
      // output's own settings ("path" is the full recording path for
      // OBS's file outputs) — cleanup later deletes exactly this.
      obs_output_t *output = obs_frontend_get_recording_output();
      if (output) {
        obs_data_t *settings = obs_output_get_settings(output);
        if (settings) {
          const char *path = obs_data_get_string(settings, "path");
          if (path && *path)
            m_testFilePath = path;
          obs_data_release(settings);
        }
      }
    }

    const double fps = obs_get_active_fps();
    const double cpu = m_cpuInfo ? os_cpu_usage_info_query(m_cpuInfo) : 0.0;
    if (m_elapsedSeconds >= 2) { // tick 1 counters are still warming up
      m_fpsSum += fps;
      ++m_fpsSamples;
      m_cpuSum += cpu;
      ++m_cpuSamples;
      if (cpu > m_cpuPeak)
        m_cpuPeak = cpu;
    }
    emit progress(m_elapsedSeconds, fps, cpu);

    if (m_elapsedSeconds >= TEST_DURATION_SECONDS) {
      sampleFinalMetrics();
      obs_frontend_recording_stop();
      m_state = State::Settling;
      m_settleTicks = 0;
    }
    return;
  }

  if (m_state == State::Settling) {
    ++m_settleTicks;
    const auto newFiles = currentTestFiles();
    // Settled when the recorder is off and the test's files (the
    // recording and any remuxed copy) stopped changing for a poll.
    const bool settled = !obs_frontend_recording_active() &&
                         !newFiles.empty() && newFiles == m_lastNewFiles;
    m_lastNewFiles = newFiles;
    // The settle budget is what remains of the per-attempt watchdog
    // (ATTEMPT_TIMEOUT_SECONDS) after the 30-second recording.
    constexpr int kMaxSettleTicks =
        ATTEMPT_TIMEOUT_SECONDS - TEST_DURATION_SECONDS;
    if (settled || m_settleTicks > kMaxSettleTicks) {
      if (!settled)
        blog(LOG_WARNING, "[Monitoring] Test files did not settle; cleaning "
                          "up what the test created anyway");
      finishAttempt();
    }
    return;
  }
}

void StabilityMonitor::sampleFinalMetrics() {
  Metrics &m = m_attemptMetrics;
  m.durationSeconds = static_cast<double>(m_elapsedSeconds);
  m.totalFrames =
      static_cast<uint32_t>(obs_get_total_frames() - m_baseTotalFrames);
  m.laggedFrames =
      static_cast<uint32_t>(obs_get_lagged_frames() - m_baseLaggedFrames);
  m.skippedFrames = m_skippedBaselineTaken
                        ? static_cast<uint32_t>(recordingSkippedFrames() -
                                                m_baseSkippedFrames)
                        : 0;
  m.averageFps = m_fpsSamples > 0 ? m_fpsSum / m_fpsSamples : 0.0;
  m.targetFps = m_targetFps;
  m.averageCpuPercent = m_cpuSamples > 0 ? m_cpuSum / m_cpuSamples : 0.0;
  m.peakCpuPercent = m_cpuPeak;
}

void StabilityMonitor::deleteTestFiles() {
  // m_lastNewFiles holds full paths (see currentTestFiles()).
  for (const auto &[path, size] : m_lastNewFiles) {
    (void)size;
    std::error_code ec;
    if (std::filesystem::remove(path, ec))
      blog(LOG_INFO, "[Monitoring] Deleted test recording %s", path.c_str());
    else if (!ec)
      blog(LOG_WARNING, "[Monitoring] Test recording already gone: %s",
           path.c_str());
    else
      blog(LOG_WARNING, "[Monitoring] Could not delete test recording %s: %s",
           path.c_str(), ec.message().c_str());
  }
}

void StabilityMonitor::finishAttempt() {
  m_timer->stop();
  deleteTestFiles();

  if (m_cancelled) {
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
       record.stability.stable ? "STABLE" : "UNSTABLE");

  if (record.stability.stable) {
    finish(TestOutcome::Kind::Stable);
    return;
  }
  if (m_attempt < m_maxAttempts) {
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
  emit finished(m_outcome);
}

} // namespace monitoring
} // namespace obs_setup
