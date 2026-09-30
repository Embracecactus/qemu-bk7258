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
    unsigned led_pin;
    unsigned key_pin;
    bool key_pullup;
    LEDColor led_color;
} BK7258BoardInfo;

struct BK7258MachineState {
    MachineState parent_obj;
    BK7258State *soc;
    LEDState *led;
    bool key_pressed;
};

struct BK7258MachineClass {
    MachineClass parent_class;
    const BK7258BoardInfo *board;
};

/* Electrical sources and revision limits: docs/system/arm/bk7258.rst. */
static const BK7258BoardInfo board_info[] = {
    {"BK7258 T5-Board (partial V1.0.2 wiring)", 1, 12, true, LED_COLOR_GREEN},
    {"BK7258 T5AI-Core (partial V1.0.1 wiring)", 9, 29, true, LED_COLOR_GREEN},
    {"BK7258 AIDK AI Toy (partial V1.0 wiring)", 40, 8, false, LED_COLOR_RED},
};

static void bk7258_key_update(BK7258MachineState *s)
{
    const BK7258BoardInfo *board = BK7258_MACHINE_GET_CLASS(s)->board;
    int level = s->key_pressed ? 0 :
                board->key_pullup ? 1 : BK7258_GPIO_FLOAT;

    if (s->soc) {
        qemu_set_irq(qdev_get_gpio_in_named(DEVICE(&s->soc->aon), "gpio-in",
                                           board->key_pin), level);
    }
}

static bool bk7258_get_key(Object *obj, Error **errp)
{
    return BK7258_MACHINE(obj)->key_pressed;
}

static void bk7258_set_key(Object *obj, bool pressed, Error **errp)
{
    BK7258MachineState *s = BK7258_MACHINE(obj);

    s->key_pressed = pressed;
    bk7258_key_update(s);
}

static bool bk7258_get_led(Object *obj, Error **errp)
{
    BK7258MachineState *s = BK7258_MACHINE(obj);

    return s->led && led_get_intensity(s->led) != 0;
}

static void bk7258_machine_init(MachineState *machine)
{
    BK7258MachineState *s = BK7258_MACHINE(machine);
    const BK7258BoardInfo *board = BK7258_MACHINE_GET_CLASS(s)->board;
    DeviceState *dev = qdev_new(TYPE_BK7258_SOC);
    BK7258State *soc = BK7258_SOC(dev);
    unsigned i;

    object_property_add_child(OBJECT(machine), "soc", OBJECT(dev));
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
            qdev_prop_set_drive(DEVICE(&soc->flashctrl.nor),
                                i ? "status-drive" : "drive",
                                blk_by_legacy_dinfo(drive));
        }
    }
    /* Loading a logical kernel is an explicit diagnostic board policy. */
    qdev_prop_set_bit(dev, "diagnostic-xip", machine->kernel_filename != NULL);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    s->soc = soc;
    s->led = led_create_simple(OBJECT(machine), GPIO_POLARITY_ACTIVE_HIGH,
                               board->led_color, "user-led");
    qdev_connect_gpio_out_named(DEVICE(&soc->aon), "gpio-out", board->led_pin,
                                qdev_get_gpio_in(DEVICE(s->led), 0));
    bk7258_key_update(s);

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
