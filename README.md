# RebaneOS

RebaneOS is a *very* early RockOS fork, created by UniqueName54321 / DifferentDance8.

## Name

RebaneOS was named after [Lyra Rebane](https://lyra.horse/), a programmer known for projects such as [`x86-css`](https://github.com/lyra).

The name originates from an earlier operating system project, **LycaraOS**, which was intended to reference Lyra but was named incorrectly. The exact reason for naming LycaraOS after her in the first place has been forgotten.

Possible explanations include:

- An appreciation for her work on `x86-css`.
- The author's tendency to name projects after relatively obscure internet personalities.
- The amusing possibility of someone discovering that an entire operating system had been named after them for seemingly no reason.

Eventually, the incorrect name became sufficiently irritating that the author decided to revisit the idea, this time using Lyra's actual surname.

RebaneOS is not affiliated with or endorsed by Lyra Rebane.

## Building on Linux and macOS

Run `./build-linux.sh` on Linux or `./build-macos.sh` on macOS. **The macOS
script has not been tested.** Both build `rockosdualbootv.1.iso`, containing
the 64-bit and 32-bit kernels and installer image; they do not launch QEMU.
Run them from any directory. Arguments are passed to GNU Make, for example:

```sh
./build-linux.sh -j4 AUDIO_BOOT_TEST_TONE=0
```

Install GNU Make, Python 3, xorriso, and the Limine host tool and boot files.
Linux also needs GCC and GNU binutils capable of building both x86-64 and
i386 ELF code. The Linux script prefers `x86_64-elf-gcc` and `x86_64-elf-ld`
when both are available, otherwise it uses native `gcc` and `ld`. On macOS,
install an `x86_64-elf` GCC/binutils cross-toolchain; Apple's compiler and
linker do not produce the required ELF kernels. Set `CC` and `LD` to override
the compiler and linker. Set `LIMINE_TOOL` and `LIMINE_DATA_DIR` if Limine is
not installed in its default locations.

The Unix build requires Python 3 with Pillow (`python-pillow` on Arch Linux,
or `python3-pil` on Debian/Ubuntu). It regenerates the logo from
`assets/rockos-logo-outlined.png` into `build/ui_assets.h`, preserving the
checked-in font atlas in `src/drivers/ui_assets.h`. Font changes still require
regenerating the atlas on Windows.

The `make run` instructions below use Windows-specific QEMU audio settings;
the Unix scripts only build the ISO.

## Linux binary compatibility (64-bit)

The 64-bit kernel includes an initial Linux x86-64 ELF compatibility layer:
Linux syscall entry, ELF interpreters and shared libraries, TLS, private user
memory, terminal I/O, and a local X11 core-protocol server. Applications do not
need to be rebuilt for RebaneOS.

Build a separate application ISO:

```sh
./build-linux.sh -j4 AUDIO_BOOT_TEST_TONE=0 linux-iso
qemu-system-x86_64 -machine pc -m 512M -boot d \
    -cdrom build/rebane-linux.iso -serial stdio
```

The first build downloads upstream BusyBox and Alpine Linux v3.22 x86-64
packages into `build/linux/`; it does not install host packages. The generated
`build/linux-root.tar` contains the unchanged applications, musl interpreter,
shared libraries and package data. Package versions, source URLs, licenses and
SHA-256 hashes are recorded in `build/linux/manifest.json`. Downloads are
cached locally; remove that generated cache to resolve a newer package set.
The ordinary installer ISO and its boot configuration remain separate.

At the RebaneOS shell:

```text
runlinux /bin/busybox echo linux-ok
runlinux /usr/bin/xeyes
```

Only one Linux process can run at a time. The initial implementation targets
x86-64 terminal applications and X11 core clients, not arbitrary Linux software:
there is no Wayland, fork/exec process tree, writable Linux root filesystem,
network socket stack, or X11 extension support. Unsupported kernel operations
return Linux error codes. Linux compatibility is not available in the 32-bit
kernel. Linux user mappings cannot overlap the kernel, which is linked at
16 MiB in the 64-bit build.

For boot-driven acceptance checks, build with
`LINUX_CMDLINE=linux_test=busybox` or `LINUX_CMDLINE=linux_test=xeyes` in the
command above. Inspect COM1 output for `linux-ok` and `[linux] exit 0` for
BusyBox; inspect the actual QEMU framebuffer for the xeyes window.
The planar-image regression test runs independently of QEMU:

```sh
gcc -std=c11 -Wall -Wextra -Iinclude tests/linux_x11_host_test.c \
    src/kernel/linux_x11.c -o build/linux_x11_host_test
build/linux_x11_host_test
```

Verified in QEMU: upstream BusyBox 1.35.0 prints `linux-ok` and exits 0;
Alpine xeyes 1.3.0 displays its eyes using the guest X11 server. Xlib may
print a nonfatal missing `XInputExtension` warning because extensions are
not advertised.

Original RockOS readme below:

# RockOS

RockOS is an experimental 64-bit x86 hobby operating system built completely from scratch in C and x86-64 assembly. Version 1 is an early milestone featuring a graphical installer, a Limine bootloader setup, and an interactive desktop environment tailored to run within **Oracle VirtualBox**.

---

## Highlights

* **Boot Support:** Limine BIOS and UEFI boot integration.
* **Video Modes:** Boot menu resolutions include 1024x768 (default), 1920x1080, and 3840x2160 (4K).
* **Kernel Architecture:** 64-bit, 32-bit, and 16-bit kernels featuring interrupt, paging, task, and TSS foundations.
* **Graphical Interface:** Framebuffer-driven UI with a complete graphical installation flow.
* **Desktop Environment:** Includes a start menu, draggable windows, and shortcuts.
* **Peripherals & Drivers:** PS/2 keyboard/mouse input, ATA PIO storage, a basic filesystem, and Intel AC'97 audio support.
* **Security:** Isolated Ring 3 proof-of-concept architecture.

> **Warning:** RockOS is a work in progress intended strictly for virtual machines like Oracle VirtualBox. **Do not** run the installer on a physical hard drive. 

>
> *Encountered bugs? Reach out on Discord via `@spaminhaler_30401`.*

---

## System Requirements

| Resource | Minimum | Recommended | Notes |

| :--- | :--- | :--- | :--- |

| **RAM** | 3 GB | 13 GB | 3 GB is the baseline; 13 GB provides ample headroom for high-resolution buffers and future Wine porting workloads. |

| **Storage** | 30 MB virtual disk | 128 MB+ virtual disk | Accommodates the 26 MiB install target and ISO. |

The kernel requires a 64-bit x86 CPU and a machine-readable framebuffer provided by Limine. 



---



## Building the ISO

To compile RockOS, whether for making your own distro or branch, you need a development environment equipped with GNU Make, an `x86_64-elf` GCC/binutils cross-toolchain, Python 3, xorriso, and Limine boot files.
To build the bootable ISO without launching external emulators:



```sh

make

```
To disable the optional boot test tone during compilation:



```sh

make AUDIO_BOOT_TEST_TONE=0

```



---



## Oracle VirtualBox Setup Guide

Because RockOS currently relies on legacy ATA controllers, configure your VirtualBox virtual machine according to the following specifications:

### 1. VM Configuration Settings

* **Name / OS Type:** Type: `Other`, Version: `Other/Unknown (64-bit)`
* **Processor / CPU:** Enable **VT-x/AMD-V** hardware virtualization. *(Note: On Windows hosts, Hyper-V or WSL2 can sometimes interfere with nested virtualization).*
* **Memory (RAM):** Set to at least 3 GB (or lower e.g., 512 MB – 1 GB for basic testing).
* **Firmware:** Compatible with both BIOS and EFI (test both if needed).
* **Audio:** Enable audio output (emulates an Intel AC'97 device).



### 2. Storage Configuration (Crucial)

* **Disk Controller:** Must use **IDE (PIIX3 / PIIX4)**. *SATA and NVMe controllers are not supported in V.1.*

* **Virtual Disks:** 

  * The installer image requires an install-target disk of at least 16 MiB (the default build generates a 64 MiB install disk + a separate 16 MiB data disk).
  * Attach the generated RockOS ISO file to the virtual optical drive.
  * Set boot priority so the VM boots from the optical drive or install target.

---

## Using RockOS



Once booted into the desktop environment, use the following shortcuts and navigation tips:

* **Desktop Toggle:** Press `Ctrl + Alt + D` to open/close the desktop interface (`Ctrl + Alt + Q` to exit).
* **Start Menu / Icons:** Access the File Manager, Terminal, System Info, and About windows.
* **Window Management:** Title bar buttons minimize, maximize, and close windows. 
  * `Tab` cycles through active windows.
  * `Esc` closes active menus, windows, or the desktop overlay.
  * `F1` displays desktop help.
  * `Ctrl + Alt + M` minimizes the currently focused window.

* **Terminal & Navigation:** 

  * Terminal retains Up/Down command history.
  * File Manager supports Arrow keys, `PgUp`/`PgDn`, `Home`/`End`, `Enter` to open, and `Backspace` to go up a directory.

* **Audio Check:** If audio playback is missing, verify that VirtualBox audio is enabled and use the RockOS shell command `audio` to inspect AC'97 initialization and DMA status.
---

## Troubleshooting

* **Guru Meditation Error on Boot:** Verify that the VM is configured as **64-bit** and that **VT-x/AMD-V** is enabled in your CPU settings.
* **Boots Straight to Shell:** The virtual disk is likely attached to a SATA/NVMe controller instead of **IDE**. Check serial debug logs (`COM1 -> Raw File`) for probe errors like `[ata] probe ... none`.
* **Installer Disk Overwrites:** The installer targets the ATA primary-slave disk by default to preserve the primary-master data disk. If no slave disk is present, it will prompt to use the primary-master disk (which will be formatted).

---

## Version History & Assets

* **v1.0:** Marks the first public milestone of RockOS. `main` remains the stable V.1 branch, while ongoing development takes place on `v2.0-dev`.
* **Third-Party Assets:** The bundled Selawik font is distributed under the terms of the SIL Open Font License 1.1.
