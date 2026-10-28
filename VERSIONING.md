# Versioning

M5tabOS follows [SemVer](https://semver.org/): `MAJOR.MINOR.PATCH`.
The version number lives in `version.txt` and is updated exclusively through
`tools/release.sh`.

## Which number to bump

While the version starts with **0.**, the system is still taking shape. Use
these rules:

| What the release contains | Bump | Example |
|---|---|---|
| Bug fixes only, with no user-visible new features | PATCH (`patch`) | 0.7.0 → 0.7.1 |
| Anything new: an app, screen, setting, or service | MINOR (`minor`) | 0.7.1 → 0.8.0 |
| Anything that **requires reflashing with `./flash.sh`** (partition table changes, NVS layout changes, or SD card file formats that older versions cannot read) | MINOR (`minor`), with a **bold** warning in the CHANGELOG | 0.8.0 → 0.9.0 |
| The system is considered stable and the app API (`kernel/app.h`) becomes a long-term compatibility commitment | MAJOR: **1.0.0** | 0.x → 1.0.0 |

After 1.0.0, any change that breaks existing apps or requires a full reflash
bumps the MAJOR version.

When in doubt between two increments, pick the larger one. Incrementing a
version number costs nothing; under-incrementing hides a breaking change.

## Day-to-day workflow

1. Every user-noticeable change gets a bullet under the **Unreleased** section
   of `CHANGELOG.md` in the same commit as the change itself.
2. When enough changes have accumulated for a release, run:
   ```bash
   tools/release.sh minor
   ```
   The script shows the old and new version numbers and asks for confirmation.
   It then updates `version.txt`, turns the "Unreleased" heading into the new
   version section stamped with today's date, and creates both the
   `Versao X.Y.Z` commit and the `vX.Y.Z` git tag.
3. Push the release: `git push origin main vX.Y.Z`.

## Safeguards keeping versions monotonic

- `release.sh` refuses to create a version that isn't strictly greater than the
  latest tag, refuses to run with uncommitted changes, and refuses to release if
  the "Unreleased" section is empty.
- The build (`CMakeLists.txt`) **fails with an error** if `version.txt` falls
  behind the latest git tag.
- The version string embedded in the binary tells you exactly what it was built
  from:
  - `0.8.0`: an exact tagged release;
  - `0.8.0+12.a1b2c3d`: 12 commits ahead of `0.8.0`, at commit `a1b2c3d`.

  This full string is displayed in Settings, in the serial console (`estado`),
  and in the boot log, ensuring a development build can never be mistaken for an
  official release. Uncommitted working-tree changes are not reflected in the
  string: `+N.hash` strictly means "built from commit".
