#!/bin/sh
# Release what is on development: show what would go out, ask, then move
# main up to it and push both. The push to main is what makes GitHub build
# and publish the release (.github/workflows/build.yml).
#   make ship
set -eu
die() { echo "ship: $*" >&2; exit 1; }

[ "$(git rev-parse --abbrev-ref HEAD)" = development ] || die "not on development"
[ -z "$(git status --porcelain)" ] || die "there are uncommitted changes"
git fetch -q --tags origin
git merge-base --is-ancestor origin/development development || die "development is behind origin/development: pull first"
git merge-base --is-ancestor origin/main development || die "main has commits development does not: merge them first"
[ "$(git rev-parse origin/main)" != "$(git rev-parse development)" ] || die "nothing to ship: main is already here"

BASE=$(cat VERSION)
LAST=$(git tag --list "v$BASE.*" | sed "s/^v$BASE\.//" | sort -n | tail -1)
V=$BASE.$(( ${LAST:-0} + 1 ))

echo "Shipping $V: $(git rev-parse --short origin/main) -> $(git rev-parse --short development)"
echo
git log --no-merges --pretty='  %h %s' origin/main..development
echo
git diff --stat origin/main..development | tail -1
if [ -z "$(git diff --name-only origin/main..development | grep -Ev '\.md$|^shots/')" ]; then
	echo "Only notes and pictures changed: GitHub will not build a release for this."
fi
echo
printf 'Push this to main and release it as %s? [y/N] ' "$V"
read -r ANSWER
[ "$ANSWER" = y ] || die "nothing was pushed"

git push origin development development:main
git branch -f main origin/main >/dev/null

if command -v gh >/dev/null; then
	echo "Waiting for GitHub to build it..."
	sleep 10
	RUN=$(gh run list --branch main --workflow build --limit 1 --json databaseId -q '.[0].databaseId')
	gh run watch "$RUN" --exit-status >/dev/null && gh release view "v$V" --json url -q .url
fi
