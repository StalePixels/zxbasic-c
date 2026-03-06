#!/bin/bash
#
# Sync Python reference source from upstream boriel-basic/zxbasic.
#
# This fetches the latest from the canonical Python repo and updates
# our src/ directory (READ-ONLY reference) to match. Any changes show
# up as a diff we can review and adapt the C port to.
#
# Usage: ./csrc/scripts/sync-upstream.sh [branch]
#   branch: upstream branch to sync from (default: master)
#
# What it does:
#   1. Fetches python-upstream remote
#   2. Checks out their src/ and tests/ into our working tree
#   3. Shows what changed so you can update the C port accordingly
#
# Safe to run repeatedly — only updates tracked files.

set -euo pipefail

UPSTREAM_BRANCH="${1:-master}"
REMOTE="python-upstream"

# Ensure remote exists
if ! git remote get-url "$REMOTE" >/dev/null 2>&1; then
    echo "Adding remote '$REMOTE' -> https://github.com/boriel-basic/zxbasic.git"
    git remote add "$REMOTE" https://github.com/boriel-basic/zxbasic.git
fi

echo "Fetching from $REMOTE..."
git fetch "$REMOTE" "$UPSTREAM_BRANCH"

echo ""
echo "=== Changes in upstream src/ ==="
git diff HEAD -- src/ -- ":(exclude)src/__pycache__" <<< "$(git diff HEAD..."$REMOTE/$UPSTREAM_BRANCH" -- src/)" 2>/dev/null | head -100 || true

# Show summary
CHANGED=$(git diff --stat HEAD..."$REMOTE/$UPSTREAM_BRANCH" -- src/ tests/functional/ 2>/dev/null | tail -1)
echo ""
echo "Upstream changes: $CHANGED"
echo ""

read -p "Apply upstream src/ and tests/ to working tree? [y/N] " -n 1 -r
echo ""

if [[ $REPLY =~ ^[Yy]$ ]]; then
    # Checkout upstream's src/ and tests/functional/ over ours
    git checkout "$REMOTE/$UPSTREAM_BRANCH" -- src/ tests/functional/
    echo ""
    echo "Updated src/ and tests/functional/ from upstream."
    echo "Review with: git diff --cached"
    echo "Commit with: git commit -m 'sync: update Python reference from upstream boriel-basic/zxbasic'"
else
    echo "No changes applied. You can manually cherry-pick with:"
    echo "  git checkout $REMOTE/$UPSTREAM_BRANCH -- src/"
    echo "  git checkout $REMOTE/$UPSTREAM_BRANCH -- tests/functional/"
fi
