/* SPDX-License-Identifier: Apache-2.0 */
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/drivers/timer/arm_arch_timer.h>
#include <zephyr/arch/arm64/lib_helpers.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/printk.h>
#include <inttypes.h>

#define NCPU 4
#define STREAM_REPEATS 8
#define CHASE_REPEATS 4
#define IRQ_SAMPLES 16
#define RT_JOBS 8
#define DTCM_BASE ((uintptr_t)DT_REG_ADDR(DT_NODELABEL(bench_dtcm)))
#define MM_BASE ((uintptr_t)DT_REG_ADDR(DT_NODELABEL(bench_cached)))
#define NC_BASE ((uintptr_t)DT_REG_ADDR(DT_NODELABEL(bench_uncached)))
#define COUNTER_HZ CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC
/* R82AE's CNTP register bank is the physical timer, INTID 29. */
#define BENCH_IRQ ARM_TIMER_SECURE_IRQ
#define LOAD_BYTES (128 * 1024)
#define PRIVATE_WORDS 64

BUILD_ASSERT(CONFIG_MP_MAX_NUM_CPUS == NCPU);
BUILD_ASSERT(BENCH_IRQ == 29);
BUILD_ASSERT(DT_REG_SIZE(DT_NODELABEL(bench_cached)) >= 3 * 1024 * 1024);

K_THREAD_STACK_ARRAY_DEFINE(worker_stacks, NCPU, 4096);
K_THREAD_STACK_ARRAY_DEFINE(load_stacks, NCPU, 2048);
static struct k_thread workers[NCPU], loaders[NCPU];
static struct k_sem check_private[NCPU], start_work[NCPU], start_load[NCPU];
K_SEM_DEFINE(written, 0, NCPU);
K_SEM_DEFINE(checked, 0, NCPU);
K_SEM_DEFINE(bench_done, 0, 1);
K_SEM_DEFINE(rt_done, 0, NCPU);
K_SEM_DEFINE(load_ready, 0, NCPU);
K_SEM_DEFINE(load_done, 0, NCPU);
static atomic_t failures, stop_load;
static uint64_t mpids[NCPU], sctlrs[NCPU], dtcm_regs[NCPU], load_passes[NCPU];
static bool private_ok[NCPU];
static uint64_t rt_start;
static unsigned int mem_cases, rt_tasks, irq_samples;
static volatile uint64_t sink;

struct sample {
	uint64_t scheduled, isr, thread, finish;
};

struct timer_test {
	struct k_sem ready;
	struct sample samples[IRQ_SAMPLES];
	uint64_t next, period;
	unsigned int count, target, skipped, completed;
	uint32_t checksum;
};

static struct timer_test timers[NCPU];

static inline uint64_t now(void)
{
	uint64_t value;

	__asm__ volatile("isb; mrs %0, cntpct_el0" : "=r"(value) :: "memory");
	return value;
}

static inline void complete_accesses(void)
{
	__asm__ volatile("dsb sy" ::: "memory");
}

/* The reference startup calls this hook on every PE before entering EL1. */
void z_arm64_el2_plat_init(void)
{
	uint64_t value;

	__asm__ volatile("mrs %0, cnthctl_el2" : "=r"(value));
	value |= 3;
	__asm__ volatile("msr cnthctl_el2, %0" :: "r"(value));
	__asm__ volatile("mrs %0, actlr_el2" : "=r"(value));
	value |= 1; /* ACTLR_EL2.REGIONS: EL1 may enable its local DTCM. */
	__asm__ volatile("msr actlr_el2, %0; isb" :: "r"(value) : "memory");
}

static bool memory_attributes(const char *name, uintptr_t base, size_t size,
			      uint8_t expected)
{
	unsigned int key = irq_lock();
	uint64_t selected = read_prselr_el1(), mair = read_mair_el1();
	unsigned int matches = 0, attr = 0;

	for (unsigned int i = 0; i < (read_mpuir_el1() & 255); i++) {
		write_prselr_el1(i);
		__asm__ volatile("isb" ::: "memory");
		uint64_t first = read_prbar_el1() & ~UINT64_C(63);
		uint64_t last = read_prlar_el1();

		if ((last & 1) && base >= first && base + size - 1 <= (last | 63)) {
			matches++;
			attr = (mair >> (((last >> 1) & 7) * 8)) & 255;
		}
	}
	write_prselr_el1(selected);
	irq_unlock(key);
	bool ok = matches == 1 && attr == expected;

	printk("SI_CL1_MAP target=%s base=%" PRIxPTR " bytes=%zu mair=%x matches=%u status=%s\n",
	       name, base, size, attr, matches, ok ? "PASS" : "FAIL");
	return ok;
}

/* Ordinary loads, with four accumulators. The result is checked outside ROI. */
static __noinline uint64_t read_stream(const uint64_t *data, size_t words)
{
	uint64_t a = 0, b = 0, c = 0, d = 0;

	for (size_t i = 0; i < words; i += 4) {
		a += data[i]; b += data[i + 1]; c += data[i + 2]; d += data[i + 3];
	}
	return a + b + c + d;
}

static void memory_result(const char *target, const char *op, size_t size,
			  unsigned int repeats, uint64_t bytes, uint64_t accesses,
			  uint64_t begin, uint64_t end, uint64_t checksum, bool ok)
{
	mem_cases++;
	if (!ok || end <= begin) {
		atomic_inc(&failures);
		ok = false;
	}
	printk("SI_CL1_MEM target=%s op=%s ws_bytes=%zu iterations=%u useful_bytes=%" PRIu64
	       " accesses=%" PRIu64 " begin=%" PRIu64 " end=%" PRIu64
	       " ticks=%" PRIu64 " counter_hz=%u checksum=%" PRIx64
	       " irq_masked=1 status=%s\n", target, op, size, repeats, bytes,
	       accesses, begin, end, end - begin, COUNTER_HZ, checksum, ok ? "PASS" : "FAIL");
}

static void memory_test(const char *target, uintptr_t base, size_t size)
{
	uint64_t *data = (uint64_t *)base;
	size_t words = size / sizeof(*data), lines = size / 64;
	uint64_t expected = 0, checksum = 0, begin, end;
	unsigned int key;

	for (size_t i = 0; i < words; i++) {
		data[i] = i ^ UINT64_C(0x13579bdf);
		expected += data[i];
	}
	sink = read_stream(data, words); /* warm-up, excluded from ROI */
	complete_accesses();
	key = irq_lock();
	begin = now();
	for (unsigned int r = 0; r < STREAM_REPEATS; r++) {
		__asm__ volatile("" ::: "memory");
		checksum += read_stream(data, words);
	}
	complete_accesses();
	end = now();
	irq_unlock(key);
	memory_result(target, "read", size, STREAM_REPEATS, size * STREAM_REPEATS,
		      words * STREAM_REPEATS, begin, end, checksum,
		      checksum == expected * STREAM_REPEATS);

	key = irq_lock();
	begin = now();
	for (unsigned int r = 0; r < STREAM_REPEATS; r++) {
		for (size_t i = 0; i < words; i++) {
			data[i] = UINT64_C(0x2468ace0) + r;
		}
		__asm__ volatile("" ::: "memory");
	}
	complete_accesses(); /* Store completion to cache/NC target; no cache flush. */
	end = now();
	irq_unlock(key);
	bool ok = true;

	for (size_t i = 0; i < words; i++) {
		ok &= data[i] == UINT64_C(0x2468ace0) + STREAM_REPEATS - 1;
	}
	memory_result(target, "write", size, STREAM_REPEATS, size * STREAM_REPEATS,
		      words * STREAM_REPEATS, begin, end, data[words - 1], ok);

	/* Odd stride visits every line of the power-of-two working set once. */
	for (size_t i = 0; i < lines; i++) {
		data[i * 8] = base + ((i + 73) & (lines - 1)) * 64;
	}
	uintptr_t cursor = base;

	for (size_t i = 0; i < lines; i++) {
		cursor = *(uintptr_t *)cursor;
	}
	size_t remaining = lines * CHASE_REPEATS;

	complete_accesses();
	key = irq_lock();
	begin = now();
	/* Keep the counted loop within one 64 B instruction-cache line. */
	__asm__ volatile(
		".p2align 6\n"
		"1: ldr %x[cursor], [%x[cursor]]\n"
		"subs %x[count], %x[count], #1\n"
		"b.ne 1b\n"
		: [cursor] "+&r" (cursor), [count] "+&r" (remaining)
		: : "cc", "memory");
	complete_accesses();
	end = now();
	irq_unlock(key);
	sink = cursor;
	memory_result(target, "chase", size, CHASE_REPEATS, lines * CHASE_REPEATS * 8,
		      lines * CHASE_REPEATS, begin, end, cursor, cursor == base);
}

static inline void timer_control(uint64_t value)
{
	__asm__ volatile("msr cntp_ctl_el0, %0; isb" :: "r"(value) : "memory");
}

static inline void timer_compare(uint64_t value)
{
	__asm__ volatile("msr cntp_cval_el0, %0; isb" :: "r"(value) : "memory");
}

static void timer_isr(const void *unused)
{
	/* First C-level timestamp, after the Zephyr exception/GIC dispatch. */
	uint64_t entered = now();
	struct timer_test *test = &timers[arch_curr_cpu()->id];

	ARG_UNUSED(unused);
	timer_control(0);
	if (test->count >= test->target) {
		atomic_inc(&failures);
		return;
	}
	test->samples[test->count].scheduled = test->next;
	test->samples[test->count].isr = entered;
	test->count++;
	if (test->count < test->target) {
		test->next += test->period;
		uint64_t current = now();

		if (test->next <= current) {
			unsigned int skip = (current - test->next) / test->period + 1;

			test->skipped += skip;
			test->next += skip * test->period;
		}
		timer_compare(test->next);
		timer_control(1);
	}
	k_sem_give(&test->ready);
}

/* Bounded integer sensor-filter/control proxy; deterministic, no I/O in ROI. */
static __noinline uint32_t control_job(uint32_t state)
{
	for (unsigned int i = 0; i < 64; i++) {
		state = state * 1664525U + 1013904223U;
		state ^= state >> 13;
	}
	/* Keep the arithmetic inside the two counter reads, including with IPA. */
	__asm__ volatile("" : "+r"(state) :: "memory");
	return state;
}

static void timer_test(unsigned int cpu, bool loaded)
{
	struct timer_test *test = &timers[cpu];

	test->target = loaded ? RT_JOBS : IRQ_SAMPLES;
	test->period = loaded ? (COUNTER_HZ / 4000) << cpu : COUNTER_HZ / 10000;
	test->count = test->skipped = 0;
	test->completed = test->checksum = 0;
	test->next = loaded ? rt_start : now() + test->period;
	k_sem_reset(&test->ready);
	irq_enable(BENCH_IRQ);
	timer_compare(test->next);
	timer_control(1);
	for (unsigned int i = 0; i < test->target; i++) {
		if (k_sem_take(&test->ready, K_SECONDS(1)) != 0) {
			atomic_inc(&failures);
			break;
		}
		test->samples[i].thread = now();
		if (loaded) {
			test->checksum ^= control_job(i + 1);
		}
		test->samples[i].finish = now();
		test->completed++;
	}
	timer_control(0);
	irq_disable(BENCH_IRQ);
	/* Printing and validation are deferred until every measured task stops. */
}

static void report_timer(unsigned int cpu, bool loaded)
{
	struct timer_test *test = &timers[cpu];
	const char *phase = loaded ? "loaded" : "idle";
	uint64_t irq_min = UINT64_MAX, irq_max = 0, irq_sum = 0;
	uint64_t wake_min = UINT64_MAX, wake_max = 0, wake_sum = 0;
	uint64_t exec_min = UINT64_MAX, exec_max = 0, exec_sum = 0, response_max = 0;
	unsigned int misses = 0;
	bool ok = test->completed == test->target;

	irq_samples += test->completed;
	if (loaded) {
		rt_tasks++;
		uint32_t expected = 0;

		for (unsigned int i = 0; i < RT_JOBS; i++) {
			expected ^= control_job(i + 1);
		}
		if (test->checksum != expected) {
			atomic_inc(&failures);
			ok = false;
		}
	}
	for (unsigned int i = 0; i < test->completed; i++) {
		struct sample *s = &test->samples[i];
		uint64_t irq = s->isr - s->scheduled, wake = s->thread - s->isr;
		uint64_t exec = s->finish - s->thread, response = s->finish - s->scheduled;

		if (s->scheduled > s->isr || s->isr > s->thread || s->thread > s->finish) {
			atomic_inc(&failures);
			ok = false;
		}
		irq_min = MIN(irq_min, irq); irq_max = MAX(irq_max, irq); irq_sum += irq;
		wake_min = MIN(wake_min, wake); wake_max = MAX(wake_max, wake); wake_sum += wake;
		exec_min = MIN(exec_min, exec); exec_max = MAX(exec_max, exec); exec_sum += exec;
		response_max = MAX(response_max, response);
		misses += response > test->period;
		printk("SI_CL1_IRQ_SAMPLE phase=%s cpu=%u sample=%u scheduled=%" PRIu64
		       " isr=%" PRIu64 " thread=%" PRIu64 " finish=%" PRIu64 "\n",
		       phase, cpu, i, s->scheduled, s->isr, s->thread, s->finish);
	}
	printk("SI_CL1_IRQ phase=%s cpu=%u source=CNTP_PPI29 samples=%u period_ticks=%" PRIu64
	       " irq_min=%" PRIu64 " irq_max=%" PRIu64 " irq_sum=%" PRIu64
	       " thread_min=%" PRIu64 " thread_max=%" PRIu64 " thread_sum=%" PRIu64
	       " skipped=%u status=%s\n", phase, cpu, test->completed, test->period,
	       irq_min, irq_max, irq_sum, wake_min, wake_max, wake_sum, test->skipped,
	       ok ? "PASS" : "FAIL");
	if (loaded) {
		printk("SI_CL1_RT cpu=%u priority=%u period_ticks=%" PRIu64
		       " deadline_ticks=%" PRIu64 " releases=%u deadline_miss=%u skipped=%u"
		       " exec_min=%" PRIu64 " exec_max=%" PRIu64 " exec_sum=%" PRIu64
		       " response_max=%" PRIu64 " checksum=%x schedulable=%s status=%s\n",
		       cpu, cpu + 1, test->period, test->period, test->completed, misses,
		       test->skipped, exec_min, exec_max, exec_sum, response_max, test->checksum,
		       misses || test->skipped ? "FAIL" : "PASS",
		       ok ? "PASS" : "FAIL");
	}
}

static void worker(void *arg, void *unused1, void *unused2)
{
	unsigned int cpu = (uintptr_t)arg;
	uint64_t reg, el;
	uint64_t *private = (uint64_t *)DTCM_BASE;

	ARG_UNUSED(unused1); ARG_UNUSED(unused2);
	__asm__ volatile("mrs %0, mpidr_el1" : "=r"(mpids[cpu]));
	__asm__ volatile("mrs %0, sctlr_el1" : "=r"(sctlrs[cpu]));
	__asm__ volatile("mrs %0, CurrentEL" : "=r"(el));
	__asm__ volatile("mrs %0, S3_0_C15_C0_2" : "=r"(reg));
	reg |= 1;
	__asm__ volatile("msr S3_0_C15_C0_2, %0; isb" :: "r"(reg) : "memory");
	__asm__ volatile("mrs %0, S3_0_C15_C0_2" : "=r"(dtcm_regs[cpu]));
	private_ok[cpu] = (arch_curr_cpu()->id == cpu && el == 4 &&
		(sctlrs[cpu] & 0x1005) == 0x1005 &&
		(dtcm_regs[cpu] & ~UINT64_C(0x3fff)) == DTCM_BASE &&
		((dtcm_regs[cpu] >> 2) & 63) == 8 && (dtcm_regs[cpu] & 1));
	for (unsigned int i = 0; i < PRIVATE_WORDS; i++) {
		private[i] = mpids[cpu] ^ i;
	}
	complete_accesses();
	k_sem_give(&written);
	k_sem_take(&check_private[cpu], K_FOREVER);
	for (unsigned int i = 0; i < PRIVATE_WORDS; i++) {
		private_ok[cpu] &= private[i] == (mpids[cpu] ^ i);
	}
	if (!private_ok[cpu]) {
		atomic_inc(&failures);
	}
	k_sem_give(&checked);
	k_sem_take(&start_work[cpu], K_FOREVER);
	if (cpu == 0) {
		if (!memory_attributes("TCM", DTCM_BASE, 128 * 1024, 0x44) ||
		    !memory_attributes("MM", MM_BASE, 8 * 1024 * 1024, 0xff) ||
		    !memory_attributes("SRAM_NC", NC_BASE, 8 * 1024 * 1024, 0x44)) {
			atomic_inc(&failures);
		}
		memory_test("TCM", DTCM_BASE, 16 * 1024);
		memory_test("L1", MM_BASE, 16 * 1024);
		memory_test("L2", MM_BASE, 256 * 1024);
		memory_test("SRAM_NC", NC_BASE, 256 * 1024);
		timer_test(cpu, false);
		report_timer(cpu, false);
		k_sem_give(&bench_done);
		k_sem_take(&start_work[cpu], K_FOREVER);
	}
	timer_test(cpu, true);
	k_sem_give(&rt_done);
}

static void background_load(void *arg, void *unused1, void *unused2)
{
	unsigned int cpu = (uintptr_t)arg;
	uint64_t *data = (uint64_t *)(MM_BASE + 2 * 1024 * 1024 + cpu * LOAD_BYTES);
	uint64_t sum = 0;

	ARG_UNUSED(unused1); ARG_UNUSED(unused2);
	for (unsigned int i = 0; i < LOAD_BYTES / 8; i++) {
		data[i] = i;
	}
	k_sem_give(&load_ready);
	k_sem_take(&start_load[cpu], K_FOREVER);
	while (!atomic_get(&stop_load)) {
		sum += read_stream(data, LOAD_BYTES / 8);
		load_passes[cpu]++;
		__asm__ volatile("" ::: "memory");
		k_yield();
	}
	if (!load_passes[cpu] ||
	    sum != (LOAD_BYTES / 8) * (uint64_t)(LOAD_BYTES / 8 - 1) / 2 * load_passes[cpu]) {
		atomic_inc(&failures);
	}
	k_sem_give(&load_done);
}

int main(void)
{
	unsigned int cpu_mask = 0, tcm_mask = 0;
	uint64_t freq;

	__asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
	printk("SI_CL1_START version=1 cpus=%u counter_hz=%" PRIu64 "\n", NCPU, freq);
	if (freq != COUNTER_HZ) {
		atomic_inc(&failures);
	}
	IRQ_CONNECT(BENCH_IRQ, IRQ_DEFAULT_PRIORITY, timer_isr, NULL, 0);
	for (unsigned int i = 0; i < NCPU; i++) {
		k_sem_init(&check_private[i], 0, 1);
		k_sem_init(&start_work[i], 0, 1);
		k_sem_init(&start_load[i], 0, 1);
		k_sem_init(&timers[i].ready, 0, IRQ_SAMPLES);
		k_thread_create(&workers[i], worker_stacks[i], K_THREAD_STACK_SIZEOF(worker_stacks[i]),
				worker, (void *)(uintptr_t)i, NULL, NULL, i + 1, 0, K_FOREVER);
		__ASSERT_NO_MSG(k_thread_cpu_pin(&workers[i], i) == 0);
		k_thread_start(&workers[i]);
	}
	for (unsigned int i = 0; i < NCPU; i++) {
		k_sem_take(&written, K_FOREVER);
	}
	for (unsigned int i = 0; i < NCPU; i++) {
		k_sem_give(&check_private[i]);
	}
	for (unsigned int i = 0; i < NCPU; i++) {
		k_sem_take(&checked, K_FOREVER);
	}
	for (unsigned int i = 0; i < NCPU; i++) {
		unsigned int affinity = (mpids[i] >> 8) & 255;

		if (affinity >= NCPU || (mpids[i] & 0xfffcff) != 0x10000 ||
		    (cpu_mask & BIT(affinity))) {
			atomic_inc(&failures);
		} else {
			cpu_mask |= BIT(affinity);
		}
		tcm_mask |= private_ok[i] ? BIT(i) : 0;
		printk("SI_CL1_CPU cpu=%u mpidr=%" PRIx64 " el=1 sctlr=%" PRIx64
		       " dtcm=%" PRIx64 " private=%s\n", i, mpids[i], sctlrs[i],
		       dtcm_regs[i], private_ok[i] ? "PASS" : "FAIL");
	}
	k_sem_give(&start_work[0]);
	k_sem_take(&bench_done, K_FOREVER);
	for (unsigned int i = 0; i < NCPU; i++) {
		k_thread_create(&loaders[i], load_stacks[i], K_THREAD_STACK_SIZEOF(load_stacks[i]),
				background_load, (void *)(uintptr_t)i, NULL, NULL, 10, 0, K_FOREVER);
		__ASSERT_NO_MSG(k_thread_cpu_pin(&loaders[i], i) == 0);
		k_thread_start(&loaders[i]);
	}
	for (unsigned int i = 0; i < NCPU; i++) {
		k_sem_take(&load_ready, K_FOREVER);
	}
	rt_start = now() + COUNTER_HZ / 1000;
	for (unsigned int i = 0; i < NCPU; i++) {
		k_sem_give(&start_work[i]);
		k_sem_give(&start_load[i]);
	}
	for (unsigned int i = 0; i < NCPU; i++) {
		k_sem_take(&rt_done, K_FOREVER);
	}
	atomic_set(&stop_load, 1);
	for (unsigned int i = 0; i < NCPU; i++) {
		k_sem_take(&load_done, K_FOREVER);
	}
	for (unsigned int i = 0; i < NCPU; i++) {
		report_timer(i, true);
		printk("SI_CL1_LOAD cpu=%u ws_bytes=%u passes=%" PRIu64 "\n", i, LOAD_BYTES, load_passes[i]);
	}
	printk("SI_CL1_RESULT status=%s failures=%ld cpu_mask=%x tcm_mask=%x"
	       " mem_cases=%u rt_tasks=%u irq_samples=%u\n",
	       atomic_get(&failures) ? "FAIL" : "PASS", (long)atomic_get(&failures),
	       cpu_mask, tcm_mask, mem_cases, rt_tasks, irq_samples);
	uart_poll_out(DEVICE_DT_GET(DT_CHOSEN(zephyr_console)), 4);
	return atomic_get(&failures) != 0;
}
