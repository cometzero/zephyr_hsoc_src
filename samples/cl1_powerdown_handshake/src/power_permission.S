/* SPDX-License-Identifier: Apache-2.0 */
#include <zephyr/toolchain.h>
#include <zephyr/linker/sections.h>

/* Isolated validation application only. R82 TRM 102670_0101_02:
 * ACTLR_EL2.POWER[2] permits EL1 implementation-defined power register writes;
 * HCR_EL2.TIDCP[20] must not trap those accesses. Preserve boot's x23..x25.
 */
GTEXT(z_arm64_el2_plat_prep_c)
SECTION_FUNC(TEXT, z_arm64_el2_plat_prep_c)
	mrs x0, actlr_el2
	orr x0, x0, #4
	msr actlr_el2, x0
	mrs x0, hcr_el2
	bic x0, x0, #(1 << 20)
	msr hcr_el2, x0
	isb
	ret
