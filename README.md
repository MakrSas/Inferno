# Inferno, iOS port

The emulator half of [Inferno for iPhone](https://github.com/MakrSas/Inferno-iOS): a fork of
[Inferno](https://github.com/ChefKissInc/Inferno) by ChefKiss, carrying the changes needed to run it
as a library inside an iPhone app. Build it, and the app, by the instructions there.

> **Unofficial.** Not affiliated with or endorsed by ChefKiss. Inferno is by Visual Ehrmanntraut and
> the Inferno team; if it is useful to you, consider [supporting them](https://ko-fi.com/chefkiss).
> For Inferno itself, see [its repository](https://github.com/ChefKissInc/Inferno) and
> [the guides](https://chefkiss.dev/guides/inferno/).

## What this branch changes

The `ios` branch sits on Inferno's `dev`.

- **A library, not a program.** `-Dshared_lib=true` builds `libqemu-aarch64-softmmu.dylib`, and the
  app calls `qemu_init` and `qemu_main_loop` itself. `CONFIG_IOS` tells iOS from macOS where their
  APIs differ. (`meson.build`, `meson_options.txt`, `system/main.c`, `include/qemu/osdep.h`,
  `block/file-posix.c`)
- **Coroutines without signals.** iOS has no usable `makecontext`, and the `sigaltstack` backend
  relies on `SIGUSR2`, which the debugger that JIT requires swallows. Coroutines use libucontext.
  (`util/coroutine-ucontext.c`)
- **JIT under TXM.** On iOS 26 and later the code buffer is mapped executable first and handed to the
  attached debugger to bless, the way UTM does it. (`tcg/region.c`)
- **The screen, in process.** Display and input for the embedding app, read straight from the
  emulator's framebuffer rather than through VNC. (`ui/inferno-embed.c`, `include/ui/inferno-embed.h`)
- **A USB host for networking.** The guest's USB port is answered in process by a CDC-NCM host that
  hands its Ethernet to `-netdev user`, so the guest gets internet with no companion VM.
  (`hw/net/apple-ncm-host.c`)
- **A console that does not stall.** UART output is gathered and written in bursts, instead of two
  system calls per character from the vCPU thread. (`hw/char/apple_uart.c`)
- **Less memory per address space.** Subpage section tables use a coarse grid refined on demand,
  which saves hundreds of megabytes on Apple machines. (`system/physmem.c`)
- **Display size and scale as machine properties**, with the touch surface derived from them.
  (`hw/arm/t8030.c`, `hw/input/mt-spi.c`)
- **Android builds** against bionic. (`meson.build`, `util/oslib-posix.c`, `system/main.c`)
- **No ChefKiss branding.** The boot splash artwork and the "ChefKiss Inferno" name are removed, as
  [the branding notice](ui/icons/CKBrandingNotice.md) requires of forks, and the machine starts
  without a splash. The name is gone from the version, the window and VNC titles and the device
  tree; the copyright lines crediting the Inferno team stay. (`hw/display/apple_displaypipe_v4.c`,
  `ui/icons/`, `VERSION`, `hw/arm/boot.c`, `system/vl.c`, `ui/`)

## Issues

Problems with this branch belong in [Inferno for iPhone's issues](https://github.com/MakrSas/Inferno-iOS/issues),
not upstream: ChefKiss did not write these changes.

## Licence

As Inferno's: GPL-3.0 for the project as a whole, ChefKiss's own code under AGPL-3.0, and code
inherited from QEMU under its original terms. Each file says which. New files in this branch are
AGPL-3.0-or-later. See [`LICENSE`](LICENSE).

Provided as is, without warranty of any kind. No Apple firmware, image or key is distributed here.
Apple and the QEMU project are not affiliated with this fork.
