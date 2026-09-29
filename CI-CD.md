# CI/CD Pipeline

## Overview

| Role                 | Platform         | Repository                                                    |
|----------------------|------------------|---------------------------------------------------------------|
| Source of truth      | GitLab           | `gitlab.com/ultimaventures-playground/obs-quickstart-plugin`  |
| Build/distribution mirror | GitHub (public) | `github.com/ultimaventures/obs-quickstart-plugin`        |

All development happens on GitLab. GitHub exists only because the OBS plugin
template's build system is GitHub Actions workflows — the mirror lets us use
them unmodified. The GitHub repo is public, so Actions minutes are unlimited
and free.

Decisions (2026-09-28):

- Releases are published from **GitHub Releases**, where OBS users expect to
  find plugin downloads. The old "download artifacts and re-upload to GitLab"
  round-trip is dropped.
- The GitHub repo is a read-only build/distribution mirror: its description
  points at GitLab as the real repo, and Issues are disabled so contributions
  don't land in the wrong place.

## How a change flows

1. Work on a feature branch, push to GitLab.
2. GitLab CI runs fast checks (formatting, etc.).
3. The GitLab **push mirror** syncs the branch to GitHub automatically.
4. GitHub Actions runs the build matrix (Windows / macOS / Linux) plus format
   checks.
5. Open a merge request on GitLab; merge after review.
6. Release: push a tag → Actions builds all platforms → artifacts attached to a
   GitHub Release. No round-trip back to GitLab.

## Mirror configuration

> To be verified via the GitLab API once agent access is configured
> (2026-09-28).

- Expected: repository push mirror, GitLab → GitHub, automatic on every push.
- Watch-out: the mirror authenticates with a stored credential. If it expires,
  builds silently stop — nothing fails loudly on the GitLab side. If a push to
  GitLab doesn't appear on GitHub within a few minutes, check
  Settings → Repository → Mirroring on GitLab first.

## For the build agent

- GitLab access: **personal access token** (project access tokens require
  Premium/Ultimate on gitlab.com SaaS — verified 2026-09-28). Create a
  **fine-grained** token at User Settings → Access Tokens: name it
  `muse-agent-OBS-quickstart`, restrict Group and project access to the
  obs-quickstart-plugin project only, expiry up to 365 days. Stored in the
  Secure Vault; never in plaintext (no `glab` config files — `glab` stores
  tokens in plaintext by default).
- Publishing flow (verified 2026-09-29): `git push` over HTTPS cannot
  authenticate from this environment — `git-receive-pack` only accepts HTTP
  Basic auth, and the secure credential can only be exchanged in the
  `PRIVATE-TOKEN` header. Publish branch content via the GitLab API instead:
  `POST /projects/:id/repository/branches` to create the branch, then
  `POST /projects/:id/repository/commits` with the file actions
  (see `~/workspace/obs-plugin-review/publish_worktree.py`). MRs, pipelines,
  and approvals all go through the REST API.
- Granular permissions (2026-09-28; User permission deliberately omitted):
  - Repository → Code: Download, Push, Read
  - Repository → Branch: Create, Read
  - Repository → Merge Request: Create, Read, Update
  - Group and project → Project → Project: Read
  - CI/CD → Pipeline: Read
  - No Repository Tag permission: tags pushed through the mirror trigger
    public GitHub Release builds, so tag creation stays a human action.
- Pushing a branch to GitLab triggers the mirror → GitHub Actions build
  automatically. No separate push to GitHub needed.
- Build status is publicly readable on the GitHub repo (no auth needed).
- When the token expires, pushes/MRs will start failing with 401s — that's
  the signal to rotate it (create a new PAT, replace the vault entry).

## Security hardening (review 2026-09-29, corrected 2026-09-29)

- **Mirror scope:** the GitLab push mirror syncs branches to GitHub, where any
  pushed branch's `.github/workflows` will run. Mirror **all branches**, not
  protected-only: pre-merge build gating only works if GitHub actually builds
  the feature branch. The earlier "protected branches only" advice is withdrawn
  — it contradicts the gating below (a protected-only mirror means GitHub never
  builds MR source branches, so there is nothing to gate on). Security instead
  comes from: only project members can push branches; `main` and tags are
  protected on GitLab; code-signing secrets (if added later) are gated behind a
  GitHub Environment with required reviewers, so a mirrored branch alone can't
  spend them.
- **Build gating (Free tier):** GitLab cannot see GitHub check status, so MRs
  merge ungated. Fix: have the GitHub Actions workflow post a commit status
  back to GitLab (`POST /projects/:id/statuses/:sha`) at the end of each run —
  post even on failure (gate that step with `if: always()`). The posted status
  creates or joins an `external` pipeline on the commit; enable **"Pipelines
  must succeed"** in `main`'s merge-request settings (available on the Free
  tier) so an MR can't merge until the external build passes. Named per-status
  required checks ("status X must succeed") are a paid-tier feature
  (Premium/Ultimate) — not available here.
  - **Race (review 2026-09-29):** mirror push → workflow start → first status
    leaves a window where the MR looks green with no external pipeline. Fix:
    the first GitLab CI job on the branch posts a `pending` commit status for
    the build context before anything else, so the external pipeline is never
    empty — the MR shows "running", not "green", until GitHub reports back.
    The GitLab `pending` post and the GitHub result **must use the identical
    status `name` (and `ref`)**, otherwise the pending status never clears.
  - **Open question:** whether "Pipelines must succeed" reads the MR's head
    (GitLab) pipeline or the external one. Must be tested live on a real MR
    before relying on the gate. **Proof plan (review 2026-09-29):** open a
    throwaway MR carrying a deliberately failing build — if it can merge
    anyway, the gate doesn't work.
  - **Trust boundary:** any pusher can edit the workflow and post a forged
    `success` status with the secret. Treat this as a *build* gate only — it
    proves the code compiled, not that it's safe. Keep required maintainer
    approvals on MRs to `main` as the actual code-review gate.
  - **Least-privilege status credential (review 2026-09-29):** posting commit
    statuses needs an `api`-scope token, and any pusher can write a workflow
    that reads whatever secret the workflow holds. Use the weakest credential
    that can post statuses — a fine-grained PAT limited to this project (or a
    project token with the Developer role where available; project tokens need
    Premium/Ultimate on gitlab.com SaaS) — never a Maintainer/Owner token.
    Code-signing secrets, if added later, live in a `main`-only GitHub
    Environment with required reviewers, so a mirrored branch alone can't
    spend them.
  This mechanism is designed but **not yet tested end-to-end**: verify on a
  real MR before relying on it. (The mirror itself is also still unverified —
  as of 2026-09-29 the GitHub mirror only shows `main` and
  `sprint-0-template-updates`; `sprint1/profile-module` has not appeared —
  see Mirror configuration above.)

## Troubleshooting

| Symptom                              | Check first                                                        |
|--------------------------------------|--------------------------------------------------------------------|
| Push to GitLab, no Actions run       | GitLab → Settings → Repository → Mirroring: mirror status / errors |
| Actions build fails on one platform  | Failed job log on GitHub; template workflows in `.github/workflows/` |
| Release tag produced no artifacts    | Confirm the tag reached the GitHub mirror; release workflow triggers on tags |
| Format check fails in GitLab CI      | Run the template's clang-format-19 check locally before pushing    |
