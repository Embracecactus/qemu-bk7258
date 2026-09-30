/*
 * BK7258 diagnostic SoC and three board machine definitions.
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Register provenance and explicit omissions: docs/system/arm/bk7258.rst.
 */
#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qemu/log.h"
#include "qapi/error.h"
#include "hw/arm/bk7258.h"
#include "hw/arm/boot.h"
#include "hw/core/boards.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-clock.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "system/address-spaces.h"
#include "system/system.h"
#include "target/arm/internals.h"
#include "migration/vmstate.h"

#define SRAM_SIZE (640 * KiB)
#define TCM_SIZE (16 * KiB)
#define FLASH_BASE 0x02000000
#define SRAM_BASE 0x28000000
#define SYS_BASE 0x44010000
#define NS_OFFSET 0x10000000
#define RESET_RELEASE 1U
#define POWER_DOWN 2U
#define CPU_HALT 8U

static const uint32_t uart_base[] = { 0x44820000, 0x45830000, 0x45840000 };
static const unsigned uart_irq[] = { 4, 15, 16 };

static void bk7258_update_irqs(BK7258State *s)
{
    unsigned cpu, uart;

    for (cpu = 0; cpu < 3; cpu++) {
        for (uart = 0; uart < 3; uart++) {
            qemu_set_irq(qdev_get_gpio_in(DEVICE(&s->cpu[cpu]), uart_irq[uart]),
                         !(s->cpu_control[cpu] & 4) &&
                         !!(s->uart_levels & s->irq_enable[cpu][0] &
                            (1U << uart_irq[uart])));
        }
    }
}

static void bk7258_uart_irq(void *opaque, int n, int level)
{
    BK7258State *s = opaque;
    uint32_t bit = 1U << uart_irq[n];

    s->uart_levels = (s->uart_levels & ~bit) | (level ? bit : 0);
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
    case 0x80 ... 0x94:
        index = (offset - 0x80) / 4;
        return s->irq_enable[index / 2][index % 2];
    case 0xa0 ... 0xb4:
        index = (offset - 0xa0) / 4;
        return index % 2 ? 0 : s->uart_levels & s->irq_enable[index / 2][0];
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

    if (!memory_region_init_rom(&s->flash, obj, "bk7258.xip", s->flash_size,
                                errp) ||
        !memory_region_init_ram(&s->sram, obj, "bk7258.sram", SRAM_SIZE,
                                errp)) {
        return;
    }
    memory_region_add_subregion(memory, FLASH_BASE, &s->flash);
    memory_region_add_subregion(memory, SRAM_BASE, &s->sram);
    bk7258_alias(&s->flash_ns, obj, "bk7258.xip-ns", &s->flash, memory,
                 FLASH_BASE + NS_OFFSET, s->flash_size);
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
        sysbus_connect_irq(bus, 0, qdev_get_gpio_in(dev, i));
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
    s->uart_levels = 0;
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
    qdev_init_gpio_in(DEVICE(obj), bk7258_uart_irq, 3);
}

static const Property bk7258_properties[] = {
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

static void bk7258_machine_init(MachineState *machine)
{
    DeviceState *dev = qdev_new(TYPE_BK7258_SOC);
    BK7258State *soc = BK7258_SOC(dev);
    unsigned i;

    object_property_add_child(OBJECT(machine), "soc", OBJECT(dev));
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    for (i = 0; i < 3; i++) {
        armv7m_load_kernel(soc->cpu[i].cpu,
                          i ? NULL : machine->kernel_filename,
                          soc->boot_vector,
                          soc->flash_size - (soc->boot_vector - FLASH_BASE));
    }
}

static void bk7258_machine_class_init(ObjectClass *klass, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(klass);

    static const char * const valid_cpu_types[] = {
        ARM_CPU_TYPE_NAME("cortex-m33"), NULL,
    };

    mc->desc = data;
    mc->valid_cpu_types = valid_cpu_types;
    mc->init = bk7258_machine_init;
    mc->default_cpus = mc->min_cpus = mc->max_cpus = 3;
    mc->default_cpu_type = ARM_CPU_TYPE_NAME("cortex-m33");
    mc->no_floppy = true;
    mc->no_cdrom = true;
    mc->no_parallel = true;
}

static const TypeInfo bk7258_types[] = {
    {
        .name = TYPE_BK7258_SOC,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(BK7258State),
        .instance_init = bk7258_init,
        .class_init = bk7258_class_init,
    }, {
        .name = MACHINE_TYPE_NAME("t5_board"),
        .parent = TYPE_MACHINE,
        .class_init = bk7258_machine_class_init,
        .class_data = "BK7258 T5 board (experimental diagnostic model)",
    }, {
        .name = MACHINE_TYPE_NAME("t5ai_core"),
        .parent = TYPE_MACHINE,
        .class_init = bk7258_machine_class_init,
        .class_data = "BK7258 T5-AI Core (experimental diagnostic model)",
    }, {
        .name = MACHINE_TYPE_NAME("aidk_ai_toy"),
        .parent = TYPE_MACHINE,
        .class_init = bk7258_machine_class_init,
        .class_data = "BK7258 AIDK AI Toy (experimental diagnostic model)",
    },
};

DEFINE_TYPES(bk7258_types)
