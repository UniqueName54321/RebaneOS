#!/usr/bin/env python3
"""Isolated, graphical/manual VirtualBox installation or live-image sessions."""

import argparse
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
import time
import uuid


class RunError(Exception):
    pass


def say(message):
    print("\n==> " + message, flush=True)


def find_vbox():
    executable = shutil.which("VBoxManage") or shutil.which("VBoxManage.exe")
    if executable:
        return executable
    if os.name == "nt":
        program_files = os.environ.get("ProgramFiles", r"C:\Program Files")
        candidate = Path(program_files) / "Oracle" / "VirtualBox" / "VBoxManage.exe"
        if candidate.is_file():
            return str(candidate)
    raise RunError("Missing VBoxManage; install VirtualBox or make VBoxManage available on PATH.")


def command_text(args):
    if os.name == "nt":
        return subprocess.list2cmdline(args)
    return " ".join(shlex.quote(str(arg)) for arg in args)


def require_iso(path):
    if not path.is_file() or path.stat().st_size == 0:
        raise RunError("Selected ISO is missing or empty: " + str(path))


def absolute_path(value):
    path = Path(value).expanduser()
    if not path.is_absolute():
        raise argparse.ArgumentTypeError("expected an absolute path: " + value)
    return path.resolve()


class VMRun:
    def __init__(self, vbox, directory):
        self.vbox = vbox
        self.directory = directory
        self.name = ""
        self.vm = ""
        self.variant = "standard"
        self.iso = None
        self.install_disk = directory / "install.vdi"
        self.data_disk = directory / "data.vdi"

    def invoke(self, *args, capture=False):
        return subprocess.run(
            [self.vbox] + [str(arg) for arg in args],
            check=True,
            stdout=subprocess.PIPE if capture else None,
            encoding="utf-8" if capture else None,
            errors="replace" if capture else None,
        )

    def write(self, name, value):
        (self.directory / name).write_text(str(value) + "\n", encoding="utf-8")

    def read(self, name):
        value = (self.directory / name).read_text(encoding="utf-8").strip()
        if not value:
            raise RunError("Empty run metadata: " + str(self.directory / name))
        return value

    def info(self):
        result = self.invoke("showvminfo", self.vm, "--machinereadable", capture=True)
        values = {}
        for line in result.stdout.splitlines():
            key, separator, value = line.partition("=")
            if separator:
                if value.startswith('"') and value.endswith('"'):
                    value = re.sub(r'\\([\\"])', r'\1', value[1:-1])
                values[key] = value
        return values

    def state(self):
        state = self.info().get("VMState")
        if not state:
            raise RunError("Could not read power state for VM: " + self.name)
        return state

    def require_poweroff(self):
        state = self.state()
        if state != "poweroff":
            raise RunError("VM must be fully powered off (currently: " + state + ").")

    def load(self):
        if not self.directory.is_dir() or not (self.directory / "vm.name").is_file():
            raise RunError("Not a RebaneOS test directory: " + str(self.directory))
        self.name = self.read("vm.name")
        self.vm = self.read("vm.uuid") if (self.directory / "vm.uuid").is_file() else self.name
        if (self.directory / "variant").is_file():
            self.variant = self.read("variant")
        if self.variant not in ("standard", "linux", "browser"):
            raise RunError("Unknown recorded variant: " + self.variant)
        iso_record = "iso.path" if (self.directory / "iso.path").is_file() else "installer-iso.path"
        self.iso = Path(self.read(iso_record))
        if not self.iso.is_absolute():
            raise RunError("Recorded ISO path is not absolute: " + str(self.iso))
        info = self.info()
        config = info.get("CfgFile")
        if not config:
            raise RunError("Cannot establish that the VM belongs to this run (missing CfgFile).")
        try:
            Path(config).resolve().relative_to(self.directory)
        except ValueError:
            raise RunError("Refusing to operate on a VM outside this run directory: " + config)
        if (self.directory / "vm.uuid").is_file() and info.get("UUID") != self.vm:
            raise RunError("Recorded VM UUID does not match VirtualBox.")
        if self.variant == "standard":
            if not self.install_disk.is_file() or not self.data_disk.is_file():
                raise RunError("Expected test disks are missing from " + str(self.directory))
        else:
            require_iso(self.iso)

    def attach(self, port, device, kind, medium):
        self.invoke(
            "storageattach", self.vm, "--storagectl", "IDE", "--port", port,
            "--device", device, "--type", kind, "--medium", medium,
        )

    def start(self):
        self.invoke("startvm", self.vm, "--type", "gui")
        print("\nVM: " + self.name)
        print("Run directory: " + str(self.directory))
        print("Serial log: " + str(self.directory / "serial.log"))
        action = "resume" if self.read("stage") == "installer" else "boot"
        print("After powering off, continue with:\n  " + self.followup(action), flush=True)

    def followup(self, action):
        repository = Path(__file__).resolve().parent.parent
        if os.name == "nt":
            def quote(value):
                return "'" + str(value).replace("'", "''") + "'"
            return ("& " + quote(repository / "rebane.ps1") +
                    " -Action " + action.capitalize() +
                    " -RunDir " + quote(self.directory))
        return command_text([str(repository / "rebane.sh"), action, str(self.directory)])

    def provision(self, variant, iso):
        self.variant = variant
        self.iso = iso
        self.name = "RebaneOS-Test-" + self.directory.name
        self.vm = str(uuid.uuid4())
        self.write("vm.name", self.name)
        self.write("vm.uuid", self.vm)
        self.write("variant", variant)
        self.write("iso.path", iso)
        # Retain the original run format for existing tools and installation runs.
        self.write("installer-iso.path", iso)
        self.write("stage", "provisioning")
        say("Provisioning isolated VM: " + self.name)
        print("TEST DIRECTORY: " + str(self.directory), flush=True)
        self.invoke(
            "createvm", "--name", self.name, "--uuid", self.vm,
            "--ostype", "Other_64", "--basefolder", self.directory, "--register",
        )
        self.invoke(
            "modifyvm", self.vm, "--memory", 4096 if variant == "standard" else 768,
            "--cpus", 1, "--firmware", "bios", "--chipset", "piix3",
            "--x86-pae", "on", "--x86-long-mode", "on",
            "--mouse", "ps2", "--keyboard", "ps2",
            "--nic1", "nat", "--nic-type1", "82540EM", "--cable-connected1", "on",
            "--uart1", "0x3F8", 4, "--uart-mode1", "file", self.directory / "serial.log",
            "--boot1", "dvd", "--boot2", "disk" if variant == "standard" else "none",
            "--boot3", "none", "--boot4", "none",
        )
        self.invoke("storagectl", self.vm, "--name", "IDE", "--add", "ide", "--controller", "PIIX4")
        if variant == "standard":
            self.invoke("createmedium", "disk", "--filename", self.data_disk, "--size", 16, "--format", "VDI")
            self.invoke("createmedium", "disk", "--filename", self.install_disk, "--size", 64, "--format", "VDI")
            self.attach(0, 0, "hdd", self.data_disk)
            self.attach(0, 1, "hdd", self.install_disk)
        self.attach(1, 0, "dvddrive", iso)
        self.write("stage", "installer" if variant == "standard" else "live")
        say("Starting the graphical installer" if variant == "standard" else "Starting the graphical live ISO (" + variant + ")")
        self.start()
        if variant != "standard":
            print("\nManual graphical live-boot session: inspect the guest in VirtualBox.")
            print("This image has no installer workflow; no installed-disk verification is performed.", flush=True)
            return
        print("""
In the VirtualBox window:
  1. Finish the installer preferences.
  2. Press S to skip network setup.
  3. Install onto the 64 MiB PRIMARY-SLAVE disk.
  4. When the installer says "Installed", close the VM window and choose
     "Power off the machine" (NOT "Save the machine state").

Keep this terminal open. After power-off the script checks the installed MBR,
rearranges boot media, and starts the installed system. This is a manual GUI
installation, not an automated installation or a claim that the guest passed.

If interrupted, continue with:
  """ + self.followup("resume"), flush=True)
        self.resume()

    def verify_install(self):
        raw = self.directory / "check-install.raw"
        if raw.exists():
            raise RunError("Refusing to overwrite inspection copy: " + str(raw))
        say("Checking the installed disk for an MBR signature and partition table")
        self.invoke("clonemedium", self.install_disk, raw, "--format", "RAW")
        with raw.open("rb") as stream:
            sector = stream.read(512)
        error = None
        if len(sector) != 512 or sector[510:512] != b"\x55\xaa":
            error = "No 0x55AA boot-sector signature; installation may be incomplete."
        elif not any(sector[446:510]):
            error = "No partition table entries found; installation may be incomplete."
        if error:
            raise RunError(error + "\nInspection copy retained at: " + str(raw))
        print("MBR boot signature and partition table are present (not a full guest test).", flush=True)
        # Only our temporary inspection copy is removed, never either VDI.
        self.invoke("closemedium", "disk", raw)
        raw.unlink()

    def boot_installed(self):
        self.require_poweroff()
        say("Switching to installed-disk boot")
        self.attach(0, 0, "hdd", "none")
        self.attach(0, 1, "hdd", "none")
        self.attach(0, 0, "hdd", self.install_disk)
        # Keep this run's data disk detached, matching the original isolated test.
        self.attach(1, 0, "dvddrive", "emptydrive")
        self.invoke("modifyvm", self.vm, "--boot1", "disk", "--boot2", "none", "--boot3", "none", "--boot4", "none")
        self.write("stage", "installed")
        say("Starting RebaneOS from its installed VDI")
        self.start()

    def resume(self):
        stage = self.read("stage")
        if self.variant != "standard":
            if stage != "live":
                raise RunError("Unrecognised live-image stage: " + stage)
            self.boot()
            return
        if stage == "installed":
            self.boot()
            return
        if stage != "installer":
            raise RunError("Unrecognised installation stage: " + stage)
        print("\nComplete the graphical installer and power off the VM.")
        print("Do NOT save the VM state. Waiting for a complete power-off...", flush=True)
        while True:
            state = self.state()
            if state == "poweroff":
                break
            if state in ("aborted", "saved"):
                raise RunError("VM is " + state + ", not powered off. Resolve in VirtualBox first.")
            time.sleep(2)
        self.verify_install()
        self.boot_installed()

    def boot(self):
        stage = self.read("stage")
        expected = "installed" if self.variant == "standard" else "live"
        if stage != expected:
            raise RunError("Cannot boot stage " + stage + "; use resume to continue installation.")
        self.require_poweroff()
        if self.variant != "standard":
            print("Manual graphical live-boot session; no installed-disk verification.", flush=True)
        self.start()


def main():
    parser = argparse.ArgumentParser(
        description="Create or continue isolated graphical/manual VirtualBox sessions. "
        "Never deletes existing VMs/disks or forcibly powers off a VM."
    )
    commands = parser.add_subparsers(dest="command", required=True)
    new = commands.add_parser("new", help="launch a new installer or live-image VM")
    new.add_argument("--variant", choices=("standard", "linux", "browser"), required=True)
    new.add_argument("--iso", type=absolute_path, required=True)
    new.add_argument("--test-root", type=absolute_path, required=True)
    for command in ("resume", "boot"):
        sub = commands.add_parser(command, help="continue a recorded run without building")
        sub.add_argument("run_dir", type=Path)
    args = parser.parse_args()
    vbox = find_vbox()
    if args.command == "new":
        require_iso(args.iso)
        args.test_root.mkdir(parents=True, exist_ok=True)
        directory = Path(tempfile.mkdtemp(prefix="run-", dir=str(args.test_root))).resolve()
        VMRun(vbox, directory).provision(args.variant, args.iso)
    else:
        run = VMRun(vbox, args.run_dir.expanduser().resolve())
        run.load()
        if args.command == "resume":
            run.resume()
        else:
            run.boot()
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except subprocess.CalledProcessError as error:
        print("ERROR: VirtualBox command failed (exit " + str(error.returncode) + "): " + command_text(error.cmd), file=sys.stderr)
        sys.exit(error.returncode if error.returncode > 0 else 1)
    except (RunError, OSError) as error:
        print("ERROR: " + str(error), file=sys.stderr)
        sys.exit(1)
    except KeyboardInterrupt:
        print("\nInterrupted. The VM and run files are retained; nothing was forcibly powered off.", file=sys.stderr)
        sys.exit(130)
