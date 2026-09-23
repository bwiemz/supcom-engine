#!/usr/bin/env bash
# Formatting ratchet: lines changed since <base> must follow .clang-format.
# Untouched code is never reformatted wholesale.
#
#   tools/check_format.sh [base]    # base defaults to origin/main
#   tools/check_format.sh --fix [base]
#
# CLANG_FORMAT picks the binary (CI pins the version the style was set with).
set -euo pipefail

fix=0
if [[ "${1:-}" == "--fix" ]]; then
    fix=1
    shift
fi
base="${1:-origin/main}"
clang_format="${CLANG_FORMAT:-clang-format}"
cd "$(git rev-parse --show-toplevel)"

if ! git rev-parse --verify --quiet "${base}^{commit}" >/dev/null; then
    echo "format: '${base}' is not a commit (fetch it, or pass another base)" >&2
    exit 2
fi

paths=(src tests)
if [[ "${fix}" -eq 1 ]]; then
    git clang-format --binary "${clang_format}" "${base}" -- "${paths[@]}"
    exit 0
fi

# --diff exits 1 when it has a diff (LLVM 18+) and 2 or more when it failed.
set +e
diff="$(git clang-format --binary "${clang_format}" --diff "${base}" -- "${paths[@]}" 2>&1)"
status=$?
set -e
if ((status > 1)); then
    echo "${diff}" >&2
    echo "format: git clang-format failed (exit ${status})" >&2
    exit "${status}"
fi
if [[ -z "${diff}" || "${diff}" == *"no modified files to format"* ||
      "${diff}" == *"clang-format did not modify any files"* ]]; then
    echo "format: changed lines follow .clang-format"
    exit 0
fi
echo "${diff}"
echo "format: changed lines differ from .clang-format; run tools/check_format.sh --fix ${base}"
exit 1
