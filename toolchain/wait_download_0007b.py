import subprocess, sys, time, os

REPO = "c:/Users/admin/Desktop/zzq/deliverables/ai-vox3/openvela/contest_repo"
OUT = "c:/Users/admin/Desktop/zzq/deliverables/ai-vox3/openvela/contest_repo/toolchain/build/openvela_firmware_audiobook/nuttx_0007b.bin"
SSH = 'ssh -o HostKeyAlias=github.com -i C:/Users/admin/.ssh/id_ed25519'
PREV = "02a8cec"  # last known firmware tip built from c90f928 (0008 probes)
SRC = "1615f43"   # our source push that must be built

def sh(cmd, **kw):
    return subprocess.run(cmd, shell=True, cwd=REPO,
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE, **kw)

def ls_remote_firmware():
    r = sh(f'GIT_SSH_COMMAND="{SSH}" git ls-remote origin firmware')
    out = r.stdout.decode(errors="replace").strip()
    for line in out.splitlines():
        if line.endswith("refs/heads/firmware"):
            return line.split()[0]
    return None

def main():
    print(f"[wait] prev firmware tip={PREV}; waiting for build of source {SRC}")
    deadline = time.time() + 20*60
    tip = None
    while time.time() < deadline:
        try:
            t = ls_remote_firmware()
        except Exception as e:
            t = None
            print("[wait] ls-remote error:", e)
        if t and t != PREV:
            tip = t
            print(f"[wait] NEW firmware tip={t}")
            break
        time.sleep(20)
    if not tip:
        print("[wait] TIMEOUT: no new firmware appeared")
        sys.exit(2)

    # establish origin/firmware ref then dump nuttx.bin
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    fr = sh(f'GIT_SSH_COMMAND="{SSH}" git fetch origin +refs/heads/firmware:refs/remotes/origin/firmware')
    if fr.returncode != 0:
        print("[wait] fetch refspec rc=%d\n%s" % (fr.returncode, fr.stderr.decode(errors="replace")))
        sys.exit(3)
    show = sh(f'git show origin/firmware:nuttx.bin > "{OUT}"')
    if show.returncode != 0:
        print("[wait] git show rc=%d\n%s" % (show.returncode, show.stderr.decode(errors="replace")))
        sys.exit(4)
    sz = os.path.getsize(OUT)
    print(f"[wait] DOWNLOADED firmware tip={tip} -> {OUT} ({sz} bytes)")
    if sz < 100000:
        print("[wait] ERROR: bin too small, likely empty")
        sys.exit(5)
    print("[wait] DONE")

if __name__ == "__main__":
    main()
