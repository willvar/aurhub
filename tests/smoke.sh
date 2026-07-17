#!/usr/bin/env bash
set -euo pipefail

INDEXER=$1
SERVER=$2
SEARCH_BENCH=$3
GENERATION_COMPARE=$4
TMP=$(mktemp -d)
PORT=$((20000 + $$ % 20000))
SERVER_PID=
SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)

cleanup() {
    if [[ -n "${SERVER_PID}" ]]; then
        kill "${SERVER_PID}" 2>/dev/null || true
        wait "${SERVER_PID}" 2>/dev/null || true
    fi
    rm -rf "${TMP}"
}
trap cleanup EXIT

git init -q "${TMP}/work"
git -C "${TMP}/work" config user.name aurhub-test
git -C "${TMP}/work" config user.email aurhub@example.invalid

cat >"${TMP}/work/.SRCINFO" <<'EOF'
pkgbase = alpha-one
	pkgdesc = Alpha package for smoke tests
	pkgver = 1.2.3
	pkgrel = 1
	arch = any
	url = https://example.invalid/alpha
	groups = test-group
	license = MIT
	depends = libc
	replaces = alpha-old
pkgname = alpha-one
EOF
git -C "${TMP}/work" add .SRCINFO
git -C "${TMP}/work" commit -qm alpha
git -C "${TMP}/work" branch pkg-alpha

cat >"${TMP}/work/.SRCINFO" <<'EOF'
pkgbase = beta-suite
	pkgdesc = Beta base package
	pkgver = 4.0
	pkgrel = 2
	arch = any
	license = Apache-2.0
	makedepends = cmake
	depends = base-runtime
pkgname = beta-cli
	pkgdesc = Beta command line client
	depends = base-runtime
	depends = readline
pkgname = beta-tools
	pkgdesc = Beta tools collection
	depends = zlib
EOF
git -C "${TMP}/work" add .SRCINFO
git -C "${TMP}/work" commit -qm beta
git -C "${TMP}/work" branch pkg-beta

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
git --git-dir="${TMP}/repo.git" update-ref -d "refs/heads/${DEFAULT_BRANCH}"

"${INDEXER}" --repo "${TMP}/repo.git" --output "${TMP}/aurhub.snapshot" \
    --jobs 2
INITIAL_HASH=$(sha256sum "${TMP}/aurhub.snapshot" | cut -d' ' -f1)
"${INDEXER}" --repo "${TMP}/repo.git" --output "${TMP}/aurhub.snapshot" \
    >"${TMP}/no-change.log" 2>&1
grep -q 'already up to date' "${TMP}/no-change.log"
UNCHANGED_HASH=$(sha256sum "${TMP}/aurhub.snapshot" | cut -d' ' -f1)
[[ "${INITIAL_HASH}" == "${UNCHANGED_HASH}" ]]

git -C "${TMP}/work" checkout -q pkg-alpha
cat >"${TMP}/work/.SRCINFO" <<'EOF'
pkgbase = alpha-one
	pkgdesc = Alpha package updated incrementally
	pkgver = 2.0
	pkgrel = 1
	arch = any
	url = https://example.invalid/alpha
	groups = test-group
	license = MIT
	depends = libc
	replaces = alpha-old
pkgname = alpha-one
EOF
git -C "${TMP}/work" add .SRCINFO
git -C "${TMP}/work" commit -qm alpha-v2
git --git-dir="${TMP}/repo.git" fetch -q "${TMP}/work" \
    +refs/heads/pkg-alpha:refs/heads/pkg-alpha
"${INDEXER}" --repo "${TMP}/repo.git" --output "${TMP}/aurhub.snapshot" \
    >"${TMP}/incremental.log" 2>&1
grep -q 'updated=1' "${TMP}/incremental.log"

"${SEARCH_BENCH}" --snapshot "${TMP}/aurhub.snapshot" --iterations 3 \
    --warmup 1 --query tools --query definitely-not-present \
    >"${TMP}/search-bench.log"
grep -q 'query="tools" matches=1' "${TMP}/search-bench.log" || {
    cat "${TMP}/search-bench.log" >&2
    exit 1
}
grep -q 'query="definitely-not-present" matches=0' \
    "${TMP}/search-bench.log" || {
    cat "${TMP}/search-bench.log" >&2
    exit 1
}

"${SERVER}" --snapshot "${TMP}/aurhub.snapshot" --port "${PORT}" --workers 2 \
    >"${TMP}/server.log" 2>&1 &
SERVER_PID=$!

READY=0
for _ in $(seq 1 50); do
    if curl -fsS "http://127.0.0.1:${PORT}/health" >"${TMP}/health"; then
        READY=1
        break
    fi
    sleep 0.05
done

if [[ ${READY} -ne 1 ]]; then
    cat "${TMP}/server.log" >&2
    exit 1
fi

grep -q 'packages: 4' "${TMP}/health"

curl -fsS --globoff \
    "http://127.0.0.1:${PORT}/rpc?v=5&type=search&arg=tools" \
    >"${TMP}/search.json"
grep -q '"Name":"beta-tools"' "${TMP}/search.json"

curl -fsS --globoff \
    "http://127.0.0.1:${PORT}/rpc?v=5&type=info&arg[]=alpha-one&arg[]=beta-cli" \
    >"${TMP}/info.json"
grep -q '"resultcount":2' "${TMP}/info.json"
grep -q '"Name":"alpha-one"' "${TMP}/info.json"
grep -q '"Name":"beta-cli"' "${TMP}/info.json"
grep -q '"Depends":\["base-runtime","readline"\]' "${TMP}/info.json"

curl -fsS --globoff \
    "http://127.0.0.1:${PORT}/rpc?v=5&type=info&arg[]=alpha-one" \
    >"${TMP}/alpha.json"
grep -q '"Version":"2.0-1"' "${TMP}/alpha.json"
grep -q '"Replaces":\["alpha-old"\]' "${TMP}/alpha.json"
grep -q '"Groups":\["test-group"\]' "${TMP}/alpha.json"

curl -fsS --globoff \
    "http://127.0.0.1:${PORT}/rpc?v=5&type=info&arg[]=shared-package" \
    >"${TMP}/shared-winner.json"
grep -q '"Version":"2.0-1"' "${TMP}/shared-winner.json"
grep -q 'Duplicate winner branch' "${TMP}/shared-winner.json"

curl -fsS "http://127.0.0.1:${PORT}/packages.gz" |
    gzip -dc >"${TMP}/packages"
grep -qx 'alpha-one' "${TMP}/packages"
grep -qx 'beta-cli' "${TMP}/packages"
grep -qx 'beta-tools' "${TMP}/packages"
[[ $(grep -xc 'shared-package' "${TMP}/packages") -eq 1 ]]

python3 "${SCRIPT_DIR}/http_protocol.py" "${PORT}"

git -C "${TMP}/work" checkout -q pkg-alpha
cat >"${TMP}/work/.SRCINFO" <<'EOF'
pkgbase = alpha-one
	pkgdesc = Alpha package hot reloaded
	pkgver = 3.0
	pkgrel = 1
	arch = any
	url = https://example.invalid/alpha
	groups = test-group
	license = MIT
	depends = libc
	replaces = alpha-old
pkgname = alpha-one
EOF
git -C "${TMP}/work" add .SRCINFO
git -C "${TMP}/work" commit -qm alpha-v3
git --git-dir="${TMP}/repo.git" fetch -q "${TMP}/work" \
    +refs/heads/pkg-alpha:refs/heads/pkg-alpha
"${INDEXER}" --repo "${TMP}/repo.git" --output "${TMP}/aurhub.snapshot" \
    >"${TMP}/hot-reload.log" 2>&1

HOT_RELOADED=0
for _ in $(seq 1 100); do
    if curl -fsS --globoff \
        "http://127.0.0.1:${PORT}/rpc?v=5&type=info&arg[]=alpha-one" \
        >"${TMP}/alpha-hot.json" &&
        grep -q '"Version":"3.0-1"' "${TMP}/alpha-hot.json"; then
        HOT_RELOADED=1
        break
    fi
    sleep 0.02
done
if [[ ${HOT_RELOADED} -ne 1 ]]; then
    cat "${TMP}/server.log" >&2
    exit 1
fi

cp "${TMP}/aurhub.snapshot" "${TMP}/aurhub.snapshot.valid"
printf 'not a snapshot\n' >"${TMP}/aurhub.snapshot.bad"
mv "${TMP}/aurhub.snapshot.bad" "${TMP}/aurhub.snapshot"
REJECTED=0
for _ in $(seq 1 100); do
    if grep -q 'rejected snapshot generation' "${TMP}/server.log"; then
        REJECTED=1
        break
    fi
    sleep 0.02
done
if [[ ${REJECTED} -ne 1 ]]; then
    cat "${TMP}/server.log" >&2
    exit 1
fi
curl -fsS --globoff \
    "http://127.0.0.1:${PORT}/rpc?v=5&type=info&arg[]=alpha-one" \
    >"${TMP}/alpha-after-reject.json"
grep -q '"Version":"3.0-1"' "${TMP}/alpha-after-reject.json"

mv "${TMP}/aurhub.snapshot.valid" "${TMP}/aurhub.snapshot"
RESTORED=0
for _ in $(seq 1 100); do
    ACTIVATIONS=$(grep -c 'activated snapshot generation' \
        "${TMP}/server.log" || true)
    if [[ ${ACTIVATIONS} -ge 2 ]]; then
        RESTORED=1
        break
    fi
    sleep 0.02
done
if [[ ${RESTORED} -ne 1 ]]; then
    cat "${TMP}/server.log" >&2
    exit 1
fi

"${INDEXER}" --repo "${TMP}/repo.git" \
    --output "${TMP}/reference.snapshot" --full --jobs 2 \
    >"${TMP}/reference.log" 2>&1
"${GENERATION_COMPARE}" "${TMP}/aurhub.snapshot" \
    "${TMP}/reference.snapshot"

kill "${SERVER_PID}"
wait "${SERVER_PID}"
SERVER_PID=

git --git-dir="${TMP}/repo.git" update-ref -d refs/heads/pkg-duplicate-z
"${INDEXER}" --repo "${TMP}/repo.git" --output "${TMP}/aurhub.snapshot" \
    >"${TMP}/fallback.log" 2>&1
grep -q 'deleted=1' "${TMP}/fallback.log"

"${SERVER}" --snapshot "${TMP}/aurhub.snapshot" --port "${PORT}" --workers 2 \
    >"${TMP}/fallback-server.log" 2>&1 &
SERVER_PID=$!
for _ in $(seq 1 50); do
    if curl -fsS --globoff \
        "http://127.0.0.1:${PORT}/rpc?v=5&type=info&arg[]=shared-package" \
        >"${TMP}/shared-fallback.json"; then
        break
    fi
    sleep 0.05
done
grep -q '"Version":"1.0-1"' "${TMP}/shared-fallback.json"
grep -q 'Duplicate fallback branch' "${TMP}/shared-fallback.json"
kill "${SERVER_PID}"
wait "${SERVER_PID}"
SERVER_PID=

git --git-dir="${TMP}/repo.git" update-ref -d refs/heads/pkg-beta
"${INDEXER}" --repo "${TMP}/repo.git" --output "${TMP}/aurhub.snapshot" \
    --max-overlay-records 0 >"${TMP}/delete.log" 2>&1
grep -q 'deleted=1' "${TMP}/delete.log"
grep -q 'compacted=1' "${TMP}/delete.log"
"${SEARCH_BENCH}" --snapshot "${TMP}/aurhub.snapshot" --iterations 3 \
    --warmup 1 --query beta-tools --query alpha-one \
    >"${TMP}/delete-bench.log"
grep -q 'query="beta-tools" matches=0' \
    "${TMP}/delete-bench.log"
grep -q 'query="alpha-one" matches=1' \
    "${TMP}/delete-bench.log"

echo "aurhub smoke test passed"
