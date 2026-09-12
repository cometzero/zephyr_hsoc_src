/*
 * SPDX-FileCopyrightText: <text>Copyright 2026 Arm Limited and/or its
 * affiliates <open-source-office@arm.com></text>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>

#include <zephyr/kernel.h>
#include <zephyr/arch/arm64/timer.h>
#include <zephyr/init.h>
#include <zephyr/sys/printk.h>

static int si_timer_frequency_check(void)
{
	uint64_t cntfrq = read_cntfrq_el0();
	bool match = cntfrq == CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC;

	printk("APOLLO_SI1_TIMER cntfrq=%llu configured=%u status=%s\n",
	       (unsigned long long)cntfrq,
	       CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC,
	       match ? "PASS" : "FAIL");

	return match ? 0 : -EINVAL;
}

SYS_INIT(si_timer_frequency_check, APPLICATION,
	 CONFIG_KERNEL_INIT_PRIORITY_DEFAULT);
