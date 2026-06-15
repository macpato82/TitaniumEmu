/*
 * TI AM5728 CPSW (GMAC_SW) 3-port gigabit Ethernet switch - minimal model.
 *
 * Models enough of the classic cpsw_3g IP (the same block as AM335x/DRA7) for
 * the RISC OS "EtherNIC_CPSW" driver on the Elesar Titanium to bring the link
 * up and move packets:  the SS/PORT/CPDMA/STATERAM/ALE/SL/MDIO/WR register
 * blocks, one host CPDMA channel pair (TX0/RX0) using CPPI descriptors in the
 * 8 KiB descriptor SRAM, an always-link-up MDIO PHY, and the wrapper RX/TX
 * pulse interrupts.  The wire side is a standard QEMU NIC, so attaching
 *   -nic user,model=titanium-cpsw
 * bridges RISC OS to the host via SLIRP NAT.
 *
 * Register map / descriptor layout per AM572x TRM (SPRUHZ6K) section 24.11.
 *
 * Base 0x48484000, 16 KiB:  registers 0x48484000..0x48486000, then the
 * CPPI descriptor RAM 0x48486000..0x48488000.
 */

#include "qemu/osdep.h"
#include "hw/sysbus.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "system/dma.h"
#include "net/net.h"
#include "net/eth.h"

#define TYPE_TITANIUM_CPSW "titanium-cpsw"
OBJECT_DECLARE_SIMPLE_TYPE(TitaniumCPSWState, TITANIUM_CPSW)

#define CPSW_REG_SIZE   0x2000          /* register window (SS..WR)        */
#define CPSW_RAM_SIZE   0x2000          /* CPPI descriptor SRAM, 8 KiB     */
#define CPSW_RAM_BASE   0x48486000      /* absolute base of the descr. RAM */

/* --- register offsets (relative to 0x48484000) ----------------------------*/
/* SS - subsystem / switch control */
#define R_CPSW_ID_VER       0x000
#define R_CPSW_CONTROL      0x004
#define R_CPSW_SOFT_RESET   0x008
#define R_CPSW_STAT_PORT_EN 0x00C
#define R_CPSW_PTYPE        0x010
/* CPDMA control (base 0x800) */
#define R_TX_IDVER          0x800
#define R_TX_CONTROL        0x804       /* bit0 TX_EN */
#define R_TX_TEARDOWN       0x808
#define R_RX_IDVER          0x810
#define R_RX_CONTROL        0x814       /* bit0 RX_EN */
#define R_RX_TEARDOWN       0x818
#define R_CPDMA_SOFT_RESET  0x81C
#define R_DMACONTROL        0x820
#define R_DMASTATUS         0x824
#define R_RX_BUFFER_OFFSET  0x828
#define R_TX_INTSTAT_RAW    0x880
#define R_TX_INTSTAT_MASKED 0x884
#define R_TX_INTMASK_SET    0x888
#define R_TX_INTMASK_CLEAR  0x88C
#define R_CPDMA_IN_VECTOR   0x890
#define R_CPDMA_EOI_VECTOR  0x894
#define R_RX_INTSTAT_RAW    0x8A0
#define R_RX_INTSTAT_MASKED 0x8A4
#define R_RX_INTMASK_SET    0x8A8
#define R_RX_INTMASK_CLEAR  0x8AC
#define R_DMA_INTSTAT_RAW   0x8B0
#define R_DMA_INTSTAT_MASKED 0x8B4
#define R_DMA_INTMASK_SET   0x8B8
#define R_DMA_INTMASK_CLEAR 0x8BC
/* STATERAM (base 0xA00): per-channel head-descriptor and completion pointers */
#define R_TX_HDP(n)         (0xA00 + 4 * (n))
#define R_RX_HDP(n)         (0xA20 + 4 * (n))
#define R_TX_CP(n)          (0xA40 + 4 * (n))
#define R_RX_CP(n)          (0xA60 + 4 * (n))
/* ALE (base 0xD00) */
#define R_ALE_IDVER         0xD00
#define R_ALE_CONTROL       0xD08
/* CPGMAC_SL ports (base 0xD80 / 0xDC0) */
#define R_SL1_IDVER         0xD80
#define R_SL1_MACCONTROL    0xD84
#define R_SL1_MACSTATUS     0xD88
#define R_SL1_SOFT_RESET    0xD8C
#define R_SL2_IDVER         0xDC0
#define R_SL2_MACCONTROL    0xDC4
#define R_SL2_MACSTATUS     0xDC8
#define R_SL2_SOFT_RESET    0xDCC
/* MDIO (base 0x1000) */
#define R_MDIO_VER          0x1000
#define R_MDIO_CONTROL      0x1004
#define R_MDIO_ALIVE        0x1008
#define R_MDIO_LINK         0x100C
#define R_MDIO_USERACCESS0  0x1080
#define R_MDIO_USERPHYSEL0  0x1084
#define R_MDIO_USERACCESS1  0x1088
#define R_MDIO_USERPHYSEL1  0x108C
/* WR - wrapper / interrupt (base 0x1200) */
#define R_WR_IDVER          0x1200
#define R_WR_SOFT_RESET     0x1204
#define R_WR_CONTROL        0x1208
#define R_WR_INT_CONTROL    0x120C
#define R_WR_C0_RX_THRESH_EN 0x1210
#define R_WR_C0_RX_EN       0x1214
#define R_WR_C0_TX_EN       0x1218
#define R_WR_C0_MISC_EN     0x121C
#define R_WR_C0_RX_THRESH_STAT 0x1240
#define R_WR_C0_RX_STAT     0x1244
#define R_WR_C0_TX_STAT     0x1248
#define R_WR_C0_MISC_STAT   0x124C

/* MDIO USERACCESS bit fields */
#define MDIO_GO        (1u << 31)
#define MDIO_WRITE     (1u << 30)
#define MDIO_ACK       (1u << 29)
#define MDIO_REGADR_SH 21
#define MDIO_PHYADR_SH 16

/* CPPI descriptor word3 flags */
#define CPDMA_SOP      (1u << 31)
#define CPDMA_EOP      (1u << 30)
#define CPDMA_OWNER    (1u << 29)
#define CPDMA_EOQ      (1u << 28)
#define CPDMA_PKTLEN_MASK 0xFFFF

#define CPSW_MAX_FRAME 9216

struct TitaniumCPSWState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;     /* register block */
    MemoryRegion cppi_ram;  /* descriptor SRAM */
    AddressSpace dma_as;
    NICState *nic;
    NICConf conf;

    qemu_irq irq_rxthresh;  /* GIC SPI 114 */
    qemu_irq irq_rx;        /* GIC SPI 115 */
    qemu_irq irq_tx;        /* GIC SPI 116 */
    qemu_irq irq_misc;      /* GIC SPI 117 */

    uint32_t regs[CPSW_REG_SIZE / 4];

    /* port-written completion-pointer values (compared against host acks) */
    uint32_t tx_cp_portval;
    uint32_t rx_cp_portval;

    /* simple per-address MDIO PHY register file (32 regs) */
    uint16_t phy_regs[32];

    uint8_t frame[CPSW_MAX_FRAME];
};

/* ---- DMA helpers (descriptors live in the CPPI RAM mapped in dma_as) ----- */

static uint32_t cpsw_ld32(TitaniumCPSWState *s, hwaddr addr)
{
    return address_space_ldl_le(&s->dma_as, addr, MEMTXATTRS_UNSPECIFIED, NULL);
}

static void cpsw_st32(TitaniumCPSWState *s, hwaddr addr, uint32_t val)
{
    address_space_stl_le(&s->dma_as, addr, val, MEMTXATTRS_UNSPECIFIED, NULL);
}

static int cpsw_trace = -1;
#define CPSW_TRACE(...) \
    do { if (cpsw_trace) { fprintf(stderr, "[cpsw] " __VA_ARGS__); } } while (0)
static void cpsw_trace_init(void)
{
    if (cpsw_trace < 0) {
        cpsw_trace = getenv("TITANIUM_CPSW_TRACE") ? 1 : 0;
    }
}

/* ---- interrupt evaluation ------------------------------------------------ */

static void cpsw_update_irqs(TitaniumCPSWState *s)
{
    bool tx = (s->regs[R_TX_INTSTAT_RAW / 4] & s->regs[R_TX_INTMASK_SET / 4] & 1)
              && (s->regs[R_WR_C0_TX_EN / 4] & 1);
    bool rx = (s->regs[R_RX_INTSTAT_RAW / 4] & s->regs[R_RX_INTMASK_SET / 4] & 1)
              && (s->regs[R_WR_C0_RX_EN / 4] & 1);

    s->regs[R_WR_C0_TX_STAT / 4] = tx ? 1 : 0;
    s->regs[R_WR_C0_RX_STAT / 4] = rx ? 1 : 0;
    s->regs[R_TX_INTSTAT_MASKED / 4] =
        s->regs[R_TX_INTSTAT_RAW / 4] & s->regs[R_TX_INTMASK_SET / 4];
    s->regs[R_RX_INTSTAT_MASKED / 4] =
        s->regs[R_RX_INTSTAT_RAW / 4] & s->regs[R_RX_INTMASK_SET / 4];

    qemu_set_irq(s->irq_tx, tx);
    qemu_set_irq(s->irq_rx, rx);
}

/* ---- transmit ------------------------------------------------------------ */

static void cpsw_transmit(TitaniumCPSWState *s)
{
    uint32_t desc = s->regs[R_TX_HDP(0) / 4];
    uint32_t last = 0;
    unsigned framelen = 0;

    if (!(s->regs[R_TX_CONTROL / 4] & 1) || desc == 0) {
        return;
    }

    while (desc != 0) {
        uint32_t w0 = cpsw_ld32(s, desc + 0);   /* next      */
        uint32_t w1 = cpsw_ld32(s, desc + 4);   /* buffer    */
        uint32_t w2 = cpsw_ld32(s, desc + 8);   /* off|len   */
        uint32_t w3 = cpsw_ld32(s, desc + 12);  /* flags|len */
        uint32_t boff = (w2 >> 16) & 0xFFFF;
        uint32_t blen = w2 & 0xFFFF;

        if (blen && framelen + blen <= sizeof(s->frame)) {
            address_space_read(&s->dma_as, w1 + boff, MEMTXATTRS_UNSPECIFIED,
                               s->frame + framelen, blen);
            framelen += blen;
        }

        /* hand the descriptor back to the host */
        w3 &= ~CPDMA_OWNER;
        if (w0 == 0) {
            w3 |= CPDMA_EOQ;
        }
        cpsw_st32(s, desc + 12, w3);
        last = desc;

        if (w3 & CPDMA_EOP) {
            CPSW_TRACE("TX %u bytes\n", framelen);
            qemu_send_packet(qemu_get_queue(s->nic), s->frame, framelen);
            framelen = 0;
        }
        desc = w0;
    }

    /* completion: port writes the last processed descriptor to TX0_CP */
    s->tx_cp_portval = last;
    s->regs[R_TX_CP(0) / 4] = last;
    s->regs[R_TX_HDP(0) / 4] = 0;
    s->regs[R_TX_INTSTAT_RAW / 4] |= 1;
    cpsw_update_irqs(s);
}

/* ---- receive ------------------------------------------------------------- */

static bool cpsw_can_receive(NetClientState *nc)
{
    TitaniumCPSWState *s = qemu_get_nic_opaque(nc);
    return (s->regs[R_RX_CONTROL / 4] & 1) && s->regs[R_RX_HDP(0) / 4] != 0;
}

static ssize_t cpsw_receive(NetClientState *nc, const uint8_t *buf, size_t size)
{
    TitaniumCPSWState *s = qemu_get_nic_opaque(nc);
    uint32_t desc = s->regs[R_RX_HDP(0) / 4];
    uint32_t w0, w1, w2, w3, blen, boff;

    if (!(s->regs[R_RX_CONTROL / 4] & 1) || desc == 0) {
        return -1;          /* no buffer posted - ask the stack to requeue */
    }
    if (size > CPSW_MAX_FRAME) {
        size = CPSW_MAX_FRAME;
    }

    w0 = cpsw_ld32(s, desc + 0);
    w1 = cpsw_ld32(s, desc + 4);
    w2 = cpsw_ld32(s, desc + 8);
    w3 = cpsw_ld32(s, desc + 12);
    boff = s->regs[R_RX_BUFFER_OFFSET / 4] & 0xFFFF;
    blen = w2 & 0xFFFF;
    if (size > blen) {
        size = blen;
    }

    CPSW_TRACE("RX %zu bytes -> desc %08x buf %08x\n", size, desc, w1);
    address_space_write(&s->dma_as, w1 + boff, MEMTXATTRS_UNSPECIFIED,
                        buf, size);

    /* fill the descriptor: SOP|EOP single-fragment, owner cleared, length */
    w2 = (boff << 16) | (size & 0xFFFF);
    w3 = CPDMA_SOP | CPDMA_EOP | (size & CPDMA_PKTLEN_MASK);
    if (w0 == 0) {
        w3 |= CPDMA_EOQ;
    }
    cpsw_st32(s, desc + 8, w2);
    cpsw_st32(s, desc + 12, w3);

    s->rx_cp_portval = desc;
    s->regs[R_RX_CP(0) / 4] = desc;
    s->regs[R_RX_HDP(0) / 4] = w0;          /* advance to next free buffer */
    s->regs[R_RX_INTSTAT_RAW / 4] |= 1;
    cpsw_update_irqs(s);
    return size;
}

/* ---- MDIO ---------------------------------------------------------------- */

static void cpsw_mdio_access(TitaniumCPSWState *s, uint32_t v)
{
    unsigned phy = (v >> MDIO_PHYADR_SH) & 0x1F;
    unsigned reg = (v >> MDIO_REGADR_SH) & 0x1F;
    uint32_t res;

    /* the (single) PHY responds at every probed address: link is always up */
    s->regs[R_MDIO_ALIVE / 4] |= (1u << phy);
    s->regs[R_MDIO_LINK / 4]  |= (1u << phy);

    if (v & MDIO_WRITE) {
        s->phy_regs[reg] = v & 0xFFFF;
        res = MDIO_ACK;                     /* GO cleared, ACK set */
        CPSW_TRACE("MDIO wr phy%u reg%u = %04x\n", phy, reg, v & 0xFFFF);
    } else {
        res = MDIO_ACK | (s->phy_regs[reg] & 0xFFFF);
        CPSW_TRACE("MDIO rd phy%u reg%u -> %04x\n", phy, reg, s->phy_regs[reg]);
    }
    s->regs[R_MDIO_USERACCESS0 / 4] = res;  /* GO bit cleared -> access done */
}

/* Micrel KSZ9031 vendor-specific PHY status (reg 31) bits - the EtherCPSW
 * driver reads this for link/speed and treats "no speed bit" as link-down. */
#define KSZ_VSPHYST     31
#define KSZ_ST_LNKFAIL  0x0001
#define KSZ_ST_DPLXDET  0x0008
#define KSZ_ST_SPD1000  0x0040
#define KSZ_VSPHYST_UP  (KSZ_ST_SPD1000 | KSZ_ST_DPLXDET) /* 1Gb full, link up */

static void cpsw_phy_reset(TitaniumCPSWState *s)
{
    memset(s->phy_regs, 0, sizeof(s->phy_regs));
    s->phy_regs[0]  = 0x1140;   /* BMCR:  autoneg enabled, 1000 Mbps          */
    s->phy_regs[1]  = 0x796D;   /* BMSR:  link up, autoneg complete + caps    */
    s->phy_regs[2]  = 0x0022;   /* PHYIDR1 (Micrel KSZ9031)                   */
    s->phy_regs[3]  = 0x1622;   /* PHYIDR2 (Micrel KSZ9031)                   */
    s->phy_regs[4]  = 0x01E1;   /* ANAR:  100/10 full/half advertised         */
    s->phy_regs[5]  = 0xC1E1;   /* ANLPAR: link partner, autoneg ack          */
    s->phy_regs[9]  = 0x0200;   /* 1000BASE-T control: 1000full advertised    */
    s->phy_regs[10] = 0x7800;   /* 1000BASE-T status: LP 1000full capable     */
    s->phy_regs[KSZ_VSPHYST] = KSZ_VSPHYST_UP;
}

/* ---- MMIO ---------------------------------------------------------------- */

static uint64_t cpsw_read(void *opaque, hwaddr offset, unsigned size)
{
    TitaniumCPSWState *s = opaque;
    uint32_t idx = (offset & ~3u) / 4;

    switch (offset) {
    case R_CPSW_ID_VER:   return 0x4EC81903; /* CPSW3G revision               */
    case R_TX_IDVER:
    case R_RX_IDVER:      return 0x000C0100;
    case R_ALE_IDVER:     return 0x00290105;
    case R_SL1_IDVER:
    case R_SL2_IDVER:     return 0x4ED90101;
    case R_MDIO_VER:      return 0x00070106;
    case R_WR_IDVER:      return 0x00010700;
    /* self-clearing soft-reset bits read back 0 */
    case R_CPSW_SOFT_RESET:
    case R_CPDMA_SOFT_RESET:
    case R_SL1_SOFT_RESET:
    case R_SL2_SOFT_RESET:
    case R_WR_SOFT_RESET: return 0;
    case R_DMASTATUS:     return 0;          /* idle, no errors               */
    case R_SL1_MACSTATUS:
    case R_SL2_MACSTATUS: return 0;
    case R_CPDMA_IN_VECTOR:
        return s->regs[R_TX_INTSTAT_MASKED / 4] ? (1u << 16) :
               (s->regs[R_RX_INTSTAT_MASKED / 4] ? 1u : 0);
    default:
        if (idx < CPSW_REG_SIZE / 4) {
            return s->regs[idx];
        }
        return 0;
    }
}

static void cpsw_write(void *opaque, hwaddr offset, uint64_t val, unsigned size)
{
    TitaniumCPSWState *s = opaque;
    uint32_t idx = (offset & ~3u) / 4;

    switch (offset) {
    /* soft resets self-clear: never store the set bit */
    case R_CPSW_SOFT_RESET:
    case R_CPDMA_SOFT_RESET:
    case R_SL1_SOFT_RESET:
    case R_SL2_SOFT_RESET:
    case R_WR_SOFT_RESET:
        return;

    case R_TX_INTMASK_SET:
        s->regs[idx] |= val; cpsw_update_irqs(s); return;
    case R_TX_INTMASK_CLEAR:
        s->regs[R_TX_INTMASK_SET / 4] &= ~(uint32_t)val;
        cpsw_update_irqs(s); return;
    case R_RX_INTMASK_SET:
        s->regs[idx] |= val; cpsw_update_irqs(s); return;
    case R_RX_INTMASK_CLEAR:
        s->regs[R_RX_INTMASK_SET / 4] &= ~(uint32_t)val;
        cpsw_update_irqs(s); return;

    case R_CPDMA_EOI_VECTOR:
        cpsw_update_irqs(s);
        return;

    case R_TX_HDP(0):
        s->regs[idx] = val;
        cpsw_transmit(s);
        return;

    case R_RX_HDP(0):
        s->regs[idx] = val;
        CPSW_TRACE("RX0_HDP posted = %08x\n", (uint32_t)val);
        /* a freshly posted buffer may let queued RX frames flow */
        if (cpsw_can_receive(qemu_get_queue(s->nic))) {
            qemu_flush_queued_packets(qemu_get_queue(s->nic));
        }
        return;

    case R_TX_CP(0):
        /* host ack: deassert only when it matches the port-written value */
        if ((uint32_t)val == s->tx_cp_portval) {
            s->regs[R_TX_INTSTAT_RAW / 4] &= ~1u;
            cpsw_update_irqs(s);
        }
        return;
    case R_RX_CP(0):
        if ((uint32_t)val == s->rx_cp_portval) {
            s->regs[R_RX_INTSTAT_RAW / 4] &= ~1u;
            cpsw_update_irqs(s);
        }
        return;

    case R_RX_CONTROL:
        s->regs[idx] = val;
        CPSW_TRACE("RX_CONTROL = %08x\n", (uint32_t)val);
        if ((val & 1) && cpsw_can_receive(qemu_get_queue(s->nic))) {
            qemu_flush_queued_packets(qemu_get_queue(s->nic));
        }
        return;

    case R_TX_CONTROL:
        s->regs[idx] = val;
        CPSW_TRACE("TX_CONTROL = %08x\n", (uint32_t)val);
        return;

    case R_MDIO_USERACCESS0:
        if (val & MDIO_GO) {
            cpsw_mdio_access(s, val);
        } else {
            s->regs[idx] = val;
        }
        return;

    case R_WR_C0_TX_EN:
    case R_WR_C0_RX_EN:
        s->regs[idx] = val;
        cpsw_update_irqs(s);
        return;

    default:
        if (idx < CPSW_REG_SIZE / 4) {
            s->regs[idx] = val;
        }
        return;
    }
}

static const MemoryRegionOps cpsw_ops = {
    .read = cpsw_read,
    .write = cpsw_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
};

static void cpsw_set_link(NetClientState *nc)
{
    TitaniumCPSWState *s = qemu_get_nic_opaque(nc);
    bool up = !nc->link_down;

    s->phy_regs[1] = up ? 0x796D : 0x7809;       /* BMSR link-status bit  */
    s->phy_regs[KSZ_VSPHYST] = up ? KSZ_VSPHYST_UP : KSZ_ST_LNKFAIL;
    s->regs[R_MDIO_LINK / 4] = up ? 0xFFFFFFFF : 0;
}

static NetClientInfo net_cpsw_info = {
    .type = NET_CLIENT_DRIVER_NIC,
    .size = sizeof(NICState),
    .can_receive = cpsw_can_receive,
    .receive = cpsw_receive,
    .link_status_changed = cpsw_set_link,
};

static void cpsw_reset_hold(Object *obj, ResetType type)
{
    TitaniumCPSWState *s = TITANIUM_CPSW(obj);

    memset(s->regs, 0, sizeof(s->regs));
    s->tx_cp_portval = 0;
    s->rx_cp_portval = 0;
    cpsw_phy_reset(s);
    qemu_set_irq(s->irq_tx, 0);
    qemu_set_irq(s->irq_rx, 0);
    qemu_set_irq(s->irq_rxthresh, 0);
    qemu_set_irq(s->irq_misc, 0);
}

static void cpsw_realize(DeviceState *dev, Error **errp)
{
    TitaniumCPSWState *s = TITANIUM_CPSW(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    memory_region_init_io(&s->iomem, OBJECT(s), &cpsw_ops, s,
                          "titanium-cpsw.regs", CPSW_REG_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);

    memory_region_init_ram(&s->cppi_ram, OBJECT(s), "titanium-cpsw.cppi",
                           CPSW_RAM_SIZE, &error_fatal);
    sysbus_init_mmio(sbd, &s->cppi_ram);

    sysbus_init_irq(sbd, &s->irq_rxthresh);
    sysbus_init_irq(sbd, &s->irq_rx);
    sysbus_init_irq(sbd, &s->irq_tx);
    sysbus_init_irq(sbd, &s->irq_misc);

    address_space_init(&s->dma_as, get_system_memory(), "titanium-cpsw-dma");

    cpsw_trace_init();
    qemu_macaddr_default_if_unset(&s->conf.macaddr);
    s->nic = qemu_new_nic(&net_cpsw_info, &s->conf,
                          object_get_typename(OBJECT(dev)), dev->id,
                          &dev->mem_reentrancy_guard, s);
    qemu_format_nic_info_str(qemu_get_queue(s->nic), s->conf.macaddr.a);
}

static const VMStateDescription vmstate_cpsw = {
    .name = "titanium-cpsw",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, TitaniumCPSWState, CPSW_REG_SIZE / 4),
        VMSTATE_UINT32(tx_cp_portval, TitaniumCPSWState),
        VMSTATE_UINT32(rx_cp_portval, TitaniumCPSWState),
        VMSTATE_UINT16_ARRAY(phy_regs, TitaniumCPSWState, 32),
        VMSTATE_END_OF_LIST()
    }
};

static const Property cpsw_props[] = {
    DEFINE_NIC_PROPERTIES(TitaniumCPSWState, conf),
};

static void cpsw_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->realize = cpsw_realize;
    dc->vmsd = &vmstate_cpsw;
    rc->phases.hold = cpsw_reset_hold;
    device_class_set_props(dc, cpsw_props);
    set_bit(DEVICE_CATEGORY_NETWORK, dc->categories);
}

static const TypeInfo cpsw_info = {
    .name = TYPE_TITANIUM_CPSW,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(TitaniumCPSWState),
    .class_init = cpsw_class_init,
};

static void cpsw_register_types(void)
{
    type_register_static(&cpsw_info);
}

type_init(cpsw_register_types)
