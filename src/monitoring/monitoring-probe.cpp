#include "monitoring-probe.hpp"

#include "../settings/settings-module.hpp"

#include <media-io/video-io.h>
#include <obs.h>
#include <util/config-file.h>

namespace obs_setup {
namespace monitoring {
namespace {

bool startsWith(const std::string &s, const char *prefix) {
  const std::string p(prefix);
  return s.size() >= p.size() && s.compare(0, p.size(), p) == 0;
}

} // namespace

StreamingProbe::~StreamingProbe() { stop(); }

bool StreamingProbe::start(config_t *config, const std::string &preset,
                           const std::string &filePath, std::string &error) {
  stop();
  if (!config) {
    error = "No active profile configuration.";
    return false;
  }
  const char *simpleEncoder =
      config_get_string(config, "SimpleOutput", "StreamEncoder");
  m_encoderId = simpleEncoder
                    ? settings::libobsEncoderIdForSimpleEncoder(simpleEncoder)
                    : "";
  if (m_encoderId.empty()) {
    error = "The profile has no streaming encoder configured.";
    return false;
  }

  // The SimpleOutput::Update() construction, from the profile keys:
  // CBR at VBitrate, the family preset. The FFmpeg NVENC encoder
  // takes its preset under "preset2"; every other encoder uses
  // "preset" (same special case as the frontend's own code).
  obs_data_t *encSettings = obs_data_create();
  obs_data_set_string(encSettings, "rate_control", "CBR");
  uint64_t bitrate = config_get_uint(config, "SimpleOutput", "VBitrate");
  if (bitrate == 0)
    bitrate = 6000;
  obs_data_set_int(encSettings, "bitrate", static_cast<int64_t>(bitrate));
  const EncoderFamily family = familyForEncoderId(m_encoderId);
  if (family == EncoderFamily::NVENC && startsWith(m_encoderId, "ffmpeg_"))
    obs_data_set_string(encSettings, "preset2", preset.c_str());
  else
    obs_data_set_string(encSettings, "preset", preset.c_str());

  m_encoder = obs_video_encoder_create(
      m_encoderId.c_str(), "quickstart_stream_probe", encSettings, nullptr);
  obs_data_release(encSettings);
  if (!m_encoder) {
    error =
        "Could not create the streaming test encoder (" + m_encoderId + ").";
    return false;
  }
  obs_encoder_set_video(m_encoder, obs_get_video());

  // An audio encoder is REQUIRED for the muxer output to start at
  // all — see the class comment (libobs can_begin_data_capture).
  // Same SimpleOutput key the real stream's audio uses.
  uint64_t audioBitrate = config_get_uint(config, "SimpleOutput", "ABitrate");
  if (audioBitrate == 0)
    audioBitrate = 160;
  obs_data_t *audioSettings = obs_data_create();
  obs_data_set_int(audioSettings, "bitrate",
                   static_cast<int64_t>(audioBitrate));
  m_audioEncoder = obs_audio_encoder_create(
      "ffmpeg_aac", "quickstart_stream_probe_audio", audioSettings, 0, nullptr);
  obs_data_release(audioSettings);
  if (!m_audioEncoder) {
    error = "Could not create the streaming test audio encoder.";
    stop();
    return false;
  }
  obs_encoder_set_audio(m_audioEncoder, obs_get_audio());

  obs_data_t *outSettings = obs_data_create();
  obs_data_set_string(outSettings, "path", filePath.c_str());
  m_output = obs_output_create("ffmpeg_muxer", "quickstart_stream_probe",
                               outSettings, nullptr);
  obs_data_release(outSettings);
  if (!m_output) {
    error = "Could not create the streaming test output.";
    stop();
    return false;
  }
  obs_output_set_video_encoder(m_output, m_encoder);
  obs_output_set_audio_encoder(m_output, m_audioEncoder, 0);
  if (!obs_output_start(m_output)) {
    const char *lastError = obs_output_get_last_error(m_output);
    error = "The streaming test encoder did not start";
    if (lastError && *lastError) {
      error += ": ";
      error += lastError;
    } else {
      error += ".";
    }
    stop();
    return false;
  }
  blog(LOG_INFO,
       "[Monitoring] Streaming probe running: encoder %s at preset "
       "'%s', %llu kbps CBR -> %s",
       m_encoderId.c_str(), preset.c_str(),
       static_cast<unsigned long long>(bitrate), filePath.c_str());
  return true;
}

void StreamingProbe::stopOutput() {
  if (m_output && obs_output_active(m_output))
    obs_output_stop(m_output);
}

void StreamingProbe::stop() {
  stopOutput();
  if (m_output) {
    obs_output_release(m_output);
    m_output = nullptr;
  }
  if (m_encoder) {
    obs_encoder_release(m_encoder);
    m_encoder = nullptr;
  }
  if (m_audioEncoder) {
    obs_encoder_release(m_audioEncoder);
    m_audioEncoder = nullptr;
  }
}

bool StreamingProbe::active() const {
  return m_output && obs_output_active(m_output);
}

uint32_t StreamingProbe::skippedFrames() const {
  if (!m_encoder)
    return 0;
  video_t *video = obs_encoder_video(m_encoder);
  if (!video)
    return 0;
  return video_output_get_skipped_frames(video);
}

std::string StreamingProbe::livePreset() const {
  if (!m_encoder)
    return "";
  obs_data_t *settings = obs_encoder_get_settings(m_encoder);
  if (!settings)
    return "";
  std::string preset;
  const char *p = obs_data_get_string(settings, "preset");
  if (!p || !*p)
    p = obs_data_get_string(settings, "preset2");
  if (p && *p)
    preset = p;
  obs_data_release(settings);
  return preset;
}

} // namespace monitoring
} // namespace obs_setup
