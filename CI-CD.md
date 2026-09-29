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

## Security hardening (review 2026-09-29)

- **Mirror scope:** the GitLab push mirror syncs branches to GitHub, where any
  pushed branch's `.github/workflows` will run. Configure the mirror to sync
  **protected branches only**, and protect `main` and tags on GitLab — otherwise
  anyone with push access could run workflow code against the repo's secrets.
  Trade-off: feature branches won't get GitHub Actions builds until merged.
- **Secrets:** gate code-signing secrets (if added later) behind a
  [GitHub Environment](https://docs.github.com/en/actions/deployment/targeting-different-environments/using-environments-for-deployment)
  with required reviewers, so a mirrored branch alone can't spend them.
- **Build gating:** GitLab cannot see GitHub check status, so MRs merge
  ungated. Fix: have the GitHub Actions workflow post a commit status back to
  GitLab (`POST /projects/:id/statuses/:sha`) at the end of each run, and mark
  that status required in `main`'s branch protection. Simpler and more robust
  than a GitLab CI job polling GitHub's check-runs API.

## Troubleshooting

| Symptom                              | Check first                                                        |
|--------------------------------------|--------------------------------------------------------------------|
| Push to GitLab, no Actions run       | GitLab → Settings → Repository → Mirroring: mirror status / errors |
| Actions build fails on one platform  | Failed job log on GitHub; template workflows in `.github/workflows/` |
| Release tag produced no artifacts    | Confirm the tag reached the GitHub mirror; release workflow triggers on tags |
| Format check fails in GitLab CI      | Run the template's clang-format-19 check locally before pushing    |
