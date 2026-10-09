# DiskForge Rescue

A USB stick that starts any PC into a small Linux desktop with DiskForge on it, plus a few other repair
tools. It's for when the PC's own system won't start, when a drive is dying and you don't want to boot from
it, or when it's a Windows PC and you'd rather not install anything on it.

What's on the desktop: DiskForge, PhotoRec (Recover Files), TestDisk, a file manager, Firefox, a terminal, a
task manager and a text editor. The boot menu also has memtest86+ for testing the memory.

Nothing on the PC changes unless you tell it to. It doesn't mount the PC's drives by itself, doesn't use its
swap, doesn't TRIM anything, and closing a laptop's lid doesn't put it to sleep in the middle of a copy.

## Making the stick
In DiskForge: File → Make a Rescue USB, pick the ISO and the stick. That makes one FAT32 partition and copies
the ISO's files onto it, checking each one. The stick stays readable everywhere (Windows too), there's room
left over for files, and the rescue system writes its logs there.

The ISO can also go onto a stick as-is (Write Image to USB, Rufus, balenaEtcher, dd). That stick also starts
old BIOS-only PCs, but it's read-only, so it can't keep logs.

## Secure Boot
It starts with Secure Boot on. The first steps are Debian's signed boot files, exactly as Debian ships them:
shim (signed by Microsoft), then GRUB and the Linux kernel (signed by Debian). Everything after that is ours.

Two things don't work with Secure Boot on:
- **memtest86+** isn't signed, so the menu says to turn Secure Boot off for it.
- **The menu's picture:** Debian's GRUB won't load fonts with Secure Boot on, so the menu is plain text.

## Logs
Every start leaves a folder in `logs/` on the stick, named by date, time and the PC's model:

| File | What's in it |
|---|---|
| `summary.txt` | start here: the PC, UEFI or BIOS, Secure Boot on or off, graphics, network, drives, failed services, error count |
| `journal.txt` | everything the system logged since it started |
| `errors.txt` | just the warnings and errors |
| `kernel.txt` | the kernel's own messages |
| `hardware.txt` | dmidecode, lscpu, lspci, lsusb, UEFI boot entries |
| `disks.txt` | lsblk, blkid, smartctl for every drive |
| `udisks.txt` | what UDisks sees (what DiskForge works from) |
| `network.txt` | NetworkManager and addresses |
| `desktop.log`, `Xorg.0.log` | the desktop starting up, or why it didn't |
| `diskforge.log` | what DiskForge did (`--log`) |

They're saved a couple of minutes after starting, every 3 minutes after that, when shutting down, and when
you click Save Logs. The stick is only writable for the moment they're copied, so pulling it out at the
wrong time is unlikely to break it. The 30 newest are kept.

The logs have the PC's and the drives' serial numbers in them. Look before posting them somewhere public.

When the ISO runs as a CD (a VM's CD drive) it can't write to itself. Two ways around that:
- Plug in a stick made from the same ISO (in VMware: VM → Removable Devices → the stick → Connect). The
  logs go to that stick.
- Pick "Detailed messages" in the boot menu and give the VM a serial port that writes to a file: the
  kernel and the system log go there as they happen. In VMware: VM Settings → Add → Serial Port → Use
  output file.

## Building it
```
rescue/build.sh
```
It needs podman and about 10 GB free. No root: it all happens in a rootless container, nothing gets
installed on your PC. The first build downloads about 1.5 GB of Debian packages and keeps them in
`~/.cache/diskforge-rescue`. The ISO ends up in `rescue/out/`, about 1.2 GB.

`rescue/build.sh --no-app` reuses the DiskForge .deb from the last build, when only the rescue files changed.

What's where:

| | |
|---|---|
| `rescue/build.sh` | builds DiskForge's .deb, the builder container, then runs `make-image.sh` in it |
| `rescue/Containerfile` | the builder: Debian 13 with mmdebstrap, xorriso, GRUB, shim, memtest86+ |
| `rescue/make-image.sh` | makes the system with mmdebstrap, packs it with squashfs, puts the ISO together |
| `rescue/setup.sh` | runs inside the new system: the user, the desktop icons, services, the initrd |
| `rescue/packages.txt` | everything that goes in, with notes |
| `rescue/files/` | copied over the new system as-is (configs, the log saver, launchers) |
| `rescue/boot/` | the boot menu |
| `rescue/art/` | the boot menu and desktop pictures |
| `rescue/README.txt` | the README on the stick |

Each build gets its own ID. The stick, the initrd and GRUB all check it, so a second live stick in the same
PC can't get mixed up with this one.

## Testing it in a VM
QEMU with Secure Boot on, the way a normal PC has it (Microsoft's keys enrolled). Debian's `ovmf` package has
the firmware files (`OVMF_CODE_4M.secboot.fd` and `OVMF_VARS_4M.ms.fd`):
```
cp OVMF_VARS_4M.ms.fd vars.fd
qemu-system-x86_64 -enable-kvm -cpu host -m 4096 -machine q35,smm=on \
  -global driver=cfi.pflash01,property=secure,value=on \
  -drive if=pflash,format=raw,unit=0,readonly=on,file=OVMF_CODE_4M.secboot.fd \
  -drive if=pflash,format=raw,unit=1,file=vars.fd \
  -cdrom rescue/out/diskforge-rescue-*.iso -serial file:serial.log
```
A stick made with Make a Rescue USB can be tested the same way: make it on an image file (as a loop device,
see `rescueUsb()` in `tests/uitest.cpp`), then give QEMU
`-drive file=stick.img,format=raw,if=none,id=s -device qemu-xhci -device usb-storage,drive=s`.

In VMware: a new VM for "Other Linux 6.x kernel 64-bit", then VM Settings → Options → Advanced → Firmware type
UEFI, with Secure Boot ticked. Give it at least 2 GB of memory (4 is better) and the ISO as its CD.
