# Building RASP

Build it in two stages. **Stage 1** puts the real DSP on real hardware for
about $80 with no soldering, so you can hear it and measure latency and
CPU. **Stage 2** is the full floor pedal.

Prices are rough USD from distributor listings checked in October 2026. Check
stock before ordering: some audio ICs have long factory lead times.

---

## Stage 1: bench test on a Daisy Pod (≈ $70–90 + cables)

| Buy | Qty | Notes |
|---|---|---|
| **Electrosmith Daisy Pod** | 1 | Includes a Daisy Seed (65 MB). Line-level 3.5 mm in/out, headphone amp, 2 knobs, 2 buttons, encoder and TRS MIDI in. Listed at $68–89 |
| Micro-USB data cable | 1 | For flashing and power |
| 3.5 mm ↔ 6.35 mm (or XLR) cables | 2 | Interface line out → Pod in; Pod out → interface or monitors |
| *Your audio interface + mic* | – | The interface is the mic preamp for now |

**Signal path:** mic → interface preamp → interface line out → **Pod IN** →
**Pod headphone out** (listen here, about 1 ms through the box) → optionally back into
the interface to record.

**Flash it** (Linux/macOS/WSL; Windows works with the Daisy toolchain installer):
```sh
# toolchain: arm-none-eabi-gcc 13 (e.g. apt install gcc-arm-none-eabi libnewlib-arm-none-eabi
#            libstdc++-arm-none-eabi-newlib), make, dfu-util
git clone --recursive https://github.com/electro-smith/libDaisy && make -C libDaisy
cd rasp/daisy/pod
make LIBDAISY_DIR=../../../libDaisy
# put the Seed in DFU mode: hold BOOT, tap RESET, release BOOT
make LIBDAISY_DIR=../../../libDaisy program-boot   # once: Electrosmith bootloader
# tap RESET, then within ~2 s:
make LIBDAISY_DIR=../../../libDaisy program-dfu
```

**Pod controls:**

| Control | Does |
|---|---|
| Encoder turn | Picks the preset: huge / natural / dark / bright / live / raw. LED 1 colour shows which |
| Encoder press | Level-matched bypass. LED 1 is off while bypassed |
| Knob 1 / Knob 2 | Drive / chop |
| Button 1 | Input gain: 0, +6, +12 or +18 dB |
| Button 2 | Thicken + flavour on/off. Hold 2 s to clear the feedback notches |
| LED 2 | Red: chop is compressing more than 3 dB. Blue: a feedback notch is active |

**What to measure on the bench before Stage 2:**
- **Round-trip latency:** send a click out of the interface into the Pod and record the Pod's output.
  The estimate is about 1 ms.
- **CPU:** add libDaisy's `CpuLoadMeter` around `proc.process`. It should comfortably stay
  under 100%. The heaviest stage is the 4× oversampled drive.
- **Your ears:** sing through it, flip presets, use bypass.

---

## Stage 2: the full floor pedal (≈ $280–380 in parts)

The firmware is `daisy/main.cpp`; its header lists every pin. **No PCB or
schematic exists yet.** Build v1 point-to-point on perfboard, or have the
schematic drawn first (the next step on offer). Part numbers below are real,
current parts; quantities match the firmware.

### Brain

| Part | Qty | ≈ Price | Notes |
|---|---|---|---|
| Electrosmith **Daisy Seed 65 MB** | 1 | $30–50 | 64 MB SDRAM holds the 1.5 MB of delay lines. VIN accepts about 4–17 V |
| 2×20 female headers, 2.54 mm | 2 | $2 | So the Seed plugs in rather than being soldered |

### Analog: mic in, outputs, headphones

| Part | Qty | ≈ Price | Notes |
|---|---|---|---|
| **THAT1512P08-U** mic preamp (DIP-8) | 1 | $6–8 | Gain set by one resistor network. Use the application circuit in the THAT 1510/1512 datasheet. Runs on ±5 to ±20 V |
| Reverse-log pot for preamp gain + RG network parts | 1 set | $5 | Per the datasheet's variable-gain circuit; this is the "input gain" knob |
| **THAT1646P08-U** balanced line driver (DIP-8) | 2 | $7 each | Drives the L and R XLR outs. ±4 to ±18 V supply |
| DPDT toggle, ON-ON | 1 | $3 | Mic/line switch: a resistive pad (≈ −36 dB) after each THAT1646 for mic level |
| **NJM4556AD** headphone op-amp (DIP-8) | 1 | $1.25 | For IEM monitoring. Stock is patchy ("not for new designs"); buy 2 |
| Dual-gang 16 mm pot, A10K (log) | 1 | $3 | Headphone volume |
| Resistor divider into Daisy IN | – | – | Scale the preamp output to about 1 Vrms, the Seed's input ceiling (AC-coupled) |

### Bypass relay (power-fail fallback)

| Part | Qty | ≈ Price | Notes |
|---|---|---|---|
| **Panasonic TQ2-5V**, DPDT, non-latching, 5 V coil | 1 | $4 | De-energized, it hard-wires XLR in → XLR out L. The firmware energizes it at boot |
| 2N3904 transistor + 1 kΩ base resistor | 1 | $0.20 | Relay driver from pin D27 |
| 1N4148 diode | 1 | $0.05 | Flyback across the coil |

### Controls and LEDs

| Part | Qty | ≈ Price | Notes |
|---|---|---|---|
| 16 mm pots, B10K linear (e.g. Alpha 16 mm) | 16 | $1.50 each | Knobs. Two banks of 8 through the multiplexers |
| 45 mm slide pots, 10 kΩ linear (e.g. Bourns PTA45) | 4 | $3 each | Section level faders: control, thicken, flavour, output |
| 9 mm pots or trimmers, 10 kΩ linear | 3 | $1 each | Bleed, de-ess and glue trims |
| **CD4051B** 8:1 analog multiplexer (DIP-16) | 2 | $0.60 each | Pot banks into A0 and A1 |
| **74HC595** shift register (DIP-16) | 1 | $0.50 | Drives 8 LEDs from 3 pins |
| 3 mm LEDs + bezels, 1 kΩ resistors | 8 | $3 total | Effect, chop, bleed, thicken, flavour, tap/preset, limiter, feedback |
| Soft-touch **momentary** SPST footswitches | 4 | $4 each | Bypass, thicken, flavour, tap. Bypass is digital, so these must be momentary, not latching |
| ON-OFF-ON toggles (SPDT centre-off) | 2 | $3 each | Tone (dark/natural/bright) and delay time |
| SPDT ON-ON toggle | 2 | $2 each | Mono switch and ground lift |
| Momentary pushbutton | 1 | $1 | Solo (hold 2 s to save the preset) |
| Knob caps | 19 | $15 | – |

### Connectors

| Part | Qty | ≈ Price | Notes |
|---|---|---|---|
| **Neutrik NC3FD-L-1**, female XLR panel | 1 | $4–6 | Mic in |
| **Neutrik NC3MD-L-1**, male XLR panel | 3 | $4–5 each | Out L, out R, dry thru (a passive split of the mic input) |
| 6.35 mm stereo (TRS) panel jack | 1 | $3 | Headphones / IEM |
| 2.1 mm DC panel jack, centre-negative | 1 | $2 | Power in |

### Power: 12 V DC, not 9 V

There's no common module that turns 9 V into ±12 V, but 12 V-input ones are
standard, and most vocal pedals run on 12 V anyway.

| Part | Qty | ≈ Price | Notes |
|---|---|---|---|
| 12 V DC, 1 A, centre-negative wall adapter | 1 | $10 | – |
| **Traco TMA 1212D** isolated DC-DC, 12 V → ±12 V, 1 W (5-pin SIP) | 1 | $10 | Analog rails for the THAT chips and the NJM4556. Confirm the part in Traco's catalogue before ordering. Driving loud headphones may need a 2 W module |
| 1N5817 Schottky diode | 1 | $0.20 | Reverse-polarity protection |
| Decoupling caps (100 nF ceramic per IC, 47–100 µF electrolytic per rail) | – | $5 | – |

The Daisy Seed runs from the same 12 V through its VIN pin. **v1 has no +48 V
phantom power**, so use a dynamic mic (SM58, Beta 58A, e835). That's the right
choice live anyway.

### Enclosure, board, wiring

| Part | Qty | ≈ Price | Notes |
|---|---|---|---|
| **Hammond 1456KK4BKBK** sloped aluminium console, 259 × 254 × 102 mm | 1 | $50–70 | Room for 4×4 knobs, 4 faders and 4 footswitches. Or have a custom folded steel enclosure made |
| Perfboard or a custom PCB (e.g. JLCPCB/PCBWay, 5 boards) | – | $20–40 | A PCB needs the schematic first |
| IC sockets (8- and 16-pin), hookup wire, standoffs, screws | – | $20 | – |

### Tools
Soldering iron, multimeter, wire strippers, a step drill (or punches) for
the panel, and a 24 mm hole punch for the XLRs.

---

## Wiring summary (matches `daisy/main.cpp`)

```
XLR in ─┬─► THAT1512 (gain pot) ─► divider ─► Daisy IN L ──► DSP ─► Daisy OUT L/R
        ├─► XLR dry thru (passive split)                          │
        └─► relay NC contact ─────────────────────┐               ├─► THAT1646 ×2 ─► mic/line pad ─► relay NO ─► XLR out L/R
                                                    └── (power off: in → out L)
                                                                   └─► NJM4556 (vol pot) ─► headphone jack
Pots: 16 → 2× CD4051 (A0, A1; selects D0–D5) · faders A2–A5 · trims A6–A8
Footswitches D10–D13 · solo D14 · tone D6/D7 · time D8/D9 · mono D28
LEDs via 74HC595 on D24/D25/D26 · relay driver D27 · USB = MIDI + flashing
```

## Status

- Both firmwares (`daisy/` and `daisy/pod/`) compile and link against
  libDaisy v9.0.0, with no warnings in RASP's own code. Neither has run on a
  board yet.
- CPU load and round-trip latency are unmeasured; Stage 1 measures both.
- The analog design (preamp gain network, pads, dividers, headphone stage) is
  specified at the block level above. It needs a schematic and a bench check of levels.

Sources checked: [Daisy Seed datasheet](https://www.electrokit.com/upload/product/41020/41020659/Daisy_Seed_datasheet_v1.0.5.pdf) ·
[Daisy Seed listing (VIN 4–17 V)](https://www.signalsounds.com/electro-smith-daisy-seed-development-board) ·
[Daisy Pod](https://www.electro-smith.com/daisy/pod) ·
[Daisy audio levels (forum)](https://forum.electro-smith.com/t/not-getting-line-levels-into-out-of-daisy-seed/2495) ·
[THAT1512](https://jp.farnell.com/that-corporation/that1512p08-u/-5v-20v-dip-8-/dp/1354162) ·
[THAT1646](https://tr.farnell.com/that-corporation/that1646p08-u/ic-line-driver-balanced-1ch-8dip/dp/1354158) ·
[Panasonic TQ2-5V](https://www.cytechsystems.com/product/tq2-5v) ·
[NJM4556AD](https://jdslabs.com/product/jrc-njm4556ad) ·
[Neutrik NC3FD-L-1](https://www.markertek.com/product/nc3fd-l-1/neutrik-nc3fd-l-1-3-pin-female-xlr-panel-chassis-mount-connector-latching) ·
[Neutrik NC3MD-L-1](https://www.kirstein.de/en/Accessories-XLR-Plugs-Sockets/Neutrik-NC3MD-L-1-XLR-Panel-Connector-Male.html) ·
[Hammond 1456KK4BKBK](https://uk.rs-online.com/web/p/general-purpose-enclosures/2777486?gb=s)
