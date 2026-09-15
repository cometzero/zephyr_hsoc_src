/* SPDX-License-Identifier: Apache-2.0 */
#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/irq.h>
#include <zephyr/sys/sys_io.h>

BUILD_ASSERT(CONFIG_MP_MAX_NUM_CPUS == 4, "This probe expects CL1's four cores");

K_THREAD_STACK_ARRAY_DEFINE(stacks, 4, 2048);
static struct k_thread workers[4];
/* Debug-visible checkpoints, NOT retained context or an AON allocation. */
atomic_t cl1_powerdown_ready_mask;
volatile uint32_t cl1_gic_waker[4];
volatile uint32_t cl1_gic_sleep_state[4];
volatile uint32_t cl1_gic_enabled_before[4], cl1_gic_enabled_after[4];
volatile uint32_t cl1_gic_pending_before[4], cl1_gic_pending_after[4];
volatile uint32_t cl1_gic_ctlr[4];
static const uintptr_t redistributors[] = {
	DT_REG_ADDR_BY_IDX(DT_NODELABEL(gic), 1),
	DT_REG_ADDR_BY_IDX(DT_NODELABEL(gic), 2),
	DT_REG_ADDR_BY_IDX(DT_NODELABEL(gic), 3),
	DT_REG_ADDR_BY_IDX(DT_NODELABEL(gic), 4),
};
static atomic_t started;

static void powerdown(void *arg, void *unused1, void *unused2)
{
	unsigned int cpu = (unsigned int)(uintptr_t)arg;
	uint64_t control;

	ARG_UNUSED(unused1);
	ARG_UNUSED(unused2);
	/* irq_lock() also holds Zephyr's SMP global lock. Use local masking so
	 * every pinned worker can reach the all-core rendezvous independently.
	 */
	(void)arch_irq_lock();
	/* This board runs the virtual timer. Physical timer EL1 accesses are
	 * trapped with the normal CNTHCTL_EL2 configuration; leave them alone.
	 */
	__asm__ volatile("msr CNTV_CTL_EL0, xzr\n"
			 "isb" ::: "memory");
	/* Stop interrupt delivery at this core. No global GICD reset: CL0 lives. */
	__asm__ volatile("msr ICC_IGRPEN1_EL1, xzr\n"
			 "msr ICC_IGRPEN0_EL1, xzr\n"
			 "isb" ::: "memory");
	/* Match Zephyr gicv3_cpuif_init's local SGI/PPI disable + RWP wait,
	 * but preserve pending bits for diagnosis. Never change shared GICD.
	 */
	uintptr_t rdist = redistributors[cpu];
	cl1_gic_enabled_before[cpu] = sys_read32(rdist + 0x10100);
	cl1_gic_pending_before[cpu] = sys_read32(rdist + 0x10200);
	sys_write32(UINT32_MAX, rdist + 0x10180);
	for (unsigned int n = 0; n < 1000000; ++n) {
		cl1_gic_ctlr[cpu] = sys_read32(rdist);
		if (!(cl1_gic_ctlr[cpu] & BIT(3))) {
			break;
		}
	}
	cl1_gic_enabled_after[cpu] = sys_read32(rdist + 0x10100);
	cl1_gic_pending_after[cpu] = sys_read32(rdist + 0x10200);
	if ((cl1_gic_ctlr[cpu] & BIT(3)) || cl1_gic_enabled_after[cpu]) {
		cl1_gic_sleep_state[cpu] = 4; /* local disable failed */
		for (;;) {
			arch_nop();
		}
	}
	/* GIC-720AE WAKER: ProcessorSleep is available in CL1 view 2.
	 * Never set global Sleep or write view-0-only PWRR through this view.
	 * Bound the acknowledgement wait; failure must not reach PWRDN/WFI.
	 */
	uintptr_t waker = redistributors[cpu] + 0x14;
	cl1_gic_sleep_state[cpu] = 1;
	sys_write32(sys_read32(waker) | BIT(1), waker);
	__asm__ volatile("dsb sy" ::: "memory");
	for (unsigned int n = 0; n < 1000000; ++n) {
		cl1_gic_waker[cpu] = sys_read32(waker);
		if (cl1_gic_waker[cpu] & BIT(2)) {
			cl1_gic_sleep_state[cpu] = 2;
			break;
		}
	}
	if (cl1_gic_sleep_state[cpu] != 2) {
		cl1_gic_sleep_state[cpu] = 3;
		for (;;) {
			arch_nop();
		}
	}
	atomic_or(&cl1_powerdown_ready_mask, BIT(cpu));
	while (atomic_get(&cl1_powerdown_ready_mask) != 0xf) {
		arch_nop();
	}
	/* Destructive capability test: no architectural context restore is claimed.
	 * R82 TRM 6.8: PWRDN requests the hardware cache/coherency powerdown
	 * handshake when WFI executes. Plain debugger execution gating cannot.
	 */
	__asm__ volatile("mrs %0, S3_0_C15_C2_7" : "=r"(control));
	control |= 1;
	__asm__ volatile("dsb sy\n"
			 "msr S3_0_C15_C2_7, %0\n"
			 "isb\n"
			 "1: wfi\n"
			 "b 1b" :: "r"(control) : "memory");
	CODE_UNREACHABLE;
}

static int arm_powerdown(const struct shell *shell, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	if (!atomic_cas(&started, 0, 1)) {
		return -EALREADY;
	}
	/* The probe uses no network/MHU/PFDI drivers. Disable its UART SPI before
	 * all cores stop; the polling printk output above remains the last marker.
	 */
	shell_print(shell, "CL1_PDOWN_ARMED: destructive PWRDN/WFI; resume NOT_IMPLEMENTED");
	for (unsigned int i = 0; i < 4; ++i) {
		k_thread_create(&workers[i], stacks[i], K_THREAD_STACK_SIZEOF(stacks[i]),
				powerdown, (void *)(uintptr_t)i, NULL, NULL,
				K_PRIO_COOP(0), 0, K_FOREVER);
		if (k_thread_cpu_pin(&workers[i], i) != 0) {
			shell_error(shell, "CPU pin failed before powerdown");
			return -EINVAL;
		}
	}
	irq_disable(32 + 7); /* CL1 PL011 SPI 7, from board DTS */
	/* Do not let a local cooperative worker preempt this launcher before all
	 * four workers have been started; it waits at the all-core barrier.
	 */
	k_sched_lock();
	for (unsigned int i = 0; i < 4; ++i) {
		k_thread_start(&workers[i]);
	}
	k_sched_unlock();
	return 0;
}

SHELL_CMD_REGISTER(cl1_powerdown, NULL,
	"DESTRUCTIVE: enter four-core PWRDN/WFI, no context resume", arm_powerdown);

int main(void)
{
	printk("CL1_POWERDOWN_PROBE_READY: isolated image; no context resume\n");
	return 0;
}
