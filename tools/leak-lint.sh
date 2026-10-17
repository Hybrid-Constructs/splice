#!/usr/bin/env bash
#
# Fails if the tree (or its history, or the staged diff) contains something that
# does not belong in a public repository: key material, absolute home paths,
# references that escape the repo root, or CI settings that would hand secrets
# or self-hosted runners to code from outside contributors.
#
# Usage (from anywhere inside the repo):
#   tools/leak-lint.sh              # scan tracked files
#   tools/leak-lint.sh --staged     # scan only what is about to be committed
#   tools/leak-lint.sh --history    # scan every commit on every ref
#
# Extra patterns: point LEAKLINT_EXTRA_DENYLIST at a file of extended regexes
# (one per line, '#' comments). Keep that file OUT of this repo - a denylist of
# internal names is itself a list of internal names.

set -uo pipefail

MODE="tree"
case "${1:-}" in
    --staged)  MODE="staged" ;;
    --history) MODE="history" ;;
    "")        ;;
    *) echo "usage: $0 [--staged|--history]" >&2; exit 2 ;;
esac

cd "$(git rev-parse --show-toplevel)"

SELF="tools/leak-lint.sh"

# label|regex
PATTERNS=(
    'private key block|-----BEGIN [A-Z ]*PRIVATE KEY-----'
    'absolute macOS home path|/Users/[A-Za-z0-9._-]+/'
    'absolute Windows home path|[A-Za-z]:\\\\Users\\\\'
    'hard-coded credential|(api[_-]?key|secret|token|passw(or)?d)[[:space:]]*[:=][[:space:]]*["'"'"'][^"'"'"']{8,}'
    'path escaping the repo root|(^|[^.A-Za-z0-9_])\.\./[A-Za-z0-9_-]+/'
)

# Workflow-only rules, matched on non-comment lines of .github/workflows/*.
WORKFLOW_PATTERNS=(
    'self-hosted runner|self-hosted'
    'secret access|secrets[.:]'
    'pull_request_target trigger|pull_request_target'
    'reusable workflow from another repo|uses:[[:space:]]*[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+/\.github/workflows/'
)

if [ -n "${LEAKLINT_EXTRA_DENYLIST:-}" ]; then
    if [ ! -f "$LEAKLINT_EXTRA_DENYLIST" ]; then
        echo "LEAKLINT_EXTRA_DENYLIST not found: $LEAKLINT_EXTRA_DENYLIST" >&2
        exit 2
    fi
    while IFS= read -r line; do
        case "$line" in ''|'#'*) continue ;; esac
        PATTERNS+=("denylist entry|$line")
    done < "$LEAKLINT_EXTRA_DENYLIST"
fi

FAIL=0

report() { # label, detail
    echo "LEAK-LINT [$1]"
    echo "$2" | sed 's/^/    /'
    FAIL=1
}

# Emit "location:content" lines for the chosen mode on stdin-less terms.
scan_text() { # regex
    case "$MODE" in
        tree)
            git grep -nIE -e "$1" -- . ":!$SELF" 2>/dev/null ;;
        staged)
            git diff --cached -U0 --no-color -- . ":!$SELF" \
                | grep -E '^\+[^+]' | grep -E -e "$1" ;;
        history)
            git log --all -p -U0 --no-color -- . ":!$SELF" 2>/dev/null \
                | grep -E '^\+[^+]' | grep -E -e "$1" ;;
    esac
}

for entry in "${PATTERNS[@]}"; do
    label="${entry%%|*}"; re="${entry#*|}"
    hits="$(scan_text "$re" || true)"
    [ -n "$hits" ] && report "$label" "$hits"
done

# Workflow policy (tree mode only: diff streams carry no file names to filter on).
# Pure comment lines are ignored so the rules can be documented in the YAML.
if [ "$MODE" = "tree" ]; then
    for entry in "${WORKFLOW_PATTERNS[@]}"; do
        label="${entry%%|*}"; re="${entry#*|}"
        hits="$(git grep -nIE -e "$re" -- '.github/workflows' 2>/dev/null \
                | grep -Ev '^[^:]+:[0-9]+:[[:space:]]*#' || true)"
        [ -n "$hits" ] && report "workflow: $label" "$hits"
    done
fi

if [ "$FAIL" -ne 0 ]; then
    echo
    echo "leak-lint: FAILED ($MODE)" >&2
    exit 1
fi
echo "leak-lint: clean ($MODE)"
