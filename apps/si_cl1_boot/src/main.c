/* SPDX-License-Identifier: Apache-2.0 */
#include <zephyr/kernel.h>
#include <zephyr/arch/arm64/lib_helpers.h>
#include <zephyr/sys/printk.h>
#include <inttypes.h>

#define NCPU 4
BUILD_ASSERT(CONFIG_MP_MAX_NUM_CPUS == NCPU);
BUILD_ASSERT(!IS_ENABLED(CONFIG_ARM_MPU));
K_THREAD_STACK_ARRAY_DEFINE(stacks, NCPU, 2048);
static struct k_thread workers[NCPU];
K_SEM_DEFINE(done, 0, NCPU);
static uint64_t mpids[NCPU], frequencies[NCPU], levels[NCPU];
static unsigned int actual_cpu[NCPU];

/* QBox boot smoke does not access PMSA or TCM registers. */
void z_arm64_el2_plat_init(void)
{
	uint64_t value;
	__asm__ volatile("mrs %0, cnthctl_el2" : "=r"(value));
	value |= 3;
	__asm__ volatile("msr cnthctl_el2, %0; isb" :: "r"(value) : "memory");
}

static void worker(void *arg, void *unused1, void *unused2)
{
	unsigned int cpu = (uintptr_t)arg;
	ARG_UNUSED(unused1);
	ARG_UNUSED(unused2);
	actual_cpu[cpu] = arch_curr_cpu()->id;
	__asm__ volatile("mrs %0, mpidr_el1" : "=r"(mpids[cpu]));
	__asm__ volatile("mrs %0, cntfrq_el0" : "=r"(frequencies[cpu]));
	__asm__ volatile("mrs %0, CurrentEL" : "=r"(levels[cpu]));
	k_sem_give(&done);
}

int main(void)
{
	unsigned int mask = 0;
	bool pass = true;

	for (unsigned int cpu = 0; cpu < NCPU; cpu++) {
		k_thread_create(&workers[cpu], stacks[cpu], K_THREAD_STACK_SIZEOF(stacks[cpu]),
			worker, (void *)(uintptr_t)cpu, NULL, NULL, 1, 0, K_FOREVER);
		__ASSERT_NO_MSG(k_thread_cpu_pin(&workers[cpu], cpu) == 0);
		k_thread_start(&workers[cpu]);
	}
	for (unsigned int cpu = 0; cpu < NCPU; cpu++) {
		if (k_sem_take(&done, K_SECONDS(5)) != 0) {
			printk("SI_CL1_BOOT_RESULT status=FAIL reason=smp-timeout\n");
			return 0;
		}
	}
	for (unsigned int cpu = 0; cpu < NCPU; cpu++) {
		bool ok = actual_cpu[cpu] == cpu && levels[cpu] == 4 &&
			frequencies[cpu] == CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC;
		/* Voting startup may assign a different physical PE to logical CPU 0. */
		uint64_t affinity = mpids[cpu] & 0xffffff;
		ok &= affinity >= 0x10000 && affinity <= 0x10300 && !(affinity & 0xff);
		for (unsigned int other = 0; other < cpu; other++) {
			ok &= affinity != (mpids[other] & 0xffffff);
		}
		pass &= ok;
		if (ok) {
			mask |= BIT(cpu);
		}
		printk("SI_CL1_BOOT_CPU cpu=%u mpidr=%" PRIx64 " counter_hz=%" PRIu64
		       " status=%s\n", cpu, mpids[cpu], frequencies[cpu], ok ? "PASS" : "FAIL");
	}
	printk("SI_CL1_BOOT_RESULT status=%s cpu_mask=%x counter_hz=%u\n",
	       pass && mask == 0xf ? "PASS" : "FAIL", mask, CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC);
	return 0;
}
