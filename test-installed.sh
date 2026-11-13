#!/usr/bin/env bash
# RebaneOS installed-system regression test (VirtualBox, Linux).
# Creates a NEW isolated VM per test run. Never modifies existing VMs or VDIs.
set -Eeuo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
TEST_ROOT="${REBANE_TEST_ROOT:-${XDG_STATE_HOME:-$HOME/.local/state}/rebaneos-installed-tests}"
JOBS="${JOBS:-2}"

say() { printf '\n==> %s\n' "$*"; }
die() { printf 'ERROR: %s\n' "$*" >&2; exit 1; }

usage() {
    cat <<'EOF'
Usage:
  ./test-installed.sh                 Build ISO; create fresh VM/disks; install; boot installed disk
  ./test-installed.sh resume RUN_DIR  Continue after shutting down the installer VM
  ./test-installed.sh boot RUN_DIR    Reboot a previously installed test VM
  ./test-installed.sh --help

Environment:
  JOBS=2                      Build parallelism (default 2)
  REBANE_TEST_ROOT=/some/path Directory for test VMs and disks (outside repo by default)

The guest installer itself is graphical and must be completed manually.
This tool DOES NOT delete or modify any existing VM or disk.
EOF
}

require() {
    command -v "$1" >/dev/null 2>&1 || die "Missing required program: $1"
}

vm_state() {
    local vm="$1" state
    state="$(VBoxManage showvminfo "$vm" --machinereadable \
        | sed -n 's/^VMState="\([^"]*\)"$/\1/p' | head -n 1)"
    [[ -n "$state" ]] || die "Could not read power state for VM: $vm"
    printf '%s\n' "$state"
}

load_run() {
    RUN_DIR="$(cd -- "${1:?Run directory required}" && pwd -P)"
    [[ -f "$RUN_DIR/vm.name" ]] || die "Not a RebaneOS test directory: $RUN_DIR"
    VM="$(cat "$RUN_DIR/vm.name")"
    [[ -n "$VM" ]] || die "Empty VM name in $RUN_DIR/vm.name"
    INSTALL_DISK="$RUN_DIR/install.vdi"
    DATA_DISK="$RUN_DIR/data.vdi"
    ISO="$SCRIPT_DIR/rockosdualbootv.1.iso"
    [[ -f "$INSTALL_DISK" && -f "$DATA_DISK" ]] \
        || die "Expected test disks are missing from $RUN_DIR"
}

verify_install() {
    local raw="$RUN_DIR/check-install.raw"
    [[ ! -e "$raw" ]] || die "Refusing to overwrite $raw"

    say 'Checking the installed disk for an MBR signature and partition table'
    VBoxManage clonemedium "$INSTALL_DISK" "$raw" --format RAW >/dev/null

    if ! python3 - "$raw" <<'PY'
import sys
from pathlib import Path
p = Path(sys.argv[1])
with p.open('rb') as f:
    sector = f.read(512)
if len(sector) != 512 or sector[510:512] != b'\x55\xaa':
    print('No 0x55AA boot-sector signature; installation may be incomplete.', file=sys.stderr)
    sys.exit(1)
if not any(sector[446:510]):
    print('No partition table entries found; installation may be incomplete.', file=sys.stderr)
    sys.exit(1)
print('MBR boot signature and partition table are present.')
PY
    then
        printf '\nInspection copy retained at: %s\n' "$raw" >&2
        die 'Installation verification failed. Preserve the VM and inspect installer logs.'
    fi
    rm -- "$raw" # Only our temporary inspection copy, not the VDI.
}

boot_installed() {
    local state
    state="$(vm_state "$VM")"
    [[ "$state" == poweroff ]] || die "VM must be fully powered off (currently: $state)."

    say 'Switching to installed-disk boot'
    # Both disks were created for this run. Detaching does not delete either file.
    VBoxManage storageattach "$VM" --storagectl IDE --port 0 --device 0 \
        --type hdd --medium none
    VBoxManage storageattach "$VM" --storagectl IDE --port 0 --device 1 \
        --type hdd --medium none
    VBoxManage storageattach "$VM" --storagectl IDE --port 0 --device 0 \
        --type hdd --medium "$INSTALL_DISK"
    # Keep the data VDI safely detached for the isolated scheduler test.
    VBoxManage storageattach "$VM" --storagectl IDE --port 1 --device 0 \
        --type dvddrive --medium emptydrive
    VBoxManage modifyvm "$VM" --boot1 disk --boot2 none --boot3 none --boot4 none
    printf 'installed\n' > "$RUN_DIR/stage"

    say 'Starting RebaneOS from its installed VDI'
    VBoxManage startvm "$VM" --type gui
    printf '\nInstalled-system VM: %s\n' "$VM"
    printf 'Serial log: %s\n' "$RUN_DIR/serial.log"
    printf 'After powering off, reboot this VM with:\n  %q boot %q\n' "$SCRIPT_DIR/test-installed.sh" "$RUN_DIR"
}

resume_run() {
    local state stage
    stage="$(cat "$RUN_DIR/stage")"
    case "$stage" in
        installed)
            say 'This run is already in installed-disk mode'
            state="$(vm_state "$VM")"
            [[ "$state" == poweroff ]] || die "VM is $state; cannot start it again."
            VBoxManage startvm "$VM" --type gui
            return
            ;;
        installer) ;;
        *) die "Unrecognised test stage: $stage" ;;
    esac

    printf '\nComplete the graphical installer and power off the VM.\n'
    printf 'Do NOT save the VM state. Waiting for a complete power-off...\n'

    # The script does not force-poweroff the VM or assume installation has completed.
    while true; do
        state="$(vm_state "$VM")"
        case "$state" in
            poweroff) break ;;
            aborted|saved) die "VM is $state, not powered off. Resolve in VirtualBox first." ;;
        esac
        sleep 2
    done
    verify_install
    boot_installed
}

new_run() {
    [[ "$JOBS" =~ ^[1-9][0-9]*$ ]] || die 'JOBS must be a positive integer'
    [[ -f "$SCRIPT_DIR/build-linux.sh" ]] || die 'Put test-installed.sh in the RebaneOS repository root'

    say 'Building a fresh RebaneOS ISO'
    ( cd "$SCRIPT_DIR" && ./build-linux.sh -B "-j$JOBS" )
    ISO="$SCRIPT_DIR/rockosdualbootv.1.iso"
    [[ -s "$ISO" ]] || die "Build completed but ISO is missing: $ISO"

    mkdir -p -- "$TEST_ROOT"
    RUN_DIR="$(mktemp -d "$TEST_ROOT/run-XXXXXXXX")"
    VM="RebaneOS-Test-$(basename "$RUN_DIR")"
    INSTALL_DISK="$RUN_DIR/install.vdi"
    DATA_DISK="$RUN_DIR/data.vdi"
    printf '%s\n' "$VM" > "$RUN_DIR/vm.name"
    printf 'installer\n' > "$RUN_DIR/stage"
    printf '%s\n' "$ISO" > "$RUN_DIR/installer-iso.path"

    say "Provisioning isolated VM: $VM"
    VBoxManage createvm --name "$VM" --ostype Other_64 \
        --basefolder "$RUN_DIR" --register
    VBoxManage modifyvm "$VM" \
        --memory 4096 --cpus 1 --firmware bios --chipset piix3 \
        --x86-pae on --x86-long-mode on \
        --mouse ps2 --keyboard ps2 \
        --nic1 nat --nic-type1 82540EM --cable-connected1 on \
        --uart1 0x3F8 4 --uart-mode1 file "$RUN_DIR/serial.log" \
        --boot1 dvd --boot2 disk --boot3 none --boot4 none
    VBoxManage storagectl "$VM" --name IDE --add ide --controller PIIX4
    VBoxManage createmedium disk --filename "$DATA_DISK" --size 16 --format VDI
    VBoxManage createmedium disk --filename "$INSTALL_DISK" --size 64 --format VDI
    VBoxManage storageattach "$VM" --storagectl IDE --port 0 --device 0 \
        --type hdd --medium "$DATA_DISK"
    VBoxManage storageattach "$VM" --storagectl IDE --port 0 --device 1 \
        --type hdd --medium "$INSTALL_DISK"
    VBoxManage storageattach "$VM" --storagectl IDE --port 1 --device 0 \
        --type dvddrive --medium "$ISO"

    say 'Starting the ISO installer'
    VBoxManage startvm "$VM" --type gui
    cat <<EOF

TEST VM: $VM
TEST DIRECTORY: $RUN_DIR

In the VirtualBox window:
  1. Finish the installer preferences.
  2. Press S to skip network setup (requires your earlier Skip fix).
  3. Install onto the 64 MiB PRIMARY-SLAVE disk.
  4. When the installer says "Installed", close the VM window and choose
     "Power off the machine" (NOT "Save the machine state").

Keep this terminal open. The script will detect power-off, verify the
installation, rearrange boot media, and start the INSTALLED RebaneOS.

If this terminal is interrupted, resume with:
  "$SCRIPT_DIR/test-installed.sh" resume "$RUN_DIR"
EOF
    resume_run
}

main() {
    if [[ "${1:-}" == -h || "${1:-}" == --help || "${1:-}" == help ]]; then
        usage
        return
    fi
    require VBoxManage
    require python3
    case "${1:-}" in
        ''|all)
            [[ $# -le 1 ]] || die 'Unexpected extra arguments'
            new_run
            ;;
        resume)
            [[ $# -eq 2 ]] || die 'Expected: resume RUN_DIR'
            load_run "$2"
            resume_run
            ;;
        boot)
            [[ $# -eq 2 ]] || die 'Expected: boot RUN_DIR'
            load_run "$2"
            [[ "$(cat "$RUN_DIR/stage")" == installed ]] || die 'This VM has not reached installed-disk stage'
            [[ "$(vm_state "$VM")" == poweroff ]] || die 'Power off the VM first'
            VBoxManage startvm "$VM" --type gui
            ;;
        -h|--help|help)
            usage
            ;;
        *) usage; die "Unrecognised command: $1" ;;
    esac
}

main "$@"
