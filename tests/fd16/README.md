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

## Regression (legacy builds)

```bash
source $IDF_PATH/export.sh
idf.py -C examples/master_node set-target esp32p4
idf.py -C examples/master_node build
idf.py -C examples/slave_node set-target esp32p4
idf.py -C examples/slave_node build
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

Then run `examples/fd16_node`; `rx_frames` should track `tx_frames` and
`crc_errors`/`sync_errors` remain zero.