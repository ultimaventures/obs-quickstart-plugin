#include "monitoring-ladder-state.hpp"

#include <filesystem>

#include <obs-module.h>
#include <obs.h>
#include <util/config-file.h>

namespace fs = std::filesystem;

namespace obs_setup {
namespace monitoring {
namespace {

std::string ladderStatePath() {
  char *raw = obs_module_config_path("stability-ladder.ini");
  if (!raw)
    return "";
  std::string path(raw);
  bfree(raw);
  return path;
}

bool openState(config_t **out, bool create) {
  const std::string path = ladderStatePath();
  if (path.empty())
    return false;
  if (create) {
    // config_open creates the FILE, not its parent directories, and
    // the module's plugin_config directory does not exist until
    // something creates it — on a fresh install this write is the
    // something. (Rig-caught 2026-11-08: without this, the first
    // ladder record silently failed to land.)
    std::error_code ec;
    fs::create_directories(fs::u8path(path).parent_path(), ec);
  }
  if (config_open(out, path.c_str(),
                  create ? CONFIG_OPEN_ALWAYS : CONFIG_OPEN_EXISTING) !=
          CONFIG_SUCCESS ||
      !*out)
    return false;
  return true;
}

bool saveState(config_t *config) {
  return config_save_safe(config, "tmp", nullptr) == CONFIG_SUCCESS;
}

} // namespace

bool savePendingRetest(const PendingRetest &pending) {
  config_t *config = nullptr;
  if (!openState(&config, true)) {
    blog(LOG_ERROR, "[Monitoring] Could not open the ladder state file");
    return false;
  }
  config_set_string(config, "Pending", "Profile", pending.profileName.c_str());
  config_set_string(config, "Pending", "Stage", pending.stage.c_str());
  config_set_string(config, "Pending", "FromQuality",
                    pending.fromQuality.c_str());
  config_set_string(config, "Pending", "ToQuality", pending.toQuality.c_str());
  config_set_bool(config, "Pending", "RemindSuppressed",
                  pending.remindSuppressed);
  const bool ok = saveState(config);
  if (!ok)
    blog(LOG_ERROR, "[Monitoring] Could not save the ladder state file");
  config_close(config);
  return ok;
}

bool loadPendingRetest(PendingRetest &out) {
  config_t *config = nullptr;
  if (!openState(&config, false))
    return false;
  const char *profile = config_get_string(config, "Pending", "Profile");
  const bool found = profile && *profile;
  if (found) {
    out.profileName = profile;
    const char *stage = config_get_string(config, "Pending", "Stage");
    out.stage = stage ? stage : "";
    const char *from = config_get_string(config, "Pending", "FromQuality");
    out.fromQuality = from ? from : "";
    const char *to = config_get_string(config, "Pending", "ToQuality");
    out.toQuality = to ? to : "";
    out.remindSuppressed =
        config_get_bool(config, "Pending", "RemindSuppressed");
  }
  config_close(config);
  return found;
}

void clearPendingRetest() {
  config_t *config = nullptr;
  if (!openState(&config, true))
    return;
  // No section-removal call in the config API: remove every key the
  // record owns. loadPendingRetest keys off Profile, so an emptied
  // section reads as "no record".
  config_remove_value(config, "Pending", "Profile");
  config_remove_value(config, "Pending", "Stage");
  config_remove_value(config, "Pending", "FromQuality");
  config_remove_value(config, "Pending", "ToQuality");
  config_remove_value(config, "Pending", "RemindSuppressed");
  if (!saveState(config))
    blog(LOG_ERROR, "[Monitoring] Could not clear the ladder state file");
  config_close(config);
}

void suppressPendingReminder() {
  PendingRetest pending;
  if (!loadPendingRetest(pending))
    return;
  pending.remindSuppressed = true;
  savePendingRetest(pending);
}

} // namespace monitoring
} // namespace obs_setup
