# Apollo SI CL1 standalone benchmark

Ported from `/srv/oss_contrib/firmware/zena-si/zephyr/bench`. Build with the
`zephyr-si-cl1-bench` Yocto recipe. The `apollo_si_cl1_bench` board uses four
Cortex-R82 PEs, EL2 entry, PMSA, a 100 MHz counter, View2 GIC and CL1 PL011.
It has no AP, CL0, HIPC or PFDI dependency. The existing production
`apollo_qvp_safety_island_c1` board is unchanged.

Memory tests cover private DTCM, L1-sized and L2-sized MM SRAM working sets,
and uncached SRAM. Physical timer IRQ latency and four pinned periodic threads
are measured both idle and with memory interference. Guest counter ticks, not
host time, define all measurements. `SI_CL1_RESULT status=PASS` requires all
functional checks. Cache labels are intentions until matching ROI statistics
confirm the traffic. QBox is functional evidence; gem5 timing is uncalibrated
and does not establish physical Cortex-R82AE bandwidth or WCET.

The model assumes 128 KiB private DTCM per PE, 64 KiB L1D and shared 512 KiB L2.
MM SRAM is at 0x40000000, NC SRAM at 0x48000000, DTCM at 0x100100000 and
image LLRAM at 0x140000000. These are isolated benchmark integration choices.
