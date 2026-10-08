#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Danny S
# SPDX-License-Identifier: GPL-3.0-or-later
#
#   packaging/release.sh 0.4.3            test everything, then tag, upload and update the PKGBUILD
#   packaging/release.sh --dry-run 0.4.3  same tests, nothing published
#   packaging/release.sh --pull 0.4.3     take a bad release down, point the PKGBUILD back at the last good one
#
# Never reuse a version number: a different file under an old version breaks the sha256 check.
set -euo pipefail

repo=DannyS124/diskforge
root="$(cd "$(dirname "$0")/.." && pwd)"
pkgbuild="$root/packaging/arch/PKGBUILD"
cd "$root"

die() { echo "error: $*" >&2; exit 1; }

set_pkgbuild() { # version hash
    sed -i "s/^pkgver=.*/pkgver=$1/; s/^pkgrel=.*/pkgrel=1/; s/^sha256sums=.*/sha256sums=('$2')/" "$pkgbuild"
}

if [[ "${1:-}" == "--pull" ]]; then
    v="${2:?usage: release.sh --pull <version>}"
    gh release view "v$v" -R "$repo" >/dev/null 2>&1 || die "no release v$v"
    gh release delete "v$v" -R "$repo" --cleanup-tag --yes
    git tag -d "v$v" >/dev/null 2>&1 || true

    prev=$(gh release list -R "$repo" --exclude-drafts --exclude-pre-releases --limit 1 --json tagName --jq '.[0].tagName')
    [[ -n "$prev" ]] || die "no earlier release to go back to"
    hash=$(gh release download "$prev" -R "$repo" -p SHA256SUMS -O - | cut -d' ' -f1)
    set_pkgbuild "${prev#v}" "$hash"
    git commit -qm "pull $v, back to ${prev#v}" "$pkgbuild"
    git push -q
    echo "v$v is gone and the PKGBUILD builds ${prev#v} again."
    echo "Fix the problem and release a new version number (not $v)."
    exit 0
fi

dry=0
if [[ "${1:-}" == "--dry-run" ]]; then dry=1; shift; fi
v="${1:?usage: release.sh [--dry-run] <version>}"

[[ -z "$(git status --porcelain)" ]] || die "commit or stash your changes first"
grep -q "VERSION $v\$" CMakeLists.txt || die "set VERSION $v in CMakeLists.txt first"
grep -q "release version=\"$v\"" data/io.github.DannyS124.DiskForge.metainfo.xml.in || die "add a <release version=\"$v\"> to the metainfo first"
grep -q "^## $v\$" CHANGELOG.md || die "add a '## $v' section to CHANGELOG.md first"
! git rev-parse -q --verify "refs/tags/v$v" >/dev/null || die "v$v already exists, use a new version"

echo "==> building and running the self-tests"
cmake -S . -B build >/dev/null
cmake --build build -j"$(nproc)" >/dev/null
./build/diskforge-selftest --guard | tail -1
sudo ./build/diskforge-selftest | tail -1

tmp=$(mktemp -d)
cleanup() {
    rm -rf "$tmp"
    if [[ $dry == 1 || ${published:-0} == 0 ]]; then
        git tag -d "v$v" >/dev/null 2>&1 || true
        git checkout -q -- "$pkgbuild"
    fi
}
trap cleanup EXIT

echo "==> making the release tarball"
git tag -s "v$v" -m "DiskForge $v"
tarball="diskforge-$v.tar.gz"
git archive --format=tar.gz --prefix="diskforge-$v/" -o "$tmp/$tarball" "v$v"
(cd "$tmp" && sha256sum "$tarball" > SHA256SUMS)
hash=$(cut -d' ' -f1 "$tmp/SHA256SUMS")
set_pkgbuild "$v" "$hash"

echo "==> test-building the package (makepkg uses the local tarball and still checks the hash)"
mkdir "$tmp/pkg"
cp "$pkgbuild" "$tmp/$tarball" "$tmp/pkg/"
(cd "$tmp/pkg" && makepkg -f --noconfirm >"$tmp/makepkg.log" 2>&1) || { tail -20 "$tmp/makepkg.log"; die "package build failed, nothing published"; }

if [[ $dry == 1 ]]; then
    echo "==> dry run OK: $tarball builds, sha256 $hash. Nothing was published."
    exit 0
fi

echo "==> publishing v$v"
notes=$(awk -v v="## $v" '$0 == v {on=1; next} /^## / {on=0} on' CHANGELOG.md)
git commit -qm "release $v" "$pkgbuild"
git push -q
git push -q origin "v$v"
gh release create "v$v" "$tmp/$tarball" "$tmp/SHA256SUMS" -R "$repo" --verify-tag --latest \
    --title "DiskForge $v" --notes "$notes

sha256: \`$hash\`"
published=1
echo "==> done: https://github.com/$repo/releases/tag/v$v"
