#!/usr/bin/env bash
# Submit (or update) sys-antinag in the Homebrew App Store by opening a pull request against
# https://github.com/fortheusers/switch-hbas-repo
#
# What it does
#   1. checks gh/git/jq, that you are logged in to GitHub, and that the release zip from pkgbuild.json exists
#   2. forks switch-hbas-repo to your account (re-uses an existing fork)
#   3. clones your fork into a temp dir, branches from the current upstream default branch
#   4. copies hbas/sys-antinag/ to packages/sys-antinag/ (pkgbuild.json, icon.png, screen.png)
#   5. commits, pushes the branch to your fork and opens the pull request
#
# Nothing is pushed or created before you confirm. Use --dry-run to do steps 3-4 locally only
# (read-only clone of upstream, no fork, no push, no pull request).
#
# Usage:  tools/submit-to-store.sh [--dry-run] [--yes]
set -euo pipefail

UPSTREAM="fortheusers/switch-hbas-repo"
PKG="sys-antinag"
DRY=0; YES=0
for a in "$@"; do
  case "$a" in
    --dry-run) DRY=1 ;;
    --yes|-y)  YES=1 ;;
    -h|--help) sed -n '2,19p' "$0"; exit 0 ;;
    *) echo "unknown option: $a" >&2; exit 2 ;;
  esac
done

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
src="$here/../hbas/$PKG"
say()  { printf '\n\033[1m==> %s\033[0m\n' "$*"; }
die()  { printf '\033[31merror:\033[0m %s\n' "$*" >&2; exit 1; }
ask()  { [ "$YES" = 1 ] && return 0; read -r -p "$1 [y/N] " r; [[ "$r" =~ ^[Yy]$ ]]; }

for t in git jq curl; do command -v "$t" >/dev/null || die "$t is not installed"; done
if [ "$DRY" = 0 ]; then
  command -v gh >/dev/null || die "gh (GitHub CLI) is not installed: https://cli.github.com/"
  gh auth status >/dev/null 2>&1 || die "not logged in to GitHub: run 'gh auth login' first"
fi

say "Checking the package files"
for f in pkgbuild.json icon.png screen.png; do [ -s "$src/$f" ] || die "missing $src/$f"; done
jq -e . "$src/pkgbuild.json" >/dev/null || die "pkgbuild.json is not valid JSON"
version="$(jq -r '.info.version' "$src/pkgbuild.json")"
title="$(jq -r '.info.title' "$src/pkgbuild.json")"
zip_url="$(jq -r '.assets[] | select(.type=="zip") | .url' "$src/pkgbuild.json")"
[ "$(jq -r '.package' "$src/pkgbuild.json")" = "$PKG" ] || die "pkgbuild.json 'package' is not '$PKG'"
echo "  $title  version $version"
echo "  zip: $zip_url"
curl -fsIL "$zip_url" >/dev/null || die "the release zip is not reachable - publish the GitHub release (tag v$version) first"
echo "  release zip is reachable"

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
repo="$work/repo"

if [ "$DRY" = 1 ]; then
  say "Dry run: cloning upstream read-only (no fork, no push, no pull request)"
  git clone -q --depth 1 "https://github.com/$UPSTREAM.git" "$repo"
  default="$(git -C "$repo" rev-parse --abbrev-ref HEAD)"
  me="you"
else
  me="$(gh api user --jq .login)"
  say "Forking $UPSTREAM to $me (re-uses an existing fork)"
  ask "Create/use the fork $me/$(basename "$UPSTREAM") now?" || die "cancelled"
  gh repo fork "$UPSTREAM" --clone=false >/dev/null 2>&1 || true
  # the fork can take a moment to appear
  for i in 1 2 3 4 5 6; do gh repo view "$me/$(basename "$UPSTREAM")" >/dev/null 2>&1 && break; sleep 5; done
  gh repo view "$me/$(basename "$UPSTREAM")" >/dev/null 2>&1 || die "fork not found: $me/$(basename "$UPSTREAM")"

  say "Cloning your fork and syncing with upstream"
  git clone -q --depth 1 "https://github.com/$me/$(basename "$UPSTREAM").git" "$repo"
  default="$(gh repo view "$UPSTREAM" --json defaultBranchRef --jq .defaultBranchRef.name)"
  git -C "$repo" remote add upstream "https://github.com/$UPSTREAM.git"
  git -C "$repo" fetch -q --depth 1 upstream "$default"
fi

if [ -d "$repo/packages/$PKG" ]; then action="Update"; else action="Add"; fi
branch="$PKG-$version"
say "$action packages/$PKG on branch $branch"
if [ "$DRY" = 1 ]; then
  git -C "$repo" switch -q -c "$branch"
else
  git -C "$repo" switch -q -c "$branch" "upstream/$default"
fi
mkdir -p "$repo/packages/$PKG"
cp "$src/pkgbuild.json" "$src/icon.png" "$src/screen.png" "$repo/packages/$PKG/"
git -C "$repo" add "packages/$PKG"
if git -C "$repo" diff --cached --quiet; then die "nothing to submit: upstream already has exactly these files"; fi
git -C "$repo" -c user.name="$(git config user.name || echo "$me")" \
               -c user.email="$(git config user.email || echo "$me@users.noreply.github.com")" \
               commit -q -m "$action $title ($PKG) $version"
git -C "$repo" show --stat --format='%s' HEAD | sed 's/^/  /'

if [ "$DRY" = 1 ]; then
  say "Dry run finished: the commit above was made in a temp clone and is discarded. Nothing was sent."
  exit 0
fi

say "Pushing branch $branch to $me/$(basename "$UPSTREAM") and opening the pull request"
ask "Push and open the pull request against $UPSTREAM?" || die "cancelled (nothing was pushed)"
git -C "$repo" push -q -u origin "$branch"

body="$(cat <<EOF
$action **$title** (\`$PKG\`) $version.

An Atmosphere sysmodule plus an overlay that remove the "Please turn flight mode off" popup games show while the console is in flight mode. Only that popup is blocked; the patch lives in memory only.

- Source (GPL-2.0-only): https://github.com/SciLor/sys-antinag
- Release: https://github.com/SciLor/sys-antinag/releases/tag/v$version
- Package built locally with spinarak: the zip installs the sysmodule folder (\`atmosphere/contents/420000BADBEE0000/\`) and \`switch/.overlays/sys-antinag.ovl\`.
- I am the author and approve listing it here. Tested on real hardware (firmware 23.0.1, Atmosphere 1.12.1).
EOF
)"
gh pr create --repo "$UPSTREAM" --head "$me:$branch" --base "$default" \
  --title "$action $title ($PKG) $version" --body "$body"
say "Done. Track the pull request in your browser; reviewers may ask for small changes."
