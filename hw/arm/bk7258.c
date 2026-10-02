/*
 * BK7258 shared diagnostic SoC. Board wiring and loaders are separate.
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Register provenance and explicit omissions: docs/system/arm/bk7258.rst.
 */
#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qemu/log.h"
#include "qemu/error-report.h"
#include "qapi/error.h"
#include "hw/arm/bk7258.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-clock.h"
#include "hw/core/qdev-properties.h"
#include "system/address-spaces.h"
#include "system/system.h"
#include "target/arm/internals.h"
#include "migration/vmstate.h"

#define SRAM_SIZE (640 * KiB)
#define TCM_SIZE (16 * KiB)
#define SRAM_BASE 0x28000000
#define SYS_BASE 0x44010000
#define NS_OFFSET 0x10000000
#define RESET_RELEASE 1U
#define POWER_DOWN 2U
#define CPU_HALT 8U
#define TICK_ROUTES ((1U << 29) | (1U << 30) | (1U << 27))

static const uint32_t uart_base[] = { 0x44820000, 0x45830000, 0x45840000 };
static const unsigned uart_irq[] = { 4, 15, 16 };

static void bk7258_update_core_clocks(BK7258State *s)
{
    /*
     * These are the CP SDK's documented non-DCO operating points. They
     * describe digital timing inputs, not PLL lock, voltage or TCG speed.
     * The bus output has no DMA/peripheral consumer until routing is proven.
     */
    static const struct {
        uint8_t mode;
        uint8_t speeds;
        unsigned mhz[3];
        unsigned bus_mhz;
    } tuples[] = {
        /* Preserve the existing diagnostic reset, not a silicon POR claim. */
        { 0x00, 0, { 26, 26, 26 }, 26 },
        { 0x00, 7, { 26, 26, 26 }, 26 },
        { 0x30, 6, { 240, 480, 480 }, 240 },
        { 0x20, 6, { 160, 320, 320 }, 160 },
        { 0x31, 7, { 240, 240, 240 }, 240 },
        { 0x33, 7, { 120, 120, 120 }, 120 },
        { 0x35, 7, { 80, 80, 80 }, 80 },
        { 0x37, 7, { 60, 60, 60 }, 60 },
    };
    unsigned mode = s->clock_mode & 0x7f;
    unsigned speeds = 0;
    unsigned hz[3] = { 0 };
    unsigned bus_hz = 0;
    unsigned key;
    bool found = false;

    if (!s->experimental_core_clocks) {
        for (unsigned i = 0; i < 3; i++) {
            clock_update(s->coreclk[i], clock_get(s->cpuclk));
        }
        clock_update(s->busclk, clock_get(s->cpuclk));
        return;
    }
    for (unsigned i = 0; i < 3; i++) {
        speeds |= ((s->cpu_control[i] >> 4) & 1) << i;
    }
    key = (mode << 3) | speeds;
    for (unsigned n = 0; n < ARRAY_SIZE(tuples); n++) {
        if (tuples[n].mode != mode || tuples[n].speeds != speeds) {
            continue;
        }
        found = true;
        /* Use only the committed enable after the analog register transfer. */
        if (mode == 0 || (s->analog[5] & (1U << 5))) {
            for (unsigned i = 0; i < 3; i++) {
                hz[i] = tuples[n].mhz[i] * 1000000U;
            }
            bus_hz = tuples[n].bus_mhz * 1000000U;
        }
        break;
    }
    if (!found && (!s->core_clock_unimplemented || s->core_clock_key != key)) {
        qemu_log_mask(LOG_UNIMP,
                      "bk7258-sys: experimental core clock tuple "
                      "mode=0x%02x speeds=0x%x is unsupported\n", mode, speeds);
    }
    s->core_clock_key = key;
    s->core_clock_unimplemented = !found;
    for (unsigned i = 0; i < 3; i++) {
        clock_update_hz(s->coreclk[i], hz[i]);
    }
    clock_update_hz(s->busclk, bus_hz);
}

static void bk7258_update_uart_clocks(BK7258State *s)
{
    static const unsigned gate[] = { 2, 10, 11 };
    static const unsigned shift[] = { 8, 11, 14 };

    for (unsigned i = 0; i < 3; i++) {
        unsigned mode = (s->clock_mode >> shift[i]) & 7;
        unsigned hz = 0;

        /* UART's APLL-named source is unresolved, not the modeled SPI net. */
        if ((s->peripheral_clocks & (1U << gate[i])) && !(mode & 4)) {
            hz = clock_get_hz(s->xtalclk) >> (mode & 3);
        }
        clock_update_hz(s->uartclk[i], hz);
    }
}

static void bk7258_update_spi_clocks(BK7258State *s)
{
    static const unsigned gate[] = { 1, 9 };

    for (unsigned i = 0; i < 2; i++) {
        Clock *source = s->clock_select & (1U << (4 + i)) ?
                        s->apllclk : s->xtalclk;

        clock_update(s->spiclk[i], s->peripheral_clocks & (1U << gate[i]) ?
                     clock_get(source) : 0);
    }
}

static void bk7258_audio_clock_commit(BK7258State *s, uint32_t written,
                                     uint32_t old_power, uint32_t old_control,
                                     uint32_t old_coefficient)
{
    const uint32_t trigger = 1U << 18;
    const uint32_t control = 0xc2a0ae86;
    uint32_t config = s->analog[25];
    uint32_t coefficient = s->analog[26];
    bool powered = !(s->analog[5] & (1U << 13));
    bool valid_control = (config & ~trigger) == control;
    unsigned profile_hz = coefficient == 0x8973ca6f ? 98304000 :
                          coefficient == 0x88af2ec9 ? 90316800 : 0;

    if (!s->experimental_spi_apll) {
        return;
    }
    /*
     * SDK-backed profiles and pulse sequence; activation at the committed
     * falling edge is an explicit ideal digital policy, not PLL lock timing.
     * Reconfiguration/powerdown invalidates the pulse rather than inventing
     * analog retention or relock behavior. No hardware status bit is supplied.
     */
    if ((written & (1U << 26)) && !profile_hz) {
        qemu_log_mask(LOG_UNIMP,
                      "bk7258-sys: ideal SPI APLL coefficient 0x%08x "
                      "unsupported\n", coefficient);
    }
    if ((written & (1U << 25)) && !valid_control) {
        qemu_log_mask(LOG_UNIMP,
                      "bk7258-sys: ideal SPI APLL control 0x%08x "
                      "unsupported\n", config & ~trigger);
    }
    if (!powered || !valid_control || !profile_hz ||
        ((old_power ^ s->analog[5]) & (1U << 13)) ||
        ((old_control ^ config) & ~trigger) ||
        old_coefficient != coefficient) {
        s->apll_pulse_armed = false;
        s->apll_hz = 0;
    }
    if (powered && valid_control && profile_hz &&
        ((old_control ^ config) & trigger)) {
        if (config & trigger) {
            s->apll_pulse_armed = true;
            s->apll_hz = 0;
        } else {
            if (s->apll_pulse_armed) {
                s->apll_hz = profile_hz;
            }
            s->apll_pulse_armed = false;
        }
    }
    clock_update_hz(s->apllclk, s->apll_hz);
    bk7258_update_spi_clocks(s);
}

static void bk7258_update_i2c_clocks(BK7258State *s)
{
    static const unsigned gate[] = { 0, 8 };

    for (unsigned i = 0; i < 2; i++) {
        clock_update(s->i2cclk[i], s->peripheral_clocks & (1U << gate[i]) ?
                     clock_get(s->xtalclk) : 0);
    }
}

static void bk7258_update_saradc_clock(BK7258State *s)
{
    /* Only XTAL is sourced; do not substitute SPI's ideal APLL experiment. */
    bool available = s->experimental_saradc &&
                     (s->peripheral_clocks & (1U << 5)) &&
                     !(s->clock_mode & (1U << 17)) &&
                     (s->analog[2] & (1U << 15));

    clock_update(s->sadcclk, available ? clock_get(s->xtalclk) : 0);
}

static void bk7258_update_timer_clocks(BK7258State *s)
{
    static const unsigned gate[] = { 4, 13 };

    for (unsigned i = 0; i < 2; i++) {
        Clock *source = s->clock_mode & (1U << (20 + i)) ?
                        s->xtalclk : s->lpoclk;

        clock_update(s->timerclk[i], s->peripheral_clocks & (1U << gate[i]) ?
                     clock_get(source) : 0);
    }
}

static void bk7258_update_pwm_clocks(BK7258State *s)
{
    static const unsigned gate[] = { 3, 12 };

    for (unsigned i = 0; i < 2; i++) {
        /* CLK32 topology remains unresolved; only sourced XTAL is connected. */
        clock_update(s->pwmclk[i],
                     (s->peripheral_clocks & (1U << gate[i])) &&
                     (s->clock_mode & (1U << (18 + i))) ?
                     clock_get(s->xtalclk) : 0);
    }
}

static void bk7258_update_wdt_clock(BK7258State *s)
{
    unsigned divider = 1U << (((s->clock_select >> 2) & 3) + 1);

    clock_update_hz(s->wdtclk[0], clock_get_hz(s->roscclk) / 32);
    clock_update_hz(s->wdtclk[1], s->peripheral_clocks & (1U << 31) ?
                    clock_get_hz(s->lpoclk) / divider : 0);
}

static void bk7258_update_tick_clocks(BK7258State *s)
{
    static const unsigned bit[] = { 29, 30, 27 };

    for (unsigned i = 0; i < 3; i++) {
        clock_update(s->refclk[i], s->power_sleep & (1U << bit[i]) ?
                     clock_get(s->lpoclk) : 0);
    }
}

static void bk7258_lpo_changed(void *opaque, ClockEvent event)
{
    BK7258State *s = BK7258_SOC(opaque);

    bk7258_update_tick_clocks(s);
    bk7258_update_wdt_clock(s);
    bk7258_update_timer_clocks(s);
}

static void bk7258_analog_complete(void *opaque)
{
    BK7258State *s = opaque;
    uint32_t written = s->analog_busy;
    uint32_t old_power = s->analog[5];
    uint32_t old_control = s->analog[25];
    uint32_t old_coefficient = s->analog[26];

    for (unsigned i = 0; i < ARRAY_SIZE(s->analog); i++) {
        if (s->analog_busy & (1U << i)) {
            s->analog[i] = s->analog_pending[i];
        }
    }
    s->analog_busy = 0;
    bk7258_audio_clock_commit(s, written, old_power, old_control,
                              old_coefficient);
    bk7258_update_core_clocks(s);
    bk7258_update_saradc_clock(s);
    clock_update_hz(s->roscclk, s->analog[5] & (1U << 14) ? 0 : 32000);
    bk7258_update_wdt_clock(s);
    bk7258_update_timer_clocks(s);
}

static void bk7258_update_irqs(BK7258State *s)
{
    unsigned cpu, irq;

    for (cpu = 0; cpu < 3; cpu++) {
        uint64_t enabled = s->irq_enable[cpu][0] |
                           (uint64_t)s->irq_enable[cpu][1] << 32;
        for (irq = 0; irq < 64; irq++) {
            qemu_set_irq(qdev_get_gpio_in(DEVICE(&s->cpu[cpu]), irq),
                         !(s->cpu_control[cpu] & 4) &&
                         !!((s->irq_levels | s->private_irqs[cpu]) &
                            enabled & (UINT64_C(1) << irq)));
        }
    }
}

static void bk7258_irq(void *opaque, int n, int level)
{
    BK7258State *s = opaque;
    uint64_t bit = UINT64_C(1) << n;

    s->irq_levels = (s->irq_levels & ~bit) | (level ? bit : 0);
    bk7258_update_irqs(s);
}

static void bk7258_mailbox_irq(void *opaque, int core, int level)
{
    BK7258State *s = opaque;

    s->private_irqs[core] = level ? UINT64_C(1) << 63 : 0;
    bk7258_update_irqs(s);
}

static MemTxResult bk7258_sys_read(void *opaque, hwaddr offset,
                                   uint64_t *value, unsigned size,
                                   MemTxAttrs attrs)
{
    BK7258State *s = opaque;
    unsigned index;

    switch (offset) {
    case 0x10:
    case 0x14:
    case 0x18:
        *value = s->cpu_control[(offset - 0x10) / 4];
        break;
    case 0x20:
        *value = s->clock_mode;
        break;
    case 0x28:
        *value = s->clock_select;
        break;
    case 0x40:
        *value = s->power_sleep;
        break;
    case 0x44:
        *value = s->flash_bus_config;
        break;
    case 0xc0 ... 0xd8:
        *value = s->gpio_mux[(offset - 0xc0) / 4];
        break;
    case 0xe8:
        *value = s->analog_busy;
        break;
    case 0x100 ... 0x16c:
        *value = s->analog[(offset - 0x100) / 4];
        break;
    case 0x30:
        *value = s->peripheral_clocks;
        break;
    case 0x80 ... 0x94:
        index = (offset - 0x80) / 4;
        *value = s->irq_enable[index / 2][index % 2];
        break;
    case 0xa0 ... 0xb4:
        index = (offset - 0xa0) / 4;
        *value = ((s->irq_levels | s->private_irqs[index / 2]) >>
                (32 * (index % 2))) &
               s->irq_enable[index / 2][index % 2];
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "bk7258-sys: read offset 0x%" HWADDR_PRIx
                      " is not implemented\n", offset);
        return MEMTX_ERROR;
    }
    return MEMTX_OK;
}

static bool bk7258_cpu_enabled(uint32_t value)
{
    return (value & RESET_RELEASE) && !(value & (POWER_DOWN | CPU_HALT));
}

typedef struct BK7258CPUControl {
    uint32_t value;
    bool reset_release;
} BK7258CPUControl;

static void bk7258_cpu_control_work(CPUState *cs, run_on_cpu_data data)
{
    g_autofree BK7258CPUControl *control = data.host_ptr;
    ARMCPU *cpu = ARM_CPU(cs);
    bool enabled = bk7258_cpu_enabled(control->value);

    /* Work executes in the target CPU context, in register-write order. */
    if (control->reset_release) {
        object_property_set_uint(OBJECT(cpu), "init-svtor",
                                 control->value & 0xffffff00, &error_abort);
        cpu_reset(cs);
    }
    arm_set_cpu_power_state(cpu, enabled ? PSCI_ON : PSCI_OFF);
    cs->halted = !enabled;
}

static MemTxResult bk7258_sys_write(void *opaque, hwaddr offset,
                                    uint64_t value, unsigned size,
                                    MemTxAttrs attrs)
{
    BK7258State *s = opaque;
    unsigned index;
    uint32_t old;
    BK7258CPUControl *control;

    switch (offset) {
    case 0x10:
    case 0x14:
    case 0x18:
        index = (offset - 0x10) / 4;
        old = s->cpu_control[index];
        s->cpu_control[index] = value;
        /*
         * Running-vector writes are software handshakes, not reset strobes.
         * Halt and power gates preserve CPU state; only reset release reloads
         * the vector table. Queue every gating transition so rapid successive
         * writes cannot race the target CPU's current power state.
         */
        if ((old ^ value) & (RESET_RELEASE | POWER_DOWN | CPU_HALT)) {
            control = g_new(BK7258CPUControl, 1);
            control->value = value;
            control->reset_release = !(old & RESET_RELEASE) &&
                                     (value & RESET_RELEASE);
            async_run_on_cpu(CPU(s->cpu[index].cpu), bk7258_cpu_control_work,
                             RUN_ON_CPU_HOST_PTR(control));
        }
        bk7258_update_core_clocks(s);
        bk7258_update_irqs(s);
        break;
    case 0x20:
        s->clock_mode = value;
        bk7258_update_core_clocks(s);
        if (value & ((1U << 10) | (1U << 13) | (1U << 16))) {
            qemu_log_mask(LOG_UNIMP,
                          "bk7258-sys: UART APLL source is not implemented\n");
        }
        bk7258_update_uart_clocks(s);
        bk7258_update_saradc_clock(s);
        bk7258_update_timer_clocks(s);
        bk7258_update_pwm_clocks(s);
        break;
    case 0x40:
        s->power_sleep = value;
        bk7258_entry_probe_ready(&s->entry_probe,
            (s->peripheral_clocks & (1U << 15)) && !(value & (1U << 3)));
        bk7258_update_tick_clocks(s);
        break;
    case 0x44:
        /*
         * SDK sys2flsh_2wire is bit 7. This is configuration readback only:
         * the byte-transaction Flash model has no wire or bus-lane timing.
         * Cache/FPU sleep controls and other bits remain unsupported.
         */
        if (value & ~(1U << 7)) {
            qemu_log_mask(LOG_UNIMP,
                          "bk7258-sys: unsupported SYS2Flash configuration\n");
            return MEMTX_ERROR;
        }
        s->flash_bus_config = value;
        break;
    case 0xc0 ... 0xd8:
        s->gpio_mux[(offset - 0xc0) / 4] = value;
        break;
    case 0x100 ... 0x16c:
        index = (offset - 0x100) / 4;
        if (s->analog_busy & (1U << index)) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "bk7258-sys: analog register %u is busy\n", index);
            break;
        }
        s->analog_pending[index] = value;
        s->analog_busy |= 1U << index;
        /* Register transfer latency, not analog settling or PLL lock. */
        if (!timer_pending(s->analog_timer)) {
            timer_mod(s->analog_timer,
                      qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 1000);
        }
        break;
    case 0x28:
        s->clock_select = value;
        if ((value & 0x30) && !s->experimental_spi_apll) {
            qemu_log_mask(LOG_UNIMP,
                          "bk7258-sys: SPI APLL source is not implemented\n");
        }
        bk7258_update_spi_clocks(s);
        bk7258_update_wdt_clock(s);
        break;
    case 0x30:
        s->peripheral_clocks = value;
        bk7258_entry_probe_ready(&s->entry_probe,
            (value & (1U << 15)) && !(s->power_sleep & (1U << 3)));
        bk7258_update_wdt_clock(s);
        bk7258_update_uart_clocks(s);
        bk7258_update_saradc_clock(s);
        bk7258_update_timer_clocks(s);
        bk7258_update_pwm_clocks(s);
        bk7258_update_i2c_clocks(s);
        bk7258_update_spi_clocks(s);
        break;
    case 0x80 ... 0x94:
        index = (offset - 0x80) / 4;
        s->irq_enable[index / 2][index % 2] = value;
        bk7258_update_irqs(s);
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "bk7258-sys: write offset 0x%" HWADDR_PRIx
                      " is not implemented\n", offset);
        return MEMTX_ERROR;
    }
    return MEMTX_OK;
}

static const MemoryRegionOps bk7258_sys_ops = {
    .read_with_attrs = bk7258_sys_read,
    .write_with_attrs = bk7258_sys_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void bk7258_alias(MemoryRegion *alias, Object *owner, const char *name,
                         MemoryRegion *target, MemoryRegion *space,
                         hwaddr address, uint64_t size)
{
    memory_region_init_alias(alias, owner, name, target, 0, size);
    memory_region_add_subregion(space, address, alias);
}

static void bk7258_realize(DeviceState *dev, Error **errp)
{
    BK7258State *s = BK7258_SOC(dev);
    Object *obj = OBJECT(dev);
    MemoryRegion *memory = get_system_memory();
    static const uint32_t sram_alias_base[] = {
        0x08000000, 0x18000000, 0x38000000,
    };
    unsigned i;
    MemoryRegion *xip;

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->flashctrl), errp) ||
        !memory_region_init_ram(&s->sram, obj, "bk7258.sram", SRAM_SIZE,
                                errp)) {
        return;
    }
    if (!s->diagnostic_xip) {
        if (s->flash_size != memory_region_size(&s->flashctrl.xip)) {
            error_setg(errp,
                       "BK7258 physical NOR mode requires its exact XIP size");
            return;
        }
        xip = &s->flashctrl.xip;
        sysbus_mmio_map(SYS_BUS_DEVICE(&s->flashctrl), 0, 0x44030000);
        bk7258_alias(&s->flashctrl_ns, obj, "bk7258.flashctrl-ns",
                     &s->flashctrl.regs, memory, 0x54030000, 0x1000);
    } else {
        if (s->flashctrl.nor->blk || s->flashctrl.nor->status_blk) {
            error_setg(errp, "BK7258 diagnostic XIP cannot use a NOR backend");
            return;
        }
        if (!memory_region_init_rom(&s->flash, obj, "bk7258.diagnostic-xip",
                                     s->flash_size, errp)) {
            return;
        }
        xip = &s->flash;
    }
    memory_region_add_subregion(memory, BK7258_FLASH_BASE, xip);
    memory_region_add_subregion(memory, SRAM_BASE, &s->sram);
    bk7258_alias(&s->flash_ns, obj, "bk7258.xip-ns", xip, memory,
                 BK7258_FLASH_BASE + NS_OFFSET, s->flash_size);
    for (i = 0; i < 3; i++) {
        g_autofree char *name = g_strdup_printf("bk7258.sram-alias%u", i);
        bk7258_alias(&s->sram_alias[i], obj, name, &s->sram, memory,
                     sram_alias_base[i], SRAM_SIZE);
    }

    memory_region_init(&s->dma_memory, obj, "bk7258.dma-memory",
                       UINT64_C(1) << 32);
    for (i = 0; i < 4; i++) {
        static const uint32_t base[] = {
            0x08000000, 0x18000000, 0x28000000, 0x38000000,
        };
        g_autofree char *name = g_strdup_printf("bk7258.dma-sram%u", i);

        bk7258_alias(&s->dma_sram[i], obj, name, &s->sram, &s->dma_memory,
                     base[i], SRAM_SIZE);
    }

    s->cpuclk = clock_new(obj, "cpuclk");
    /* Legacy diagnostic source remains independent of the optional clocks. */
    clock_set_hz(s->cpuclk, 26000000);
    s->busclk = clock_new(obj, "busclk");
    clock_set(s->busclk, clock_get(s->cpuclk));
    s->apllclk = clock_new(obj, "apllclk");
    s->xtalclk = clock_new(obj, "xtalclk");
    s->roscclk = clock_new(obj, "roscclk");
    s->div32kclk = clock_new(obj, "div32kclk");
    clock_set_hz(s->xtalclk, 26000000);
    clock_set_hz(s->roscclk, 32000);
    /* SDK nominal timebase; not a model of the physical divider/jitter. */
    clock_set_hz(s->div32kclk, 32000);
    qdev_connect_clock_in(DEVICE(&s->aon), "div32k", s->div32kclk);
    qdev_connect_clock_in(DEVICE(&s->aon), "rosc", s->roscclk);
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->aon), errp)) {
        return;
    }
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->entry_probe), errp)) {
        return;
    }
    if (s->entry_probe.has_otp) {
        memory_region_add_subregion(memory, 0x4b100000, &s->entry_probe.otp);
    }
    if (s->entry_probe.has_r7a) {
        memory_region_add_subregion_overlap(memory, 0x440001e8,
                                            &s->entry_probe.r7a, 1);
    }
    qdev_connect_clock_in(dev, "lpo", s->aon.lpo);
    qdev_connect_clock_in(DEVICE(&s->saradc), "clk", s->sadcclk);
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->saradc), errp)) {
        return;
    }
    if (s->experimental_saradc) {
        sysbus_mmio_map(SYS_BUS_DEVICE(&s->saradc), 0, 0x45890000);
        bk7258_alias(&s->saradc_ns, obj, "bk7258.saradc-ns",
                     &s->saradc.iomem, memory, 0x55890000, 0x1000);
    }
    for (i = 0; i < 3; i++) {
        DeviceState *cpu = DEVICE(&s->cpu[i]);
        g_autofree char *name = g_strdup_printf("bk7258.cpu%u", i);
        g_autofree char *itcm_name = g_strdup_printf("bk7258.itcm%u", i);
        g_autofree char *dtcm_name = g_strdup_printf("bk7258.dtcm%u", i);
        g_autofree char *clock_name = g_strdup_printf("coreclk%u", i);

        memory_region_init(&s->cpu_memory[i], obj, name, UINT64_C(1) << 32);
        memory_region_init_alias(&s->shared_alias[i], obj, "shared", memory,
                                 0, UINT64_C(1) << 32);
        memory_region_add_subregion_overlap(&s->cpu_memory[i], 0,
                                            &s->shared_alias[i], -1);
        if (!memory_region_init_ram(&s->itcm[i], obj, itcm_name, TCM_SIZE,
                                    errp) ||
            !memory_region_init_ram(&s->dtcm[i], obj, dtcm_name, TCM_SIZE,
                                    errp)) {
            return;
        }
        memory_region_add_subregion(&s->cpu_memory[i], 0, &s->itcm[i]);
        memory_region_add_subregion(&s->cpu_memory[i], 0x20000000, &s->dtcm[i]);
        bk7258_alias(&s->itcm_ns[i], obj, "itcm-ns", &s->itcm[i],
                     &s->cpu_memory[i], NS_OFFSET, TCM_SIZE);
        bk7258_alias(&s->dtcm_ns[i], obj, "dtcm-ns", &s->dtcm[i],
                     &s->cpu_memory[i], 0x30000000, TCM_SIZE);
        qdev_prop_set_string(cpu, "cpu-type", ARM_CPU_TYPE_NAME("cortex-m33"));
        qdev_prop_set_uint32(cpu, "num-irq", 64);
        qdev_prop_set_uint32(cpu, "num-prio-bits", 3);
        qdev_prop_set_uint32(cpu, "init-svtor", i ? 0 : s->boot_vector);
        qdev_prop_set_bit(cpu, "start-powered-off", i != 0);
        s->coreclk[i] = clock_new(obj, clock_name);
        clock_set(s->coreclk[i], clock_get(s->cpuclk));
        qdev_connect_clock_in(cpu, "cpuclk", s->coreclk[i]);
        s->refclk[i] = clock_new(obj, name);
        clock_set(s->refclk[i], clock_get(s->lpoclk));
        qdev_connect_clock_in(cpu, "refclk", s->refclk[i]);
        object_property_set_link(OBJECT(cpu), "memory",
                                 OBJECT(&s->cpu_memory[i]), &error_abort);
        if (!sysbus_realize(SYS_BUS_DEVICE(cpu), errp)) {
            return;
        }
    }

    for (i = 0; i < 2; i++) {
        DeviceState *wdt = DEVICE(&s->wdt[i]);
        SysBusDevice *bus = SYS_BUS_DEVICE(wdt);
        uint32_t address = i ? 0x44800000 : 0x44000600;
        g_autofree char *name = g_strdup_printf("wdtclk%u", i);

        s->wdtclk[i] = clock_new(obj, name);
        clock_set_hz(s->wdtclk[i], i ? 0 : 1000);
        qdev_prop_set_bit(wdt, "aon", i == 0);
        qdev_connect_clock_in(wdt, "clk", s->wdtclk[i]);
        if (!sysbus_realize(bus, errp)) {
            return;
        }
        sysbus_mmio_map(bus, 0, address);
        if (i) {
            sysbus_connect_irq(bus, 0,
                qdev_get_gpio_in_named(DEVICE(&s->cpu[0]), "NMI", 0));
        }
        bk7258_alias(&s->wdt_ns[i], obj, "bk7258.wdt-ns", &s->wdt[i].iomem,
                     memory, address + NS_OFFSET, i ? 0x1000 : 4);
    }

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->mailbox), errp)) {
        return;
    }
    for (i = 0; i < 3; i++) {
        MemoryRegion *space = i ? &s->cpu_memory[i] : memory;
        MemoryRegion *port = &s->mailbox.iomem[i];

        /* Separate bus views carry physical master identity, including NS. */
        memory_region_add_subregion(space, 0x41000000, port);
        bk7258_alias(&s->mailbox_ns[i], obj, "bk7258.mailbox-ns", port,
                     space, 0x51000000, 0x100);
        sysbus_connect_irq(SYS_BUS_DEVICE(&s->mailbox), i,
                           qdev_get_gpio_in_named(dev, "mailbox", i));
    }

    qdev_connect_clock_in(DEVICE(&s->ckmn), "reference", s->xtalclk);
    qdev_connect_clock_in(DEVICE(&s->ckmn), "measured", s->roscclk);
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->ckmn), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->ckmn), 0, 0x448a0000);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->ckmn), 0, qdev_get_gpio_in(dev, 21));
    bk7258_alias(&s->ckmn_ns, obj, "bk7258.ckmn-ns", &s->ckmn.iomem,
                 memory, 0x548a0000, 0x1000);

    sysbus_mmio_map(SYS_BUS_DEVICE(&s->aon), 0, 0x44000000);
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->aon), 1, 0x44000400);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->aon), 0, qdev_get_gpio_in(dev, 55));
    bk7258_alias(&s->aon_ns[0], obj, "bk7258.pmu-ns", &s->aon.pmu,
                 memory, 0x54000000, 0x200);
    bk7258_alias(&s->aon_ns[1], obj, "bk7258.gpio-ns", &s->aon.gpio,
                 memory, 0x54000400, 0x200);

    for (i = 0; i < 2; i++) {
        static const uint32_t base[] = { 0x44810000, 0x45800000 };
        static const unsigned irq[] = { 3, 13 };
        DeviceState *timer = DEVICE(&s->timer[i]);
        SysBusDevice *bus = SYS_BUS_DEVICE(timer);

        qdev_connect_clock_in(timer, "pclk", s->timerclk[i]);
        if (!sysbus_realize(bus, errp)) {
            return;
        }
        sysbus_mmio_map(bus, 0, base[i]);
        sysbus_connect_irq(bus, 0, qdev_get_gpio_in(dev, irq[i]));
        bk7258_alias(&s->timer_ns[i], obj, "bk7258.timer-ns",
                     &s->timer[i].iomem, memory, base[i] + NS_OFFSET, 0x100);
    }

    for (i = 0; i < 2; i++) {
        DeviceState *i2c = DEVICE(&s->i2c[i]);
        SysBusDevice *bus = SYS_BUS_DEVICE(i2c);
        uint32_t base = 0x45850000 + 0x10000 * i;
        g_autofree char *name = g_strdup_printf("i2c%u", i);

        qdev_prop_set_string(i2c, "bus-name", name);
        qdev_connect_clock_in(i2c, "pclk", s->i2cclk[i]);
        if (!sysbus_realize(bus, errp)) {
            return;
        }
        sysbus_mmio_map(bus, 0, base);
        sysbus_connect_irq(bus, 0, qdev_get_gpio_in(dev, i ? 14 : 6));
        bk7258_alias(&s->i2c_ns[i], obj, "bk7258.i2c-ns",
                     &s->i2c[i].iomem, memory, base + NS_OFFSET, 0x100);
    }

    for (i = 0; i < 2; i++) {
        DeviceState *spi = DEVICE(&s->spi[i]);
        SysBusDevice *bus = SYS_BUS_DEVICE(spi);
        uint32_t base = 0x44870000 + 0x1010000 * i;
        g_autofree char *name = g_strdup_printf("spi%u", i);

        qdev_prop_set_string(spi, "bus-name", name);
        qdev_connect_clock_in(spi, "pclk", s->spiclk[i]);
        if (!sysbus_realize(bus, errp)) {
            return;
        }
        sysbus_mmio_map(bus, 0, base);
        sysbus_connect_irq(bus, 0, qdev_get_gpio_in(dev, i ? 17 : 7));
        bk7258_alias(&s->spi_ns[i], obj, "bk7258.spi-ns",
                     &s->spi[i].iomem, memory, base + NS_OFFSET, 0x100);
    }

    for (i = 0; i < 2; i++) {
        SysBusDevice *bus = SYS_BUS_DEVICE(&s->pwm[i]);
        uint32_t base = 0x458a0000 + i * 0x50000;

        qdev_connect_clock_in(DEVICE(bus), "pclk", s->pwmclk[i]);
        if (!sysbus_realize(bus, errp)) {
            return;
        }
        sysbus_mmio_map(bus, 0, base);
        sysbus_connect_irq(bus, 0, qdev_get_gpio_in(dev, i ? 43 : 5));
        bk7258_alias(&s->pwm_ns[i], obj, "bk7258.pwm-ns",
                     &s->pwm[i].iomem, memory, base + NS_OFFSET, 0x100);
    }

    for (i = 0; i < 2; i++) {
        DeviceState *dma = DEVICE(&s->dma[i]);
        SysBusDevice *bus = SYS_BUS_DEVICE(dma);
        uint32_t base = 0x45020000 + 0x10000 * i;

        qdev_prop_set_uint8(dma, "unit", i);
        object_property_set_link(OBJECT(dma), "dma-memory",
                                 OBJECT(&s->dma_memory), &error_abort);
        /* Fixed diagnostic HCLK, not a measured DMA/core clock-tree ratio. */
        qdev_connect_clock_in(dma, "hclk", s->cpuclk);
        if (!sysbus_realize(bus, errp)) {
            return;
        }
        sysbus_mmio_map(bus, 0, base);
        sysbus_connect_irq(bus, 0, qdev_get_gpio_in(dev, i ? 57 : 0));
        bk7258_alias(&s->dma_ns[i], obj, "bk7258.dma-ns", &s->dma[i].iomem,
                     memory, base + NS_OFFSET, 0x400);
    }

    qdev_connect_clock_in(DEVICE(&s->rtc), "lpo", s->aon.lpo);
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->rtc), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->rtc), 0, 0x44000200);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->rtc), 0, qdev_get_gpio_in(dev, 54));
    bk7258_alias(&s->rtc_ns, obj, "bk7258.rtc-ns", &s->rtc.iomem,
                 memory, 0x54000200, 0x100);

    memory_region_init_io(&s->sysctrl, obj, &bk7258_sys_ops, s,
                          "bk7258.sysctrl", 0x1000);
    memory_region_add_subregion(memory, SYS_BASE, &s->sysctrl);
    bk7258_alias(&s->sysctrl_ns, obj, "bk7258.sysctrl-ns", &s->sysctrl,
                 memory, SYS_BASE + NS_OFFSET, 0x1000);
    for (i = 0; i < 3; i++) {
        DeviceState *uart = DEVICE(&s->uart[i]);
        SysBusDevice *bus = SYS_BUS_DEVICE(uart);
        g_autofree char *name = g_strdup_printf("uartclk%u", i);

        s->uartclk[i] = clock_new(obj, name);
        qdev_connect_clock_in(uart, "pclk", s->uartclk[i]);
        qdev_prop_set_chr(uart, "chardev", serial_hd(i));
        if (!sysbus_realize(bus, errp)) {
            return;
        }
        sysbus_mmio_map(bus, 0, uart_base[i]);
        sysbus_connect_irq(bus, 0, qdev_get_gpio_in(dev, uart_irq[i]));
        bk7258_alias(&s->uart_ns[i], obj, "bk7258.uart-ns", &s->uart[i].iomem,
                     memory, uart_base[i] + NS_OFFSET, 0x1000);
    }
}

static void bk7258_reset(DeviceState *dev)
{
    BK7258State *s = BK7258_SOC(dev);

    s->cpu_control[0] = s->boot_vector | RESET_RELEASE;
    s->cpu_control[1] = POWER_DOWN;
    s->cpu_control[2] = POWER_DOWN | CPU_HALT;
    memset(s->irq_enable, 0, sizeof(s->irq_enable));
    s->irq_levels = 0;
    memset(s->private_irqs, 0, sizeof(s->private_irqs));
    s->clock_select = s->peripheral_clocks = s->clock_mode = 0;
    s->power_sleep = TICK_ROUTES;
    /* Direct-load model convention, not a claim about the silicon POR value. */
    s->flash_bus_config = 0;
    memset(s->gpio_mux, 0, sizeof(s->gpio_mux));
    memset(s->analog, 0, sizeof(s->analog));
    s->apll_hz = 0;
    s->apll_pulse_armed = false;
    clock_update_hz(s->apllclk, 0);
    s->core_clock_key = 0;
    s->core_clock_unimplemented = false;
    bk7258_update_core_clocks(s);
    bk7258_update_saradc_clock(s);
    clock_update_hz(s->roscclk, 32000);
    s->analog_busy = 0;
    timer_del(s->analog_timer);
    bk7258_update_tick_clocks(s);
    bk7258_update_wdt_clock(s);
    bk7258_update_uart_clocks(s);
    bk7258_update_timer_clocks(s);
    bk7258_update_i2c_clocks(s);
    bk7258_update_spi_clocks(s);
    bk7258_update_pwm_clocks(s);
    bk7258_update_irqs(s);
}

static void bk7258_init(Object *obj)
{
    BK7258State *s = BK7258_SOC(obj);
    unsigned i;

    for (i = 0; i < 3; i++) {
        object_initialize_child(obj, "cpu[*]", &s->cpu[i], TYPE_ARMV7M);
        object_initialize_child(obj, "uart[*]", &s->uart[i], TYPE_BK7258_UART);
    }
    for (i = 0; i < 2; i++) {
        g_autofree char *name = g_strdup_printf("timerclk%u", i);

        object_initialize_child(obj, "timer[*]", &s->timer[i],
                                TYPE_BK7258_TIMER);
        s->timerclk[i] = clock_new(obj, name);
        g_clear_pointer(&name, g_free);
        name = g_strdup_printf("i2cclk%u", i);
        object_initialize_child(obj, "i2c[*]", &s->i2c[i], TYPE_BK7258_I2C);
        s->i2cclk[i] = clock_new(obj, name);
        g_clear_pointer(&name, g_free);
        name = g_strdup_printf("spiclk%u", i);
        object_initialize_child(obj, "spi[*]", &s->spi[i], TYPE_BK7258_SPI);
        s->spiclk[i] = clock_new(obj, name);
        g_clear_pointer(&name, g_free);
        name = g_strdup_printf("pwmclk%u", i);
        object_initialize_child(obj, "pwm[*]", &s->pwm[i], TYPE_BK7258_PWM);
        s->pwmclk[i] = clock_new(obj, name);
        object_initialize_child(obj, "dma[*]", &s->dma[i], TYPE_BK7258_DMA);
        object_initialize_child(obj, "wdt[*]", &s->wdt[i], TYPE_BK7258_WDT);
    }
    object_initialize_child(obj, "saradc", &s->saradc, TYPE_BK7258_SARADC);
    object_initialize_child(obj, "entry-probe", &s->entry_probe,
                            TYPE_BK7258_ENTRY_PROBE);
    s->sadcclk = clock_new(obj, "sadcclk");
    object_initialize_child(obj, "aon", &s->aon, TYPE_BK7258_AON);
    object_initialize_child(obj, "rtc", &s->rtc, TYPE_BK7258_RTC);
    s->lpoclk = qdev_init_clock_in(DEVICE(obj), "lpo", bk7258_lpo_changed,
                                  s, ClockUpdate);
    qdev_alias_clock(DEVICE(&s->aon), "x32k", DEVICE(obj), "lpo-external");
    object_initialize_child(obj, "ckmn", &s->ckmn, TYPE_BK7258_CKMN);
    object_initialize_child(obj, "mailbox", &s->mailbox, TYPE_BK7258_MAILBOX);
    object_initialize_child(obj, "flashctrl", &s->flashctrl, TYPE_BK7258_FLASH);
    object_property_add_alias(obj, "flash-nor", OBJECT(&s->flashctrl), "nor");
    qdev_init_gpio_in_named(DEVICE(obj), bk7258_mailbox_irq, "mailbox", 3);
    qdev_init_gpio_in(DEVICE(obj), bk7258_irq, 64);
    s->analog_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                   bk7258_analog_complete, s);
}

static void bk7258_finalize(Object *obj)
{
    timer_free(BK7258_SOC(obj)->analog_timer);
}

static const Property bk7258_properties[] = {
    DEFINE_PROP_BOOL("experimental-saradc", BK7258State,
                     experimental_saradc, false),
    DEFINE_PROP_BOOL("experimental-core-clocks", BK7258State,
                     experimental_core_clocks, false),
    DEFINE_PROP_BOOL("experimental-spi-apll", BK7258State,
                     experimental_spi_apll, false),
    DEFINE_PROP_BOOL("diagnostic-xip", BK7258State, diagnostic_xip, false),
    DEFINE_PROP_UINT32("xip-size", BK7258State, flash_size,
                       (8 * MiB / 34) * 32),
    DEFINE_PROP_UINT32("boot-vector", BK7258State, boot_vector, 0x02010000),
};

static const VMStateDescription bk7258_vmstate = {
    .name = "bk7258-soc",
    .unmigratable = true,
};

static void bk7258_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = bk7258_realize;
    dc->vmsd = &bk7258_vmstate;
    device_class_set_legacy_reset(dc, bk7258_reset);
    device_class_set_props(dc, bk7258_properties);
    dc->user_creatable = false;
}

static const TypeInfo bk7258_types[] = {
    {
        .name = TYPE_BK7258_SOC,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(BK7258State),
        .instance_init = bk7258_init,
        .instance_finalize = bk7258_finalize,
        .class_init = bk7258_class_init,
    },
};

DEFINE_TYPES(bk7258_types)
