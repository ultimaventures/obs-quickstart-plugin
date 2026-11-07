#ifndef OBS_SETUP_UI_UI_MODULE_HPP
#define OBS_SETUP_UI_UI_MODULE_HPP

namespace obs_setup {
namespace ui {

/**
 * @brief Shows a simple Qt MessageBox saying "Hello World".
 */
void show_hello_world_dialog();

/**
 * @brief Shows the Quickstart settings dialog (modal).
 *
 * Creates a SettingsDialog parented to the OBS main window and runs it
 * with exec(). Must be called on the GUI thread.
 */
void show_settings_dialog();

} // namespace ui
} // namespace obs_setup

#endif // OBS_SETUP_UI_UI_MODULE_HPP
