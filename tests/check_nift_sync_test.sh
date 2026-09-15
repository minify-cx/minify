#!/usr/bin/env bash
# Tests for check-nift-sync.sh release-tag-based synchronization.
#
# check-nift-sync.sh must verify Nift's vendored payload against the sibling
# repository's release tag for the version Nift declares it vendors, NOT the
# sibling working tree / HEAD. These tests construct throwaway git fixture
# repos and assert the six required behaviours:
#   1. sibling HEAD exactly at the release tag            -> pass
#   2. sibling HEAD ahead with development commits        -> pass
#   3. dirty sibling working tree                         -> pass (tags only)
#   4. vendored payload differs from declared release tag -> fail (payload)
#   5. declared vendored version/tag missing              -> fail (tag)
#   6. sibling has a newer release than the vendored one  -> pass + update flag
# plus: checker-machinery self-sync mismatch              -> fail
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SCRIPT="$REPO_ROOT/scripts/check-nift-sync.sh"
test -f "$SCRIPT" || { echo "checker not found: $SCRIPT" >&2; exit 2; }

# Extract PAYLOAD_FILES=( ... ) from the checker script (tokenize per line).
mapfile -t PAYLOAD < <(sed -n '/^PAYLOAD_FILES=(/,/^)/p' "$SCRIPT" \
  | sed '1s/.*(//; $s/).*//' | tr ' \t' '\n\n' | grep -vE '^\s*$')
[[ ${#PAYLOAD[@]} -gt 0 ]] || { echo "failed to extract PAYLOAD_FILES" >&2; exit 2; }

# Detect repo kind from the payload list and read the declared vendored version.
if printf '%s\n' "${PAYLOAD[@]}" | grep -q '^include/json.h$'; then
  KIND=jsonic
else
  KIND=minify
fi
DECLARED_VERSION="$(sed -n 's/^DECLARED_VERSION="\(.*\)"$/\1/p' "$SCRIPT" | head -1)"
[[ -n "$DECLARED_VERSION" ]] || { echo "failed to extract DECLARED_VERSION" >&2; exit 2; }

T=$(mktemp -d "${TMPDIR:-/tmp}/nift-sync-test.XXXXXX")
trap 'rm -rf "$T"' EXIT

# make_sibling <dir> [newer_release 0|1] [dirty 0|1] [head_ahead 0|1]
# Creates a sibling git repo whose v1.0.0 tag content matches the fixture
# payload files. Optionally advances HEAD with dev commits, tags a newer
# release, or leaves the working tree dirty.
make_sibling() {
  local dir="$1" newer="${2:-0}" dirty="${3:-0}" head_ahead="${4:-0}"
  mkdir -p "$dir/scripts"
  cp "$SCRIPT" "$dir/scripts/check-nift-sync.sh"
  for f in "${PAYLOAD[@]}"; do
    mkdir -p "$(dirname "$dir/$f")"
    printf 'tag-content %s\n' "$f" > "$dir/$f"
  done
  (cd "$dir" \
    && git init -q \
    && git config user.email t@example.com \
    && git config user.name t \
    && git add -A \
    && git commit -qm "release v$DECLARED_VERSION" \
    && git tag "v$DECLARED_VERSION")
  if [[ "$head_ahead" -eq 1 ]]; then
    (cd "$dir" \
      && printf 'dev work\n' >> README.md \
      && git add -A \
      && git commit -qm "post-release development")
  fi
  if [[ "$newer" -eq 1 ]]; then
    (cd "$dir" \
      && printf 'next release\n' >> README.md \
      && git add -A \
      && git commit -qm "next release" \
      && git tag v2.0.0)
  fi
  if [[ "$dirty" -eq 1 ]]; then
    printf 'dirty working tree\n' >> "$dir/README.md"
  fi
}

# make_nift <dir>  Lays out the Nift-side fixture (embedded payload + wrapper).
make_nift() {
  local dir="$1"
  if [[ "$KIND" == jsonic ]]; then
    mkdir -p "$dir/src" "$dir/jsonic/scripts"
    printf '#include "../jsonic/include/json.h"\n' > "$dir/src/Json.h"
    cp "$SCRIPT" "$dir/jsonic/scripts/check-nift-sync.sh"
    for f in "${PAYLOAD[@]}"; do
      mkdir -p "$(dirname "$dir/jsonic/$f")"
      printf 'tag-content %s\n' "$f" > "$dir/jsonic/$f"
    done
  else
    mkdir -p "$dir/scripts"
    cp "$SCRIPT" "$dir/scripts/check-nift-sync.sh"
    for f in "${PAYLOAD[@]}"; do
      mkdir -p "$(dirname "$dir/$f")"
      printf 'tag-content %s\n' "$f" > "$dir/$f"
    done
  fi
}

run_check() {
  local sibling="$1" nift="$2"
  if [[ "$KIND" == jsonic ]]; then
    (cd / && bash "$sibling/scripts/check-nift-sync.sh" "$nift")
  else
    (cd / && bash "$sibling/scripts/check-nift-sync.sh" "$nift")
  fi
}

RUN_OUT=""; RUN_ST=0
capture() {  # runs run_check and records status + output without set -e abort
  local out st
  set +e
  out=$(run_check "$1" "$2" 2>&1)
  st=$?
  set -e
  RUN_OUT="$out"
  RUN_ST=$st
}

expect() {
  local name="$1" want_status="$2" status="$3" output="$4"
  if [[ "$status" -eq "$want_status" ]]; then
    echo "PASS [$name] (exit $status)"
  else
    echo "FAIL [$name]: expected exit $want_status, got $status" >&2
    echo "$output" >&2
    exit 1
  fi
}

expect_contains() {
  local name="$1" output="$2" needle="$3"
  if grep -qF "$needle" <<<"$output"; then
    echo "PASS [$name] report: $needle"
  else
    echo "FAIL [$name]: report missing '$needle'" >&2
    echo "$output" >&2
    exit 1
  fi
}

# --- 1. sibling HEAD exactly at the release tag -------------------------------
S="$T/s1"; N="$T/n1"
make_sibling "$S" 0 0 0
make_nift "$N"
capture "$S" "$N"
expect "head-at-tag" 0 "$RUN_ST" "$RUN_OUT"
expect_contains "head-at-tag" "$RUN_OUT" "payload matches tag:     yes"

# --- 2. sibling HEAD ahead with development commits ---------------------------
S="$T/s2"; N="$T/n2"
make_sibling "$S" 0 0 1
make_nift "$N"
capture "$S" "$N"
expect "head-ahead-dev" 0 "$RUN_ST" "$RUN_OUT"
expect_contains "head-ahead-dev" "$RUN_OUT" "payload matches tag:     yes"

# --- 3. dirty sibling working tree --------------------------------------------
S="$T/s3"; N="$T/n3"
make_sibling "$S" 0 1 0
make_nift "$N"
capture "$S" "$N"
expect "dirty-tree" 0 "$RUN_ST" "$RUN_OUT"
expect_contains "dirty-tree" "$RUN_OUT" "payload matches tag:     yes"

# --- 4. vendored payload differs from its declared release tag ----------------
S="$T/s4"; N="$T/n4"
make_sibling "$S" 0 0 0
make_nift "$N"
if [[ "$KIND" == jsonic ]]; then
  printf 'TAMPERED\n' > "$N/jsonic/include/json.h"
else
  printf 'TAMPERED\n' > "$N/include/minify/Minify.h"
fi
capture "$S" "$N"
expect "payload-differs" 1 "$RUN_ST" "$RUN_OUT"
expect_contains "payload-differs" "$RUN_OUT" "payload differs from release tag"
expect_contains "payload-differs" "$RUN_OUT" "payload matches tag:     no"

# --- 5. declared vendored version/tag missing ---------------------------------
S="$T/s5"; N="$T/n5"
make_sibling "$S" 0 0 0
# Remove the declared-version tag so it has no matching release tag.
(cd "$S" && git tag -d "v$DECLARED_VERSION" >/dev/null)
make_nift "$N"
capture "$S" "$N"
expect "tag-missing" 1 "$RUN_ST" "$RUN_OUT"
expect_contains "tag-missing" "$RUN_OUT" "no matching release tag"

# --- 6. sibling has a newer release than the vendored version -----------------
S="$T/s6"; N="$T/n6"
make_sibling "$S" 1 0 0
make_nift "$N"
capture "$S" "$N"
expect "newer-release" 0 "$RUN_ST" "$RUN_OUT"
expect_contains "newer-release" "$RUN_OUT" "latest sibling release:  v2.0.0"
expect_contains "newer-release" "$RUN_OUT" "payload matches tag:     yes"
expect_contains "newer-release" "$RUN_OUT" "update available:        yes"

# --- 7. checker-machinery self-sync mismatch ----------------------------------
S="$T/s7"; N="$T/n7"
make_sibling "$S" 0 0 0
make_nift "$N"
# Tamper with the vendored copy of the checker itself.
if [[ "$KIND" == jsonic ]]; then
  printf '# tampered\n' >> "$N/jsonic/scripts/check-nift-sync.sh"
else
  printf '# tampered\n' >> "$N/scripts/check-nift-sync.sh"
fi
capture "$S" "$N"
expect "checker-desync" 1 "$RUN_ST" "$RUN_OUT"
expect_contains "checker-desync" "$RUN_OUT" "check-nift-sync.sh differs between sibling and vendored"

echo "check-nift-sync release-tag synchronization tests passed ($KIND)"