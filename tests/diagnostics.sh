#!/usr/bin/env bash
set -euo pipefail

INDEXER=$1
QUERY_BENCH=$2
TMP=$(mktemp -d)

cleanup() {
    rm -rf "${TMP}"
}
trap cleanup EXIT

git init -q "${TMP}/work"
git -C "${TMP}/work" config user.name aurhub-test
git -C "${TMP}/work" config user.email aurhub@example.invalid

cat >"${TMP}/work/.SRCINFO" <<'EOF'
pkgbase = clean
	pkgver = 1
	pkgrel = 1
	arch = any
pkgname = clean
EOF
git -C "${TMP}/work" add .SRCINFO
git -C "${TMP}/work" commit -qm clean
git -C "${TMP}/work" branch pkg-clean

cat >"${TMP}/work/.SRCINFO" <<'EOF'
pkgbase = warning
	pkgver = 1
	pkgrel = 1
	arch = x86_64
pkgname = warning
	source = package-source.tar.gz
EOF
git -C "${TMP}/work" add .SRCINFO
git -C "${TMP}/work" commit -qm warning
git -C "${TMP}/work" branch pkg-warning

cat >"${TMP}/work/.SRCINFO" <<'EOF'
pkgbase = quarantined
	pkgver = 1
	pkgrel = 1
	arch = any
	depend = libc
pkgname = quarantined
EOF
git -C "${TMP}/work" add .SRCINFO
git -C "${TMP}/work" commit -qm quarantined
git -C "${TMP}/work" branch pkg-quarantined

DEFAULT_BRANCH=$(git -C "${TMP}/work" symbolic-ref --short HEAD)
git clone --bare -q "${TMP}/work" "${TMP}/repo.git"
git --git-dir="${TMP}/repo.git" update-ref -d \
    "refs/heads/${DEFAULT_BRANCH}"

"${INDEXER}" --repo "${TMP}/repo.git" --output "${TMP}/snapshot" \
    --diagnostics "${TMP}/diagnostics.tsv" --jobs 2 \
    >"${TMP}/index.log" 2>&1

grep -q 'quarantined=1' "${TMP}/index.log"
grep -q 'warning_branches=1' "${TMP}/index.log"
grep -q $'warning\tignored_package_field\tpkg-warning\t6\tsource' \
    "${TMP}/diagnostics.tsv"
grep -q $'fatal\tunknown_field\tpkg-quarantined\t5\tdepend' \
    "${TMP}/diagnostics.tsv"

"${QUERY_BENCH}" --snapshot "${TMP}/snapshot" --type info \
    --query clean --iterations 3 --warmup 1 | grep -q 'matches=1'
"${QUERY_BENCH}" --snapshot "${TMP}/snapshot" --type info \
    --query warning --iterations 3 --warmup 1 | grep -q 'matches=1'
"${QUERY_BENCH}" --snapshot "${TMP}/snapshot" --type info \
    --query quarantined --iterations 3 --warmup 1 2>/dev/null | \
    grep -q 'matches=0'

git -C "${TMP}/work" checkout -q pkg-clean
cat >"${TMP}/work/.SRCINFO" <<'EOF'
pkgbase = clean
	pkgver = 2
	pkgrel = 1
	arch = any
	depend = broken
pkgname = clean
EOF
git -C "${TMP}/work" add .SRCINFO
git -C "${TMP}/work" commit -qm clean-quarantined
git --git-dir="${TMP}/repo.git" fetch -q "${TMP}/work" \
    +refs/heads/pkg-clean:refs/heads/pkg-clean
"${INDEXER}" --repo "${TMP}/repo.git" --output "${TMP}/snapshot" \
    >"${TMP}/quarantine-update.log" 2>&1
grep -q 'quarantined=1' "${TMP}/quarantine-update.log"
"${QUERY_BENCH}" --snapshot "${TMP}/snapshot" --type info \
    --query clean --iterations 3 --warmup 1 2>/dev/null | grep -q 'matches=0'

cat >"${TMP}/work/.SRCINFO" <<'EOF'
pkgbase = clean
	pkgver = 3
	pkgrel = 1
	arch = any
pkgname = clean
EOF
git -C "${TMP}/work" add .SRCINFO
git -C "${TMP}/work" commit -qm clean-restored
git --git-dir="${TMP}/repo.git" fetch -q "${TMP}/work" \
    +refs/heads/pkg-clean:refs/heads/pkg-clean
"${INDEXER}" --repo "${TMP}/repo.git" --output "${TMP}/snapshot" \
    >"${TMP}/restore-update.log" 2>&1
grep -q 'quarantined=0' "${TMP}/restore-update.log"
"${QUERY_BENCH}" --snapshot "${TMP}/snapshot" --type info \
    --query clean --iterations 3 --warmup 1 | grep -q 'matches=1'

echo "diagnostics integration test passed"
