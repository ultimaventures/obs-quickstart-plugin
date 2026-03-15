#include "ui-module.hpp"
#include <QMainWindow>
#include <QMessageBox>
#include <obs-frontend-api.h>

namespace obs_setup {
namespace ui {

void show_hello_world_dialog() {
  // Get the main OBS window to parent the dialog correctly
  QMainWindow *main_window = (QMainWindow *)obs_frontend_get_main_window();
  QMessageBox::information(main_window, "OBS Setup Test", "Hello World");
}

} // namespace ui
} // namespace obs_setup
