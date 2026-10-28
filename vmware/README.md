# VMware Player

The `Anix.vmx` profile boots the project's hybrid BIOS/UEFI ISO through VMware's EFI firmware. It provides 1 GiB RAM, one vCPU, SVGA, a SATA CD-ROM, and a COM1 serial log. Secure Boot is disabled because the Limine EFI loader is not signed for this project.

Build the shell-first ISO in WSL:

```sh
make iso-shell
```

Then open `vmware/Anix.vmx` with VMware Workstation Player. It points to this workspace's `build/anix-shell.iso`; if you move the workspace, update the ISO and serial log paths in the VMX file. Kernel serial output is written to `vmware/anix-serial.log`.

The VMware profile uses the shell-first image, which skips the SDK fixture suite. The standard `make iso` image retains that suite for QEMU regression testing.

The current kernel can enumerate virtual PCI devices, but it does not yet have a network driver or a VMware SVGA driver. The shell is drawn through the firmware-provided framebuffer and mirrored to serial. Keyboard input currently uses the PS/2-compatible controller. The current scheduler uses one CPU, so the VM profile intentionally allocates one vCPU. VMware boot to the visible shell prompt is verified; physical laptop keyboard support has not been verified on hardware.

To try the shell on an x86-64 laptop, use `make iso-shell`, write `build/anix-shell.iso` to a USB drive in raw/DD mode, then select the UEFI USB boot entry from the firmware's one-time boot menu with Secure Boot disabled. This is an experimental kernel, so test it as a one-time boot and leave the installed operating system untouched. The image includes BIOS and UEFI boot paths, but the kernel currently relies on firmware GOP graphics and a PS/2-compatible keyboard. USB HID, Wi-Fi, touchpad, NVMe, AHCI, and laptop-specific power-management drivers are not implemented. Laptop models that do not expose the built-in keyboard through a PS/2-compatible controller will need a USB keyboard driver before local shell input can work.
