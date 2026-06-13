/*
 * Elesar Titanium machine model (TI AM5728, dual Cortex-A15) - EXPERIMENTAL
 *
 * Phase 1 skeleton for booting RISC OS 5 (Titanium port). Provides just
 * enough of the AM5728 to bring the RISC OS HAL to its first debug output:
 *   - Cortex-A15 cluster
 *   - GIC (the AM5728 "MPU_INTC") at 0x48210000
 *   - DDR (EMIF1 CS0) at 0x80000000
 *   - UART1 (16550-compatible) at 0x4806A000, used by the HAL for HAL_Debug
 *
 * Addresses are taken from the AM5728 Technical Reference Manual and the
 * RISC OS Open HAL_Titanium source. The QSPI staged boot loader is bypassed:
 * the ROM image is loaded directly into DRAM and the CPU started there.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 or later.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/units.h"
#include "hw/sysbus.h"
#include "hw/arm/boot.h"
#include "hw/boards.h"
#include "hw/loader.h"
#include "hw/qdev-properties.h"
#include "system/reset.h"
#include "qemu/error-report.h"
#include "hw/cpu/a15mpcore.h"
#include "hw/arm/machines-qom.h"
#include "hw/char/serial-mm.h"
#include "hw/intc/arm_gic_common.h"
#include "system/system.h"
#include "system/address-spaces.h"
#include "qom/object.h"
#include "target/arm/cpu-qom.h"

/* AM5728 physical memory map (TRM "L4 / device memory map" + HAL_Titanium) */
#define TITANIUM_DRAM_BASE   0x80000000ULL  /* EMIF1 CS0, up to 1 GiB        */
#define TITANIUM_GIC_BASE    0x48210000     /* MPU_INTC: GICD@+0x1000, GICC@+0x2000 */
#define TITANIUM_UART1_BASE  0x4806A000     /* HAL debug UART (UART1)        */
#define TITANIUM_UART1_IRQ   67             /* GIC SPI for UART1 (MPU_IRQ 67) */

#define TITANIUM_OCMC_BASE   0x40300000     /* OCMC_RAM1, 512 KiB on-chip SRAM */
#define TITANIUM_OCMC_SIZE   (512 * 1024)
#define TITANIUM_QSPI_BASE   0x5C000000ULL  /* QSPI flash memory-mapped window  */

/* AM5728 MPU GIC exposes 160 shared peripheral interrupts */
#define TITANIUM_NUM_SPI     160

/* Boot CPU entry point, set from the ROM's TI GP-boot header at init time */
static uint32_t titanium_boot_entry;

static void titanium_cpu_reset(void *opaque)
{
    ARMCPU *cpu = opaque;

    cpu_reset(CPU(cpu));
    cpu_set_pc(CPU(cpu), titanium_boot_entry);
}

/*
 * Emulate the AM5728 mask-ROM GP boot: the ROM image begins with an 8-byte
 * TI GP header [size][dest]. The mask ROM copies `size` bytes (the first-stage
 * loader, s/Top in HAL_Titanium) to `dest` in on-chip RAM and jumps there.
 * The first-stage then sets up clocks/DDR/UART and loads the rest of the ROM
 * from QSPI. We also map the whole ROM at the QSPI window so those reads work.
 */
static void titanium_load_rom(MachineState *machine)
{
    g_autofree uint8_t *data = NULL;
    gsize len = 0;
    g_autoptr(GError) gerr = NULL;
    uint32_t gp_size, gp_dest;

    if (!machine->firmware) {
        warn_report("titanium: no ROM supplied (use -bios TITANIUM.ROM)");
        return;
    }
    if (!g_file_get_contents(machine->firmware, (gchar **)&data, &len, &gerr)) {
        error_report("titanium: cannot read ROM '%s': %s",
                     machine->firmware, gerr->message);
        exit(1);
    }
    if (len < 8) {
        error_report("titanium: ROM too small to contain a GP header");
        exit(1);
    }

    gp_size = ldl_le_p(data);
    gp_dest = ldl_le_p(data + 4);

    if (gp_size == 0 || (gsize)gp_size + 8 > len) {
        error_report("titanium: bad GP header (size=0x%x, file=0x%zx)",
                     gp_size, (size_t)len);
        exit(1);
    }

    /* First-stage loader into on-chip RAM, restored on reset */
    rom_add_blob_fixed("titanium.gpboot", data + 8, gp_size, gp_dest);

    /* The complete ROM, visible at the QSPI memory-mapped window */
    rom_add_blob_fixed("titanium.qspiflash", data, len, TITANIUM_QSPI_BASE);

    titanium_boot_entry = gp_dest;
    qemu_register_reset(titanium_cpu_reset, ARM_CPU(first_cpu));
}

/*
 * PHASE 2: minimal PRCM/clock stub over the L4 peripheral space.
 *
 * The boot first-stage programs DPLLs and module clocks then polls for status.
 * Rather than a full PRCM model, this backs the L4 region with a sparse
 * register store and synthesises the two status patterns the boot waits on:
 *
 *   - DPLL lock: a CM_IDLEST_DPLL register sits 4 bytes after its
 *     CM_CLKMODE_DPLL. When CLKMODE.EN (bits[2:0]) == 7 (DPLL_LOCK), report
 *     ST_DPLL_CLK (bit 0) set in the following word.
 *   - Module IDLEST: CLKCTRL IDLEST bits are never written, so they read back
 *     as 0 (FULL/functional), satisfying "wait for module ready" loops.
 *
 * To be replaced by proper PRCM/CTRL/EMIF device models.
 */
typedef struct {
    GHashTable *regs;   /* word-aligned offset -> last written value */
} TitaniumL4;

static uint64_t titanium_l4_read(void *opaque, hwaddr off, unsigned size)
{
    TitaniumL4 *s = opaque;
    hwaddr woff = off & ~(hwaddr)3;
    gpointer key = GUINT_TO_POINTER((guint)woff);

    if (getenv("TITANIUM_TRACE")) {
        static GHashTable *cnt;
        if (!cnt) {
            cnt = g_hash_table_new(NULL, NULL);
        }
        guint c = GPOINTER_TO_UINT(g_hash_table_lookup(cnt, key)) + 1;
        g_hash_table_insert(cnt, key, GUINT_TO_POINTER(c));
        if (c == 200000) {
            fprintf(stderr, "TITANIUM-L4: spin read at phys 0x%08x\n",
                    (unsigned)(0x44000000 + woff));
        }
    }

    /*
     * A register that software writes is a control register. Two cases:
     *  - CM_CORE_AON clock-domain state control (CM_*_CLKSTCTRL, ~0x4A005000):
     *    the boot writes a clock-domain transition request then polls the same
     *    register's state-status bits [17:16] for completion. Report the
     *    transition done so the poll exits.
     *  - Otherwise return the written value verbatim (so CLKCTRL IDLEST bits
     *    read back as 0 = FULL, satisfying "wait for module ready" loops).
     */
    if (g_hash_table_contains(s->regs, key)) {
        uint32_t v = GPOINTER_TO_UINT(g_hash_table_lookup(s->regs, key));
        if (woff >= 0x06005000 && woff < 0x06006000) { /* CM_CORE_AON state */
            v |= 0x00030000;
        }
        return v;
    }

    /*
     * Peripheral REVISION/IDR register at page offset 0x00. The HAL reads it
     * to confirm a module is present and clocked (waits for nonzero). Scoped to
     * the L4_PER (0x48000000) and L4_WKUP (0x4AE00000) peripheral regions so it
     * doesn't affect PRCM/EMIF control registers that also live at offset 0x00.
     */
    if ((woff & 0xfff) == 0) {
        uint32_t phys = 0x44000000 + (uint32_t)woff;
        if ((phys >= 0x48000000 && phys < 0x49000000) ||
            (phys >= 0x4AE00000 && phys < 0x4B000000)) {
            return 0x50600801;   /* generic OMAP IP revision */
        }
    }

    /*
     * I2C_SYSS at page offset 0x90: bit0 = RDONE (reset done). The HAL resets
     * the I2C controllers (L4_PER, 0x48060000..0x48080000) and polls SYSS for
     * completion. Report reset done.
     */
    if ((woff & 0xfff) == 0x90) {
        uint32_t phys = 0x44000000 + (uint32_t)woff;
        if (phys >= 0x48060000 && phys < 0x48080000) {
            return 1;
        }
    }

    /*
     * A register that is never written but sits immediately after a written
     * (nonzero) control register is a read-only status register the boot is
     * polling - DPLL lock (CM_IDLEST_DPLL after CM_CLKMODE_DPLL), clock/PRM
     * transition-done, etc. Report ready (bit 0 set) so the poll completes.
     */
    if (woff >= 4) {
        uint32_t prev = GPOINTER_TO_UINT(g_hash_table_lookup(s->regs,
                                        GUINT_TO_POINTER((guint)(woff - 4))));
        if (prev != 0) {
            return 1;
        }
    }
    return 0;
}

static void titanium_l4_write(void *opaque, hwaddr off, uint64_t val,
                              unsigned size)
{
    TitaniumL4 *s = opaque;
    hwaddr woff = off & ~(hwaddr)3;

    /*
     * OMAP peripheral soft-reset, scoped to the L4_PER peripheral region
     * (0x48000000..0x49000000: timers, GPIO, I2C, McSPI, ...). Their
     * SYSCONFIG/TIOCP_CFG register is at page offset 0x10 and its SOFTRESET
     * request bit (bit0 DMTimer-style, bit1 type-1 SYSCONFIG) self-clears once
     * reset completes. Model reset as instantaneous by clearing those bits, so
     * the HAL's "write SOFTRESET, poll until clear" loops finish. SYSSTATUS
     * RESETDONE at 0x14 reads back set via the status heuristic. Scoped to
     * L4_PER so it can't disturb PRCM/EMIF registers that also sit at off 0x10.
     */
    {
        uint32_t phys = 0x44000000 + (uint32_t)woff;
        if (phys >= 0x48000000 && phys < 0x49000000 &&
            (woff & 0xfff) == 0x10) {
            val &= ~(uint64_t)0x3;
        }
        /*
         * USB1/USB2 subsystem (USBOTGSS) SYSCONFIG at +0x10: the HAL also polls
         * bit17 (a self-clearing init/standby-transition bit) to clear. Model
         * it as completing instantly.
         */
        if (phys >= 0x48880000 && phys < 0x48960000 &&
            (woff & 0xfff) == 0x10) {
            val &= ~(uint64_t)0x20000;
        }
    }

    g_hash_table_insert(s->regs, GUINT_TO_POINTER((guint)woff),
                        GUINT_TO_POINTER((guint)val));
}

static const MemoryRegionOps titanium_l4_ops = {
    .read = titanium_l4_read,
    .write = titanium_l4_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
};

/*
 * OMAP UART extension registers (above the 16550 core at offset 0x20).
 *
 * The 16550-compatible part (RHR/THR/IER/LCR/LSR/... at 0x00-0x1C) is handled
 * by a serial_mm device, which is what actually emits characters. The OMAP
 * UART adds registers at 0x20+ that the HAL drives during UART setup; modelled
 * here so the HAL's UART init completes and HAL_Debug output works:
 *   0x20 MDR1  - mode select (0 = UART 16x); stored
 *   0x44 SSR   - bit0 TX_FIFO_FULL: report not full (TX ready)
 *   0x50 MVR   - module version: nonzero
 *   0x54 SYSC  - bit1 SOFTRESET self-clears (instant reset)
 *   0x58 SYSS  - bit0 RESETDONE: reset complete
 */
typedef struct {
    uint32_t regs[64];
} TitaniumUartExt;

static uint64_t titanium_uartext_read(void *opaque, hwaddr off, unsigned size)
{
    TitaniumUartExt *u = opaque;
    hwaddr uoff = 0x20 + (off & ~(hwaddr)3);

    switch (uoff) {
    case 0x58: return 1;            /* SYSS: RESETDONE */
    case 0x44: return 0;            /* SSR: TX_FIFO_FULL = 0 (TX ready) */
    case 0x50: return 0x50410805;   /* MVR: module version (nonzero) */
    default:   return u->regs[((off & ~(hwaddr)3) >> 2) & 63];
    }
}

static void titanium_uartext_write(void *opaque, hwaddr off, uint64_t val,
                                   unsigned size)
{
    TitaniumUartExt *u = opaque;
    hwaddr uoff = 0x20 + (off & ~(hwaddr)3);

    if (uoff == 0x54) {
        val &= ~(uint64_t)0x2;      /* SYSC: SOFTRESET self-clears */
    }
    u->regs[((off & ~(hwaddr)3) >> 2) & 63] = (uint32_t)val;
}

static const MemoryRegionOps titanium_uartext_ops = {
    .read = titanium_uartext_read,
    .write = titanium_uartext_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
};

static void titanium_uart_init(MemoryRegion *sysmem, hwaddr base,
                               qemu_irq irq, Chardev *chr)
{
    TitaniumUartExt *uext = g_new0(TitaniumUartExt, 1);
    MemoryRegion *ext = g_new(MemoryRegion, 1);

    /* 16550 core (handles char I/O and emits output) */
    serial_mm_init(sysmem, base, 2, irq, 48000000 / 16, chr,
                   DEVICE_NATIVE_ENDIAN);

    /* OMAP extension registers at base+0x20 (over the L4 stub) */
    memory_region_init_io(ext, NULL, &titanium_uartext_ops, uext,
                          "titanium.uart-ext", 0xE0);
    memory_region_add_subregion(sysmem, base + 0x20, ext);
}

static void titanium_init(MachineState *machine)
{
    MemoryRegion *sysmem = get_system_memory();
    unsigned int smp_cpus = machine->smp.cpus;
    DeviceState *gicdev;
    SysBusDevice *gicbusdev;
    qemu_irq pic[TITANIUM_NUM_SPI];
    int n;

    /* Cortex-A15 cluster */
    for (n = 0; n < smp_cpus; n++) {
        Object *cpuobj = object_new(machine->cpu_type);

        if (object_property_find(cpuobj, "reset-cbar")) {
            object_property_set_int(cpuobj, "reset-cbar",
                                    TITANIUM_GIC_BASE, &error_abort);
        }
        /*
         * Keep EL3 (the Cortex-A15 default): the AM5728 GP boot runs in Secure
         * state, so the first-stage executes secure CP15 operations (ACTLR
         * etc.). Forcing has_el3=false made those trap as Undefined.
         */
        /*
         * Route SMC through QEMU's PSCI emulation. The real AM5728 secure ROM
         * services early SMCs; we don't model that, so without this the first
         * SMC traps to an unset monitor vector and loops in prefetch aborts.
         * With SMC conduit, unknown calls return NOT_SUPPORTED and boot proceeds.
         */
        object_property_set_int(cpuobj, "psci-conduit",
                                1 /* QEMU_PSCI_CONDUIT_SMC */, &error_abort);

        qdev_realize(DEVICE(cpuobj), NULL, &error_fatal);
    }

    /*
     * GIC (MPU_INTC). a15mpcore_priv bundles the GIC and must be created
     * after the CPUs so it can wire to their generic-timer outputs.
     */
    gicdev = qdev_new(TYPE_A15MPCORE_PRIV);
    qdev_prop_set_uint32(gicdev, "num-cpu", smp_cpus);
    qdev_prop_set_uint32(gicdev, "num-irq", TITANIUM_NUM_SPI + GIC_INTERNAL);
    gicbusdev = SYS_BUS_DEVICE(gicdev);
    sysbus_realize_and_unref(gicbusdev, &error_fatal);
    sysbus_mmio_map(gicbusdev, 0, TITANIUM_GIC_BASE);

    for (n = 0; n < TITANIUM_NUM_SPI; n++) {
        pic[n] = qdev_get_gpio_in(gicdev, n);
    }
    for (n = 0; n < smp_cpus; n++) {
        DeviceState *cpudev = DEVICE(qemu_get_cpu(n));

        sysbus_connect_irq(gicbusdev, n,
                           qdev_get_gpio_in(cpudev, ARM_CPU_IRQ));
        sysbus_connect_irq(gicbusdev, n + smp_cpus,
                           qdev_get_gpio_in(cpudev, ARM_CPU_FIQ));
        sysbus_connect_irq(gicbusdev, n + 2 * smp_cpus,
                           qdev_get_gpio_in(cpudev, ARM_CPU_VIRQ));
        sysbus_connect_irq(gicbusdev, n + 3 * smp_cpus,
                           qdev_get_gpio_in(cpudev, ARM_CPU_VFIQ));
    }

    /* DDR */
    memory_region_add_subregion(sysmem, TITANIUM_DRAM_BASE, machine->ram);

    /* On-chip RAM (OCMC_RAM1) - the GP first-stage loader runs here */
    MemoryRegion *ocmc = g_new(MemoryRegion, 1);
    memory_region_init_ram(ocmc, NULL, "titanium.ocmc", TITANIUM_OCMC_SIZE,
                           &error_fatal);
    memory_region_add_subregion(sysmem, TITANIUM_OCMC_BASE, ocmc);

    /* QSPI flash memory-mapped window - holds the full ROM image */
    MemoryRegion *qspi = g_new(MemoryRegion, 1);
    memory_region_init_ram(qspi, NULL, "titanium.qspiflash", 64 * 1024 * 1024,
                           &error_fatal);
    memory_region_add_subregion(sysmem, TITANIUM_QSPI_BASE, qspi);

    /*
     * PHASE 2: minimal PRCM/clock stub over the L4 peripheral space (PRCM,
     * CTRL, pin mux, EMIF cfg, ...). Backs the region so accesses don't abort,
     * and synthesises DPLL-lock / module-ready status (see titanium_l4_*).
     * Real devices (UART, GIC) sit on top at higher priority.
     */
    TitaniumL4 *l4s = g_new0(TitaniumL4, 1);
    l4s->regs = g_hash_table_new(NULL, NULL);
    MemoryRegion *l4 = g_new(MemoryRegion, 1);
    memory_region_init_io(l4, NULL, &titanium_l4_ops, l4s,
                          "titanium.l4stub", 0x18000000);
    memory_region_add_subregion_overlap(sysmem, 0x44000000, l4, -1);

    /* UART1 - the HAL's debug port (16550 core + OMAP extension registers) */
    titanium_uart_init(sysmem, TITANIUM_UART1_BASE,
                       pic[TITANIUM_UART1_IRQ], serial_hd(0));

    /* Boot via the emulated AM5728 mask-ROM GP path */
    titanium_load_rom(machine);
}

static void titanium_machine_init(MachineClass *mc)
{
    mc->desc = "Elesar Titanium (TI AM5728, Cortex-A15) [experimental]";
    mc->init = titanium_init;
    /* RISC OS runs on a single A15 core only; the other cores stay idle. */
    mc->max_cpus = 1;
    mc->default_cpus = 1;
    mc->default_cpu_type = ARM_CPU_TYPE_NAME("cortex-a15");
    mc->default_ram_id = "titanium.dram";
    mc->default_ram_size = 2 * GiB;
}

/*
 * DEFINE_MACHINE_ARM (not plain DEFINE_MACHINE) attaches the
 * arm_machine_interfaces[] so the board is recognised by qemu-system-arm.
 */
DEFINE_MACHINE_ARM("titanium", titanium_machine_init)
