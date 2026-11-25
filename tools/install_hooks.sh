#!/usr/bin/env sh
# SPDX-License-Identifier: GPL-2.0-only
set -eu
ROOT="$(git rev-parse --show-toplevel)"
git -C "$ROOT" config core.hooksPath tools/hooks
chmod +x "$ROOT/tools/hooks/pre-commit"
echo "Installed git hooks from tools/hooks/."
