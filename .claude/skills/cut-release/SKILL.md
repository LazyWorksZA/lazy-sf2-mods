---
name: cut-release
description: Cut a Lazy SF2 Patch release - verify the release branch, live-test the MSI install, upgrade and uninstall, date the changelog, merge and tag only with the owner's OK, then watch the Release workflow and clean up. Use when preparing or publishing a vX.Y.Z release.
---

# Cut a release

Never merge to `master`, tag or push without the owner's explicit OK for that step (AGENTS.md). Do not open pull requests.

## Checklist

1. **Release branch.** Branch `release-X.Y` off `master`; merge every feature branch into it. Each branch was reviewed first (`review-task`). `git status` is clean, no game files staged.
2. **CHANGELOG.md.** Rename `## Unreleased` to `## X.Y.Z (YYYY-MM-DD)` (the workflow extracts the release notes by that heading), and add a fresh empty `## Unreleased` above it. Check every user-visible change is listed.
3. **Build and tests.** `.\installer\build-installer.ps1 -Version X.Y.Z -Clean` with no warnings, then `build\Release\tools\lazysf2-net-tests.exe` and `dotnet test installer\LazyWorks.SF2Patch.Manager.Tests`.
4. **Live install test** (follow `live-test` rules; the owner must be present, because the MSI is per machine and needs UAC):
   - Install the previous published MSI (`gh release download vPREV --pattern '*.msi'`), start the game once, quit.
   - Install the new MSI over it (upgrade). Check `lazysf2.log` shows the new build with its hooks installed, and `lazysf2.manifest.txt` is right.
   - Uninstall from Windows Apps. The game folder must be back to stock: hash-compare against the game backup. Only files in the manifest may be gone.
5. **Owner OK**, then merge into `master`, tag `vX.Y.Z` (must match `^\d+\.\d+\.\d+$`) and push both:
   ```powershell
   git switch master; git merge --no-ff release-X.Y; git tag vX.Y.Z; git push origin master vX.Y.Z
   ```
6. **Watch the Release workflow** (it builds from scratch, attests, creates the release):
   ```powershell
   gh run list --workflow Release --limit 1
   gh run watch <run-id> --exit-status
   ```
7. **Verify the release.** `gh release view vX.Y.Z` must list `Lazy-SF2-Patch-X.Y.Z.msi`, `Lazy-SF2-Patch-X.Y.Z-Setup.exe`, `winmm.dll` and `SHA256SUMS.txt`, with the changelog notes. Spot-check one file with `gh attestation verify <file> --repo <owner>/<repo>`.
8. **Clean up.** Delete merged branches (local and remote) and their worktrees (`git worktree remove`, `git branch -d`). Merge the matching `lazy-sf2-recon` branches there (same flow, owner's OK). Stop any agents whose work landed.

## Report

Version, tag, workflow run result, asset list, what was tested live (and what was not), branches removed.
