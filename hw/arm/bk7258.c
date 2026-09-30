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

static void bk7258_update_tick_clocks(BK7258State *s)
{
    static const unsigned bit[] = { 29, 30, 27 };

    for (unsigned i = 0; i < 3; i++) {
        clock_update_hz(s->refclk[i],
                        s->power_sleep & (1U << bit[i]) ? 32000 : 0);
    }
}

static void bk7258_analog_complete(void *opaque)
{
    BK7258State *s = opaque;

    for (unsigned i = 0; i < ARRAY_SIZE(s->analog); i++) {
        if (s->analog_busy & (1U << i)) {
            s->analog[i] = s->analog_pending[i];
        }
    }
    s->analog_busy = 0;
    clock_update_hz(s->roscclk, s->analog[5] & (1U << 14) ? 0 : 32000);
}

static void bk7258_update_wdt_clock(BK7258State *s)
{
    unsigned divider = 1U << (((s->clock_select >> 2) & 3) + 1);

    clock_update_hz(s->wdtclk[1],
                    s->peripheral_clocks & (1U << 31) ? 32000 / divider : 0);
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

static uint64_t bk7258_sys_read(void *opaque, hwaddr offset, unsigned size)
{
    BK7258State *s = opaque;
    unsigned index;

    switch (offset) {
    case 0x10:
    case 0x14:
    case 0x18:
        return s->cpu_control[(offset - 0x10) / 4];
    case 0x20:
        return s->clock_mode;
    case 0x28:
        return s->clock_select;
    case 0x40:
        return s->power_sleep;
    case 0xc0 ... 0xd8:
        return s->gpio_mux[(offset - 0xc0) / 4];
    case 0xe8:
        return s->analog_busy;
    case 0x100 ... 0x16c:
        return s->analog[(offset - 0x100) / 4];
    case 0x30:
        return s->peripheral_clocks;
    case 0x80 ... 0x94:
        index = (offset - 0x80) / 4;
        return s->irq_enable[index / 2][index % 2];
    case 0xa0 ... 0xb4:
        index = (offset - 0xa0) / 4;
        return ((s->irq_levels | s->private_irqs[index / 2]) >>
                (32 * (index % 2))) &
               s->irq_enable[index / 2][index % 2];
    default:
        qemu_log_mask(LOG_UNIMP, "bk7258-sys: read offset 0x%" HWADDR_PRIx
                      " is not implemented\n", offset);
        return 0;
    }
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

static void bk7258_sys_write(void *opaque, hwaddr offset,
                             uint64_t value, unsigned size)
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
        bk7258_update_irqs(s);
        break;
    case 0x20:
        s->clock_mode = value;
        break;
    case 0x40:
        s->power_sleep = value;
        bk7258_update_tick_clocks(s);
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
        bk7258_update_wdt_clock(s);
        break;
    case 0x30:
        s->peripheral_clocks = value;
        bk7258_update_wdt_clock(s);
        break;
    case 0x80 ... 0x94:
        index = (offset - 0x80) / 4;
        s->irq_enable[index / 2][index % 2] = value;
        bk7258_update_irqs(s);
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "bk7258-sys: write offset 0x%" HWADDR_PRIx
                      " is not implemented\n", offset);
        break;
    }
}

static const MemoryRegionOps bk7258_sys_ops = {
    .read = bk7258_sys_read,
    .write = bk7258_sys_write,
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
        if (s->flash_size != BK7258_XIP_SIZE) {
            error_setg(errp,
                       "BK7258 physical NOR mode requires its exact XIP size");
            return;
        }
        xip = &s->flashctrl.xip;
        sysbus_mmio_map(SYS_BUS_DEVICE(&s->flashctrl), 0, 0x44030000);
        bk7258_alias(&s->flashctrl_ns, obj, "bk7258.flashctrl-ns",
                     &s->flashctrl.regs, memory, 0x54030000, 0x1000);
    } else {
        if (s->flashctrl.nor.blk || s->flashctrl.nor.status_blk) {
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

    s->cpuclk = clock_new(obj, "cpuclk");
    /* Fixed diagnostic clock; PLL/DVFS and clock-gating are not modeled. */
    clock_set_hz(s->cpuclk, 26000000);
    for (i = 0; i < 3; i++) {
        DeviceState *cpu = DEVICE(&s->cpu[i]);
        g_autofree char *name = g_strdup_printf("bk7258.cpu%u", i);
        g_autofree char *itcm_name = g_strdup_printf("bk7258.itcm%u", i);
        g_autofree char *dtcm_name = g_strdup_printf("bk7258.dtcm%u", i);

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
        qdev_connect_clock_in(cpu, "cpuclk", s->cpuclk);
        s->refclk[i] = clock_new(obj, name);
        clock_set_hz(s->refclk[i], 32000);
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

    s->xtalclk = clock_new(obj, "xtalclk");
    s->roscclk = clock_new(obj, "roscclk");
    clock_set_hz(s->xtalclk, 26000000);
    clock_set_hz(s->roscclk, 32000);
    qdev_connect_clock_in(DEVICE(&s->ckmn), "reference", s->xtalclk);
    qdev_connect_clock_in(DEVICE(&s->ckmn), "measured", s->roscclk);
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->ckmn), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->ckmn), 0, 0x448a0000);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->ckmn), 0, qdev_get_gpio_in(dev, 21));
    bk7258_alias(&s->ckmn_ns, obj, "bk7258.ckmn-ns", &s->ckmn.iomem,
                 memory, 0x548a0000, 0x1000);

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->aon), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->aon), 0, 0x44000000);
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->aon), 1, 0x44000400);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->aon), 0, qdev_get_gpio_in(dev, 55));
    bk7258_alias(&s->aon_ns[0], obj, "bk7258.pmu-ns", &s->aon.pmu,
                 memory, 0x54000000, 0x200);
    bk7258_alias(&s->aon_ns[1], obj, "bk7258.gpio-ns", &s->aon.gpio,
                 memory, 0x54000400, 0x200);

    memory_region_init_io(&s->sysctrl, obj, &bk7258_sys_ops, s,
                          "bk7258.sysctrl", 0x1000);
    memory_region_add_subregion(memory, SYS_BASE, &s->sysctrl);
    bk7258_alias(&s->sysctrl_ns, obj, "bk7258.sysctrl-ns", &s->sysctrl,
                 memory, SYS_BASE + NS_OFFSET, 0x1000);
    for (i = 0; i < 3; i++) {
        DeviceState *uart = DEVICE(&s->uart[i]);
        SysBusDevice *bus = SYS_BUS_DEVICE(uart);

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
    memset(s->gpio_mux, 0, sizeof(s->gpio_mux));
    memset(s->analog, 0, sizeof(s->analog));
    clock_update_hz(s->roscclk, 32000);
    s->analog_busy = 0;
    timer_del(s->analog_timer);
    bk7258_update_tick_clocks(s);
    bk7258_update_wdt_clock(s);
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
        object_initialize_child(obj, "wdt[*]", &s->wdt[i], TYPE_BK7258_WDT);
    }
    object_initialize_child(obj, "aon", &s->aon, TYPE_BK7258_AON);
    object_initialize_child(obj, "ckmn", &s->ckmn, TYPE_BK7258_CKMN);
    object_initialize_child(obj, "mailbox", &s->mailbox, TYPE_BK7258_MAILBOX);
    object_initialize_child(obj, "flashctrl", &s->flashctrl, TYPE_BK7258_FLASH);
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
