#!/usr/bin/env bash
set -euo pipefail

INDEXER=$1
QUERY_BENCH=$2
GENERATION_COMPARE=$3
TMP=$(mktemp -d)

cleanup() {
    rm -rf "${TMP}"
}
trap cleanup EXIT

git init -q "${TMP}/work"
git -C "${TMP}/work" config user.name aurhub-test
git -C "${TMP}/work" config user.email aurhub@example.invalid

cat >"${TMP}/work/.SRCINFO" <<'EOF'
pkgbase = alpha-one
	pkgdesc = Alpha package version one
	pkgver = 1.0
	pkgrel = 1
	arch = any
pkgname = alpha-one
EOF
git -C "${TMP}/work" add .SRCINFO
git -C "${TMP}/work" commit -qm alpha-v1
ALPHA_V1=$(git -C "${TMP}/work" rev-parse HEAD)
git -C "${TMP}/work" branch pkg-alpha

cat >"${TMP}/work/.SRCINFO" <<'EOF'
pkgbase = duplicate-fallback
	pkgdesc = Duplicate fallback branch
	pkgver = 1.0
	pkgrel = 1
	arch = any
pkgname = shared-package
EOF
git -C "${TMP}/work" add .SRCINFO
git -C "${TMP}/work" commit -qm duplicate-fallback
git -C "${TMP}/work" branch pkg-duplicate-a

cat >"${TMP}/work/.SRCINFO" <<'EOF'
pkgbase = duplicate-winner
	pkgdesc = Duplicate winner branch
	pkgver = 2.0
	pkgrel = 1
	arch = any
pkgname = shared-package
EOF
git -C "${TMP}/work" add .SRCINFO
git -C "${TMP}/work" commit -qm duplicate-winner
git -C "${TMP}/work" branch pkg-duplicate-z

DEFAULT_BRANCH=$(git -C "${TMP}/work" symbolic-ref --short HEAD)
git clone --bare -q "${TMP}/work" "${TMP}/repo.git"
git --git-dir="${TMP}/repo.git" update-ref -d \
    "refs/heads/${DEFAULT_BRANCH}"

"${INDEXER}" --repo "${TMP}/repo.git" \
    --output "${TMP}/current.snapshot" --jobs 2
cp "${TMP}/current.snapshot" "${TMP}/initial.snapshot"
cp "${TMP}/current.snapshot" "${TMP}/collapse.snapshot"
cp "${TMP}/current.snapshot" "${TMP}/fallback.snapshot"

git -C "${TMP}/work" checkout -q pkg-alpha
cat >"${TMP}/work/.SRCINFO" <<'EOF'
pkgbase = alpha-one
	pkgdesc = Alpha package version two
	pkgver = 2.0
	pkgrel = 1
	arch = any
pkgname = alpha-one
EOF
git -C "${TMP}/work" add .SRCINFO
git -C "${TMP}/work" commit -qm alpha-v2
git --git-dir="${TMP}/repo.git" fetch -q "${TMP}/work" \
    +refs/heads/pkg-alpha:refs/heads/pkg-alpha
"${INDEXER}" --repo "${TMP}/repo.git" \
    --output "${TMP}/current.snapshot" --max-overlay-records 10 \
    >"${TMP}/update.log" 2>&1
grep -q 'updated=1' "${TMP}/update.log"
grep -q 'overlay_records=2' "${TMP}/update.log"
grep -q 'compacted=0' "${TMP}/update.log"

git -C "${TMP}/work" checkout -qb pkg-gamma pkg-alpha
cat >"${TMP}/work/.SRCINFO" <<'EOF'
pkgbase = gamma-one
	pkgdesc = Gamma package added cumulatively
	pkgver = 1.0
	pkgrel = 1
	arch = any
pkgname = gamma-one
EOF
git -C "${TMP}/work" add .SRCINFO
git -C "${TMP}/work" commit -qm gamma-v1
git --git-dir="${TMP}/repo.git" fetch -q "${TMP}/work" \
    +refs/heads/pkg-gamma:refs/heads/pkg-gamma
"${INDEXER}" --repo "${TMP}/repo.git" \
    --output "${TMP}/current.snapshot" --max-overlay-records 10 \
    >"${TMP}/add.log" 2>&1
grep -q 'added=1' "${TMP}/add.log"
grep -q 'overlay_records=4' "${TMP}/add.log"
grep -q 'compacted=0' "${TMP}/add.log"

"${INDEXER}" --repo "${TMP}/repo.git" \
    --output "${TMP}/reference.snapshot" --full --jobs 2
"${GENERATION_COMPARE}" "${TMP}/current.snapshot" \
    "${TMP}/reference.snapshot"
"${QUERY_BENCH}" --snapshot "${TMP}/current.snapshot" --type info \
    --query alpha-one --iterations 3 --warmup 1 | grep -q 'matches=1'
"${QUERY_BENCH}" --snapshot "${TMP}/current.snapshot" --type info \
    --query gamma-one --iterations 3 --warmup 1 | grep -q 'matches=1'

git -C "${TMP}/work" checkout -q pkg-alpha
cat >"${TMP}/work/.SRCINFO" <<'EOF'
pkgbase = alpha-one
	pkgdesc = Alpha package version three
	pkgver = 3.0
	pkgrel = 1
	arch = any
pkgname = alpha-one
EOF
git -C "${TMP}/work" add .SRCINFO
git -C "${TMP}/work" commit -qm alpha-v3
git --git-dir="${TMP}/repo.git" fetch -q "${TMP}/work" \
    +refs/heads/pkg-alpha:refs/heads/pkg-alpha
"${INDEXER}" --repo "${TMP}/repo.git" \
    --output "${TMP}/current.snapshot" --max-overlay-records 3 \
    >"${TMP}/auto-compact.log" 2>&1
grep -q 'overlay_records=4' "${TMP}/auto-compact.log"
grep -q 'compacted=1' "${TMP}/auto-compact.log"
"${INDEXER}" --repo "${TMP}/repo.git" \
    --output "${TMP}/reference.snapshot" --full --jobs 2
"${GENERATION_COMPARE}" "${TMP}/current.snapshot" \
    "${TMP}/reference.snapshot"

git --git-dir="${TMP}/repo.git" update-ref refs/heads/pkg-alpha \
    "${ALPHA_V1}"
"${INDEXER}" --repo "${TMP}/repo.git" \
    --output "${TMP}/collapse.snapshot" --max-overlay-records 10 \
    >"${TMP}/collapse-overlay.log" 2>&1
grep -q 'overlay_records=2' "${TMP}/collapse-overlay.log"
grep -q 'compacted=0' "${TMP}/collapse-overlay.log"
git --git-dir="${TMP}/repo.git" update-ref -d refs/heads/pkg-gamma
"${INDEXER}" --repo "${TMP}/repo.git" \
    --output "${TMP}/collapse.snapshot" --max-overlay-records 10 \
    >"${TMP}/collapse.log" 2>&1
grep -q 'overlay_records=0' "${TMP}/collapse.log"
grep -q 'collapsed=1' "${TMP}/collapse.log"
[[ $(stat -c '%i' "${TMP}/collapse.snapshot") == \
   $(stat -c '%i' "${TMP}/collapse.snapshot.base") ]]
"${GENERATION_COMPARE}" "${TMP}/collapse.snapshot" \
    "${TMP}/initial.snapshot"

git --git-dir="${TMP}/repo.git" update-ref -d \
    refs/heads/pkg-duplicate-z
"${INDEXER}" --repo "${TMP}/repo.git" \
    --output "${TMP}/fallback.snapshot" --max-overlay-records 10 \
    >"${TMP}/fallback.log" 2>&1
grep -q 'deleted=1' "${TMP}/fallback.log"
"${INDEXER}" --repo "${TMP}/repo.git" \
    --output "${TMP}/reference.snapshot" --full --jobs 2
"${GENERATION_COMPARE}" "${TMP}/fallback.snapshot" \
    "${TMP}/reference.snapshot"
"${QUERY_BENCH}" --snapshot "${TMP}/fallback.snapshot" --type search \
    --query fallback --iterations 3 --warmup 1 | grep -q 'matches=1'

git --git-dir="${TMP}/repo.git" update-ref -d \
    refs/heads/pkg-duplicate-a
"${INDEXER}" --repo "${TMP}/repo.git" \
    --output "${TMP}/fallback.snapshot" --max-overlay-records 0 \
    >"${TMP}/forced-compact.log" 2>&1
grep -q 'deleted=1' "${TMP}/forced-compact.log"
grep -q 'compacted=1' "${TMP}/forced-compact.log"
"${INDEXER}" --repo "${TMP}/repo.git" \
    --output "${TMP}/reference.snapshot" --full --jobs 2
"${GENERATION_COMPARE}" "${TMP}/fallback.snapshot" \
    "${TMP}/reference.snapshot"
"${QUERY_BENCH}" --snapshot "${TMP}/fallback.snapshot" --type info \
    --query shared-package --iterations 3 --warmup 1 2>/dev/null | \
    grep -q 'matches=0'

echo "generation smoke test passed"
