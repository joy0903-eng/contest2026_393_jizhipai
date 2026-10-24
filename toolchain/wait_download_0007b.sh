#!/bin/bash
cd "c:/Users/admin/Desktop/zzq/deliverables/ai-vox3/openvela/contest_repo"
export GIT_SSH_COMMAND="ssh -o HostKeyAlias=github.com -i C:/Users/admin/.ssh/id_ed25519"
PREV=d5b8f2816bd04c61a9af0e1ab7cc9d74b59ba3a9
OUT=toolchain/build/openvela_firmware_audiobook/nuttx_0007h.bin
mkdir -p toolchain/build/openvela_firmware_audiobook
for i in $(seq 1 60); do
  TIP=$(git ls-remote origin firmware | awk '/refs\/heads\/firmware/{print $1}')
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
  sleep 20
done
echo "TIMEOUT no new firmware"
exit 2
