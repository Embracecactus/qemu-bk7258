/* SPDX-License-Identifier: GPL-2.0-or-later */
/* AI-assisted downstream experiment. These targets are test instruments. */
#include "qemu/osdep.h"
#include "qemu/module.h"
#include "qemu/main-loop.h"
#include "qapi/error.h"
#include "migration/vmstate.h"
#include "hw/i2c/i2c.h"
#include "hw/i2c/smbus_master.h"
#include "hw/i2c/i2c_mux_pca954x.h"

/* Serialization is not exercised here; native bus and target code is linked. */
const VMStateInfo vmstate_info_uint8;
const VMStateInfo vmstate_info_int32;

typedef struct TestTarget {
    I2CSlave parent;
    unsigned starts, stops, nacks, writes, reads;
    uint8_t data;
    bool reject;
} TestTarget;
#define TARGET(s) ((TestTarget *)(s))
static int event(I2CSlave *s, enum i2c_event e)
{
    TestTarget *t = TARGET(s);
    switch (e) {
    case I2C_START_SEND:
    case I2C_START_RECV:
        t->starts++;
        return t->reject;
    case I2C_FINISH:
        t->stops++;
        break;
    case I2C_NACK:
        t->nacks++;
        break;
    default:
        g_assert_not_reached();
    }
    return 0;
}
static int send_byte(I2CSlave *s, uint8_t data)
{
    TARGET(s)->writes++;
    TARGET(s)->data = data;
    return 0;
}
static uint8_t recv_byte(I2CSlave *s)
{
    TARGET(s)->reads++;
    return TARGET(s)->data;
}
static void target_class(ObjectClass *oc, const void *data)
{
    I2CSlaveClass *sc = I2C_SLAVE_CLASS(oc);
    sc->event = event;
    sc->send = send_byte;
    sc->recv = recv_byte;
}
static const TypeInfo target_type = {
    .name = "test-i2c-readdress-target",
    .parent = TYPE_I2C_SLAVE,
    .instance_size = sizeof(TestTarget),
    .class_init = target_class,
};
static const TypeInfo host_type = {
    .name = "test-i2c-readdress-host", .parent = TYPE_DEVICE,
};
static DeviceState *host;
static I2CBus *bus;
static TestTarget *target(I2CBus *b, uint8_t addr)
{
    return TARGET(i2c_slave_create_simple(b, target_type.name, addr));
}
static void setup(void)
{
    host = qdev_new(host_type.name);
    qdev_realize(host, NULL, &error_abort);
    bus = i2c_init_bus(host, "test-i2c");
}
static void cleanup(void)
{
    g_assert_false(i2c_bus_busy(bus));
    object_unparent(OBJECT(host));
    object_unref(OBJECT(host));
}
static void readdress(void)
{
    setup();
    TestTarget *a = target(bus, 0x50), *b = target(bus, 0x51);
    g_assert_cmpint(i2c_start_transfer_readdress(bus, 0x50, false), ==, 0);
    g_assert_cmpint(i2c_send(bus, 0x35), ==, 0);
    g_assert_cmpint(i2c_start_transfer_readdress(bus, 0x51, false), ==, 0);
    g_assert_cmpint(i2c_send(bus, 0xc7), ==, 0);
    g_assert_cmpuint(a->writes, ==, 1);
    g_assert_cmpuint(b->writes, ==, 1);
    g_assert_cmpuint(a->stops + b->stops, ==, 0);
    g_assert_cmpint(i2c_start_transfer_readdress(bus, 0x50, true), ==, 0);
    g_assert_cmphex(i2c_recv(bus), ==, 0x35);
    i2c_nack(bus);
    g_assert_cmpuint(a->nacks, ==, 1);
    g_assert_cmpint(i2c_start_transfer_readdress(bus, 0x51, true), ==, 0);
    g_assert_cmphex(i2c_recv(bus), ==, 0xc7);
    i2c_nack(bus);
    g_assert_cmpuint(b->nacks, ==, 1);
    g_assert_cmpuint(a->stops + b->stops, ==, 0);
    i2c_end_transfer(bus);
    g_assert_cmpuint(a->stops, ==, 1);
    g_assert_cmpuint(b->stops, ==, 1);
    cleanup();
}
static void nak(void)
{
    setup();
    TestTarget *a = target(bus, 0x50), *b = target(bus, 0x51);
    b->reject = true;
    g_assert_cmpint(i2c_start_transfer_readdress(bus, 0x50, false), ==, 0);
    for (unsigned addr = 0x51; addr <= 0x52; addr++) {
        g_assert_cmpint(i2c_start_transfer_readdress(bus, addr, false), !=, 0);
        g_assert_true(i2c_bus_busy(bus));
        g_assert_cmpint(i2c_send(bus, 0xee), !=, 0);
        g_assert_cmphex(i2c_recv(bus), ==, 0xff);
        g_assert_cmpuint(a->writes + b->writes + a->reads + b->reads, ==, 0);
        g_assert_cmpuint(a->stops + b->stops, ==, 0);
    }
    /* A failed new address does not require a STOP before recovery. */
    g_assert_cmpint(i2c_start_transfer_readdress(bus, 0x50, false), ==, 0);
    g_assert_cmpint(i2c_send(bus, 0x42), ==, 0);
    i2c_end_transfer(bus);
    g_assert_cmpuint(a->stops, ==, 1);
    g_assert_cmpuint(b->stops, ==, 1);
    cleanup();
}
static void legacy(void)
{
    setup();
    TestTarget *a = target(bus, 0x50), *b = target(bus, 0x51);
    g_assert_cmpint(i2c_start_send(bus, 0x50), ==, 0);
    g_assert_cmpint(i2c_start_recv(bus, 0x51), ==, 0);
    /* Existing API intentionally unchanged. */
    g_assert_cmpuint(a->starts, ==, 2);
    g_assert_cmpuint(b->starts, ==, 0);
    g_assert_cmpint(i2c_start_transfer_readdress(bus, 0x51, false), !=, 0);
    i2c_end_transfer(bus);
    g_assert_cmpint(i2c_start_transfer_readdress(bus, 0x50, false), ==, 0);
    g_assert_cmpint(i2c_start_send(bus, 0x51), !=, 0);
    g_assert_cmpint(i2c_start_send_async(bus, 0x51), !=, 0);
    g_assert_cmpint(i2c_send_async(bus, 0x12), !=, 0);
    g_assert_cmpint(i2c_start_transfer_readdress(bus, 0, false), !=, 0);
    g_assert_cmpint(i2c_start_transfer_readdress(bus, 0x78, false), !=, 0);
    g_assert_cmpuint(a->starts, ==, 3);
    i2c_end_transfer(bus);
    cleanup();
}
static void pending(void *opaque)
{
    unsigned *count = opaque;
    ++*count;
}
static void ownership(void)
{
    unsigned count = 0;
    QEMUBH *bh = qemu_bh_new(pending, &count);
    setup();
    g_assert_cmpint(i2c_start_transfer_readdress(bus, 0x52, false), !=, 0);
    i2c_bus_master(bus, bh);
    i2c_schedule_pending_master(bus);
    g_assert_null(bus->bh);
    i2c_end_transfer(bus);
    i2c_schedule_pending_master(bus);
    g_assert_true(bus->bh == bh);
    g_assert_true(i2c_bus_busy(bus));
    g_assert_cmpint(i2c_start_transfer_readdress(bus, 0x50, false), !=, 0);
    qemu_bh_cancel(bh);
    i2c_bus_release(bus);
    qemu_bh_delete(bh);
    cleanup();
}
static void mux_smbus(void)
{
    setup();
    I2CSlave *mux = i2c_slave_create_simple(bus, "pca9548", 0x70);
    TestTarget *a = target(pca954x_i2c_get_bus(mux, 0), 0x50);
    TestTarget *b = target(pca954x_i2c_get_bus(mux, 1), 0x51);
    /* Real SMBus adapter + real mux: control writes take effect at STOP. */
    g_assert_cmpint(smbus_send_byte(bus, 0x70, 3), ==, 0);
    g_assert_cmpint(smbus_receive_byte(bus, 0x70), ==, 3);
    g_assert_cmpint(i2c_start_transfer_readdress(bus, 0x50, false), ==, 0);
    i2c_send(bus, 0x35);
    g_assert_cmpint(i2c_start_transfer_readdress(bus, 0x51, false), ==, 0);
    i2c_send(bus, 0xc7);
    g_assert_cmpuint(a->stops + b->stops, ==, 0);
    i2c_end_transfer(bus);
    g_assert_cmphex(a->data, ==, 0x35);
    g_assert_cmphex(b->data, ==, 0xc7);
    /* Readdress away from a pending mux write must not fake its STOP. */
    g_assert_cmpint(i2c_start_transfer_readdress(bus, 0x70, false), ==, 0);
    i2c_send(bus, 0);
    g_assert_cmpint(i2c_start_transfer_readdress(bus, 0x50, true), ==, 0);
    g_assert_cmphex(i2c_recv(bus), ==, 0x35);
    i2c_nack(bus);
    i2c_end_transfer(bus);
    g_assert_cmpint(smbus_receive_byte(bus, 0x70), ==, 0);
    g_assert_cmpint(i2c_start_transfer_readdress(bus, 0x50, true), !=, 0);
    i2c_end_transfer(bus);
    cleanup();
}
int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    module_call_init(MODULE_INIT_QOM);
    type_register_static(&target_type);
    type_register_static(&host_type);
    Object *machine = object_property_add_new_container(object_get_root(),
                                                        "machine");
    object_property_add_new_container(machine, "unattached");
    g_assert_cmpint(qemu_init_main_loop(&error_abort), ==, 0);
    g_test_add_func("/i2c/readdress/no-false-stop", readdress);
    g_test_add_func("/i2c/readdress/nak-recovery", nak);
    g_test_add_func("/i2c/readdress/legacy", legacy);
    g_test_add_func("/i2c/readdress/ownership", ownership);
    g_test_add_func("/i2c/readdress/smbus-mux", mux_smbus);
    return g_test_run();
}
