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

/**
 * @brief Shows the stability-check dialog (modal) and starts the check.
 *
 * The standalone home of the 30-second recording test (architecture
 * §7); the Sprint 6 wizard will run the same monitor as its final
 * validation step. Must be called on the GUI thread.
 */
void show_stability_dialog();

/**
 * @brief Offers the pending stability-ladder retest, if one is owed.
 *
 * Called once OBS has finished loading. When a ladder action (a
 * recording-quality step, or the full rescue) was applied in a
 * previous session and its verification retest hasn't run, this asks
 * — once, and only for the profile the action belongs to — whether
 * to run the check now. "Don't ask again" silences only this
 * reminder; the ladder record itself is kept (see
 * monitoring-ladder-state.hpp). Must be called on the GUI thread.
 */
void maybe_prompt_pending_retest();

} // namespace ui
} // namespace obs_setup

#endif // OBS_SETUP_UI_UI_MODULE_HPP
