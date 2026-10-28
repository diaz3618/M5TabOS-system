#!/bin/bash
# Releases a new version of M5tabOS. The rules for which number to bump are
# in VERSIONING.md.
#
#   tools/release.sh patch     0.7.0 -> 0.7.1  (fixes only)
#   tools/release.sh minor     0.7.0 -> 0.8.0  (something new, or that requires reflashing)
#   tools/release.sh major     0.7.0 -> 1.0.0
#   tools/release.sh 0.9.0     a chosen number (must be higher)
#
# What it does, in this order, and only after you confirm:
#   1. version.txt gets the new number;
#   2. in CHANGELOG.md, whatever is under "Unreleased" becomes that version's
#      section, dated today, and an empty "Unreleased" goes back on top;
#   3. commit "Version X.Y.Z" and annotated tag vX.Y.Z.
# Does not push anything: that's on you (the command is printed at the end).
#
# Refuses when: there are uncommitted changes, the number is not higher than
# the last released version, or the CHANGELOG's "Unreleased" section is
# empty - a version with nothing written about it is a version nobody knows
# the content of.
set -euo pipefail
cd "$(dirname "$0")/.."

fail() { echo "release: $*" >&2; exit 1; }

[ $# -eq 1 ] || { sed -n '2,8p' "$0" | sed 's/^# \{0,1\}//'; exit 1; }

git diff --quiet && git diff --cached --quiet || fail "there are uncommitted changes - commit or stash before releasing."

current=$(tr -d ' \r\n' < version.txt)
[[ "$current" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || fail "version.txt does not have X.Y.Z: '$current'"
latest=$(git describe --tags --abbrev=0 --match 'v[0-9]*' 2>/dev/null | sed 's/^v//' || true)

# Base for the math: the higher of version.txt and the last tag.
base="$current"
if [ -n "$latest" ] && [ "$(printf '%s\n%s\n' "$latest" "$current" | sort -V | tail -1)" = "$latest" ]; then
  base="$latest"
fi
IFS=. read -r MA MI PA <<< "$base"

case "$1" in
  patch) new="$MA.$MI.$((PA + 1))" ;;
  minor) new="$MA.$((MI + 1)).0" ;;
  major) new="$((MA + 1)).0.0" ;;
  *)     new="$1" ;;
esac
[[ "$new" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || fail "invalid version: '$new' (use patch, minor, major, or X.Y.Z)"

if [ -n "$latest" ]; then
  [ "$new" != "$latest" ] && [ "$(printf '%s\n%s\n' "$latest" "$new" | sort -V | tail -1)" = "$new" ] \
    || fail "$new is not higher than the last released version ($latest)."
fi
git rev-parse -q --verify "refs/tags/v$new" >/dev/null && fail "tag v$new already exists."

# "Unreleased" needs at least one line of content before the next section.
content=$(awk '/^## \[Unreleased\]/{inside=1; next} /^## \[/{inside=0} inside && NF' CHANGELOG.md | wc -l | tr -d ' ')
[ "$content" -gt 0 ] || fail "CHANGELOG.md's \"Unreleased\" section is empty - write what this version brings."

branch=$(git rev-parse --abbrev-ref HEAD)
commits=$(git rev-list --count "${latest:+v$latest..}HEAD")
echo "M5tabOS: ${latest:-none} -> $new"
echo "  branch $branch, $commits commits since the last version, $content lines in CHANGELOG"
[ "$branch" = "main" ] || echo "  WARNING: releasing outside of main."
read -r -p "Confirm? [y/N] " ok
[[ "$ok" =~ ^[yY]$ ]] || fail "cancelled."

today=$(date +%Y-%m-%d)
printf '%s\n' "$new" > version.txt
awk -v v="$new" -v d="$today" '
  !done && /^## \[Unreleased\]/ { print; print ""; print "## [" v "] - " d; done=1; next }
  { print }' CHANGELOG.md > CHANGELOG.md.new
mv CHANGELOG.md.new CHANGELOG.md

git add version.txt CHANGELOG.md
git commit -q -m "Version $new"
git tag -a "v$new" -m "M5tabOS $new"

echo "Done: commit and tag v$new created. To publish:"
echo "  git push origin $branch v$new"
