#include <obs-frontend-api.h>
#include <obs-module.h>
#include <plugin-support.h>

#include "detection/detection-module.hpp"
#include "ui/ui-module.hpp"

extern "C" {

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

/**
 * @brief Callback for the "Tools > OBS Setup Test" menu item.
 */
static void on_test_menu_item_clicked(void *private_data) {
  (void)private_data;

  // 1. Show Hello World dialog
  obs_setup::ui::show_hello_world_dialog();

  // 2. Detect and log one encoder
  obs_setup::detection::log_first_detected_encoder();
}

bool obs_module_load(void) {
  obs_log(LOG_INFO, "plugin loaded successfully (version %s)", PLUGIN_VERSION);

  // Add the "Tools > OBS Setup Test" menu item
  obs_frontend_add_tools_menu_item("OBS Setup Test", on_test_menu_item_clicked,
                                   nullptr);

  return true;
}

void obs_module_unload(void) { obs_log(LOG_INFO, "plugin unloaded"); }
}
