#!/bin/sh
# Product default: boot until a frame-PASS, at most 5 attempts per round.
# Every parameter here was measured on hardware (retry-policy-10-4a6a853d.md);
# none of them is a guess. The seed early abort is deliberately OFF: it was
# tested over 11 rounds and did not beat the 300 s window (seed-abort-11).
#
#   sh scripts/start-with-retry.sh <log> [rounds] [csv]
set -eu
LOG=${1:?usage: start-with-retry.sh <console-log> [rounds] [csv]}
ROUNDS=${2:-5}
CSV=${3:-logs/retry-$(date +%Y%m%d-%H%M%S).csv}
CMD=/d/k30i-gpu/console/send-cmd.txt

# A command older than 90 s must never fire: two orphaned reboot-fastboot files
# reset the device mid-boot (2026-11-16 21:32 and 23:04) and looked like a fault.
if [ -f "$CMD" ]; then
  now=$(date +%s); mt=$(date -r "$CMD" +%s 2>/dev/null || echo 0)
  if [ $((now - mt)) -gt 90 ]; then
    echo "start-with-retry: dropping stale command file (age $((now-mt))s)"
    rm -f "$CMD" "$CMD.lock"
  fi
fi

mkdir -p "$(dirname "$CSV")"
exec python -u "$(dirname "$0")/reproduce_desktop.py" \
  --log "$LOG" --cycles "$ROUNDS" --retry-until-pass 5 \
  --window 300 --min-frames 2 --quiesce 0 --charge-pause 0 \
  --battery-guard --cmd-file "$CMD" --out-csv "$CSV" \
  --start-banner "${START_BANNER:-0}"
