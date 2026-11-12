# Releasing Elsewhere

For whoever cuts official releases. Contributors don't need any of this: builds from source are signed locally
and run fine on your own Mac.

## Every release

1. Bump `VERSION` and commit.
2. Tag the commit, `v<version>` (for example `v0.17.0`), and push the tag. About Elsewhere links to
   `ELSEWHERE_SOURCE_URL/tree/v<version>`, which is how each build points to its exact source code, as the AGPL
   asks. A build without a pushed tag has a dead link.
3. Build the Mac disk image with `scripts/make-dmg.sh` (below). Windows installers come from the Windows
   workflow (`.github/workflows/windows.yml`).

## Mac signing and notarization

Signing is configured in `CMakeLists.txt`, not in Xcode's Signing tab (the Xcode project is regenerated). It uses:

- Bundle ID `com.fuzzllc.elsewhere` (`ELSEWHERE_BUNDLE_ID`).
- The Fuzz LLC team, `8PZ8LLTEJD`, for Xcode builds. `scripts/xcode.sh` reads it from the environment, so put
  this in your shell profile:
  ```bash
  export ELSEWHERE_DEVELOPMENT_TEAM=8PZ8LLTEJD
  ```
  Without it, Xcode builds are signed locally.
- Hardened Runtime, with audio input and third-party plug-in loading allowed.

To give the app to other people without macOS blocking it, do this once:

1. **Developer ID certificate:** in Xcode, go to Settings → Accounts, select the team, then Manage Certificates…
   → **+** → **Developer ID Application**.
2. **App-specific password:** create one at [account.apple.com](https://account.apple.com) (Sign-In and
   Security → App-Specific Passwords).
3. **Store notarization credentials** in your keychain (it prompts for the app-specific password):
   ```bash
   xcrun notarytool store-credentials elsewhere-notary --apple-id YOUR_APPLE_ID_EMAIL --team-id 8PZ8LLTEJD
   ```

After that, `scripts/make-dmg.sh` signs with Developer ID, notarizes and staples automatically, and writes
`dist/Elsewhere-<version>.dmg`.

## Windows installer

The Windows workflow builds `Elsewhere.exe` and packages it with Inno Setup (`scripts/windows/Elsewhere.iss`).
To build one locally:

```bash
iscc /DAppVersion=<version> /DSourceDir=<folder with Elsewhere.exe> scripts\windows\Elsewhere.iss
```

The installer's `AppId` identifies Elsewhere to Windows. Keep it the same from release to release, or
Windows treats each version as a different app.
