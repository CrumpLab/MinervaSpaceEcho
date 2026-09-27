# Releasing

## Making a release

1. Update the version in `CMakeLists.txt` (`project(... VERSION x.y.z)`) and
   add a `## [x.y.z] - date` section to `CHANGELOG.md`.
2. Merge to the main branch, then tag and push:
   ```sh
   git tag vx.y.z
   git push origin vx.y.z
   ```
3. CI builds and validates everything. On a `v*` tag the `release` job then
   creates a GitHub release with that version's changelog section, attaching
   `MinervaSpaceEcho-x.y.z-macOS.pkg` (installer) and
   `MinervaSpaceEcho-x.y.z-macOS.zip` (the bundles, for manual installs).
   0.x versions are marked as pre-releases.

## Signing and notarization (optional)

Without the secrets below, builds are ad-hoc signed. They work on the machine
you install them on, but macOS warns about them after download (see the
README for the workaround). To ship builds that open without warnings you
need an Apple Developer account and two certificates: *Developer ID
Application* (for the plug-ins) and *Developer ID Installer* (for the .pkg).

Add these repository secrets (Settings → Secrets and variables → Actions):

| Secret | Value |
|---|---|
| `MACOS_CERTS_P12` | Both certificates with their private keys, exported from Keychain Access as one .p12, then base64-encoded (`base64 -i certs.p12 \| pbcopy`) |
| `MACOS_CERTS_PASSWORD` | The .p12's export password |
| `MACOS_SIGN_APP` | e.g. `Developer ID Application: Your Name (TEAMID)` |
| `MACOS_SIGN_INSTALLER` | e.g. `Developer ID Installer: Your Name (TEAMID)` |
| `APPLE_ID` | Your Apple ID email |
| `APPLE_TEAM_ID` | Your 10-character team ID |
| `APPLE_APP_PASSWORD` | An app-specific password for `notarytool` (appleid.apple.com → Sign-In and Security) |

With them, `scripts/package_macos.sh` signs the bundles with the hardened
runtime, notarizes and staples them, signs the installer, and notarizes and
staples the installer too. You can run the same script locally with the same
environment variables set.
