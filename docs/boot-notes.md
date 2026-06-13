# AM5728 boot notes (as the RISC OS Titanium ROM performs it)

Derived from tracing `TITANIUM.ROM` (RISC OS 5.31) under the `titanium` QEMU
machine, cross-referenced with the AM5728 TRM and RISC OS Open `HAL_Titanium`.

## ROM image format (TI GP boot)

The ROM begins with an 8-byte TI **GP header**:

| Offset | Field | Value in TITANIUM.ROM |
|---|---|---|
| 0 | image size | `0x00000800` (2 KiB first-stage) |
| 4 | dest address | `0x40300008` (OCMC_RAM1) |
| 8 | first-stage code | begins `B <reset>` |

The SoC mask ROM copies `size` bytes to `dest` and jumps there. The first-stage
(`HAL_Titanium/s/Top`, position-independent) sets up the DPLLs, DDR, pin mux and
UART, then loads the rest of the ROM from QSPI flash. We emulate this mask-ROM
step in `titanium_load_rom()`.

## Observed first-stage sequence

1. `MSR CPSR_c, #0xD3` — Secure SVC, IRQ/FIQ masked.
2. `MRC/MCR p15 … c1,c0,0` — clear `SCTLR.M/C/I` (MMU + caches off).
3. `… c1,c0,1` — `ACTLR` config. **Requires Secure state / EL3** — with
   `has_el3=false` this faulted Undefined.
4. Early `SMC` — on real silicon the AM5728 secure ROM services it. We route
   SMC through QEMU's PSCI conduit so it returns instead of trapping to an
   unset monitor vector.
5. PRCM clock-control writes (e.g. `CM_*_CLKCTRL` around `0x4A009840`), then a
   **DPLL lock poll**:

   ```
   loop: LDR  r2, [r1, #4]   ; DPLL status
         TST  r2, #1         ; lock bit
         BEQ  loop
   ```

   A plain RAM stub never sets the lock bit, so this spins — this is the
   current stopping point. A PRCM/DPLL device that reports lock (and module
   IDLEST ready) is the next requirement.

## Clock targets (from `HAL_Titanium/s/Top`)

- Per DPLL → 768 MHz (256 MHz FUNC, 192 MHz DSS, 64 MHz QSPI)
- Core DPLL → 2128 MHz
- MPU DPLL → 2000 MHz (1000 MHz core clock)
- DDR DPLL → 2128 MHz (532 MHz EMIF)

## QEMU bring-up gotchas hit

- Keep EL3 (Cortex-A15 default) — GP boot runs Secure.
- `psci-conduit = SMC` so early SMCs don't dead-end at the monitor vector.
- Back the L4 peripheral space or accesses raise external (data) aborts.
- Secondary cores: RISC OS uses a single A15, so the machine is single-core.

## CRITICAL FINDING: no UART output in the production ROM

`HAL_DebugTX` (s/Init) is wrapped in `[ Debug ... ]` conditional assembly. The
shipped TITANIUM.ROM is built with `Debug = FALSE`:
- `HAL_DebugTX` compiles to a bare `MOV pc, lr` (no-op) — the HAL never writes
  the UART.
- The debug strings ("Entered HAL_Init", "Registering devices") are **not even
  present** in the ROM (verified with grep) because the `DebugTX` macro emits
  nothing.

Implication: **there is no UART banner to reach with this ROM.** Chasing
HAL_Debug serial output is a dead end. RISC OS's visible output goes to the
screen via the **DSS display subsystem** (→ TFP410 → HDMI), not the serial port.

The realistic path to visible output is therefore: complete HAL device init +
RISC OS kernel boot, and model the **DSS/DISPC** so the framebuffer RISC OS sets
up in DRAM can be displayed. This is a large, multi-peripheral effort, not a
matter of a few more register stubs. A debug-enabled ROM build (Debug=TRUE),
if obtainable from RISC OS Open, would be the only way to get serial output.
