/*
 * BK7258 board machines and explicit diagnostic image loading.
 * SPDX-License-Identifier: GPL-2.0-or-later
 * AI-assisted downstream experiment; not an upstream contribution.
 */
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "hw/arm/bk7258.h"
#include "hw/arm/boot.h"
#include "hw/core/boards.h"
#include "hw/core/irq.h"
#include "hw/misc/led.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "system/blockdev.h"

#define TYPE_BK7258_MACHINE MACHINE_TYPE_NAME("bk7258-base")
OBJECT_DECLARE_TYPE(BK7258MachineState, BK7258MachineClass, BK7258_MACHINE)

typedef struct BK7258BoardInfo {
    const char *description;
    unsigned num_keys;
    unsigned num_leds;
    struct {
        unsigned pin;
        bool pullup;
    } keys[3];
    struct {
        unsigned pin;
        LEDColor color;
    } leds[2];
} BK7258BoardInfo;

struct BK7258MachineState {
    MachineState parent_obj;
    BK7258State *soc;
    LEDState *led[2];
    bool key_pressed[3];
};

struct BK7258MachineClass {
    MachineClass parent_class;
    const BK7258BoardInfo *board;
};

/* Electrical sources and revision limits: docs/system/arm/bk7258.rst. */
static const BK7258BoardInfo board_info[] = {
    {
        .description = "BK7258 T5-Board (partial V1.0.2 wiring)",
        .num_keys = 1, .num_leds = 1,
        .keys = { { 12, true } },
        .leds = { { 1, LED_COLOR_GREEN } },
    }, {
        .description = "BK7258 T5AI-Core (partial V1.0.1 wiring)",
        .num_keys = 1, .num_leds = 1,
        .keys = { { 29, true } },
        .leds = { { 9, LED_COLOR_GREEN } },
    }, {
        .description = "BK7258 AIDK AI Toy (partial V1.0 wiring)",
        .num_keys = 3, .num_leds = 2,
        /* Index zero preserves the original user-key (KEY3) interface. */
        .keys = { { 8, false }, { 13, false }, { 12, false } },
        /* Net LED1 drives red LED3; net LED2 drives green LED4. */
        .leds = { { 40, LED_COLOR_RED }, { 41, LED_COLOR_GREEN } },
    },
};

static void bk7258_key_update(BK7258MachineState *s, unsigned index)
{
    const BK7258BoardInfo *board = BK7258_MACHINE_GET_CLASS(s)->board;
    int level;

    assert(index < board->num_keys);
    level = s->key_pressed[index] ? 0 :
            board->keys[index].pullup ? 1 : BK7258_GPIO_FLOAT;

    if (s->soc) {
        qemu_set_irq(qdev_get_gpio_in_named(DEVICE(&s->soc->aon), "gpio-in",
                                           board->keys[index].pin), level);
    }
}

static bool bk7258_get_key(Object *obj, Error **errp)
{
    return BK7258_MACHINE(obj)->key_pressed[0];
}

static void bk7258_set_key(Object *obj, bool pressed, Error **errp)
{
    BK7258MachineState *s = BK7258_MACHINE(obj);

    s->key_pressed[0] = pressed;
    bk7258_key_update(s, 0);
}

static bool bk7258_get_key1(Object *obj, Error **errp)
{
    return BK7258_MACHINE(obj)->key_pressed[1];
}

static void bk7258_set_key1(Object *obj, bool pressed, Error **errp)
{
    BK7258MachineState *s = BK7258_MACHINE(obj);

    s->key_pressed[1] = pressed;
    bk7258_key_update(s, 1);
}

static bool bk7258_get_key2(Object *obj, Error **errp)
{
    return BK7258_MACHINE(obj)->key_pressed[2];
}

static void bk7258_set_key2(Object *obj, bool pressed, Error **errp)
{
    BK7258MachineState *s = BK7258_MACHINE(obj);

    s->key_pressed[2] = pressed;
    bk7258_key_update(s, 2);
}

static bool bk7258_get_led(Object *obj, Error **errp)
{
    BK7258MachineState *s = BK7258_MACHINE(obj);

    return s->led[0] && led_get_intensity(s->led[0]) != 0;
}

static bool bk7258_get_led2(Object *obj, Error **errp)
{
    BK7258MachineState *s = BK7258_MACHINE(obj);

    return s->led[1] && led_get_intensity(s->led[1]) != 0;
}

static void bk7258_machine_init(MachineState *machine)
{
    BK7258MachineState *s = BK7258_MACHINE(machine);
    const BK7258BoardInfo *board = BK7258_MACHINE_GET_CLASS(s)->board;
    DeviceState *dev = qdev_new(TYPE_BK7258_SOC);
    /* Shared experiment part choice, not a verified BOM for every board. */
    DeviceState *nor = qdev_new(TYPE_BK7258_NOR);
    BK7258State *soc = BK7258_SOC(dev);
    unsigned i;

    object_property_add_child(OBJECT(machine), "soc", OBJECT(dev));
    object_property_add_child(OBJECT(machine), "nor", OBJECT(nor));
    if (drive_get(IF_PFLASH, 0, 1) && !drive_get(IF_PFLASH, 0, 0)) {
        error_report("BK7258: NOR status drive requires the array drive");
        exit(1);
    }
    for (i = 0; i < 2; i++) {
        DriveInfo *drive = drive_get(IF_PFLASH, 0, i);
        if (drive) {
            if (!i && machine->kernel_filename) {
                error_report("BK7258: choose physical NOR image or "
                             "diagnostic ELF, not both");
                exit(1);
            }
            qdev_prop_set_drive(nor,
                                i ? "status-drive" : "drive",
                                blk_by_legacy_dinfo(drive));
        }
    }
    qdev_realize_and_unref(nor, NULL, &error_fatal);
    object_property_set_link(OBJECT(dev), "flash-nor", OBJECT(nor),
                             &error_abort);
    /* Loading a logical kernel is an explicit diagnostic board policy. */
    qdev_prop_set_bit(dev, "diagnostic-xip", machine->kernel_filename != NULL);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    s->soc = soc;
    for (i = 0; i < board->num_leds; i++) {
        s->led[i] = led_create_simple(OBJECT(machine),
                                      GPIO_POLARITY_ACTIVE_HIGH,
                                      board->leds[i].color,
                                      i ? "led2" : "user-led");
        qdev_connect_gpio_out_named(DEVICE(&soc->aon), "gpio-out",
                                    board->leds[i].pin,
                                    qdev_get_gpio_in(DEVICE(s->led[i]), 0));
    }
    for (i = 0; i < board->num_keys; i++) {
        bk7258_key_update(s, i);
    }

    for (i = 0; i < 3; i++) {
        armv7m_load_kernel(soc->cpu[i].cpu,
                          i ? NULL : machine->kernel_filename,
                          soc->boot_vector,
                          soc->flash_size -
                          (soc->boot_vector - BK7258_FLASH_BASE));
    }
}

static void bk7258_machine_class_init(ObjectClass *klass, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(klass);

    static const char * const valid_cpu_types[] = {
        ARM_CPU_TYPE_NAME("cortex-m33"), NULL,
    };

    mc->valid_cpu_types = valid_cpu_types;
    mc->init = bk7258_machine_init;
    mc->default_cpus = mc->min_cpus = mc->max_cpus = 3;
    mc->default_cpu_type = ARM_CPU_TYPE_NAME("cortex-m33");
    mc->no_floppy = true;
    mc->no_cdrom = true;
    mc->no_parallel = true;
    object_class_property_add_bool(klass, "user-key-pressed",
                                    bk7258_get_key, bk7258_set_key);
    object_class_property_set_description(klass, "user-key-pressed",
        "Close the board user key contact (active low); no debounce timing");
    object_class_property_add_bool(klass, "user-led-on", bk7258_get_led, NULL);
    object_class_property_set_description(klass, "user-led-on",
        "Read-only user LED indication driven by the board GPIO connection");
}

static void bk7258_board_class_init(ObjectClass *klass, const void *data)
{
    BK7258MachineClass *bmc = BK7258_MACHINE_CLASS(klass);

    bmc->board = data;
    MACHINE_CLASS(klass)->desc = bmc->board->description;
    if (bmc->board->num_keys > 1) {
        object_class_property_add_bool(klass, "key1-pressed",
                                       bk7258_get_key1, bk7258_set_key1);
        object_class_property_set_description(klass, "key1-pressed",
            "Close KEY1 to ground; released contact has no board pull-up");
    }
    if (bmc->board->num_keys > 2) {
        object_class_property_add_bool(klass, "key2-pressed",
                                       bk7258_get_key2, bk7258_set_key2);
        object_class_property_set_description(klass, "key2-pressed",
            "Close KEY2 to ground; released contact has no board pull-up");
    }
    if (bmc->board->num_leds > 1) {
        object_class_property_add_bool(klass, "led2-on", bk7258_get_led2, NULL);
        object_class_property_set_description(klass, "led2-on",
            "Read-only green LED4 indication on schematic net LED2");
    }
}

static const TypeInfo bk7258_board_types[] = {
    {
        .name = TYPE_BK7258_MACHINE,
        .parent = TYPE_MACHINE,
        .abstract = true,
        .instance_size = sizeof(BK7258MachineState),
        .class_size = sizeof(BK7258MachineClass),
        .class_init = bk7258_machine_class_init,
    }, {
        .name = MACHINE_TYPE_NAME("t5_board"),
        .parent = TYPE_BK7258_MACHINE,
        .class_init = bk7258_board_class_init,
        .class_data = &board_info[0],
    }, {
        .name = MACHINE_TYPE_NAME("t5ai_core"),
        .parent = TYPE_BK7258_MACHINE,
        .class_init = bk7258_board_class_init,
        .class_data = &board_info[1],
    }, {
        .name = MACHINE_TYPE_NAME("aidk_ai_toy"),
        .parent = TYPE_BK7258_MACHINE,
        .class_init = bk7258_board_class_init,
        .class_data = &board_info[2],
    },
};

DEFINE_TYPES(bk7258_board_types)
