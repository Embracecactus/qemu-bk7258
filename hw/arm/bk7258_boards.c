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
#include "hw/core/qdev-properties-system.h"
#include "system/blockdev.h"

static void bk7258_machine_init(MachineState *machine)
{
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
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
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

    mc->desc = data;
    mc->valid_cpu_types = valid_cpu_types;
    mc->init = bk7258_machine_init;
    mc->default_cpus = mc->min_cpus = mc->max_cpus = 3;
    mc->default_cpu_type = ARM_CPU_TYPE_NAME("cortex-m33");
    mc->no_floppy = true;
    mc->no_cdrom = true;
    mc->no_parallel = true;
}

static const TypeInfo bk7258_board_types[] = {
    {
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

DEFINE_TYPES(bk7258_board_types)
