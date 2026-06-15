/*
 * TI AM5728 DSS / DISPC display controller — minimal framebuffer scan-out
 * for the Elesar Titanium machine (RISC OS 5).
 *
 * Models just enough of DISPC to (a) satisfy the HAL/kernel's register polls
 * and (b) scan out the GFX pipeline framebuffer that RISC OS sets up in DRAM,
 * so the screen becomes visible. Registers of interest (base 0x58001000):
 *   0x00 REVISION         (RO)
 *   0x14 SYSSTATUS        (RO, bit0 RESETDONE)
 *   0x40 CONTROL1         (bit0 LCDENABLE, bit1 DIGITALENABLE)
 *   0x80 GFX_BA_0         (framebuffer base address in DRAM)
 *   0x8C GFX_SIZE         ((w-1) | (h-1)<<16)
 *   0xA0 GFX_ATTRIBUTES   (bit0 ENABLE, bits[5:1] FORMAT)
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 or later.
 */

#include "qemu/osdep.h"
#include "qemu/module.h"
#include "qemu/main-loop.h"
#include "qemu/timer.h"
#include "hw/sysbus.h"
#include "hw/irq.h"
#include "ui/console.h"
#include "ui/surface.h"
#include "hw/display/framebuffer.h"
#include "ui/pixel_ops.h"
#include "system/address-spaces.h"
#include "exec/cpu-common.h"
#include "qom/object.h"

#define TYPE_TITANIUM_DISPC "titanium-dispc"
OBJECT_DECLARE_SIMPLE_TYPE(TitaniumDISPCState, TITANIUM_DISPC)

#define R_REVISION       (0x00 / 4)
#define R_SYSSTATUS      (0x14 / 4)
#define R_IRQSTATUS      (0x18 / 4)
#define R_IRQENABLE      (0x1C / 4)
#define R_CONTROL1       (0x40 / 4)
#define R_SIZE_DIG       (0x78 / 4)   /* digital/TV output size (HDMI path) */
#define R_SIZE_LCD       (0x7C / 4)
#define R_GFX_BA_0       (0x80 / 4)
#define R_GFX_SIZE       (0x8C / 4)
#define R_GFX_ATTRIBUTES (0xA0 / 4)
#define R_VID1_BA        (0xBC / 4)   /* VID1 overlay = 2nd output's test card (Ti banner) */
#define R_VID1_ATTRIBUTES (0xCC / 4)
#define R_VID3_BA        (0x308 / 4)  /* VID3 overlay base: aliases the desktop fb at >8bpp */
#define R_VID3_ATTRIBUTES (0x370 / 4) /* bit0 enable, bits[5:1] format (AM5728 TRM) */
#define R_VID3_PICTURE_SIZE (0x394 / 4) /* VID3 source picture size = (W-1)|(H-1)<<16 */

#define DISPC_IRQ_FRAMEDONE (1u << 0)
#define DISPC_IRQ_VSYNC     (1u << 1)
#define DISPC_VSYNC_HZ      60

#define CONTROL1_LCDENABLE     (1u << 0)
#define CONTROL1_DIGITALENABLE (1u << 1)
#define GFX_ATTR_ENABLE        (1u << 0)
#define GFX_ATTR_FORMAT_SHIFT  1
#define GFX_ATTR_FORMAT_MASK   0x1f

struct TitaniumDISPCState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    MemoryRegionSection fbsection;
    QemuConsole *con;

    uint32_t regs[0x1000 / 4];
    uint32_t palette[256];   /* CLUT8 palette (index -> 0x00RRGGBB), loaded via 0x630 */
    uint32_t width, height;
    uint32_t active_size;    /* last-written SIZE_LCD/SIZE_DIG/GFX_SIZE = current mode */
    uint32_t pend_w, pend_h; /* pending resize, applied on the main loop via resize_bh */
    QEMUBH *resize_bh;
    bool fbsection_valid;
    int invalidate;

    qemu_irq irq;
    QEMUTimer *vsync;
};

/* The video driver enables DISPC_IRQENABLE.VSYNC and waits for the interrupt
 * before it considers the display up, so we must raise a periodic VSYNC. */
static void dispc_update_irq(TitaniumDISPCState *s)
{
    qemu_set_irq(s->irq, (s->regs[R_IRQSTATUS] & s->regs[R_IRQENABLE]) != 0);
}

/* Debug aid: dump the GFX framebuffer to a PPM (TITANIUM_FB_DUMP=path), so the
 * desktop can be inspected on a headless host. Throttled to ~1 Hz. */
static void dispc_dump_ppm(TitaniumDISPCState *s)
{
    const char *path = getenv("TITANIUM_FB_DUMP");
    uint32_t ba, gsz, w, h, fmt, x, y;
    int bpp;
    uint8_t *fb;
    FILE *f;

    if (!path) {
        return;
    }
    ba = s->regs[R_GFX_BA_0];
    gsz = s->regs[R_GFX_SIZE];
    w = (gsz & 0x7ff) + 1;
    h = ((gsz >> 16) & 0x7ff) + 1;
    if (w <= 1 || h <= 1) {                 /* GFX_SIZE unset: use LCD panel size */
        uint32_t lcd = s->regs[0x07c / 4];
        w = (lcd & 0x7ff) + 1;
        h = ((lcd >> 16) & 0x7ff) + 1;
    }
    if (ba == 0 || w <= 1 || h <= 1 || w > 4096 || h > 4096) {
        return;
    }
    fmt = (s->regs[R_GFX_ATTRIBUTES] >> GFX_ATTR_FORMAT_SHIFT) & GFX_ATTR_FORMAT_MASK;
    /* OMAP DISPC GFX formats: 0x3=CLUT8, 0x6=RGB16, 0x8=RGB24, 0x9/0xC/0xD=32bpp */
    bpp = (fmt == 0x3) ? 1 : (fmt == 0x6) ? 2 : 4;
    if (getenv("TITANIUM_FB_BPP")) {
        bpp = atoi(getenv("TITANIUM_FB_BPP"));
    }

    fb = g_malloc((size_t)w * h * bpp);
    cpu_physical_memory_read(ba, fb, (size_t)w * h * bpp);
    f = fopen(path, "wb");
    if (f) {
        fprintf(f, "P6\n%u %u\n255\n", w, h);
        for (y = 0; y < h; y++) {
            for (x = 0; x < w; x++) {
                uint8_t *p = fb + ((size_t)y * w + x) * bpp;
                uint8_t r, gc, b;
                if (bpp == 1) {
                    r = gc = b = *p;        /* CLUT8: grayscale by index */
                } else if (bpp == 2) {
                    uint16_t v = lduw_le_p(p);
                    r = ((v >> 11) & 0x1f) << 3;
                    gc = ((v >> 5) & 0x3f) << 2;
                    b = (v & 0x1f) << 3;
                } else {
                    uint32_t v = ldl_le_p(p);
                    r = v & 0xff; gc = (v >> 8) & 0xff; b = (v >> 16) & 0xff;
                }
                fputc(r, f); fputc(gc, f); fputc(b, f);
            }
        }
        fclose(f);
        fprintf(stderr, "[dispc] dumped %ux%u fmt=0x%x %dbpp fb @%08x -> %s\n",
                w, h, fmt, bpp * 8, ba, path);
    }
    g_free(fb);
}

static void dispc_vsync_tick(void *opaque)
{
    TitaniumDISPCState *s = opaque;
    static int n;

    s->regs[R_IRQSTATUS] |= DISPC_IRQ_VSYNC;
    dispc_update_irq(s);
    if (++n % 60 == 0) {
        dispc_dump_ppm(s);
    }
    timer_mod(s->vsync, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
              NANOSECONDS_PER_SECOND / DISPC_VSYNC_HZ);
}

/* Source framebuffer line -> surface (xRGB8888). RISC OS uses BGRx order. */
static void draw_line32(void *opaque, uint8_t *d, const uint8_t *s,
                        int width, int deststep)
{
    while (width--) {
        uint32_t v = ldl_le_p(s);
        /* xRGB24-8888 (DISPC FORMAT 0x8): 0x00RRGGBB, R in bits 23:16. */
        uint8_t r = (v >> 16) & 0xff, g = (v >> 8) & 0xff, b = v & 0xff;
        *(uint32_t *)d = rgb_to_pixel32(r, g, b);
        s += 4;
        d += 4;
    }
}

/* 15bpp xRGB/ARGB-1555 (DISPC FORMAT 0x7/0xF): bits 14:10 R, 9:5 G, 4:0 B. */
static void draw_line15(void *opaque, uint8_t *d, const uint8_t *s,
                        int width, int deststep)
{
    while (width--) {
        uint16_t v = lduw_le_p(s);
        uint8_t r = ((v >> 10) & 0x1f) << 3;
        uint8_t g = ((v >> 5) & 0x1f) << 3;
        uint8_t b = (v & 0x1f) << 3;
        *(uint32_t *)d = rgb_to_pixel32(r, g, b);
        s += 2;
        d += 4;
    }
}

/* 24bpp packed RGB24-888 (DISPC FORMAT 0x9): 3 bytes B,G,R per pixel. */
static void draw_line24(void *opaque, uint8_t *d, const uint8_t *s,
                        int width, int deststep)
{
    while (width--) {
        uint8_t b = s[0], g = s[1], r = s[2];
        *(uint32_t *)d = rgb_to_pixel32(r, g, b);
        s += 3;
        d += 4;
    }
}

static void draw_line16(void *opaque, uint8_t *d, const uint8_t *s,
                        int width, int deststep)
{
    while (width--) {
        uint16_t v = lduw_le_p(s);
        uint8_t r = ((v >> 11) & 0x1f) << 3;
        uint8_t g = ((v >> 5) & 0x3f) << 2;
        uint8_t b = (v & 0x1f) << 3;
        *(uint32_t *)d = rgb_to_pixel32(r, g, b);
        s += 2;
        d += 4;
    }
}

/* CLUT8 (8bpp) -> grayscale by index. RISC OS's palette here is ~a gamma ramp,
 * so index-as-luminance reproduces the (largely monochrome) desktop well. */
static void draw_line8(void *opaque, uint8_t *d, const uint8_t *s,
                       int width, int deststep)
{
    TitaniumDISPCState *st = opaque;
    while (width--) {
        uint32_t rgb = st->palette[*s];   /* 0x00RRGGBB from the CLUT */
        *(uint32_t *)d = rgb_to_pixel32((rgb >> 16) & 0xff,
                                        (rgb >> 8) & 0xff, rgb & 0xff);
        s += 1;
        d += 4;
    }
}

static bool dispc_enabled(TitaniumDISPCState *s)
{
    /* Don't require GFX_ATTR_ENABLE: RISC OS toggles it during updates. */
    return (s->regs[R_CONTROL1] &
            (CONTROL1_LCDENABLE | CONTROL1_DIGITALENABLE)) &&
           s->regs[R_GFX_BA_0] != 0;
}

/*
 * Apply a pending mode resize on the main loop (scheduled via resize_bh).
 * qemu_console_resize() must NOT run on the vCPU thread (from a register write)
 * nor re-entrantly during the display refresh (from gfx_update): both leave SDL
 * rendering a black/stale surface. Running it from a bottom-half on the main
 * loop, at a safe point, works for both SDL and VNC.
 */
static void dispc_resize_bh(void *opaque)
{
    TitaniumDISPCState *s = opaque;

    if (s->pend_w > 1 && s->pend_h > 1 &&
        (s->pend_w != s->width || s->pend_h != s->height)) {
        s->width = s->pend_w;
        s->height = s->pend_h;
        s->fbsection_valid = false;
        s->invalidate = 1;
        qemu_console_resize(s->con, s->width, s->height);
    }
}

/* Is VID3 the active layer aliasing the desktop framebuffer at >8bpp?
 *
 * VID3 counts as the >8bpp desktop overlay when it is enabled, aliases the GFX
 * framebuffer base, and carries a genuine RGB FORMAT.  The FORMAT field is what
 * separates the two modes RISC OS actually produces - confirmed on hardware
 * registers:
 *   - 16M-colour: VID3 FORMAT = 0x8 (xRGB8888) -> genuine 32bpp, scan VID3.
 *   - 256-colour: VID3 left enabled aliasing the 8bpp GFX framebuffer but with
 *     FORMAT = 0x10 (and a degenerate VID3_SIZE 1x129).  This must NOT be taken
 *     as 32bpp or the 8bpp desktop renders as a 4x-repeated band; the GFX CLUT8
 *     path is correct here.
 * NOTE: VID3 PICTURE_SIZE (0x394) is the full screen resolution in BOTH cases,
 * so ONLY the FORMAT distinguishes them - 0x10 is deliberately excluded. */
static bool dispc_vid3_active(TitaniumDISPCState *s)
{
    uint32_t v3 = s->regs[R_VID3_ATTRIBUTES];
    uint32_t f = (v3 >> 1) & 0x1f;
    bool rgb = (f == 0x6 || f == 0x7 || f == 0x8 || f == 0x9 || f == 0xc ||
                f == 0xd || f == 0xe || f == 0xf);
    return (v3 & 1) && s->regs[R_VID3_BA] == s->regs[R_GFX_BA_0] &&
           s->regs[R_GFX_BA_0] != 0 && rgb;
}

/* The source picture size to scan out (VID3 PICTURE_SIZE when aliasing). */
static uint32_t dispc_source_size(TitaniumDISPCState *s)
{
    if (dispc_vid3_active(s)) {
        uint32_t psz = s->regs[R_VID3_PICTURE_SIZE];
        if ((psz & 0x7ff) && ((psz >> 16) & 0x7ff)) {
            return psz;
        }
    }
    return s->active_size;
}

static void dispc_update_geometry(TitaniumDISPCState *s, uint32_t sz)
{
    /* sz is the SOURCE picture size to scan out: VID3 PICTURE_SIZE when the
     * desktop is aliased through VID3, otherwise the last-programmed panel SIZE
     * (SIZE_DIG 0x78 / SIZE_LCD 0x7C tracked in active_size). Using the source
     * size - not the panel size - matters when RISC OS runs a smaller desktop
     * (e.g. 640x480) inside a larger output raster (e.g. 800x600): scanning the
     * 640-wide framebuffer at 800 wide sheared the image. */
    uint32_t w = (sz & 0x7ff) + 1;
    uint32_t h = ((sz >> 16) & 0x7ff) + 1;

    if (w <= 1 || h <= 1) {     /* nothing programmed yet: fall back to SIZE_LCD */
        uint32_t lcd = s->regs[R_SIZE_LCD];
        w = (lcd & 0x7ff) + 1;
        h = ((lcd >> 16) & 0x7ff) + 1;
    }

    /* Defer the actual resize to the main loop (see dispc_resize_bh). */
    if (w > 1 && h > 1 && (w != s->width || h != s->height) &&
        (w != s->pend_w || h != s->pend_h)) {
        s->pend_w = w;
        s->pend_h = h;
        qemu_bh_schedule(s->resize_bh);
    }
}

static void dispc_gfx_update(void *opaque)
{
    TitaniumDISPCState *s = opaque;
    DisplaySurface *surface;
    /*
     * The RISC OS desktop framebuffer is always GFX_BA (output 1). At higher
     * colour depths the VID3 overlay (0x370/0x308) ALIASES that same framebuffer
     * carrying the real RGB format, while the GFX layer stays format-0 (8bpp
     * CLUT for 256-colour). VID1 (0xBC) is the SECOND output's test card (the
     * Elesar "Ti" banner) and must NEVER be read. So: always scan GFX_BA, but
     * take the pixel format from VID3 when it is enabled, aliases GFX_BA, and
     * carries an RGB format; otherwise from the GFX layer.
     */
    /* The VID overlay FORMAT field is 5 bits [5:1]: RISC OS uses 0x8 (xRGB8888)
     * for genuine 32bpp.  dispc_vid3_active() also checks VID3 has a real picture
     * size, so a 256-colour mode (VID3 left enabled at FORMAT 0x10 over a
     * degenerate region) correctly falls through to the GFX CLUT8 path. */
    uint32_t fmt;
    int bpp;
    drawfn fn;

    if (dispc_vid3_active(s)) {
        fmt = (s->regs[R_VID3_ATTRIBUTES] >> 1) & 0x1f; /* desktop aliased at >8bpp */
    } else {
        fmt = (s->regs[R_GFX_ATTRIBUTES] >> 1) & 0x1f;  /* GFX layer (8bpp CLUT)   */
    }
    switch (fmt) {
    case 0x6:                               bpp = 2; fn = draw_line16; break; /* RGB565   */
    case 0x7: case 0xf:                     bpp = 2; fn = draw_line15; break; /* xRGB1555 */
    case 0x9:                               bpp = 3; fn = draw_line24; break; /* RGB24    */
    case 0x8: case 0xc: case 0xd: case 0xe: case 0x10:
                                            bpp = 4; fn = draw_line32; break; /* 32bpp    */
    default:                                bpp = 1; fn = draw_line8;  break; /* 8bpp CLUT*/
    }
    int src_width, dstride, y;
    int en = dispc_enabled(s);
    uint8_t *src, *dst;

    if (!en) {
        return;
    }
    dispc_update_geometry(s, dispc_source_size(s));
    if (s->width <= 1 || s->height <= 1) {
        return;
    }

    /* Fetch the surface AFTER any resize so we never draw to a stale surface. */
    surface = qemu_console_surface(s->con);
    if (!surface) {
        return;
    }

    /*
     * Read the framebuffer straight from guest DRAM and convert it line by line.
     * We deliberately avoid framebuffer_update_display(): it sources pixels via a
     * DIRTY_MEMORY_VGA snapshot, which we never enable on plain DRAM, so it would
     * read zeros and leave the screen black. A direct read of a 640x480 frame is
     * cheap and always shows the current contents.
     */
    /* Clamp to the surface so a lagging resize can never overflow it. */
    {
        int sw = surface_width(surface), sh = surface_height(surface);
        if (s->width > sw || s->height > sh) {
            return;
        }
    }

    src_width = s->width * bpp;
    dstride = surface_stride(surface);
    src = g_malloc((size_t)src_width * s->height);
    cpu_physical_memory_read(s->regs[R_GFX_BA_0], src,
                             (size_t)src_width * s->height);
    dst = surface_data(surface);
    for (y = 0; y < s->height; y++) {
        fn(s, dst + (size_t)y * dstride, src + (size_t)y * src_width,
           s->width, 0);
    }
    g_free(src);

    /*
     * Composite the hardware mouse pointer. RISC OS (the GC320Video driver)
     * advertises GVDisplayFeature_HardwarePointer, so it does NOT draw the
     * pointer into the GFX framebuffer: it programs a 32x32 ARGB sprite on a
     * DISPC VID overlay and moves it by rewriting the overlay POSITION every
     * frame. We only scanned out the GFX layer, so the pointer was invisible
     * even though the mouse works. Overlay registers (found by tracing the
     * live driver): 0x14C base addr, 0x154 POSITION (X | Y<<16), 0x158 SIZE
     * ((w-1) | (h-1)<<16), 0x15C ATTRIBUTES (bit0 = enable, format 0xC ARGB32).
     */
    {
        uint32_t attr  = s->regs[0x15c / 4];
        uint32_t cbase = s->regs[0x14c / 4];
        if ((attr & 1) && cbase != 0) {
            uint32_t pos = s->regs[0x154 / 4];
            uint32_t csz = s->regs[0x158 / 4];
            int cw = (int)(csz & 0x7ff) + 1;
            int ch = (int)((csz >> 16) & 0x7ff) + 1;
            int px = (int)(pos & 0xffff);
            int py = (int)((pos >> 16) & 0xffff);
            if (cw > 0 && cw <= 256 && ch > 0 && ch <= 256) {
                uint32_t *cur = g_malloc((size_t)cw * ch * 4);
                cpu_physical_memory_read(cbase, cur, (size_t)cw * ch * 4);
                for (int cy = 0; cy < ch; cy++) {
                    int sy = py + cy;
                    if (sy < 0 || sy >= s->height) {
                        continue;
                    }
                    uint32_t *drow = (uint32_t *)(dst + (size_t)sy * dstride);
                    for (int cx = 0; cx < cw; cx++) {
                        int sx = px + cx;
                        if (sx < 0 || sx >= s->width) {
                            continue;
                        }
                        uint32_t p = cur[cy * cw + cx];   /* ARGB8888 */
                        uint32_t a = p >> 24;
                        uint32_t r = (p >> 16) & 0xff;
                        uint32_t g = (p >> 8) & 0xff;
                        uint32_t b = p & 0xff;
                        if (a == 0) {
                            continue;
                        }
                        if (a != 0xff) {
                            uint32_t o = drow[sx];
                            uint32_t orr = (o >> 16) & 0xff;
                            uint32_t og = (o >> 8) & 0xff;
                            uint32_t ob = o & 0xff;
                            r = (r * a + orr * (255 - a)) / 255;
                            g = (g * a + og * (255 - a)) / 255;
                            b = (b * a + ob * (255 - a)) / 255;
                        }
                        drow[sx] = rgb_to_pixel32(r, g, b);
                    }
                }
                g_free(cur);
            }
        }
    }

    dpy_gfx_update(s->con, 0, 0, s->width, s->height);

    /* Debug: dump the rendered display SURFACE (what the user sees) to a PPM. */
    if (getenv("TITANIUM_SURF_DUMP")) {
        static int n;
        if (n++ % 120 == 119) {
            FILE *f = fopen(getenv("TITANIUM_SURF_DUMP"), "wb");
            if (f) {
                fprintf(f, "P6\n%d %d\n255\n", s->width, s->height);
                for (y = 0; y < s->height; y++) {
                    uint8_t *row = dst + (size_t)y * dstride;
                    for (int x = 0; x < s->width; x++) {
                        uint32_t v = ((uint32_t *)row)[x];
                        fputc((v >> 16) & 0xff, f);
                        fputc((v >> 8) & 0xff, f);
                        fputc(v & 0xff, f);
                    }
                }
                fclose(f);
                fprintf(stderr, "[dispc] surface dumped %dx%d\n", s->width, s->height);
            }
        }
    }
}

static void dispc_invalidate(void *opaque)
{
    TitaniumDISPCState *s = opaque;
    s->invalidate = 1;
    s->fbsection_valid = false;
}

static uint64_t dispc_read(void *opaque, hwaddr addr, unsigned size)
{
    TitaniumDISPCState *s = opaque;
    uint64_t v;

    switch (addr >> 2) {
    case R_REVISION:  v = 0x00000061; break;   /* DISPC v6.1-ish, nonzero */
    case R_SYSSTATUS: v = 1; break;            /* RESETDONE */
    case R_IRQSTATUS: v = s->regs[R_IRQSTATUS]; break;
    case R_CONTROL1:
        /* GOLCD (bit5) / GODIGITAL (bit6) are set by software to latch the
         * shadow registers and self-clear at the next vsync. Report them clear
         * so the driver's "set GO, poll until clear" loop completes. */
        v = s->regs[R_CONTROL1] & ~0x60u;
        break;
    default:          v = s->regs[(addr >> 2) & 0x3ff]; break;
    }
    if (getenv("TITANIUM_DISPC_TRACE")) {
        fprintf(stderr, "[dispc] rd %03x -> %08x\n",
                (unsigned)(addr & 0xfff), (unsigned)v);
    }
    return v;
}

static void dispc_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    TitaniumDISPCState *s = opaque;

    if (getenv("TITANIUM_DISPC_TRACE")) {
        fprintf(stderr, "[dispc] wr %03x <- %08x\n",
                (unsigned)(addr & 0xfff), (unsigned)val);
    }
    /* DISPC_IRQSTATUS is write-1-to-clear; everything else stores verbatim. */
    if ((addr >> 2) == R_IRQSTATUS) {
        s->regs[R_IRQSTATUS] &= ~(uint32_t)val;
        dispc_update_irq(s);
        return;
    }

    s->regs[(addr >> 2) & 0x3ff] = (uint32_t)val;

    /* CLUT8 palette load: RISC OS writes 256 entries to 0x630, each value
     * encoded as (index << 24) | (R << 16) | (G << 8) | B. */
    if ((addr >> 2) == (0x630 >> 2)) {
        s->palette[(val >> 24) & 0xff] = (uint32_t)val & 0x00ffffff;
        return;
    }

    if ((addr >> 2) == R_IRQENABLE) {
        dispc_update_irq(s);
        return;
    }

    /* Any change to enable/geometry/base may change the picture */
    switch (addr >> 2) {
    case R_SIZE_LCD:
    case R_SIZE_DIG:
    case R_GFX_SIZE:
        if ((uint32_t)val) {
            s->active_size = (uint32_t)val;   /* track the current mode's size */
        }
        /* fall through */
    case R_CONTROL1:
    case R_GFX_BA_0:
    case R_GFX_ATTRIBUTES:
        s->fbsection_valid = false;
        s->invalidate = 1;
        if (s->con) {
            dispc_update_geometry(s, dispc_source_size(s));
        }
        break;
    default:
        break;
    }
}

static const MemoryRegionOps dispc_ops = {
    .read = dispc_read,
    .write = dispc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
};

static const GraphicHwOps dispc_gfx_ops = {
    .invalidate = dispc_invalidate,
    .gfx_update = dispc_gfx_update,
};

static void dispc_realize(DeviceState *dev, Error **errp)
{
    TitaniumDISPCState *s = TITANIUM_DISPC(dev);

    memory_region_init_io(&s->iomem, OBJECT(s), &dispc_ops, s,
                          "titanium-dispc", 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);
    s->con = graphic_console_init(dev, 0, &dispc_gfx_ops, s);
    /* Mode-change resizes are deferred to this bottom-half so qemu_console_resize
     * runs on the main loop (safe for SDL); see dispc_resize_bh. The initial size
     * is set directly here, during init on the main thread. */
    s->resize_bh = qemu_bh_new_guarded(dispc_resize_bh, s,
                                       &dev->mem_reentrancy_guard);
    s->width = s->pend_w = 640;
    s->height = s->pend_h = 480;
    qemu_console_resize(s->con, s->width, s->height);
    s->vsync = timer_new_ns(QEMU_CLOCK_VIRTUAL, dispc_vsync_tick, s);
    timer_mod(s->vsync, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
              NANOSECONDS_PER_SECOND / DISPC_VSYNC_HZ);
}

static void dispc_reset_hold(Object *obj, ResetType type)
{
    TitaniumDISPCState *s = TITANIUM_DISPC(obj);

    memset(s->regs, 0, sizeof(s->regs));
    /* Default to a grayscale ramp until RISC OS loads its CLUT, so an
     * unprogrammed 8bpp framebuffer still shows something sensible. */
    for (int i = 0; i < 256; i++) {
        s->palette[i] = (i << 16) | (i << 8) | i;
    }
    s->width = s->height = 0;
    s->pend_w = s->pend_h = 0;
    s->fbsection_valid = false;
    s->invalidate = 1;
}

static void dispc_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->realize = dispc_realize;
    rc->phases.hold = dispc_reset_hold;
}

static const TypeInfo titanium_dispc_info = {
    .name          = TYPE_TITANIUM_DISPC,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(TitaniumDISPCState),
    .class_init    = dispc_class_init,
};

static void titanium_dispc_register_types(void)
{
    type_register_static(&titanium_dispc_info);
}

type_init(titanium_dispc_register_types)
