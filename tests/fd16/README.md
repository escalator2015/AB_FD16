# AudioBus FD16 Tests

## Unit tests (host-based)

```bash
cd tests/fd16
make run
```

Covers:
- CRC-16-CCITT known vector (0x29B1 for "123456789")
- Frame size invariant (72 bytes = 576 bits; 27.648 MHz / 576 = 48 kHz)
- Round-trip pack/unpack for INT32_MIN/MAX, zero, alternating, random
- Sequence wrap 0xFFFF → 0x0000
- One-bit and multi-bit corruption → CRC rejection
- SYNC pattern validation
- Wire byte order (big-endian deterministic)

### Si5351A parameter tests (`test_si5351a`)

Covers:
- 27.648 MHz with 25 MHz crystal → correct PLL/output multisynth params
- 27.648 MHz with 27 MHz crystal → correct params
- VCO stays in [600, 900] MHz for various frequencies
- Invalid arguments rejected (zero freq, null out, out-of-range divider)
- Output divider within multisynth range

## Build (root project)

```bash
source $IDF_PATH/export.sh
idf.py set-target esp32p4
idf.py menuconfig            # → AudioBus FD16 Configuration → set GPIO pins
idf.py build
```

## Integration / link tests (requires hardware)

Two-node PRBS31/BERT procedure and the 50 m validation matrix are documented
in `docs/FD16.md` § "Link validation strategy". The link tests are not
runnable without two ESP32-P4 boards + SN65LVDS049 + Cat6A cabling.

### Loopback smoke test (single board)

For a basic single-board loopback, connect:

- PARLIO TX data[0] → SN65LVDS049 DIN1
- SN65LVDS049 DOUT1 → (100 Ω terminated) → SN65LVDS049 RIN1
- SN65LVDS049 ROUT1 → PARLIO RX data[0]
- 27.648 MHz → SN65LVDS049 DIN2 AND PARLIO clk_in

Then run the root project; `rx_frames` should track `tx_frames` and
`crc_errors`/`sync_errors` remain zero.
