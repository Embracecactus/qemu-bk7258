/*
 * BK7258 AON PMU configuration/retention and digital GPIO.
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Register provenance: pinned SDK aon_pmu_{struct,ll}.h and gpio_{struct,ll}.h.
 * This models register transactions and digital pads, not analog power rails.
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/misc/bk7258_aon.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-clock.h"
#include "hw/core/qdev-properties.h"

static void bk7258_lpo_update(BK7258AONState *s)
{
    unsigned source = s->wake_config & 3;

    /* R41: DIVD=0, external 32k=1, ROSC=2. No source is defined for 3. */
    clock_update(s->lpo, source < ARRAY_SIZE(s->lpo_source) ?
                 clock_get(s->lpo_source[source]) : 0);
}

static void bk7258_lpo_source_update(void *opaque, ClockEvent event)
{
    bk7258_lpo_update(BK7258_AON(opaque));
}

static bool bk7258_gpio_level(BK7258AONState *s, unsigned pin)
{
    uint32_t config = s->latched_pin[pin];
    uint64_t bit = UINT64_C(1) << pin;

    if (!(config & (1U << 3)) && !(config & (1U << 6))) {
        return !!(config & 2);
    }
    if (s->connected & bit) {
        return !!(s->inputs & bit);
    }
    return (config & (1U << 5)) && (config & (1U << 4));
}

static void bk7258_gpio_update(BK7258AONState *s, unsigned pin, bool old)
{
    uint32_t config;
    bool level, enabled;
    unsigned type;
    uint64_t bit = UINT64_C(1) << pin;
    uint64_t irq_enable = 0;

    if (!s->gpio_locked) {
        s->latched_pin[pin] = s->pin[pin];
    }
    config = s->latched_pin[pin];
    level = bk7258_gpio_level(s, pin);
    enabled = (config & (1U << 12)) && (config & (1U << 2));
    type = (config >> 10) & 3;

    if (enabled && ((type == 0 && !level) || (type == 1 && level) ||
                    (type == 2 && !old && level) ||
                    (type == 3 && old && !level))) {
        s->irq_status |= bit;
    }
    if (!s->gpio_locked) {
        qemu_set_irq(s->pin_out[pin],
                     !(config & ((1U << 3) | (1U << 6))) && (config & 2));
    }
    for (unsigned i = 0; i < BK7258_GPIO_COUNT; i++) {
        if (s->latched_pin[i] & (1U << 12)) {
            irq_enable |= UINT64_C(1) << i;
        }
    }
    /* Mask reporting without discarding the software-cleared status latch. */
    qemu_set_irq(s->irq, (s->irq_status & irq_enable) != 0);
}

static void bk7258_gpio_input(void *opaque, int pin, int level)
{
    BK7258AONState *s = opaque;
    uint64_t bit = UINT64_C(1) << pin;
    bool old = bk7258_gpio_level(s, pin);

    if (level < BK7258_GPIO_FLOAT || level > 1) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "bk7258-gpio: invalid external level %d\n", level);
        return;
    }
    if (level == BK7258_GPIO_FLOAT) {
        s->connected &= ~bit;
    } else {
        s->connected |= bit;
        s->inputs = (s->inputs & ~bit) | (level ? bit : 0);
    }
    bk7258_gpio_update(s, pin, old);
}

static MemTxResult bk7258_pmu_read(void *opaque, hwaddr addr, uint64_t *value,
                                  unsigned size, MemTxAttrs attrs)
{
    BK7258AONState *s = opaque;

    switch (addr) {
    case 0x00:
        *value = s->r0;
        break;
    case 0x04:
        *value = s->r1;
        break;
    case 0x08:
        *value = s->r2;
        break;
    case 0x94:
        *value = 0; /* write-only commit keys */
        break;
    case 0x100:
        *value = s->sleep_config;
        break;
    case 0x104:
        *value = s->wake_config;
        break;
    case 0x1ec:
        *value = s->retained;
        break;
    case 0x1f0:
        *value = s->chip_id;
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "bk7258-aon: read offset 0x%" HWADDR_PRIx
                      " is not implemented\n", addr);
        return MEMTX_ERROR;
    }
    return MEMTX_OK;
}

static MemTxResult bk7258_pmu_write(void *opaque, hwaddr addr, uint64_t value,
                                   unsigned size, MemTxAttrs attrs)
{
    BK7258AONState *s = opaque;

    switch (addr) {
    case 0x00:
        s->r0 = value;
        break;
    case 0x04:
        s->r1 = value & 0x003fffff;
        break;
    case 0x08:
        s->r2 = value & 0x1ff;
        break;
    case 0x100:
        s->sleep_config = value;
        break;
    case 0x104:
        s->wake_config = value;
        if ((value & 3) == 3) {
            qemu_log_mask(LOG_UNIMP, "bk7258-aon: undefined LPO source 3\n");
        }
        bk7258_lpo_update(s);
        break;
    case 0x94:
        if (value == 0xbdb4aa55 && s->commit_key) {
            s->retained = s->r0;
            s->gpio_locked = !!(s->r0 & (1U << 31));
            for (unsigned i = 0; i < BK7258_GPIO_COUNT; i++) {
                bk7258_gpio_update(s, i, bk7258_gpio_level(s, i));
            }
        }
        s->commit_key = value == 0x424b55aa;
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "bk7258-aon: write offset 0x%" HWADDR_PRIx
                      " is not implemented\n", addr);
        return MEMTX_ERROR;
    }
    return MEMTX_OK;
}

static MemTxResult bk7258_gpio_read(void *opaque, hwaddr addr, uint64_t *value,
                                   unsigned size, MemTxAttrs attrs)
{
    BK7258AONState *s = opaque;
    unsigned pin = addr / 4;

    if (pin < BK7258_GPIO_COUNT) {
        *value = s->pin[pin] | ((s->latched_pin[pin] & 0x84) &&
                               bk7258_gpio_level(s, pin));
    } else if (addr == 0x100 || addr == 0x104) {
        *value = s->irq_status >> (addr == 0x100 ? 0 : 32);
    } else {
        qemu_log_mask(LOG_UNIMP, "bk7258-gpio: read offset 0x%" HWADDR_PRIx
                      " is not implemented\n", addr);
        return MEMTX_ERROR;
    }
    return MEMTX_OK;
}

static MemTxResult bk7258_gpio_write(void *opaque, hwaddr addr, uint64_t value,
                                    unsigned size, MemTxAttrs attrs)
{
    BK7258AONState *s = opaque;
    unsigned pin = addr / 4;

    if (pin < BK7258_GPIO_COUNT) {
        bool old = bk7258_gpio_level(s, pin);
        if (value & (1U << 13)) {
            s->irq_status &= ~(UINT64_C(1) << pin);
        }
        /* Pad input is read-only; per-pin interrupt clear is W1C. */
        s->pin[pin] = value & 0x1ffe;
        bk7258_gpio_update(s, pin, old);
    } else if (addr == 0x100 || addr == 0x104) {
        s->irq_status &= ~(value << (addr == 0x100 ? 0 : 32));
        for (unsigned i = 0; i < BK7258_GPIO_COUNT; i++) {
            bk7258_gpio_update(s, i, bk7258_gpio_level(s, i));
        }
    } else {
        qemu_log_mask(LOG_UNIMP, "bk7258-gpio: write offset 0x%" HWADDR_PRIx
                      " is not implemented\n", addr);
        return MEMTX_ERROR;
    }
    return MEMTX_OK;
}

static const MemoryRegionOps bk7258_pmu_ops = {
    .read_with_attrs = bk7258_pmu_read,
    .write_with_attrs = bk7258_pmu_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static const MemoryRegionOps bk7258_gpio_ops = {
    .read_with_attrs = bk7258_gpio_read,
    .write_with_attrs = bk7258_gpio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void bk7258_aon_reset(DeviceState *dev)
{
    BK7258AONState *s = BK7258_AON(dev);

    /* Retained GPIO/boot state survives a warm system reset in this model. */
    s->r0 = s->retained;
    s->r1 = s->r2 = s->sleep_config = s->wake_config = 0;
    bk7258_lpo_update(s);
    s->commit_key = false;
    s->gpio_locked = !!(s->retained & (1U << 31));
    s->irq_status = 0;
    for (unsigned i = 0; i < BK7258_GPIO_COUNT; i++) {
        bool old = bk7258_gpio_level(s, i);

        s->pin[i] = 0x28; /* diagnostic initial pad: output off, pull-down */
        /* Retaining a high pad across reset must not create a rising edge. */
        bk7258_gpio_update(s, i, old);
    }
}

static void bk7258_aon_init(Object *obj)
{
    BK7258AONState *s = BK7258_AON(obj);
    SysBusDevice *bus = SYS_BUS_DEVICE(obj);
    static const char *const source[] = { "div32k", "x32k", "rosc" };

    for (unsigned i = 0; i < ARRAY_SIZE(source); i++) {
        s->lpo_source[i] = qdev_init_clock_in(DEVICE(obj), source[i],
                          bk7258_lpo_source_update, s, ClockUpdate);
    }
    s->lpo = qdev_init_clock_out(DEVICE(obj), "lpo");

    memory_region_init_io(&s->pmu, obj, &bk7258_pmu_ops, s,
                          "bk7258-aon-pmu", 0x200);
    memory_region_init_io(&s->gpio, obj, &bk7258_gpio_ops, s,
                          "bk7258-aon-gpio", 0x200);
    sysbus_init_mmio(bus, &s->pmu);
    sysbus_init_mmio(bus, &s->gpio);
    sysbus_init_irq(bus, &s->irq);
    qdev_init_gpio_in_named(DEVICE(obj), bk7258_gpio_input,
                             "gpio-in", BK7258_GPIO_COUNT);
    qdev_init_gpio_out_named(DEVICE(obj), s->pin_out,
                              "gpio-out", BK7258_GPIO_COUNT);
}

static const Property bk7258_aon_properties[] = {
    DEFINE_PROP_UINT32("chip-id", BK7258AONState, chip_id, 0x24940610),
};

static void bk7258_aon_realize(DeviceState *dev, Error **errp)
{
    bk7258_lpo_update(BK7258_AON(dev));
}

static void bk7258_aon_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = bk7258_aon_realize;
    device_class_set_legacy_reset(dc, bk7258_aon_reset);
    device_class_set_props(dc, bk7258_aon_properties);
    dc->user_creatable = false;
}

static const TypeInfo bk7258_aon_info = {
    .name = TYPE_BK7258_AON,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(BK7258AONState),
    .instance_init = bk7258_aon_init,
    .class_init = bk7258_aon_class_init,
};

static void bk7258_aon_register_types(void)
{
    type_register_static(&bk7258_aon_info);
}

type_init(bk7258_aon_register_types)
