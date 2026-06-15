# RISC OS Titulator — Based on the A15 Titanium

An **experimental** QEMU machine type, `titanium`, that models enough of the
TI **AM5728** (Cortex-A15, ARMv7-A) — the SoC on Elesar's **Titanium** board —
to boot the **RISC OS 5** Titanium ROM.

> Status: **RISC OS boots to the desktop.** The real RISC OS 5.31 Titanium ROM
> runs through the AM5728 GP boot, the HAL, MMU/CAM setup, CMOS, IRQs, the OS
> tick, USB, the GC320 video/HDMI bring-up and the **entire module set** to
> `mod init done` — after which it starts the desktop (which renders to the
> DISPC framebuffer). Every blocker from page-table setup through the display
> driver has been resolved. See [Status](#status).

This is a fresh QEMU board port — *not* related to RPCEmu, which emulates the
1990s Risc PC (ARMv3/v4) and cannot host an ARMv7-A SoC.

## Why QEMU (not RPCEmu)

The Titanium is a Cortex-A15 (ARMv7-A) machine with a completely different SoC
and peripherals from the Risc PC. QEMU already provides a production Cortex-A15
core, GIC, generic timer and device framework, so a Titanium board is a
*machine-model port* rather than a from-scratch emulator.

## Contents

- `hw/arm/titanium.c` — the machine model (also provided as a patch).
- `hw/display/titanium_dispc.c` — minimal DSS/DISPC framebuffer scan-out device
  (8bpp CLUT, 16/24/32bpp via the VID3 overlay, hardware-cursor compositing).
- `hw/net/titanium_cpsw.c` — **experimental, work-in-progress** CPSW Ethernet
  NIC. The machine boots with it attached, but the RISC OS EtherCPSW driver
  aborts when networking is actually configured/used; not yet functional.
- `0001-hw-arm-add-titanium-am5728-machine.patch` — apply to **QEMU v10.2.1**
  (adds the machine, DISPC, the experimental CPSW NIC, and the AHCI
  port-multiplier soft-reset fix for the SATA boot disc; wires
  `meson.build`/`Kconfig`).
- `docs/boot-notes.md` — detailed analysis of the AM5728 boot as the ROM
  performs it, and the debug-ROM diagnosis of each blocker.

## AM5728 memory map / devices modelled (from the TRM)

| Region | Base | Notes |
|---|---|---|
| DDR (EMIF1) | `0x80000000` | 2 GiB (HW size; first-stage needs the 2nd GB) |
| OCMC_RAM1 | `0x40300000` | 512 KiB on-chip SRAM; GP first-stage runs here |
| OCMC_RAM2/RAM3 | `0x40400000` | 2 MiB on-chip SRAM — **HAL reports these as RAM banks**; backing them fixed the CAM blocker |
| GIC (MPU_INTC) | `0x48210000` | GICD `+0x1000`, GICC `+0x2000` |
| UART1 (HAL debug) | `0x4806A000` | 16550 core + OMAP extension regs |
| I2C1 / I2C4 / I2C5 | `0x48070000` / `0x4807A000` / `0x4807C000` | OMAP I2C master (interrupt-driven); I2C1 carries CMOS/RTC/PMIC |
| DSS / DISPC | `0x58001000` | framebuffer scan-out (renders once RISC OS sets a mode) |
| QSPI flash window | `0x5C000000` | full ROM image mapped here |
| L4 peripheral stub | `0x44000000` | backed window + synthesised PRCM/clock/reset status |

## Build

```sh
# 1. Get QEMU v10.2.1
git clone --depth 1 --branch v10.2.1 https://github.com/qemu/qemu.git
cd qemu

# 2. Apply this machine
patch -p1 < /path/to/0001-hw-arm-add-titanium-am5728-machine.patch

# 3. Configure + build (ARM target)
./configure --target-list=arm-softmmu --disable-docs --disable-tools
ninja -C build qemu-system-arm
```

## Run

You must supply the **RISC OS 5 Titanium ROM** (`TITANIUM.ROM`); it is not
included here. The machine emulates the AM5728 mask-ROM GP boot: it reads the
ROM's TI GP header, loads the first-stage into OCMC RAM, and starts the CPU.

```sh
./build/qemu-system-arm -M titanium -bios TITANIUM.ROM -serial stdio -display none
```

The production ROM is built `Debug=FALSE` and emits **no** serial output. To see
the boot narrate over the UART you need a `Debug=TRUE` ROM (HAL `Debug` +
kernel `DebugROMInit`/`DebugHALTX`), built from the RISC OS Open source — this
is how the blockers below were diagnosed.

## Status

**Boots all the way to the RISC OS desktop.** With a `Debug=TRUE` ROM the serial
log runs from the GP boot through the HAL, the kernel, and the *entire* module
set (≈200 modules) to `mod init done`, after which RISC OS starts the desktop
(graphical output goes to the DISPC framebuffer, not serial):

```
InitARM done · Entered HAL_Init · HAL initialised · CAM mapped
InitCMOSCache done · IRQs on · HAL_InitDevices · ModuleInitForKbdScan
... USBDriver · XHCIDriver · RTSupport ...
ModuleInit: WindowManager · Desktop · GC320Video · SATADriver · SDIODriver ...
... BASIC · FontManager · SoundDMA · Toolbox · Window · Iconbar · CDFS ...
mod init done        <- whole module set initialised; desktop starts
```

To see the desktop, run with a real display backend (`-display gtk` or `sdl`);
the DISPC device scans out the framebuffer RISC OS sets up.

### Blockers fixed

- **GP boot / secure setup** — keep EL3 (the GP boot runs Secure); route `SMC`
  through QEMU PSCI; back the L4 peripheral window.
- **PRCM / clocks** — a real Clock Manager model (DPLL/APLL lock via read-only
  IDLEST, `CLKCTRL` IDLEST from `MODULEMODE`, SerDes/PHY ready) so clock
  bring-up advances correctly instead of on blind fakes.
- **OMAP UART** — 16550 core + OMAP extension regs (MDR1/SSR/MVR/SYSC/SYSS) so
  HAL/kernel serial output works.
- **The CAM / `0xF9BFFFF0` blocker** — the kernel built the 8 MiB CAM (page map)
  but faulted forever writing it. A debug ROM dumped the HAL `PhysRamTable` and
  revealed an **unbacked OCMC_RAM2/RAM3 bank at `0x40400000`** the kernel was
  allocating page tables from. Backing it fixed the fault.
- **CMOS / I2C** — `InitCMOSCache` reads the CMOS EEPROM over I2C, and the HAL
  drives the transfer from the **I2C interrupt** (it waits, doesn't poll). Added
  a minimal interrupt-driven **OMAP I2C controller** model (status-bit
  sequencing + GIC line) so the read completes and the kernel runs on.
- **OS tick** — modelled the **OMAP DMTIMER** (20 MHz, periodic overflow IRQ),
  so `MonotonicTime` advances and `RTSupport` (which waits for a tick) completes.
- **USB** — register-level **xHCI** model (caps + reset/run handshake + empty
  root hub) so `XHCIDriver` init completes.
- **Display (GC320Video)** — DSS/HDMI bring-up: HDMI_WP soft-reset, the DSS
  video/HDMI **ADPLLs** (lock + `PLL_GO`), DISPC `GO` self-clear, a periodic
  DISPC **VSYNC interrupt**, a generated **EDID** on the DDC I2C buses, a real
  read/write board **EEPROM**, and the **GC320 GPU** idle status.
- **Storage** — SATA AHCI reset self-clear, SD **PBIAS** supply-good, and OMAP
  **HSMMC** clock-stable, so `SATADriver`/`SDIODriver` init complete.

### Result

The whole RISC OS module set initialises (`mod init done`) and RISC OS boots to
the **graphical desktop**, shown live with `-display sdl`. With a USB keyboard
attached RISC OS detects it and boots interactively.

### USB input (keyboard / mouse)

USB1's xHCI host is a **real QEMU xHCI controller** (`sysbus-xhci`) at the DWC3
host base `0x48890000` (its 0x4000 MMIO sits below the DWC3 global registers at
`+0xC100`, which the L4 stub still serves for the HAL). Attach a keyboard and a
**relative** mouse:

```sh
./build/qemu-system-arm -M titanium -bios TITANIUM.ROM -display sdl \
    -device usb-kbd -device usb-mouse
```

RISC OS's `XHCIDriver` enumerates both devices through the real controller, and
**host keyboard *and* mouse input reach RISC OS end-to-end** (verified visually).
Use the **relative** `usb-mouse` (RISC OS does not drive the absolute `usb-tablet`).

The mouse pointer is rendered by a **DISPC hardware-cursor overlay**, not drawn
into the GFX framebuffer: RISC OS's video driver advertises
`GVDisplayFeature_HardwarePointer` and programs a 32x32 ARGB sprite on a DISPC
VID overlay (base/position/size at `0x14C`/`0x154`/`0x158`), moving it by
rewriting the overlay position each frame. `titanium_dispc.c` therefore
**composites that overlay on top of the GFX layer** (alpha-blended) — without it
the pointer is invisible even though the mouse works. The pointer arrow now
appears and tracks the mouse, and clicks reach the Wimp.

**Display / host input on Wayland.** If you run `-display sdl` on a Wayland
session (e.g. KDE) and input does nothing even after clicking to focus and
`Ctrl+Alt+G` to grab, SDL's native-Wayland backend may not forward events. Force
it through XWayland:

```sh
SDL_VIDEODRIVER=x11 ./build/qemu-system-arm -M titanium -bios TITANIUM.ROM \
    -display sdl -device usb-kbd -device usb-mouse
```

VNC is also supported (`-vnc 127.0.0.1:1`) as a backend-independent display path,
which is the most reliable option when a Wayland compositor will not forward
input to the SDL window. (VNC sends absolute pointer coordinates; QEMU converts
them to relative motion for the `usb-mouse`.)

Possible follow-ups: a real colour CLUT for the 8bpp framebuffer (currently
grayscale), and tidying the synthesised L4 status heuristics into proper device
models.

### Methodology

Each blocker was cracked with a **`Debug=TRUE` ROM** (built from the RISC OS Open
sources under RPCEmu + DDE) whose kernel `DebugReg` probes dump live state
(`PhysRamTable`, CAM addresses, I2C registers) over the emulated serial port —
turning blind trace-debugging into precise, source-level diagnosis.

## Credits / licences

- QEMU: GPL-2.0-or-later. `titanium.c` / `titanium_dispc.c` are licensed the same.
- RISC OS 5 (the ROM you supply) is from [RISC OS Open](https://www.riscosopen.org/)
  under the Apache-2.0 shared-source licence; the HAL/kernel sources used as the
  boot reference are from [RISC OS Open](https://gitlab.riscosopen.org/).
- AM5728 register/address details from TI's AM5728 TRM.

Developed by RISCOS Technologies
