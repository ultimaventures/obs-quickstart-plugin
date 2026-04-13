#include "detection-module.hpp"
#include <obs-module.h>
#include <plugin-support.h>

namespace obs_setup {
namespace detection {

std::vector<DetectedEncoder> detect_available_encoders() {
  std::vector<DetectedEncoder> encoders;
  const char *id = nullptr;
  size_t idx = 0;

  // Iterate through all video encoders registered in OBS
  while (obs_enum_encoder_types(idx++, &id)) {
    if (id && obs_get_encoder_type(id) == OBS_ENCODER_VIDEO) {
      DetectedEncoder encoder;
      encoder.id = id;

      const char *display_name = obs_encoder_get_display_name(id);
      encoder.name = display_name ? display_name : "";

      const char *codec = obs_get_encoder_codec(id);
      encoder.type = codec ? codec : "";

      encoders.push_back(encoder);
    }
  }

  return encoders;
}

void log_first_detected_encoder() {
  auto encoders = detect_available_encoders();
  if (!encoders.empty()) {
    const auto &first = encoders[0];
    obs_log(LOG_INFO, "[PoC] Detected Encoder: %s (%s, %s)", first.name.c_str(),
            first.id.c_str(), first.type.c_str());
  } else {
    obs_log(LOG_WARNING, "[PoC] No video encoders detected.");
  }
}

} // namespace detection
} // namespace obs_setup
