#ifndef OBS_SETUP_DETECTION_DETECTION_MODULE_HPP
#define OBS_SETUP_DETECTION_DETECTION_MODULE_HPP

#include <string>
#include <vector>

namespace obs_setup {
namespace detection {

/**
 * @brief Represents a detected video encoder.
 */
struct DetectedEncoder {
  std::string id;
  std::string name;
  std::string type; // e.g., "h264", "hevc"
};

/**
 * @brief Detects available encoders in the current OBS environment.
 * @return A list of detected encoders.
 */
std::vector<DetectedEncoder> detect_available_encoders();

/**
 * @brief Logs the first detected encoder to the OBS log.
 */
void log_first_detected_encoder();

} // namespace detection
} // namespace obs_setup

#endif // OBS_SETUP_DETECTION_DETECTION_MODULE_HPP
