#!/usr/bin/env bash
# Formatting ratchet: lines changed since <base> must follow .clang-format.
# Untouched code is never reformatted wholesale, and a moved file only where
# it changed (tools/check_format.py).
#
#   tools/check_format.sh [base]    # base defaults to origin/main
#   tools/check_format.sh --fix [base]
#
# CLANG_FORMAT picks the binary (CI pins the version the style was set with).
set -euo pipefail
exec python3 "$(dirname "${BASH_SOURCE[0]}")/check_format.py" "$@"
