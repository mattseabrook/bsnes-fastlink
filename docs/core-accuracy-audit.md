# SNES core accuracy audit

Date: 2026-08-20

Branch: `experimental-core`

Starting revision: `e009fa4a35c9e09730602c85674d7b28f85cd941`

## Decision

Keep the current bsnes core and run it in its accuracy configuration. A complete
core replacement is not justified by the available evidence.

The 5A22/65C816 implementation in this repository is byte-for-byte identical to
the current upstream bsnes implementation at
`7d5aa1e656b9171524d01b1b22917197d8121cb4`. It also already contains the two
recent hardware-test-driven WDC65816 corrections from ares: emulation-mode PLB
stack wrapping (`e589f8d9c`) and the emulation-mode `(direct,X)` wraparound bug
(`be8fa76e7`). The fork's material accuracy regression was configuration: on
Windows it forcibly selected fast PPU/DSP behavior and delayed coprocessor
synchronization even when the accurate engines were present.

There is no public, comprehensive result set establishing a different actively
maintained core as more accurate than bsnes's accurate profile across the CPU,
PPU, SMP/DSP, DMA, and enhancement chips. ares is the strongest alternative and
has an explicit accuracy/preservation focus, but its current SFC CPU is the same
lineage and materially the same behavior. Moving it here would also require its
thread, node, debugger, configuration, and frontend architecture, replacing the
FastLink work without producing a demonstrated CPU-accuracy gain.

This is not a claim that the selected core perfectly models a console. The audit
found several explicitly documented hardware unknowns, listed below.

## Sources and pinned comparison revisions

The repository comparisons used detached research refs, not new project remotes:

| Source | Revision examined | Use |
| --- | --- | --- |
| [bsnes](https://github.com/bsnes-emu/bsnes) | `7d5aa1e656b9171524d01b1b22917197d8121cb4` (2026-05-23) | Same-lineage upstream and latest PPU correction |
| [ares](https://github.com/ares-emulator/ares) | `f23eb39ad55e16fe2dae24897f982beccd8dbf43` (2026-08-20) | Accuracy-focused successor and hardware-test fixes |
| [OpenSFC](https://github.com/starlightk7/OpenSFC) | `6574450b1a4594b2aae436cf23869b0fb5808ce8` (2026-03-24) | Reproduced SHVC-CPU-01 board schematic, symbols, and PCB connectivity |
| [gilyon/snes-tests](https://github.com/gilyon/snes-tests) | release 1.4 | Full 65C816 and SPC-700 functional tests |

Behavior was cross-checked against the official
[W65C816S data sheet](https://www.westerndesigncenter.com/wdc/documentation/w65c816s.pdf),
[fullsnes](https://problemkaputt.de/fullsnes.htm), the
[Super Famicom timing notes](https://wiki.superfamicom.org/timing), and the
[SNESdev chip-version notes](https://snes.nesdev.org/wiki/Version_differences).
The WDC document describes the processor ISA and external signals. It does not
describe the Ricoh 5A22's integrated DMA/HDMA, controller polling, multiply and
divide unit, interrupt registers, or console-specific bus scheduler; the SNES
sources are necessary for those additions.

Candidate assessment:

| Core | Assessment | Decision |
| --- | --- | --- |
| bsnes accurate | Current 5A22 code, low-level coprocessors, cycle-rendered PPU, active upstream | Selected |
| ares | Strong accuracy focus; current SFC CPU behavior is substantially shared with this core | Use as an ongoing behavioral reference, not a wholesale transplant |
| higan | Historically uncompromising accuracy/readability, but older and less active as an integration target | No replacement benefit demonstrated |
| MesenCE | Active continuation of Mesen 2 with a capable SNES core, but different GPLv3 architecture and no comprehensive evidence of greater total accuracy | Retain as a test/reference implementation |
| Snes9x | Excellent portability and compatibility focus; not positioned as a higher-accuracy replacement for bsnes accurate | Not selected |

Relevant project pages are [ares](https://ares-emu.net/),
[higan](https://github.com/higan-emu/higan), and
[MesenCE](https://github.com/nesdev-org/MesenCE).

## What OpenSFC can and cannot prove

OpenSFC is an open-hardware reproduction of the console motherboard. Its
SHVC-CPU-01 design exposes the S-CPU package pins and the PCB nets around them.
It is not a decapped Ricoh 5A22 transistor netlist or an internal logic
schematic. Consequently, a literal software-line-to-transistor proof is not
possible from this source—or from any public source found in this audit.

It is nevertheless valuable for checking the emulator's system boundary. The
QFP-100 S-CPU symbol and PCB footprint show:

| Hardware boundary | OpenSFC connection | Software counterpart | Finding |
| --- | --- | --- | --- |
| `A0..A23`, `D0..D7`, `R/W`, `CPU_RD`, `CPU_WR` | Cartridge/system address and data buses | `sfc/cpu/memory.cpp`, WDC65816 memory primitives | Address width, bank composition, reads/writes, and open-bus data flow match the boundary |
| `ROMSEL`, `WRAMSEL`, `PA0..PA7`, `PRD`, `PWR` | Cartridge, WRAM, and B-bus selection | CPU bus mapping and DMA A-bus/B-bus transfer code | The modeled bus split corresponds to the external pins |
| `HBLANK`, `VBLANK`, `NMI`, `IRQ`, `SYSCLK`, `PHI2` | PPU/clock/interrupt nets | `timing.cpp`, `irq.cpp`, `io.cpp` | Correct signal classes exist; exact internal phase behavior still requires measurements |
| `J1/J2`, `J1_OE/J2_OE`, `J.D6/J.D7` | Controller board serial lines | `$4016/$4017`, auto-joypad state machine | Port wiring and shared latch behavior agree; concurrent manual/automatic access remains partly unknown |
| `REFRESH` | DRAM refresh net | scanline refresh stall in `timing.cpp` | Total 40 master clocks are modeled; observed subphase is approximated |
| `RESET` | System reset | CPU reset vector and state initialization | Mode, stack, vector, and register reset paths are present |
| `ABORT` | Tied to `+5V` on SHVC-CPU-01 | No active external abort path | Acceptable for this stock board because the input is inactive |
| `RDY` | Tied to `+5V` on SHVC-CPU-01 | No external ready-stall input | Acceptable for this stock board because the input is inactive |
| `HALT`, `TM`, `HVCMODE` | Tied to ground | No runtime pin model | Acceptable for the examined fixed board configuration |
| `DMA`, `MEMLOCK`, `MFLAG`, `XFLAG`, `VPA`, `VDA`, `VEC_PULL` | Unconnected on this board | Mostly internalized or absent | Not observable through the stock motherboard, so OpenSFC cannot validate their internal timing |

One library-quality issue was noted in OpenSFC: the CPU `A6` symbol pin is marked
as an input while the rest of the CPU address bus is output. The PCB net itself
is connected correctly, so this appears to be symbol metadata rather than a
board-routing defect.

## 5A22 and WDC65816 line audit

Every executable and state-definition line in `bsnes/sfc/cpu` and
`bsnes/processor/wdc65816` was inspected—3,959 lines including the non-emulation
disassembler. The audit compared instruction behavior to the W65C816S data
sheet, console additions to the SNES references, external interfaces to
OpenSFC, and source behavior to current bsnes/ares. The table is a coverage
ledger, not a claim that public documents reveal the chip's internal gates.

| File | Lines | Result |
| --- | ---: | --- |
| `sfc/cpu/cpu.cpp` | 123 | Power/reset, region/version, thread entry, and interrupt vector path are coherent; entropy is deliberately configurable and accuracy mode disables behavioral hotfixes |
| `sfc/cpu/cpu.hpp` | 248 | 24-bit bus, I/O, ALU, interrupt, DMA, autojoy, and per-channel state cover the externally visible 5A22 facilities |
| `sfc/cpu/memory.cpp` | 86 | Variable 6/8/12-master-clock bus accesses, MDR/open-bus propagation, DMA edges, and refresh stalls are represented |
| `sfc/cpu/timing.cpp` | 255 | Two-clock scheduler, IRQ/NMI polling, HDMA trigger, ALU iteration, and current autojoy sequence are present; refresh phasing is approximate |
| `sfc/cpu/irq.cpp` | 94 | NMI/IRQ line, hold, transition, enable, and `$4210/$4211` clearing behavior are modeled |
| `sfc/cpu/dma.cpp` | 189 | All eight channels, transfer modes, A/B direction, fixed/reverse addressing, HDMA direct/indirect, and interleaving are modeled; two HDMA edge cases remain explicitly unverified |
| `sfc/cpu/io.cpp` | 307 | `$2180`, `$4016/17`, `$4200..421f`, `$4300..437f`, multiply/divide, IRQ timers, MEMSEL, DMA, and open-bus masking are covered; concurrent autojoy accesses and IRQ-timer writes have unknowns |
| `sfc/cpu/serialization.cpp` | 107 | All timing, I/O, ALU, DMA, and autojoy state needed for deterministic restoration is serialized |
| `wdc65816/algorithms.cpp` | 363 | Arithmetic, BCD, shifts, compares, loads, stores, and flag calculations match the ISA-level reference and are width-specialized |
| `wdc65816/instruction.cpp` | 64 | Opcode dispatch selects emulation/native and M/X width tables correctly |
| `wdc65816/instruction.hpp` | 258 | Full opcode and addressing-mode declarations are present |
| `wdc65816/instructions-modify.cpp` | 93 | Read-modify-write ordering and accumulator/memory width paths are explicit |
| `wdc65816/instructions-other.cpp` | 248 | Stack, transfers, flags, block move, interrupt, WAI/STP, XBA/XCE paths inspected; PLB uses emulation-mode stack-page wrapping |
| `wdc65816/instructions-pc.cpp` | 142 | Branch, jump, call, return, bank and page-cross behavior inspected |
| `wdc65816/instructions-read.cpp` | 209 | All read modes inspected; emulation `(direct,X)` uses the hardware wrap bug path |
| `wdc65816/instructions-write.cpp` | 176 | All store modes inspected; emulation `(direct,X)` uses the same wrap semantics |
| `wdc65816/memory.cpp` | 95 | Direct, indexed, indirect, long, stack-relative, bank/page wrap primitives inspected |
| `wdc65816/registers.hpp` | 65 | Native/emulation register widths, flags, hidden halves, PC bank, MDR, and vector state are represented |
| `wdc65816/serialization.cpp` | 33 | Architected and internal processor state are serialized |
| `wdc65816/wdc65816.cpp` | 41 | Power/reset state and interrupt entry inspected |
| `wdc65816/wdc65816.hpp` | 289 | Interface and mode-specialized operation set inspected |
| `wdc65816/disassembler.cpp` | 474 | Debug-only decode/format logic; no emulated behavior |

Current upstream bsnes has no differences in these two directories. The ares
differences are primarily its newer integer, thread, node, and debugger
framework rather than a different 5A22 execution model.

## Confirmed strengths and known gaps

Confirmed in the selected implementation:

- all documented W65C816 instructions and modes, native/emulation width rules,
  decimal arithmetic, stack behavior, vectors, and bank/page wrapping;
- hardware-tested emulation-mode PLB and `(direct,X)` wraparound behavior;
- eight-channel DMA/HDMA, variable-speed CPU bus, WRAM port, multiply/divide
  latency, NMI/IRQ registers, controller I/O, and current ares autojoy polling
  behavior (`d45be3297`);
- cycle-rendered PPU and accurate DSP paths are available and are now the
  branch defaults;
- the latest upstream high-resolution offset-per-tile PPU correction
  (`7d5aa1e6`) is applied to both PPU implementations.

Open issues that should not be hidden behind an “accurate” label:

1. `sfc/cpu/timing.cpp` records a logic-analyzer-observed DRAM-refresh pattern
   of five clocks active/three inactive, repeated five times. It implements
   six/two. Both total 40 master clocks, but exact subcycle observability is not
   represented.
2. `Channel::hdmaSetup()` initializes `hdmaDoTransfer` with a source comment
   saying hardware verification is needed.
3. Indirect HDMA reload clears the not-yet-read low byte; the source asks
   whether prior internal state should survive until that read completes.
4. Reads of `$4016/$4017` and `$4218..$421f`, and writes to `$4016`, during
   autojoy polling are explicitly undocumented in the implementation.
5. Immediate IRQ effects when writing `$4207..$420a` are marked unverified.
6. Chip revisions differ. OpenSFC's completed board is SHVC-CPU-01, while later
   5A22 revisions can expose different edge behavior; the core has a CPU
   version field but does not promise electrical simulation of every revision.

Changing these cases from documentation alone would be guesswork. The next
accuracy work should use targeted ROMs on a known CPU/PPU revision with a logic
analyzer. Useful public suites include the
[SNES test ROM collection](https://github.com/PeterLemon/SNES),
[undisbeliever's test ROMs](https://github.com/undisbeliever/snes-test-roms),
and published [DMA tests](https://snescentral.com/article.php?id=1123).

## Changes made on `experimental-core`

1. Added `FastLink/AccuracyFirst`, defaulting to `true`.
2. In that profile, disabled hotfixes, CPU/SA-1/SuperFX overclocking, fast math,
   fast PPU, PPU limit bypasses, HD Mode 7 scaling, fast/cubic/echo-shadow DSP,
   delayed coprocessor synchronization, and HLE preference.
3. Kept run-ahead available. Its speculative execution restores the committed
   serialized state; it is a latency technique rather than a persistent core
   behavior shortcut.
4. Changed libretro defaults for fast PPU, fast DSP, and delayed coprocessor
   synchronization to `OFF`.
5. Fixed the libretro SA-1 and SuperFX option lookups, which previously queried
   keys different from the keys advertised to the frontend.
6. Applied upstream's high-resolution offset-per-tile PPU fix to accurate and
   fast renderers.
7. Added `tools/libretro-smoke.cpp`, a small host that fixes accuracy options,
   executes a ROM headlessly, and writes the final video frame for inspection.

`AccuracyFirst: false` retains the prior FastLink fast-PPU/HD Mode 7 profile for
controlled comparison. Existing configuration files are normalized to the
accurate values when the new profile is enabled, rather than silently retaining
old fast-core toggles.

## Verification

Builds:

- Windows application: `BUILD_DIR=build_experimental MNT_COPY_DIR='' JOBS="$(nproc)" ./build.sh build` — passed. The existing build helper also copied the resulting executable to its configured `/mnt/PhantomIDE/bsnes.exe` destination.
- Linux libretro shared core: `make -C bsnes -f GNUmakefile clean` followed by `make -C bsnes -f GNUmakefile target=libretro binary=library local=false -j"$(nproc)"` — passed.
- Smoke host: `g++ -std=c++17 -O2 -Wall -Wextra tools/libretro-smoke.cpp -ldl -o /tmp/libretro-smoke` — passed without diagnostics.

Functional ROM results using gilyon/snes-tests release 1.4:

| ROM | Frames | Final result |
| --- | ---: | --- |
| `cputest-full.sfc` | 3,600 | `Running tests... Success`, terminal test `0649` |
| `spctest.sfc` | 3,600 | `Running tests... Success`, terminal test `0557` |

The downloaded release archive matched its published GitHub SHA-256 digest
`c3a39e15a9f3789fe143e54dfa035c988204576eafacf97df8ff3546a917c5e9`.

The CPU ROM covers all opcodes except `STP`/`WAI`, all addressing modes, edge
cases, flags, and undocumented wrapping cases. The SPC ROM covers all opcodes
except `SLEEP`/`STOP` and all supported modes. Their own documentation states
that they do not test cycle timing, CPU memory-mapped registers, interrupts,
S-SMP timers, or DSP. Passing them is strong functional evidence, not proof of
the unresolved bus-timing cases above.

## Recommended hardware-validation order

1. Measure and reproduce the 5/3 DRAM refresh phase, including DMA/HDMA overlap.
2. Build minimal ROMs for HDMA initialization and interrupted indirect-address
   reload on CPU revisions 1 and 2.
3. Sweep reads/writes to manual and automatic joypad registers through every
   128-master-clock polling phase.
4. Sweep writes to horizontal/vertical IRQ timer registers around equality and
   counter rollover.
5. Add each measured case as an automated ROM plus a deterministic libretro
   smoke assertion before modifying core behavior.

That sequence attacks the places where the source itself admits uncertainty;
replacing already-matching opcode code would not.
