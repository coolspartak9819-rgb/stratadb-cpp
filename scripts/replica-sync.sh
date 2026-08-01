#!/usr/bin/env sh
set -eu

PRIMARY_URL="${1:?usage: scripts/replica-sync.sh PRIMARY_URL REPLICA_URL}"
REPLICA_URL="${2:?usage: scripts/replica-sync.sh PRIMARY_URL REPLICA_URL}"
STATUS="$(curl -fsS "${REPLICA_URL}/replication/status")"
AFTER="$(printf '%s' "$STATUS" | sed -n 's/.*last_sequence":\([0-9][0-9]*\).*/\1/p')"

if [ -z "$AFTER" ]; then
  echo "unable to parse replica sequence" >&2
  exit 1
fi

CHANGES="$(mktemp)"
trap 'rm -f "$CHANGES"' EXIT
curl -fsS "${PRIMARY_URL}/replication/changes?after=${AFTER}" -o "$CHANGES"
curl -fsS -X POST "${REPLICA_URL}/replication/changes" --data-binary "@${CHANGES}"
printf '\n'
