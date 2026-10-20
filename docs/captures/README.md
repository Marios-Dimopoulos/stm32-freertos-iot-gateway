# Logic analyzer captures

Captured with a Raspberry Pi Pico running the sigrok-pico firmware (`pico_baseline.uf2`),
read in PulseView (nightly build; 0.4.2 does not support this firmware). Each `.sr` file is
a zip with the samples; the `.pvs` file next to it is the PulseView session (channels,
decoders, view). Open the `.sr` in PulseView, or read the samples with any zip tool.

Input protection: each probe wire goes through a 1 kOhm resistor, as the sigrok-pico user
guide recommends (inputs outside 0 to 3.3 V can damage the Pico). The Pico's GND is joined
to the Nucleo's GND.

| File | Date | What | Settings |
|---|---|---|---|
| `phase1-terminal-output.png` | Phase 1 | Tera Term output of the sensor task | not a capture |
| `i2c_bmp280_1MHz_5s.sr` | 2026-10-20 | Five BMP280 reads, one per second, no Pi connected | D2 = SCL (PB8), D3 = SDA (PB9); 1 MHz; 5 M samples (5 s); no trigger |
| `i2c_bmp280_5MHz_1transaction.sr` | 2026-10-20 | One BMP280 read in detail | same channels; 5 MHz; 500 k samples (100 ms); trigger: falling edge on D3; 10% pre-capture |

What they show is in ADR-0006 (clock), ADR-0007 and ADR-0009 (I2C and the BMP280 read).
