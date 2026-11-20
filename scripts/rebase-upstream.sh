#!/usr/bin/env bash
# Rebase the wadamesh-remote fork onto the newest upstream main, then prove the
# remote-test hooks survived and the firmware still builds.
#
#   scripts/rebase-upstream.sh            # rebase + verify + build
#   scripts/rebase-upstream.sh --no-build # rebase + verify only
set -euo pipefail

cd "$(git rev-parse --show-toplevel)"

BRANCH="$(git rev-parse --abbrev-ref HEAD)"
if [[ "$BRANCH" == "main" ]]; then
  echo "refusing to rebase main itself (keep main identical to upstream)" >&2
  exit 1
fi

echo "== fetching upstream =="
git fetch upstream --prune

echo "== rebasing $BRANCH onto upstream/main =="
git rebase upstream/main

echo "== checking hooks =="
status=0
check() {
  local file="$1" needle="$2"
  if ! grep -q -- "$needle" "$file"; then
    echo "MISSING: $needle in $file" >&2
    status=1
  else
    echo "ok: $file"
  fi
}
check src/main.cpp "[wadamesh-remote]"
check src/ui-touch/UITask.h "[wadamesh-remote]"
check src/ui-touch/UITask.cpp "[wadamesh-remote]"
check src/MyMesh.cpp "[wadamesh-remote]"
check src/remote_test/RemoteTest.cpp "namespace remote_test"

if [[ "$status" -ne 0 ]]; then
  echo "hook check failed — resolve the rebase before building" >&2
  exit 1
fi

if [[ "${1:-}" == "--no-build" ]]; then
  echo "== hooks ok (build skipped) =="
  exit 0
fi

echo "== building =="
pio run -e LilyGo_TDeck_companion_radio_touch_remote

echo "== done: $(git log --oneline -1) =="
