#!/bin/sh
# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later

set -eu

: "${CI_COMMIT_TAG:?}" "${CI_COMMIT_SHA:?}" "${CI_PIPELINE_ID:?}" "${CI_JOB_TOKEN:?}"
: "${CI_API_V4_URL:?}" "${CI_PROJECT_ID:?}" "${CI_PROJECT_URL:?}"
export GLAB_ENABLE_CI_AUTOLOGIN=true
tag=$CI_COMMIT_TAG
printf '%s\n' "$tag" | grep -Eq '^[A-Za-z0-9]+([._-][A-Za-z0-9]+)*$'
case "$tag" in HEAD|*.lock) echo 'Invalid release tag.' >&2; exit 1 ;; esac
root=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
cd "$root"
test "$(git rev-parse HEAD)" = "$CI_COMMIT_SHA"
git fetch --quiet origin '+refs/heads/nemyu:refs/remotes/origin/nemyu'
git merge-base --is-ancestor "$CI_COMMIT_SHA" refs/remotes/origin/nemyu
remote_tag=$(git ls-remote --tags origin "refs/tags/$tag" "refs/tags/$tag^{}")
remote_commit=$(printf '%s\n' "$remote_tag" | awk 'NR == 1 { commit = $1 } /\^\{\}$/ { commit = $1 } END { print commit }')
if [ "$remote_commit" != "$CI_COMMIT_SHA" ]; then
    echo 'The remote tag is missing or no longer points at this pipeline commit.' >&2
    exit 1
fi

# Only metadata crosses job boundaries; binaries stay in the Package Registry.
for platform in windows-x86_64 linux-x86_64; do
    manifest="out/$platform/release.json"
    prefix="$CI_API_V4_URL/projects/$CI_PROJECT_ID/packages/generic/nemyu-$platform/"
    case "$platform" in windows-*) suffix=zip ;; linux-*) suffix=AppImage ;; esac
    filename="nemyu-$tag-$platform.$suffix"
    jq -e --arg tag "$tag" --arg commit "$CI_COMMIT_SHA" --arg pipeline "$CI_PIPELINE_ID" \
        --arg prefix "$prefix" --arg filename "$filename" '
        .tag == $tag and .commit == $commit and .pipeline_id == $pipeline and
        (.assets | length == 2) and
        .assets[0].name == $filename and .assets[1].name == ($filename + ".sha256") and
        all(.assets[]; (.name | type == "string") and (.url | startswith($prefix)) and
            (.link_type == "package" or .link_type == "other"))
        ' "$manifest" >/dev/null
done
jq -s '[.[].assets[] | {name, url, link_type}]' \
    out/windows-x86_64/release.json out/linux-x86_64/release.json > out/release-links.json
jq -e '([.[].name] | unique | length) == 4' out/release-links.json >/dev/null
{
    printf 'Build of nemyu at %s.\n\n' "$CI_COMMIT_SHA"
    printf 'Windows: extract the ZIP and run nemyu.exe or nemyu-cli.exe. Qt and the Visual C++ runtime are included.\n\n'
    printf 'Linux: make the AppImage executable and run it. Requires x86_64 Linux with glibc 2.39 or newer, X11/XWayland and suitable graphics drivers.\n\n'
    printf 'SHA-256 checksums are attached. Downloads are stored in the GitLab Package Registry.\n'
} > out/release-notes.md

endpoint="projects/$CI_PROJECT_ID/releases/$tag"
if glab api "$endpoint" > out/existing-release.json 2> out/existing-release.err; then
    # glab adds duplicate links on an update. Upsert each named asset instead.
    glab release create "$tag" --repo "$CI_PROJECT_URL" --ref "$tag" \
        --name "Nemyu $tag" --notes-file out/release-notes.md
    index=0
    while [ "$index" -lt 4 ]; do
        jq ".[$index]" out/release-links.json > out/release-link.json
        name=$(jq -r '.name' out/release-link.json)
        id=$(jq -r --arg name "$name" '.assets.links[] | select(.name == $name) | .id' out/existing-release.json)
        if [ -n "$id" ]; then
            glab api --method PUT "$endpoint/assets/links/$id" --input out/release-link.json
        else
            glab api --method POST "$endpoint/assets/links" --input out/release-link.json
        fi
        index=$((index + 1))
    done
else
    if ! grep -Eq '(^|[^0-9])404([^0-9]|$)' out/existing-release.err; then
        cat out/existing-release.err >&2
        echo 'Could not check whether the release already exists.' >&2
        exit 1
    fi
    glab release create "$tag" --repo "$CI_PROJECT_URL" --ref "$tag" \
        --name "Nemyu $tag" --notes-file out/release-notes.md \
        --assets-links "$(cat out/release-links.json)"
fi
printf 'Published %s/-/releases/%s\n' "$CI_PROJECT_URL" "$tag"
