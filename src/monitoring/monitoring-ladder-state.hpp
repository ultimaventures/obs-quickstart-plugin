#ifndef OBS_SETUP_MONITORING_MONITORING_LADDER_STATE_HPP
#define OBS_SETUP_MONITORING_MONITORING_LADDER_STATE_HPP

#include <string>

namespace obs_setup {
namespace monitoring {

/**
 * @brief A stability-ladder step that was applied and is waiting for
 * its verification retest after an OBS restart.
 *
 * The ladder (architecture §7): when the live streaming-preset steps
 * are exhausted, the next fixes — recording quality one rung down,
 * then the full rescue — are read by OBS only when it rebuilds its
 * outputs, so they take effect across a restart. This record is how
 * the plugin remembers, across that restart, WHERE the ladder was
 * left: which profile it belongs to, what was applied, and whether
 * the user has silenced the launch reminder.
 *
 * The record and the reminder are different things (user ruling,
 * 2026-11-08): "don't ask again" sets remindSuppressed and silences
 * the launch prompt, but the record itself is KEPT — an unfinished
 * ladder means setup was exited partway, and the position must stay
 * on the books so re-entering the check can offer to pick up where
 * it left off. The record is cleared only when a retest actually
 * resolves the ladder (Stable/Marginal) or a newer ladder action
 * replaces it.
 *
 * Stored in the plugin's own config directory
 * (stability-ladder.ini), NOT in any profile: it describes the
 * plugin's progress, not the user's settings.
 */
struct PendingRetest {
  /** Profile the ladder action belongs to (frontend profile name).
   *  A pending record never fires on a different profile. */
  std::string profileName;
  /** What was applied: "quality" (one recording-quality rung) or
   *  "fallback" (the full rescue). */
  std::string stage;
  /** Recording quality before/after the step (stage "quality"). */
  std::string fromQuality;
  std::string toQuality;
  /** The launch reminder was silenced; the record remains. */
  bool remindSuppressed = false;
};

/** Saves (or replaces) the pending-retest record. */
bool savePendingRetest(const PendingRetest &pending);

/** Loads the pending record, if one exists. */
bool loadPendingRetest(PendingRetest &out);

/** Deletes the pending record (the ladder resolved). */
void clearPendingRetest();

/** Sets remindSuppressed on the existing record, keeping everything
 *  else. No-op when no record exists. */
void suppressPendingReminder();

} // namespace monitoring
} // namespace obs_setup

#endif // OBS_SETUP_MONITORING_MONITORING_LADDER_STATE_HPP
