#!/usr/bin/env bash
# Background monitor: poll for vox3-log (trigger proof) and vox3-fw (binary ready).
# Long window (~75 min) so it survives a full openvela+esp-idf build cycle after
# the user enables + triggers the workflow.
REPO_DIR="$(cd "$(dirname "$0")" && pwd)"
LOG="$REPO_DIR/.ci_monitor.log"
: > "$LOG"
echo "[monitor start $(date -u '+%H:%M:%SZ')] watching vox3-log + vox3-fw (long window)" >> "$LOG"
for i in $(seq 1 150); do  # up to ~75 min
  NOW=$(date -u '+%H:%M:%SZ')
  LOGHIT=$(git -C "$REPO_DIR" ls-remote origin vox3-log 2>/dev/null | awk '{print $1}')
  FWHIT=$(git -C "$REPO_DIR" ls-remote origin vox3-fw 2>/dev/null | awk '{print $1}')
  if [ -n "$LOGHIT" ] && ! grep -q "TRIGGERED" "$LOG" 2>/dev/null; then
    echo "[$NOW] TRIGGERED: vox3-log appeared sha=$LOGHIT" >> "$LOG"
  fi
  if [ -n "$FWHIT" ]; then
    echo "[$NOW] FIRMWARE READY: vox3-fw appeared sha=$FWHIT" >> "$LOG"
    echo "[$NOW] DONE" >> "$LOG"
    break
  fi
  if [ -n "$LOGHIT" ]; then
    echo "[$NOW] vox3-log present (build running), still waiting for vox3-fw..." >> "$LOG"
  fi
  sleep 30
done
if [ -z "$(git -C "$REPO_DIR" ls-remote origin vox3-fw 2>/dev/null | awk '{print $1}')" ]; then
  LOGHIT=$(git -C "$REPO_DIR" ls-remote origin vox3-log 2>/dev/null | awk '{print $1}')
  if [ -z "$LOGHIT" ]; then
    echo "[$(date -u '+%H:%M:%SZ')] NO MARKER BRANCH in ~75min -> Actions still NOT triggering (disabled or fork workflows not enabled)" >> "$LOG"
  else
    echo "[$(date -u '+%H:%M:%SZ')] vox3-log present but vox3-fw not yet (build still running past window)" >> "$LOG"
  fi
fi
echo "[monitor end $(date -u '+%H:%M:%SZ')]" >> "$LOG"
