#!/bin/bash
# Wait for the NEXT firmware build after the current tip.
# Usage: wait_download_next.sh <OUT.bin path>
# Captures current firmware tip as PREV at launch, then waits for it to advance.
cd "c:/Users/admin/Desktop/zzq/deliverables/ai-vox3/openvela/contest_repo"
export GIT_SSH_COMMAND="ssh -o HostKeyAlias=github.com -i C:/Users/admin/.ssh/id_ed25519"
OUT="$1"
if [ -z "$OUT" ]; then echo "usage: $0 <OUT.bin>"; exit 3; fi
PREV=$(git ls-remote origin firmware | awk '/refs\/heads\/firmware/{print $1}')
LOGSPREV=$(git ls-remote origin logs | awk '/refs\/heads\/logs/{print $1}')
echo "[0] PREV firmware=$PREV logs=$LOGSPREV  OUT=$OUT"
mkdir -p "$(dirname "$OUT")"
for i in $(seq 1 90); do
  TIP=$(git ls-remote origin firmware | awk '/refs\/heads\/firmware/{print $1}')
  LOGS=$(git ls-remote origin logs | awk '/refs\/heads\/logs/{print $1}')
  echo "[$i] firmware=$TIP logs=$LOGS"
  if [ -n "$TIP" ] && [ "$TIP" != "$PREV" ]; then
    echo "NEW firmware tip=$TIP"
    git fetch origin +refs/heads/firmware:refs/remotes/origin/firmware
    git show origin/firmware:nuttx.bin > "$OUT"
    SZ=$(stat -c%s "$OUT")
    echo "DOWNLOADED $TIP -> $OUT size=$SZ"
    if [ "$SZ" -lt 100000 ]; then echo "ERROR bin too small"; exit 5; fi
    echo "DONE"
    exit 0
  fi
  if [ -n "$LOGS" ] && [ "$LOGS" != "$LOGSPREV" ]; then
    echo "LOGS branch changed (build may have FAILED): $LOGS"
  fi
  sleep 20
done
echo "TIMEOUT no new firmware"
exit 2
