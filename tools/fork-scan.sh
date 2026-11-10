#!/usr/bin/env bash

# SPDX-FileCopyrightText: Copyright 2026 Nemyu contributors
# SPDX-License-Identifier: GPL-3.0-or-later

# Lists, as Markdown, the commits the Eglantine forks Nemyu follows made that Nemyu does not have
# yet. Run it before each update; the list is where the next update's ports start. The idea comes
# from Orchid's tools/fork-scan.sh (MythosOfs/Orchid); this one only needs git.
#
#   tools/fork-scan.sh [--since <date>] [--base <ref>] [--only <name>] [--no-fetch]
#
# --since     only commits committed after this date (default: 14 days ago)
# --base      what to compare against (default: HEAD)
# --only      scan one source (a name from SOURCES below); may be given more than once
# --no-fetch  use the refs/fork-scan/* refs from the last run
#
# A commit counts as known when its hash (7+ hex digits) is cited in a commit message on the base
# or in docs/UPSTREAM_CREDITS.md (ports name their source), when the base has a commit with the
# same subject or the same patch, or when tools/fork-scan-skip lists it (one hash and a reason per
# line, for commits looked at and turned down). Each commit is listed once, under the first source
# and branch that has it, so upstream commits that forks merged stay under upstream.

set -euo pipefail

# name | clone URL | branches (globs, space separated) | commit URL prefix
SOURCES=(
    "eglantine|https://github.com/Djihads80/Eglantine.git|nx2 nx2-experimental|https://github.com/Djihads80/Eglantine/commit/"
    "lichii|https://github.com/zortos293/Lichii.git|*|https://github.com/zortos293/Lichii/commit/"
    "orchid|https://github.com/MythosOfs/Orchid.git|main|https://github.com/MythosOfs/Orchid/commit/"
    "selevet|https://github.com/sylvieistaken/selevet-eglantine.git|main|https://github.com/sylvieistaken/selevet-eglantine/commit/"
    "pokedinner|https://git.eden-emu.dev/Franseven/PokeDinner-a-Eglantine-Fork.git|master|https://git.eden-emu.dev/Franseven/PokeDinner-a-Eglantine-Fork/commit/"
    "satsuma|https://github.com/SkiierCo/Satsuma.git|main|https://github.com/SkiierCo/Satsuma/commit/"
    # Eden-NS2 and its fork Jinra are a separate line (Eden, not Eglantine): their commits are
    # large and need adapting, not cherry-picking.
    "eden-ns2|https://github.com/0x2593/Eden-NS2.git|*|https://github.com/0x2593/Eden-NS2/commit/"
    "jinra|https://github.com/JinWooPT/Jinra.git|jinra|https://github.com/JinWooPT/Jinra/commit/"
)
# Branches that only carry CI, docs or release plumbing.
IGNORE_BRANCHES='^(docs/|ci/|release/|gh-pages$)'

since="14 days ago"
base="HEAD"
fetch=1
only=()
while [ $# -gt 0 ]; do
    case "$1" in
    --since) since="$2"; shift 2 ;;
    --base) base="$2"; shift 2 ;;
    --only) only+=("$2"); shift 2 ;;
    --no-fetch) fetch=0; shift ;;
    *) echo "usage: $0 [--since <date>] [--base <ref>] [--only <name>] [--no-fetch]" >&2; exit 2 ;;
    esac
done

cd "$(git rev-parse --show-toplevel)"
since_epoch=$(date -d "$since" +%s)

known=$(mktemp)
seen=$(mktemp)
trap 'rm -f "$known" "$seen"' EXIT
{
    git log "$base" --format=%B | grep -oE '\b[0-9a-f]{7,40}\b' | cut -c1-7 || true
    if [ -f docs/UPSTREAM_CREDITS.md ]; then
        grep -oE '\b[0-9a-f]{7,40}\b' docs/UPSTREAM_CREDITS.md | cut -c1-7 || true
    fi
    if [ -f tools/fork-scan-skip ]; then
        grep -oE '^[0-9a-f]{7,40}' tools/fork-scan-skip | cut -c1-7 || true
    fi
    git log "$base" --since="$(date -d "@$((since_epoch - 60 * 86400))" +%F)" --format='S %s'
} | sort -u > "$known"

is_known() { # <sha> <subject>
    grep -qxF "${1:0:7}" "$known" || grep -qxF "S $2" "$known"
}

selected() { # <name>
    [ ${#only[@]} -eq 0 ] && return 0
    local name
    for name in "${only[@]}"; do [ "$name" = "$1" ] && return 0; done
    return 1
}

echo "# Fork scan against \`$(git rev-parse --short "$base")\`, commits since $(date -d "$since" +%F)"
echo

for entry in "${SOURCES[@]}"; do
    IFS='|' read -r name url branches web <<< "$entry"
    selected "$name" || continue
    if [ "$fetch" = 1 ]; then
        refspecs=()
        for branch in $branches; do
            refspecs+=("+refs/heads/$branch:refs/fork-scan/$name/$branch")
        done
        if ! git fetch -q --no-tags "$url" "${refspecs[@]}" 2> /dev/null; then
            echo "fork-scan: fetching $name failed" >&2
            continue
        fi
    fi
    section=""
    while read -r ref; do
        branch=${ref#refs/fork-scan/$name/}
        [[ $branch =~ $IGNORE_BRANCHES ]] && continue
        out=$(git log --no-merges --cherry-pick --right-only --since="@$since_epoch" \
            --format='%H%x09%ct%x09%an%x09%s' "$base...$ref" |
            while IFS=$'\t' read -r sha epoch author subject; do
                is_known "$sha" "$subject" && continue
                grep -qxF "$sha" "$seen" && continue
                echo "$sha" >> "$seen"
                # shellcheck disable=SC2016 # the backticks are Markdown
                printf -- '- [`%s`](%s%s) %s, %s: %s\n' "${sha:0:10}" "$web" "$sha" \
                    "$(date -d "@$epoch" +%F)" "$author" "$subject"
            done)
        [ -n "$out" ] && section+="### $branch"$'\n\n'"$out"$'\n\n'
    done < <(git for-each-ref --sort=-committerdate --format='%(refname)' "refs/fork-scan/$name/")
    if [ -n "$section" ]; then
        echo "## $name"
        echo
        printf '%s' "$section"
    fi
done

echo "List turned-down commits in \`tools/fork-scan-skip\` (hash and reason) so the next scan leaves them out."
