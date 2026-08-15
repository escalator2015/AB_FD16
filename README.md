# AudioBus FD16

Full-duplex fixed point-to-point audio transport for **ESP32-P4** (ESP-IDF v5.5.4).

| Parameter | Value |
|-----------|-------|
| Audio IN / OUT per node | 16 channels each direction |
| Sample rate | 48,000 Hz |
| Sample format | signed 32-bit |
| Transport frame | 72 bytes / sample period |
| Line rate | 27.648 Mb/s NRZ |
| Transport clock | 27.648 MHz, free-running |
| Coding | NRZ (no 8b10b) |
| Duplex | Full duplex (simultaneous TX/RX) |
| PHY | 1 × SN65LVDS049 per endpoint |
| Cable | Cat6A, 4 twisted pairs |
| Target distance | 50 m |

**Design invariant:** `27,648,000 / 576 = 48,000 Hz` exactly (enforced at compile time).

## Features

- 16 audio channels in each direction, 32-bit signed samples, 48 kHz
- Fixed 72-byte frame per sample period, no discovery/slot-map negotiation
- CRC-16-CCITT + sequence counter + repeat-last-sample error concealment
- No 8b10b, no CDR, no turnaround, no half-duplex direction switching
- A is the sole transport-clock master; B receives the dedicated LVDS clock
- GPIO pins configurable via `menuconfig`

## Quick start

```bash
source $IDF_PATH/export.sh
idf.py set-target esp32p4
idf.py menuconfig            # → AudioBus FD16 Configuration → set GPIO pins
idf.py build
idf.py flash monitor
```

## Project layout

```
AudioBus/
├── CMakeLists.txt           # Project root (target esp32p4)
├── main/
│   ├── CMakeLists.txt       # App component (REQUIRES audiobus)
│   └── main.c               # FD16 node example (uses menuconfig pins)
├── components/audiobus/
│   ├── CMakeLists.txt       # FD16-only component
│   ├── Kconfig              # FD16 + GPIO pin configuration
│   ├── include/
│   │   ├── fd16_frame.h     # Frame constants + pack/unpack + CRC-16-CCITT
│   │   ├── audiobus_fd16.h  # Dedicated FD16 profile API
│   │   ├── audiobus_phy.h   # PHY vtable + abus_phy_fd16_create()
│   │   └── audiobus_types.h # Types + ABUS_PHY_LVDS_FD16
│   └── src/
│       ├── fd16_frame.c     # CRC-16-CCITT + big-endian serialization
│       ├── audiobus_fd16.c  # FD16 core: ring buffers, frame task, concealment
│       └── phy/
│           └── phy_lvds_fd16.c  # SN65LVDS049 PARLIO 1-bit TX/RX PHY
├── tests/fd16/              # Host unit tests + link test notes
└── docs/FD16.md             # Hardware assumptions, cable map, validation strategy
```

## Frame format (72 bytes)

| Offset | Size | Field | Value |
|--------|------|-------|-------|
| 0 | 1 | SYNC0 | 0xA5 |
| 1 | 1 | SYNC1 | 0x5A |
| 2 | 2 | SEQUENCE | uint16_t big-endian, increments every frame |
| 4 | 1 | FLAGS | 0x22 (48 kHz / 32-bit / FD16) |
| 5 | 1 | CHANNELS | 16 |
| 6 | 64 | AUDIO | 16 × int32_t, big-endian (MSB first) |
| 70 | 2 | CRC16 | CRC-16-CCITT over bytes 0..69 |

## GPIO configuration

```bash
idf.py menuconfig
# → "AudioBus FD16 Configuration" → "FD16 GPIO Pin Configuration"
```

| Kconfig symbol | Signal | SN65LVDS049 |
|----------------|--------|-------------|
| `ABUS_FD16_PIN_ROLE` | Role-select GPIO (1=master A, 0=slave B) | — |
| `ABUS_FD16_PIN_TX_DATA` | PARLIO TX data[0] | DIN1 |
| `ABUS_FD16_PIN_RX_DATA` | PARLIO RX data[0] | ROUT1 |
| `ABUS_FD16_PIN_TX_CLK` | PARLIO TX clock output (optional) | — |
| `ABUS_FD16_PIN_CLK_IN` | 27.648 MHz external clock (TX+RX) | DIN2 (master) / ROUT2 (slave) |
| `ABUS_FD16_PIN_CLK_OUT` | Clock driver input (master only) | DIN2 |
| `ABUS_FD16_PIN_I2C_SDA` | Si5351A I2C SDA (master only) | — |
| `ABUS_FD16_PIN_I2C_SCL` | Si5351A I2C SCL (master only) | — |

Also configurable: `ABUS_FD16_AUDIO_BUF_FRAMES`, `ABUS_FD16_CONCEALMENT_THRESHOLD`.

**Role selection:** the node role is read from the `ABUS_FD16_PIN_ROLE` GPIO at
startup (HIGH = master A, LOW = slave B). The same firmware runs on both nodes;
a jumper/pull-up/pull-down on the role pin decides the role. If the pin is `-1`,
the node defaults to master (A).

## Tests

```bash
cd tests/fd16
make run
```

Covers CRC-16-CCITT known vectors, frame-size invariant, round-trip pack/unpack,
sequence wrap, corruption rejection, and big-endian wire order.

## Documentation

- [`docs/FD16.md`](docs/FD16.md) — hardware assumptions, termination, cable pair
  assignment, clock tree, and the 50 m link validation strategy.
- [`docs/HARDWARE.md`](docs/HARDWARE.md) — connection diagrams for SN65LVDS049
  and Si5351A with the ESP32-P4, Master/Slave clock distinction.
- [`docs/AudioBus_FD16_Cline_Implementation_Brief.docx`](docs/AudioBus_FD16_Cline_Implementation_Brief.docx) — original implementation brief.

## License

MIT