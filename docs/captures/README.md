# Logic analyzer captures

Captured with a Raspberry Pi Pico running the sigrok-pico firmware (`pico_baseline.uf2`),
read in PulseView (nightly build; 0.4.2 does not support this firmware). Each `.sr` file is
a zip with the samples; the `.pvs` file next to it is the PulseView session (channels,
decoders, view). Open the `.sr` in PulseView, or read the samples with any zip tool.

Input protection: the I2C captures used a 1 kOhm resistor in each probe wire, as the
sigrok-pico user guide recommends (inputs outside 0 to 3.3 V can damage the Pico). The SPI
captures used **no resistors**: the probes go straight from the Nucleo's Arduino sockets to
the Pico, because every signal is 3.3 V logic and the sockets used are far from the 5 V
pin. The Pico's GND is joined to the Nucleo's GND.

SPI wiring: the Pi and the Nucleo stay connected as in ADR-0010 (female-to-female wires on
the Pi header and the Nucleo's morpho pins). The Pico taps the same signals at the Nucleo's
Arduino sockets: D2 = NSS (`A2`, PA4), D3 = SCK (`D13`, PA5), D4 = MISO (`D12`, PA6). MOSI
(`D11`, PA7) is not probed: the test script sends only zeros, so MOSI never changes, and a
probe wire on it disturbed the link (frames went bad while it was connected). In the clean
captures the Pico's GND came from the `GND` pin next to `D13`.

| File | Date | What | Settings |
|---|---|---|---|
| `phase1-terminal-output.png` | Phase 1 | Tera Term output of the sensor task | not a capture |
| `i2c_bmp280_5s.sr` | 2026-10-20 | Five BMP280 reads, one per second, no Pi connected | D2 = SCL (PB8), D3 = SDA (PB9); 1 MHz; 5 M samples (5 s); no trigger |
| `i2c_bmp280_1transaction.sr` | 2026-10-20 | One BMP280 read in detail | same channels; 5 MHz; 500 k samples (100 ms); trigger: falling edge on D3; 10% pre-capture |
| `spi_1MHz_clean_3probes.sr` | 2026-10-20 | Two frames, 1 MHz, the clean wiring: no spikes on NSS | D2 NSS, D3 SCK, D4 MISO; 20 MHz; no trigger; Pi: `poll --count 3000 --period 0.005` |
| `spi_1MHz_probe_spikes.sr` | 2026-10-20 | One frame with 12 spikes on NSS (a similar file, not kept, had 36). Each is one sample (50 ns) wide and on an SCK falling edge. The wiring was an earlier layout: a MOSI probe wire only partly connected, the Pico's ground from elsewhere. Cause not shown; it is the probe wiring that changed between this file and the clean one | same settings; D5 also enabled but not connected |
| `spi_1MHz_request_625kHz.sr` | 2026-10-20 | Two frames at 625 kHz although 1 MHz was requested (the Pi's SPI clock changed); no spikes | same settings; during this run the MOSI and MISO probe wires had been swapped, as I understood from the session |
| `spi_gap0_1MHz.sr` | 2026-10-20 | 30 back-to-back reads: NSS high 154 to 228 us between them | 20 MHz; Pi: `poll --count 30000 --period 0` |
| `spi_2MHz.sr`, `spi_4MHz.sr`, `spi_6MHz.sr` | 2026-10-20 | Frames at 2, 4 and 6 MHz requested (measured 2.000, 3.997, 5.88 MHz) | 20 MHz (2 MHz), 60 MHz (4 and 6 MHz); Pi: `poll --count 3000 --period 0.002 --speed ...` |

What they show is in ADR-0006 (clock), ADR-0007 and ADR-0009 (I2C and the BMP280 read) and
ADR-0010 (the SPI link). The `.pvs` files are the PulseView sessions that go with each `.sr`.
