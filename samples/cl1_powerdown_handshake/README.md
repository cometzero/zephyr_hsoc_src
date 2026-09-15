# CL1 powerdown handshake prerequisite

This is an opt-in **destructive standalone validation application**, not a
production suspend backend. It must never replace deployed firmware implicitly.
Load its exact ELF only into a disposable FVP. The `cl1_powerdown` shell command
is irreversible without reset. It does not save or restore architectural state.

Four pinned threads disable local GIC interfaces and execute the Cortex-R82
`IMP_CPUPWRCTLR_EL1.PWRDN` + ISB + WFI sequence. An early EL2 hook permits EL1
power-register writes. CL0 must remain alive to control power. Do not disable
SMP: CL0 starts four cores concurrently and the single-core Zephyr reset path
does not park the extra cores safely.

Before the all-core rendezvous each worker sets its own `GICR_WAKER.ProcessorSleep`
and waits, bounded to 1000000 reads, for `ChildrenAsleep`. Debug-visible
`cl1_gic_sleep_state` values are 1 (waiting), 2 (acknowledged), or 3 (timeout);
`cl1_gic_waker` records the last read. A timeout parks without claiming PWRDN.
Before WAKER, workers disable their local SGI/PPI enables and wait bounded for
GICR_CTLR.RWP to clear, matching Zephyr's GICv3 initialization sequence. The
before/after enable and pending masks are recorded; pending bits are not cleared.
Sleep state 4 means local disable failed. Shared GICD is not modified.
The addresses come from the board's four redistributor DTS registers.
This follows GIC-720AE TRM WAKER and TF-A `gicv3_rdistif_mark_core_asleep`.
CL1 uses GIC view 2: WAKER ProcessorSleep/ChildrenAsleep are available, but
GICR_PWRR is view-0-only. The app deliberately does not write PWRR or global
WAKER.Sleep; a view-0 power owner remains necessary if that handshake is required.

Reference: [Cortex-R82 TRM 102670_0101_02](https://documentation-service.arm.com/static/639b3bd83f28e54564349784),
sections 6.8 and A.2.2.41. R82AE/FVP register accessibility and the hardware
handshake still require runtime qualification; build success is insufficient.

## Evidence contract

1. Confirm the application banner and exact ELF provenance.
2. Stop CL0 PFDI monitoring for this sacrificial experiment, or use a CL0 image
   explicitly configured without CL1 liveness recovery. This app does not emit
   the production PFDI heartbeat. Do not treat a watchdog reset as successful OFF.
3. Issue `cl1_powerdown`; observe all four PWRDN bits and the debug-visible
   `cl1_powerdown_ready_mask == 0xf` using Iris. The UART ARMED marker alone is
   not confirmation that WFI was reached.
4. Ask CL0/PPU for ordinary OFF, observe core and cluster PWSR OFF. Do not use
   debugger CPU gating as a substitute for this firmware handshake.
5. ON/reset and record reset PCs. This is cold reset, **not context resume**.

Only always-on-domain memory and DRAM are assumed preserved by the project
power-off contract. CL1 LLRAM, this application's stacks and checkpoint are NOT
retained context. A real resume extension requires a reserved AON/DRAM region,
CL0 reset-vector handoff, boot-independent trampoline and all-core architectural,
GIC and timer restore. No unallocated shared-SRAM address is invented here.

## Build

Use a separate build directory and the same Zephyr module/toolchain setup as the
board's normal build, selecting this directory as the application and
`apollo_fvp_safety_island_c1` as the board. Do not run a shared Yocto build or
overwrite a deployed ELF as part of this prerequisite test. This sample disables
the production CRC prehook because direct ELF loading does not populate its
firmware-image CRC placeholder.

From the workspace root, reuse the already configured FVP recipe's read-only
SDK/modules without executing BitBake or its destructive `do_configure` script:

```sh
python3 - <<'PY'
from pathlib import Path
import os
import subprocess

root = Path.cwd()
cache = root / 'build/tmp_baremetal/work/apollo_fvp_safety_island_c1-zephyr/zephyr-demos-cl1/4.1.0+git/build/CMakeCache.txt'
values = {}
for line in cache.read_text().splitlines():
    if line and not line.startswith(('#', '//')) and '=' in line:
        key, value = line.split('=', 1)
        values[key.split(':')[0]] = value
env = os.environ.copy()
env['ZEPHYR_SDK_INSTALL_DIR'] = values['ZEPHYR_SDK_INSTALL_DIR']
env['ZEPHYR_TOOLCHAIN_VARIANT'] = 'zephyr'
app = root / 'hsoc-stack/components/system_mgmt/zephyrproject/zephyr_hsoc_src/samples/cl1_powerdown_handshake'
build = root / 'build/agent-debug/cl1-powerdown-app'
command = ['cmake', '-S', str(app), '-B', str(build), '-G', 'Ninja']
for key in ('ZEPHYR_BASE', 'ZEPHYR_MODULES', 'PYTHON_EXECUTABLE', 'Python3_EXECUTABLE', 'BOARD'):
    command.append(f'-D{key}={values[key]}')
command.append('-DZephyr_DIR=' + values['ZEPHYR_BASE'] + '/share/zephyr-package/cmake')
subprocess.run(command, env=env, check=True)
subprocess.run(['cmake', '--build', str(build), '--parallel', '4'], env=env, check=True)
PY
```

This sample uses local interrupt masking (`arch_irq_lock`), not the SMP-global
`irq_lock` that would prevent other workers reaching the barrier. It disables
the active virtual timer only: the baseline EL2 configuration traps EL1 physical
timer access. An earlier probe found this distinction material.

## Disposable runtime

Only after other FVP instances release UART ports 5000–5006, start the launcher
in one terminal and immediately attach the probe from another. Use fresh output
directories and a fresh Iris port on each run; a recently closed Iris socket can
prevent rebinding. Check listening sockets with `ss`, not a speculative TCP client.

```sh
./run_fvp.sh --machine apollo-fvp --bsp --headless --timeout 300 \
  --out-dir build/agent-debug/cl1-firmware-powerdown-run \
  -- --iris-server --iris-port 17225 --print-port-number

python3 scripts/debug/probe_fvp_cl1_firmware_powerdown.py \
  --iris-python build/tmp_baremetal/sysroots-components/x86_64/fvp-rd-aspen-native/usr/lib/fvp/fvp-rd-aspen/Iris/Python \
  --elf build/agent-debug/cl1-powerdown-app/zephyr/zephyr.elf \
  --scp-elf build/tmp_baremetal/deploy/images/apollo-fvp/si0_ramfw.elf \
  --ppu-access scp-call --port 17225 --timeout 180 \
  --output build/agent-debug/cl1-firmware-powerdown-run/firmware-powerdown.json
```

The automation requires simulation time zero. It stops at original CL1 reset
entry `0x140000000`, after RSE firmware loading, and loads the standalone ELF.
Setting the four PCs to the new ELF entry is **initial debugger boot loading**,
not restoration after suspend. The JSON binds the loaded ELF's SHA-256 and entry
address, UART output, PPU samples and final CPU power/trap registers. The sample
does not emit PFDI; the automation gates CL0 only after boot to isolate this test,
and never gates CL1 as a substitute for firmware powerdown.
CL0 remains powered but its firmware execution is paused during this experiment;
this is **not AON firmware-liveness qualification** or the production CL0 power
management protocol. Normal integration must replace this isolation with an
explicit PFDI/power-management handshake.

PASS is restricted to actual core and cluster PWSR OFF. `context_resume_passed`
always remains false. Failed ARMED-only runs are not powerdown evidence. After
inspection, terminate only this owned FVP using Iris `release(shutdown=True)` or
the launcher's bounded timeout. Do not continue the sacrificed production OS.

The current `scp-call` path also calls the compiled stock
`set_redistributor_power(base, OFF)` for CL1 view-0 bases 0x30060000 through
0x300c0000 before PPU requests. It checks TYPER affinity, WAKER=6 and RDAG=0,
verifies live helper bytes against the ELF, observes its actual STR and return,
and requires RDPD acknowledgment. CL0 redistributor and global Sleep are excluded.

## Observed qualification boundary (2026-09-14)

`cl1-firmware-powerdown-rdpd-20260914` further proved actual stock SCP RDPD
writes and acknowledgments: CL1 PWRR became 0x101/0x201/0x301/0x401 while CL0
PWRR remained zero. Subsequent actual PPU requests still did not reach OFF:
115 samples over 2.3455330804 simulated seconds stayed PWSR=8, DISR=0x100.
The ELF and WFI address match the localirq run below. Thus local interrupt
disable, WAKER acknowledgment and per-redistributor RDPD have now been exercised;
the remaining ON handshake is unresolved, not proof of unsupported hardware.

`cl1-firmware-powerdown-localirq-20260914` additionally proved SGI/PPI enables
changed from 0x08000005 to zero on all four cores, pending masks stayed zero,
RWP cleared and WAKER acknowledged. OFF still failed over 2.3465331704 simulated
seconds (115 samples). Its ELF SHA-256 is
`cb652ff6ee177a9b2cc1aa517e0a52f33341cf0519ac1ac6f70855d93232f24b`;
all final PCs were WFI at `0x1400011a8`. Read-only view-0 TYPER observations
confirmed CL1 bases 0x30060000 through 0x300c0000, with RDPD still zero.
No PWRR writes have been tested; see that run's supplemental readback artifact.

The later `cl1-firmware-powerdown-gic-20260914` run additionally proved all four
WAKER ProcessorSleep/ChildrenAsleep acknowledgements (`WAKER=6`, sleep state 2).
Its ELF SHA-256 is `b9b005ecc92a71c82bd116bc8d1052a362c40012495633e963ee3462c978e256`
and all four final PCs were the new WFI address `0x1400010e4`. Actual SCP function
calls performed the AE-key/PWPR writes, but 116 samples over 2.3510060784 simulated
seconds still showed PWSR=8. OFF remains FAIL. A documented view-0 PWRR owner
and sequence remain to be qualified; WAKER acknowledgement alone is insufficient
in this observed configuration. No PWRR writes or handshake overrides were used.

The r4 run in `build/agent-debug/cl1-firmware-powerdown-r4-20260914` cold-booted
this sample, then observed every core at `powerdown`'s WFI instruction
(`0x140001050` in SHA-256
`c6a2be0c37aeb5d7cc6d5050b87f1c838399083726aef5d825d4b50e64a7750f`).
All four `IMP_CPUPWRCTLR_EL1` values were 1, `CNTV_CTL_EL0` values were 0,
`ESR_EL2` values were 0, and `cl1_powerdown_ready_mask` was 15.

Nevertheless, 115 samples over 2.35153749 simulated seconds after static OFF
requests kept all four core PWSR values at 8 (ON). Therefore **OFF failed** and
cluster OFF was not requested. Follow-up Iris register reads showed every core
PWPR=0, PWSR=8 and DISR=0x100. This isolates a remaining PPU/model/integration
handshake question; it does not prove that real hardware lacks powerdown support.
No context retention or resume was demonstrated. This run proves execution of
the firmware prerequisite sequence, not successful completion of powerdown.

A subsequent `cl1-firmware-powerdown-scpcall-20260914` run removed the Iris
resource-write ambiguity: the controller executed the stock SCP
`ppu_v1_request_power_mode` on CL0, including actual AE-key and PWPR STRs.
Live function bytes matched the hash-bound SCP ELF, and after-store/return
breakpoints verified each call. Diagnostic CL0 registers and 256 stack bytes
were saved/restored solely around these calls; that is not user suspend/resume.
CL0 was parked between calls to prevent PFDI recovery dispatch. This default
`--ppu-access scp-call` mode does not read CL1 CPU registers before testing OFF.

Even with actual MMIO, all cores remained PWSR=8 across 115 samples spanning
2.3455537168 simulated seconds. The SCP ELF hash was
`c71851d90a0e52609eef90dad53623866e8217d7e095bb4ad2389e249f657551`.
This narrows the remaining failure beyond debugger register-storage semantics,
but still does not justify forcing PPU acknowledgements or claiming context
resume. The optional `scp-cli` route was attempted but CLI entry timed out;
it is not the validated control route.
