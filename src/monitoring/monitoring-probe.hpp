#ifndef OBS_SETUP_MONITORING_MONITORING_PROBE_HPP
#define OBS_SETUP_MONITORING_MONITORING_PROBE_HPP

#include "monitoring-module.hpp"

#include <cstdint>
#include <string>

struct config_data;
typedef struct config_data config_t;
struct obs_encoder;
typedef struct obs_encoder obs_encoder_t;
struct obs_output;
typedef struct obs_output obs_output_t;

namespace obs_setup {
namespace monitoring {

/**
 * @brief The streaming-encoder probe: the plugin's own encoder +
 * local file output, configured with the profile's streaming
 * settings.
 *
 * Why this exists: OBS's SimpleOutput decides which encoder the
 * recording output uses exactly once, when the output handler is
 * constructed (OBS 31.1.1 SimpleOutput.cpp: LoadRecordingPreset()
 * is called only from the constructor) — so no settings change,
 * saved or in-memory, can make the frontend's recording output run
 * the streaming encoder on an existing profile. Measuring the
 * streaming encode therefore requires an encoder of our own.
 *
 * What is replicated is deliberately small: the encoder settings
 * construction OBS's SimpleOutput::Update() applies to the real
 * streaming encoder, from the same SimpleOutput profile keys —
 * rate_control CBR, bitrate from VBitrate, the family preset key
 * ("preset", or "preset2" for the FFmpeg NVENC encoder id). Service
 * encoder overrides (obs_service_apply_encoder_settings) are NOT
 * applied: the frontend's service object is not reachable from the
 * plugin API, and a service override would change bitrates, not
 * whether the encoder can sustain the encode. The encode itself is
 * fully real: a live libobs encoder instance encoding the main
 * video pipeline's frames into an ffmpeg_muxer file output — the
 * same load the streaming encoder would carry, minus the network
 * output's frame-dropping behavior.
 *
 * The probe also carries an FFmpeg AAC encoder (bitrate from
 * ABitrate): not because audio load matters to the verdict, but
 * because libobs refuses to start an encoded output whose flags
 * include AUDIO unless an audio encoder is attached
 * (obs-output.c: can_begin_data_capture -> audio_valid), and
 * ffmpeg_muxer's flags include AUDIO. A video-only muxer output
 * fails obs_output_start SILENTLY — no log line, no last error —
 * which cost a debugging round to find. The frontend always
 * attaches an AAC encoder to its file output; so does the probe.
 */
class StreamingProbe {
public:
  StreamingProbe() = default;
  ~StreamingProbe();
  StreamingProbe(const StreamingProbe &) = delete;
  StreamingProbe &operator=(const StreamingProbe &) = delete;

  /**
   * Creates the encoder + output and starts the encode.
   * @param config  The active profile config (SimpleOutput keys).
   * @param preset  The streaming preset to run (the monitor's
   *                stepping cursor value).
   * @param filePath Full path of the probe recording file (.mkv).
   * @param error   Failure description when false is returned.
   */
  bool start(config_t *config, const std::string &preset,
             const std::string &filePath, std::string &error);
  /** Stops the output (if running) and releases encoder + output. */
  void stop();
  /** Stops the output without releasing anything: the settle phase
   *  re-issues stops while the file quiets, and the encoder must
   *  stay alive until final metrics are sampled. */
  void stopOutput();
  bool active() const;
  /** The probe encoder's skipped-frame counter (its video output). */
  uint32_t skippedFrames() const;
  const std::string &encoderId() const { return m_encoderId; }
  /** The running encoder's own preset readback ("" if it reports
   *  none) — the measured preset, as with the frontend path. */
  std::string livePreset() const;

private:
  obs_encoder_t *m_encoder = nullptr;
  obs_encoder_t *m_audioEncoder = nullptr;
  obs_output_t *m_output = nullptr;
  std::string m_encoderId;
};

} // namespace monitoring
} // namespace obs_setup

#endif // OBS_SETUP_MONITORING_MONITORING_PROBE_HPP
