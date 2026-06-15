# RISC OS Titulator — Based on the A15 ARM Processor


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
