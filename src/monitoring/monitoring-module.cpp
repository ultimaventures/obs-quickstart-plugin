#include "monitoring-module.hpp"

#include <array>
#include <cstdlib>

namespace obs_setup {
namespace monitoring {

EncoderFamily familyForEncoderId(const std::string &encoderId) {
  // Prefix rules mirror the settings module's classification so the
  // two modules can never disagree about which family an id is in.
  if (encoderId.rfind("obs_nvenc", 0) == 0)
    return EncoderFamily::NVENC;
  if (encoderId == "obs_x264")
    return EncoderFamily::X264;
  if (encoderId.rfind("obs_qsv", 0) == 0)
    return EncoderFamily::QSV;
  if (encoderId.rfind("h264_texture_amf", 0) == 0 ||
      encoderId.rfind("h265_texture_amf", 0) == 0 ||
      encoderId.rfind("av1_texture_amf", 0) == 0 ||
      encoderId.rfind("amd_amf", 0) == 0)
    return EncoderFamily::AMF;
  if (encoderId.rfind("com.apple.videotoolbox", 0) == 0)
    return EncoderFamily::VideoToolbox;
  return EncoderFamily::Unknown;
}

Stability evaluateStability(const Metrics &metrics) {
  Stability result;
  if (metrics.totalFrames == 0)
    return result; // No frames, no evidence: never call that stable.
  result.laggedPercent = 100.0 * static_cast<double>(metrics.laggedFrames) /
                         static_cast<double>(metrics.totalFrames);
  result.skippedPercent = 100.0 * static_cast<double>(metrics.skippedFrames) /
                          static_cast<double>(metrics.totalFrames);
  if (metrics.durationSeconds > 0.0 && metrics.targetFps > 0.0) {
    const double expected = metrics.durationSeconds * metrics.targetFps;
    result.frameRateDelivered = static_cast<double>(metrics.totalFrames) >=
                                MIN_DELIVERED_FRAME_RATIO * expected;
  }
  if (result.laggedPercent <= MAX_LAGGED_FRAME_PERCENT &&
      result.skippedPercent <= MAX_SKIPPED_FRAME_PERCENT &&
      result.frameRateDelivered) {
    result.tier = Tier::Stable;
  } else if (result.laggedPercent <= MARGINAL_LAGGED_FRAME_PERCENT &&
             result.skippedPercent <= MARGINAL_SKIPPED_FRAME_PERCENT &&
             result.frameRateDelivered) {
    result.tier = Tier::Marginal;
  } else {
    result.tier = Tier::Unstable;
  }
  result.stable = result.tier == Tier::Stable;
  return result;
}

std::optional<std::string> nextLadderStep(EncoderFamily family,
                                          const std::string &currentPreset) {
  switch (family) {
  case EncoderFamily::NVENC: {
    // Presets are "p1" (max performance) through "p7" (max quality).
    if (currentPreset.size() != 2 || currentPreset[0] != 'p')
      return std::nullopt;
    const int level = currentPreset[1] - '0';
    if (level < 2 || level > 7)
      return std::nullopt; // p1 is the bottom rung; anything else is
                           // not a verified preset string.
    return "p" + std::to_string(level - 1);
  }
  case EncoderFamily::X264: {
    // Standard preset order, fastest first (same list the settings
    // module verifies against).
    static constexpr std::array<const char *, 10> kOrder = {
        "ultrafast", "superfast", "veryfast", "faster",   "fast",
        "medium",    "slow",      "slower",   "veryslow", "placebo"};
    for (size_t i = 0; i < kOrder.size(); ++i) {
      if (currentPreset == kOrder[i]) {
        if (i == 0)
          return std::nullopt; // Already ultrafast.
        return std::string(kOrder[i - 1]);
      }
    }
    return std::nullopt;
  }
  case EncoderFamily::QSV:
  case EncoderFamily::AMF:
    // Ladders exist in the spec but the preset vocabularies are
    // unverified; see the header note. No step until verified.
    return std::nullopt;
  case EncoderFamily::VideoToolbox:
  case EncoderFamily::Unknown:
    return std::nullopt;
  }
  return std::nullopt;
}

int maxAttempts(EncoderFamily family) {
  // Exactly the families nextLadderStep() can step today.
  switch (family) {
  case EncoderFamily::NVENC:
  case EncoderFamily::X264:
    return MAX_LADDER_ATTEMPTS;
  case EncoderFamily::QSV:
  case EncoderFamily::AMF:
  case EncoderFamily::VideoToolbox:
  case EncoderFamily::Unknown:
    return 1;
  }
  return 1;
}

int attemptsAvailable(EncoderFamily family, const std::string &currentPreset) {
  int attempts = 1;
  std::string preset = currentPreset;
  while (attempts < maxAttempts(family)) {
    const auto step = nextLadderStep(family, preset);
    if (!step.has_value())
      break;
    preset = *step;
    ++attempts;
  }
  return attempts;
}

bool recordingPresetGovernedByConfig(EncoderFamily family,
                                     const std::string &recQuality) {
  // Only "Stream" quality shares the streaming encoder instance —
  // the one and only case where a Simple-mode preset key reaches
  // the encoder the recording test measures. Every other quality
  // builds a separate recording encoder with code-constructed
  // settings (see the header note for the per-family detail).
  if (recQuality != "Stream")
    return false;
  switch (family) {
  case EncoderFamily::NVENC:
  case EncoderFamily::X264:
    return true;
  case EncoderFamily::QSV:
  case EncoderFamily::AMF:
  case EncoderFamily::VideoToolbox:
  case EncoderFamily::Unknown:
    return false;
  }
  return false;
}

SettleAction SettleTracker::observe(const SettleObservation &observation) {
  if (m_done)
    return m_final;
  ++m_polls;
  const bool quiet =
      !observation.recordingActive && !observation.filesChanged &&
      (!observation.siblingExpected || observation.siblingPresent);
  if (quiet)
    ++m_quietPolls;
  else
    m_quietPolls = 0;

  if (m_polls > SETTLE_MAX_POLLS) {
    // Budget exhausted: leave the files and say where they are. The
    // budget is checked before the quiet evidence on purpose — a
    // settle timeout must never become a delete-anyway, not even on
    // a poll that happens to be quiet.
    m_done = true;
    m_final = SettleAction::LeaveFiles;
    return m_final;
  }
  if (m_quietPolls >= SETTLE_REQUIRED_QUIET_POLLS) {
    m_done = true;
    m_final = SettleAction::DeleteNow;
    return m_final;
  }
  if (observation.cancelRequested) {
    // A cancel ends the wait here, with less than the full quiet
    // evidence (the case above already handled a cancel arriving
    // with the evidence complete): leave the files.
    m_done = true;
    m_final = SettleAction::LeaveFiles;
    return m_final;
  }
  return SettleAction::Wait;
}

CleanupPlan planCleanup(bool fallbackMode, bool haveRecordingPath,
                        bool recordingExistedAtStart,
                        bool remuxSiblingExistedAtStart) {
  CleanupPlan plan;
  if (fallbackMode || !haveRecordingPath)
    return plan; // Identity unproven: report, never guess-delete.
  plan.deleteRecording = !recordingExistedAtStart;
  plan.deleteRemuxSibling = !remuxSiblingExistedAtStart;
  return plan;
}

} // namespace monitoring
} // namespace obs_setup
