/****************************************************************************
 *
 *   Copyright (C) 2012 PX4 Development Team. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name PX4 nor the names of its contributors may be
 *    used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 ****************************************************************************/

/**
 * @file px4fmu_init.c
 *
 * PX4FMU-specific early startup code.  This file implements the
 * nsh_archinitialize() function that is called early by nsh during startup.
 *
 * Code here is run before the rcS script is invoked; it should start required
 * subsystems and perform board-specific initialisation.
 */

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdbool.h>
#include <stdio.h>
#include <debug.h>
#include <errno.h>
#include <math.h>

#include <nuttx/arch.h>
#include <nuttx/spi.h>
#include <nuttx/i2c.h>
#include <nuttx/sdio.h>
#include <nuttx/mmcsd.h>
#include <nuttx/analog/adc.h>
#include <nuttx/gran.h>

#include <stm32.h>
#include "board_config.h"
#include <stm32_uart.h>

#include <arch/board/board.h>

#include <drivers/drv_hrt.h>

#include <systemlib/cpuload.h>
#include <systemlib/perf_counter.h>

#include "kbd/driver.h"

/****************************************************************************
 * Pre-Processor Definitions
 ****************************************************************************/

/* Configuration ************************************************************/

/* Debug ********************************************************************/

#ifdef CONFIG_CPP_HAVE_VARARGS
#  ifdef CONFIG_DEBUG
#    define message(...) lowsyslog(__VA_ARGS__)
#  else
#    define message(...) printf(__VA_ARGS__)
#  endif
#else
#  ifdef CONFIG_DEBUG
#    define message lowsyslog
#  else
#    define message printf
#  endif
#endif

/************************************************************************************
 * Name: stm32_boardinitialize
 *
 * Description:
 *   All STM32 architectures must provide the following entry point.  This entry point
 *   is called early in the intitialization -- after all memory has been configured
 *   and mapped but before any devices have been initialized.
 *
 ************************************************************************************/

__EXPORT void
stm32_boardinitialize(void)
{
	/* configure SPI interfaces */
	stm32_spiinitialize();

	/* configure LEDs */
	up_ledinit();

	airleash_kbd_gpioconfig();
}

/****************************************************************************
 * Name: nsh_archinitialize
 *
 * Description:
 *   Perform architecture specific initialization
 *
 ****************************************************************************/

static struct spi_dev_s *spi1;
static struct spi_dev_s *spi2;

static void adc_init();

__EXPORT int nsh_archinitialize(void)
{
	adc_init();

	/* configure power supply control/sense pins */
	stm32_configgpio(GPIO_VDD_3V3_SENSORS_EN);
	stm32_configgpio(GPIO_VDD_SYSPOWER_OFF);
	stm32_configgpio(GPIO_VDD_BT_PWR_EN);
	stm32_gpiowrite(GPIO_VDD_BT_PWR_EN, 1);

	/* configure the high-resolution time/callout interface */
	hrt_init();

	/* configure CPU load estimation */
#ifdef CONFIG_SCHED_INSTRUMENTATION
	cpuload_initialize_once();
#endif

	/* set up the serial DMA polling */
	static struct hrt_call serial_dma_call;
	struct timespec ts;

	/*
	 * Poll at 1ms intervals for received bytes that have not triggered
	 * a DMA event.
	 */
	ts.tv_sec = 0;
	ts.tv_nsec = 1000000;

	hrt_call_every(&serial_dma_call,
		       ts_to_abstime(&ts),
		       ts_to_abstime(&ts),
		       (hrt_callout)stm32_serial_dma_poll,
		       NULL);

	/* Configure SPI-based devices */

	spi1 = up_spiinitialize(1);

	if (!spi1) {
		message("[boot] FAILED to initialize SPI port 1\n");
		return -ENODEV;
	}

	/* Default SPI1 to 1MHz and de-assert the known chip selects. */
	SPI_SETFREQUENCY(spi1, 10000000);
	SPI_SETBITS(spi1, 8);
	SPI_SETMODE(spi1, SPIDEV_MODE3);
	SPI_SELECT(spi1, PX4_SPIDEV_GYRO, false);
	SPI_SELECT(spi1, PX4_SPIDEV_ACCEL_MAG, false);
	SPI_SELECT(spi1, PX4_SPIDEV_BARO, false);
	SPI_SELECT(spi1, PX4_SPIDEV_MPU, false);
	up_udelay(20);

	message("[boot] Initialized SPI port 1 (SENSORS)\n");

	/* Get the SPI port for the FRAM */

	spi2 = up_spiinitialize(2);

	if (!spi2) {
		message("[boot] FAILED to initialize SPI port 2\n");
		return -ENODEV;
	}

	/* Default SPI2 to 37.5 MHz (40 MHz rounded to nearest valid divider, F4 max)
	 * and de-assert the known chip selects. */

	// XXX start with 10.4 MHz in FRAM usage and go up to 37.5 once validated
	SPI_SETFREQUENCY(spi2, 12 * 1000 * 1000);
	SPI_SETBITS(spi2, 8);
	SPI_SETMODE(spi2, SPIDEV_MODE3);
	SPI_SELECT(spi2, SPIDEV_FLASH, false);

	message("[boot] Initialized SPI port 2 (RAMTRON FRAM)\n");

	airleash_kbd_start();

	return OK;
}

static void
adc_init()
{
#define ADC1_N_X(channel) (GPIO_ADC1_IN ## channel)
#define ADC1_N(channel) ADC1_N_X(channel)

	/* sense pins */
	stm32_configgpio(ADC1_N(ADC_BATTERY_VOLTAGE_CHANNEL));
	stm32_configgpio(ADC1_N(ADC_SENSORS_VOLTAGE_CHANNEL));

#undef ADC1_N_X
#undef ADC1_N
}