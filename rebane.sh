#!/usr/bin/env bash
# One build and graphical VirtualBox entrypoint for every ISO variant.
set -Eeuo pipefail

REPO_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"

usage() {
    cat <<'EOF'
Usage: ./rebane.sh [build|test|run|resume|boot] [OPTIONS] [RUN_DIR] [-- MAKE_ARGS...]

Commands:
  build          Build the selected ISO only
  test           Build, then open a fresh graphical test VM (default)
  run            Open a fresh graphical test VM using the existing selected ISO
  resume RUN_DIR Continue a recorded run without rebuilding
  boot RUN_DIR   Reboot a powered-off installed or live-image run

Options:
  --variant standard|linux|browser  ISO selection (prompt on a terminal)
  --jobs N                         Make parallelism (JOBS environment, default 2)
  --test-root DIR                  VM directory (REBANE_TEST_ROOT environment,
                                   default: build/vm-tests in this repository)
  -h, --help                       Show this help without requiring dependencies
  -- MAKE_ARGS...                  Pass each following argument unchanged to Make

Noninteractive variant selection defaults to standard. Standard uses the manual
GUI installer, waits for power-off, checks the installed MBR, then boots the disk.
Linux and browser images boot live: no installation or installed-disk check.
No existing VM or disk is deleted and no VM is forcibly powered off.
EOF
}

die() { printf 'ERROR: %s\n' "$*" >&2; exit 1; }

main() {
    local action=test action_set=0 variant='' jobs="${JOBS:-2}"
    local test_root="${REBANE_TEST_ROOT:-$REPO_DIR/build/vm-tests}" run_dir=''
    local target iso selection
    local -a make_args=()

    while (($#)); do
        case "$1" in
            -h|--help|help) usage; return 0 ;;
            build|test|run|resume|boot)
                if ((action_set)); then
                    [[ -z "$run_dir" && ( "$action" == resume || "$action" == boot ) ]] \
                        || die "Unexpected argument: $1"
                    run_dir="$1"
                else
                    action="$1"
                    action_set=1
                fi
                shift ;;
            --variant|--jobs|--test-root)
                (($# >= 2)) || die "Missing value for $1"
                case "$1" in
                    --variant) variant="$2" ;;
                    --jobs) jobs="$2" ;;
                    --test-root) test_root="$2" ;;
                esac
                shift 2 ;;
            --) shift; make_args=("$@"); break ;;
            -*) die "Unknown option: $1 (use -- before Make arguments)" ;;
            *)
                [[ -z "$run_dir" && ( "$action" == resume || "$action" == boot ) ]] \
                    || die "Unexpected argument: $1"
                run_dir="$1"
                shift ;;
        esac
    done

    if [[ "$action" == resume || "$action" == boot ]]; then
        [[ -n "$run_dir" ]] || die "Expected: $action RUN_DIR"
        ((${#make_args[@]} == 0)) || die 'Make arguments apply only to build and test'
        command -v python3 >/dev/null 2>&1 || die 'Missing required program: python3'
        exec python3 "$REPO_DIR/scripts/test-vm.py" "$action" "$run_dir"
    fi

    if [[ -z "$variant" ]]; then
        variant=standard
        if [[ -t 0 ]]; then
            printf 'Select ISO: 1) standard (default)  2) linux  3) browser\nChoice [1]: ' >&2
            IFS= read -r selection || selection=''
            case "$selection" in
                ''|1|standard) variant=standard ;;
                2|linux) variant=linux ;;
                3|browser) variant=browser ;;
                *) die "Unknown ISO selection: $selection" ;;
            esac
        fi
    fi
    case "$variant" in
        standard) target=all; iso="$REPO_DIR/rockosdualbootv.1.iso" ;;
        linux) target=linux-iso; iso="$REPO_DIR/build/rebane-linux.iso" ;;
        browser) target=browser-iso; iso="$REPO_DIR/build/rebane-browser.iso" ;;
        *) die "Unknown variant: $variant (expected standard, linux, or browser)" ;;
    esac

    if [[ "$action" == build || "$action" == test ]]; then
        [[ "$jobs" =~ ^[1-9][0-9]*$ ]] || die 'JOBS/--jobs must be a positive integer'
        # Apple toolchains produce Mach-O; Linux can also use its native ELF tools.
        if [[ "$(uname -s)" == Darwin ]] || \
            { command -v x86_64-elf-gcc >/dev/null 2>&1 && command -v x86_64-elf-ld >/dev/null 2>&1; }; then
            export CC="${CC:-x86_64-elf-gcc}" LD="${LD:-x86_64-elf-ld}"
        else
            export CC="${CC:-gcc}" LD="${LD:-ld}"
        fi
        printf '\n==> Building the %s ISO\n' "$variant"
        (cd -- "$REPO_DIR"; JOBS="$jobs" REBANE_BUILD_TARGET="$target" ./scripts/build-unix.sh "-j$jobs" "${make_args[@]}")
        [[ -s "$iso" ]] || die "Build completed but selected ISO is missing or empty: $iso"
        [[ "$action" != build ]] || return 0
    else
        ((${#make_args[@]} == 0)) || die 'Make arguments apply only to build and test'
        [[ -s "$iso" ]] || die "Selected ISO is missing or empty; build it first: $iso"
    fi

    command -v python3 >/dev/null 2>&1 || die 'Missing required program: python3'
    case "$test_root" in
        /*) ;;
        *) test_root="$PWD/$test_root" ;;
    esac
    exec python3 "$REPO_DIR/scripts/test-vm.py" new --variant "$variant" --iso "$iso" --test-root "$test_root"
}

main "$@"
