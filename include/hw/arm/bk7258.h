/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_ARM_BK7258_H
#define HW_ARM_BK7258_H

#include "hw/arm/armv7m.h"
#include "hw/char/bk7258_uart.h"
#include "hw/i2c/bk7258_i2c.h"
#include "hw/ssi/bk7258_spi.h"
#include "hw/dma/bk7258_dma.h"
#include "hw/timer/bk7258_rtc.h"
#include "hw/timer/bk7258_timer.h"
#include "hw/timer/bk7258_pwm.h"
#include "hw/watchdog/bk7258_wdt.h"
#include "hw/misc/bk7258_aon.h"
#include "hw/misc/bk7258_ckmn.h"
#include "hw/misc/bk7258_mailbox.h"
#include "hw/block/bk7258_flash.h"

#define BK7258_FLASH_BASE 0x02000000

#define TYPE_BK7258_SOC "bk7258-soc"
OBJECT_DECLARE_SIMPLE_TYPE(BK7258State, BK7258_SOC)

struct BK7258State {
    SysBusDevice parent_obj;
    ARMv7MState cpu[3];
    BK7258UARTState uart[3];
    BK7258PWMState pwm[2];
    MemoryRegion pwm_ns[2];
    Clock *pwmclk[2];
    BK7258DMAState dma[2];
    MemoryRegion dma_ns[2];
    MemoryRegion dma_memory;
    MemoryRegion dma_sram[4];
    BK7258SPIState spi[2];
    MemoryRegion spi_ns[2];
    Clock *spiclk[2];
    BK7258I2CState i2c[2];
    MemoryRegion i2c_ns[2];
    Clock *i2cclk[2];
    BK7258TimerState timer[2];
    MemoryRegion timer_ns[2];
    Clock *timerclk[2];
    BK7258RTCState rtc;
    MemoryRegion rtc_ns;
    BK7258WDTState wdt[2];
    BK7258AONState aon;
    BK7258CKMNState ckmn;
    BK7258MailboxState mailbox;
    BK7258FlashState flashctrl;
    MemoryRegion flashctrl_ns;
    MemoryRegion mailbox_ns[3];
    uint64_t private_irqs[3];
    Clock *xtalclk;
    Clock *roscclk;
    Clock *div32kclk;
    Clock *lpoclk;
    MemoryRegion ckmn_ns;
    MemoryRegion aon_ns[2];
    MemoryRegion wdt_ns[2];
    Clock *wdtclk[2];
    Clock *uartclk[3];
    uint32_t clock_select;
    uint32_t peripheral_clocks;
    uint32_t power_sleep;
    uint32_t flash_bus_config;
    uint32_t clock_mode;
    uint32_t gpio_mux[7];
    uint32_t analog[28];
    uint32_t analog_pending[28];
    uint32_t analog_busy;
    QEMUTimer *analog_timer;
    MemoryRegion cpu_memory[3];
    MemoryRegion shared_alias[3];
    MemoryRegion itcm[3];
    MemoryRegion dtcm[3];
    MemoryRegion itcm_ns[3];
    MemoryRegion dtcm_ns[3];
    MemoryRegion flash;
    MemoryRegion flash_ns;
    MemoryRegion sram;
    MemoryRegion sram_alias[3];
    MemoryRegion sysctrl;
    MemoryRegion sysctrl_ns;
    MemoryRegion uart_ns[3];
    Clock *cpuclk;
    Clock *coreclk[3];
    Clock *busclk;
    Clock *apllclk;
    Clock *refclk[3];
    uint32_t core_clock_key;
    bool core_clock_unimplemented;
    unsigned apll_hz;
    bool apll_pulse_armed;
    uint32_t cpu_control[3];
    uint32_t irq_enable[3][2];
    uint64_t irq_levels;
    uint32_t flash_size;
    uint32_t boot_vector;
    bool diagnostic_xip;
    bool experimental_core_clocks;
    bool experimental_spi_apll;
};

#endif
