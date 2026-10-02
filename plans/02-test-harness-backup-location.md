# Plan 02: Test harness + backup location move

Decisions recorded 2026-09-30 (reviewer round 4 + user). Two work items,
both Sprint 1.

---

## A. Test harness — first task of the next sprint

**Why:** `docs/testing.md` already mandates Google Test, >80% coverage on
core modules, and CI enforcement, but the repo has zero test infrastructure.
The profile module is the highest-risk code (backup/create/delete/rollback
of the user's real OBS profiles). The reviewer accepted deferring the
harness once; it is now the first task of the next sprint.

**Scope — minimal, not the full testing.md vision:**
- Framework: Google Test via `FetchContent` (matches `docs/testing.md`).
- One test target: `profile_tests`, compiled from
  `src/profile/profile-module.cpp` plus a `blog` stub
  (`void blog(int, const char *, ...) {}`), linked against the same Qt and
  `obs-frontend-api` the plugin already uses. No source refactor needed.
- Test cases (port from `tests/manual/filesystem-repro.cpp`, which is
  committed as the seed and verified passing standalone):
  - `copyDirectoryTree`: missing destination parent (reviewer round 3);
    non-ASCII UTF-8 directory name (reviewer round 4); nested content;
    overwrite behavior.
  - `deduplicatedName`: free name unchanged; taken → `Name 2`;
    exhaustion → empty string.
  - `outputConfigChanged`: identical snapshots → false; each field
    differing → true (construct snapshots directly, no OBS calls).
- Wire into CTest (`enable_testing()`, `add_test`) and run `ctest` in the
  GitHub Actions workflow after the build step.

**Explicit non-goals:** the OBS-touching paths (profile creation,
wizard trigger via private slot) cannot be unit-tested without a live OBS;
they stay in the manual completed/cancelled/missing-slot matrix and the
`docs/testing.md` integration plan (headless OBS via Xvfb). Do not oversell
the harness.

**Do not build blind:** the harness must be implemented where it can be
compiled and run (CI green), not from an environment without Qt/OBS deps.

**Acceptance:** `ctest` green in CI; reviewer items (copy test, non-ASCII
test, blog-stub note) closed.

---

## B. Backup location move — `obs_module_config_path`

**Decision:** move backups from `<profiles>/quickstart-backups/` to the
plugin config dir via `obs_module_config_path("quickstart-backups")`.

**Why this API** (verified against OBS 31.1.1 `libobs/obs-module.h` and
`obs-module.c`):
- It is the canonical per-plugin location:
  `<obs_config>/plugin_config/<module>/quickstart-backups/`.
- It is built from OBS's own config base, so it respects **portable mode**
  (portable installs keep everything next to the binary).
  `QStandardPaths::AppDataLocation` would write to the real user AppData
  even in portable mode — orphaned backups.
- No handle plumbing: the macro uses `obs_current_module()` internally.
- Rejected: keeping the profiles dir (OBS enumerates it; a
  `quickstart-backups/` folder risks appearing as a phantom profile).

**Decisions:**
- **No retention / no auto-deletion** (user, 2026-09-30): profiles are
  kilobytes; deleting backups buys nothing and adds irreversible
  destructive logic to a safety feature. Keep every backup. If hygiene ever
  matters, it becomes an explicit user-facing "clear old backups" action.
- Keep the `u8path`/`u8string()` handling (already in the code): OBS paths
  are UTF-8; `path(string)` on Windows decodes with the ANSI code page.

**Implementation sketch** (in `backupExistingProfile`):
```cpp
char *rawBase = obs_module_config_path("quickstart-backups");
if (!rawBase) {
  blog(LOG_ERROR, "[Profile] Could not resolve plugin config dir; backup refused");
  return false;
}
const std::string base = rawBase;  // UTF-8 bytes
bfree(rawBase);
const std::filesystem::path dest =
    std::filesystem::u8path(base) / (profileName + "_" + stamp);
// ... rest unchanged (copyDirectoryTree already creates parents, already u8)
```
- `m_lastBackupPath` semantics unchanged (stores UTF-8 bytes); the
  plugin-main test menu item that logs it keeps working.
- If `obs_module_config_path` returns NULL (config dir unset), fail the
  backup loudly — never silently fall back to the profiles dir.

**Acceptance:** backup lands under `plugin_config/obs-quickstart-plugin/`;
first-run (missing parents) and non-ASCII paths covered by the new tests.

---

## Open decisions for the user (not this plan's scope)

- **Mirror policy:** protected-only mirroring (current belief) contradicts
  the CI-CD.md gating design (feature-branch MRs never get GitHub
  statuses). Pick one deliberately and update CI-CD.md: mirror all
  branches (gate works as designed) or keep protected-only (simpler/safer;
  rely on post-merge main builds + manual verification). Before either,
  check Settings → Repository → Mirroring for the mirror's last-update
  status and error text — a repeatedly-failed mirror stalls regardless of
  settings, and there is no GitLab setting tying mirroring to release tags.
