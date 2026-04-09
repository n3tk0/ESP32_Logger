#!/usr/bin/env bash
# tools/generate_changelog.sh
#
# Generates CHANGELOG.md from git log.
# Groups commits by tag/release, formats them by type prefix:
#   feat: / fix: / refactor: / docs: / chore: / test:
#
# Usage:
#   ./tools/generate_changelog.sh              # write to CHANGELOG.md
#   ./tools/generate_changelog.sh --stdout     # print to stdout only
#   ./tools/generate_changelog.sh --since v4.0 # only commits after that tag

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$SCRIPT_DIR/.."
OUT_FILE="$REPO_DIR/CHANGELOG.md"
STDOUT_ONLY=false
SINCE_REF=""

for arg in "$@"; do
    case $arg in
        --stdout) STDOUT_ONLY=true ;;
        --since=*) SINCE_REF="${arg#*=}" ;;
        --since) shift; SINCE_REF="${1:-}" ;;
    esac
done

cd "$REPO_DIR"

# Get list of tags (newest first), add HEAD as the current untagged range
TAGS=($(git tag --sort=-version:refname 2>/dev/null | head -20))
TAGS=("HEAD" "${TAGS[@]}")

generate() {
    echo "# Changelog"
    echo ""
    echo "_Auto-generated from git log on $(date +%Y-%m-%d)_"
    echo ""

    local prev_ref=""
    for tag in "${TAGS[@]}"; do
        local range=""
        if [[ -n "$prev_ref" ]]; then
            range="${tag}..${prev_ref}"
        fi

        # Skip the HEAD..HEAD range
        [[ "$tag" == "HEAD" && -z "$prev_ref" ]] && { prev_ref="HEAD"; continue; }
        [[ -z "$range" ]] && break

        # Get commits in range
        local commits
        commits=$(git log "$range" \
            --no-merges \
            --format="%s|||%h" \
            --reverse 2>/dev/null) || continue

        [[ -z "$commits" ]] && { prev_ref="$tag"; continue; }

        # Section header
        if [[ "$prev_ref" == "HEAD" ]]; then
            local head_tag
            head_tag=$(git describe --tags HEAD 2>/dev/null || echo "Unreleased")
            echo "## $head_tag"
        else
            local date
            date=$(git log -1 --format="%as" "$prev_ref" 2>/dev/null || echo "")
            echo "## $prev_ref${date:+  ($date)}"
        fi
        echo ""

        local -A sections=()
        while IFS="|||" read -r subject hash; do
            [[ -z "$subject" ]] && continue
            local type="Other"
            case "$subject" in
                feat:*|feat\(*) type="Features"  ;;
                fix:*|fix\(*)   type="Bug Fixes"  ;;
                refactor:*)     type="Refactoring";;
                docs:*)         type="Docs"        ;;
                chore:*)        type="Chores"      ;;
                test:*)         type="Tests"       ;;
                Implement*|Add*|New*) type="Features" ;;
                Fix*)           type="Bug Fixes"  ;;
            esac
            # strip conventional prefix
            local msg="$subject"
            msg="${msg#feat: }"; msg="${msg#fix: }"; msg="${msg#refactor: }"
            msg="${msg#docs: }"; msg="${msg#chore: }"; msg="${msg#test: }"
            sections[$type]+="- ${msg} (\`$hash\`)"$'\n'
        done <<< "$commits"

        local order=("Features" "Bug Fixes" "Refactoring" "Docs" "Tests" "Chores" "Other")
        for sec in "${order[@]}"; do
            if [[ -n "${sections[$sec]:-}" ]]; then
                echo "### $sec"
                echo ""
                echo "${sections[$sec]}"
            fi
        done

        prev_ref="$tag"
    done
}

if $STDOUT_ONLY; then
    generate
else
    generate > "$OUT_FILE"
    echo "Changelog written to $OUT_FILE"
fi
