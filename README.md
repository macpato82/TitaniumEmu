# Elesar Titanium (TI AM5728) — RISC OS 5 Emulator

An **experimental** QEMU machine type, `titanium`, that models enough of the
TI **AM5728** (Cortex-A15, ARMv7-A) — the SoC on Elesar's **Titanium** board —
to boot the **RISC OS 5** Titanium ROM.

> Status: **early bring-up.** The real RISC OS 5.31 Titanium ROM's boot
> first-stage executes and runs through CPU/secure setup and PRCM clock
> configuration; it currently stops at a DPLL lock-status poll (next task).
> See [Status](#status).

This is a fresh QEMU board port — *not* related to RPCEmu, which emulates the
1990s Risc PC (ARMv3/v4) and cannot host an ARMv7-A SoC.

## Why QEMU (not RPCEmu)

The Titanium is a Cortex-A15 (ARMv7-A) machine with a completely different SoC
and peripherals from the Risc PC. QEMU already provides a production Cortex-A15
core, GIC, generic timer and device framework, so a Titanium board is a
*machine-model port* rather than a from-scratch emulator.

## Contents

- `hw/arm/titanium.c` — the machine model (also provided as a patch).
- `0001-hw-arm-add-titanium-am5728-machine.patch` — apply to **QEMU v10.2.1**
  (adds `titanium.c`, wires `hw/arm/meson.build` and `hw/arm/Kconfig`).
- `docs/boot-notes.md` — analysis of the AM5728 GP boot as the ROM performs it.

## AM5728 memory map modelled (from the TRM)

| Region | Base | Notes |
|---|---|---|
| DDR (EMIF1 CS0) | `0x80000000` | 1 GiB |
| OCMC_RAM1 | `0x40300000` | on-chip SRAM; GP first-stage runs here |
| GIC (MPU_INTC) | `0x48210000` | GICD `+0x1000`, GICC `+0x2000` |
| UART1 (HAL debug) | `0x4806A000` | 16550-compatible |
| QSPI flash window | `0x5C000000` | full ROM image mapped here |
| L4 peripheral stub | `0x44000000` | **placeholder RAM** (to be replaced by PRCM/CTRL/EMIF devices) |

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
./build/qemu-system-arm -M titanium -bios TITANIUM.ROM -nographic
```

## Status

Working:
- `titanium` machine (single Cortex-A15, as RISC OS uses) constructs the AM5728
  core: GIC, 2 GB DDR, OCMC, UART, QSPI window, and an L4 PRCM/clock stub.
- Emulated GP mask-ROM boot loads and enters the ROM's first-stage.
- First-stage runs all the way through: Secure SVC setup → `SCTLR`/`ACTLR`
  config → early secure `SMC` (via QEMU PSCI) → DPLL lock + module-clock polls
  (satisfied by the PRCM stub) → **EMIF/DDR init** → **loads the HAL from the
  QSPI window into DRAM and jumps to it**.
- The HAL is staged in DDR (≈`0xC0000000`), **enables the MMU**, and runs at
  its linked address **`0xFC000000`** — i.e. into HAL device initialisation.
- Currently stops at an OMAP peripheral **soft-reset wait** (write `SOFTRESET`,
  poll the bit to self-clear) during device init.

Next (Phase 2 continued):
- This is the limit of the generic PRCM/status heuristic: each peripheral the
  HAL now touches (timer, watchdog, UART, ...) needs its real reset/status
  semantics. Implement proper device models — starting with the OMAP
  SYSCONFIG/SYSSTATUS reset handshake and the timers — to carry the HAL to its
  first `HAL_Debug` UART output (the banner).
- Then full PRCM/CTRL/EMIF models, DSS display, MMC/SD, USB (xHCI), CPSW.

### Boot progress reached (this milestone)

`reset → OCMC first-stage → clocks/DPLLs → DDR init → HAL loaded from QSPI →
HAL in DDR → MMU on → HAL at 0xFC000000 (device init)` — ~700 basic blocks,
no aborts. See `docs/boot-notes.md`.

## Credits / licences

- QEMU: GPL-2.0-or-later. `titanium.c` is licensed the same.
- RISC OS 5 (the ROM you supply) is from [RISC OS Open](https://www.riscosopen.org/)
  under the Apache-2.0 shared-source licence; the HAL source used as the boot
  reference is [HAL_Titanium](https://gitlab.riscosopen.org/jlee/HAL_Titanium).
- AM5728 register/address details from TI's AM5728 TRM.

Developed by RISCOS Technologies
