# AudioBus FD16 — Agent Instructions

AudioBus FD16 is a proprietary real-time full-duplex point-to-point audio transport for ESP32-P4, implemented as an **ESP-IDF project** (top-level project, not a standalone component). It transports 16 audio channels in each direction (32-bit signed samples, 48 kHz) over LVDS using a fixed 72-byte frame per sample period at 27.648 Mb/s NRZ, with no 8b10b, no CDR, no turnaround, and no discovery/slot-map negotiation.

## Architecture

```
AudioBus/                    # Top-level ESP-IDF project (target esp32p4)
├── CMakeLists.txt           # Project root
├── main/
│   ├── CMakeLists.txt       # App component (REQUIRES audiobus)
│   └── main.c               # FD16 node example (uses menuconfig pins)
├── components/audiobus/
│   ├── CMakeLists.txt       # FD16-only component
│   ├── Kconfig              # FD16 + GPIO pin configuration
│   ├── include/
│   │   ├── fd16_frame.h     # Frame constants + pack/unpack + CRC-16-CCITT
│   │   ├── audiobus_fd16.h  # Dedicated FD16 profile API (init/start/stop/audio/stats)
│   │   ├── audiobus_phy.h   # PHY vtable (abus_phy_ops_t) + abus_phy_fd16_create()
│   │   └── audiobus_types.h # Types + ABUS_PHY_LVDS_FD16
│   └── src/
│       ├── fd16_frame.c     # CRC-16-CCITT + big-endian serialization
│       ├── audiobus_fd16.c  # FD16 core: ring buffers, frame task, concealment
│       └── phy/
│           └── phy_lvds_fd16.c  # SN65LVDS049 PARLIO 1-bit TX/RX PHY
├── tests/fd16/              # Host unit tests + link test notes
└── docs/FD16.md             # Hardware assumptions, cable map, validation strategy
```

**Key physical constants** (in `fd16_frame.h`):
- `ABUS_FD16_CHANNELS = 16`, `ABUS_FD16_FRAME_BYTES = 72`, `ABUS_FD16_FRAME_BITS = 576`
- `ABUS_FD16_BITRATE = 27_648_000`, `ABUS_FD16_CLOCK_HZ = 27_648_000`, `ABUS_FD16_SAMPLE_RATE = 48_000`
- Invariant (compile-time `_Static_assert`): `27_648_000 / 576 = 48_000`

## Build & Flash

```bash
source $IDF_PATH/export.sh
idf.py set-target esp32p4
idf.py menuconfig            # → AudioBus FD16 Configuration → set GPIO pins
idf.py build
idf.py flash monitor
```

**Component dependencies** (declared in `components/audiobus/CMakeLists.txt`):
`esp_driver_parlio`, `esp_driver_gpio`, `freertos`, `esp_common`, `log`, `heap`

## Coding Conventions

### Naming
- All public symbols: `abus_` prefix (e.g., `abus_fd16_init`, `abus_fd16_stats_t`)
- FD16 frame module: `abus_fd16_` prefix (e.g., `abus_fd16_pack`, `abus_fd16_crc16`)
- PHY layer: `abus_phy_` prefix (e.g., `abus_phy_fd16_create`)
- Log tags: `static const char *TAG = "abus_fd16";` (per-file variant: `"abus_phy_fd16"`)

### Error handling
- All public functions return `esp_err_t`
- Use ESP-IDF macros: `ESP_RETURN_ON_FALSE`, `ESP_RETURN_ON_ERROR`, `ESP_GOTO_ON_ERROR`
- Always validate pointer arguments at function entry:
  `ESP_RETURN_ON_FALSE(ptr != NULL, ESP_ERR_INVALID_ARG, TAG, "ptr is NULL")`

### Memory allocation
- Internal buffers: `heap_caps_calloc(n, size, MALLOC_CAP_INTERNAL)`
- DMA buffers: add `MALLOC_CAP_DMA` flag (4-byte aligned, internal SRAM)
- **No heap allocation in ISR context.** ISR callbacks only copy to a static buffer, invoke the upper-layer callback, and signal a semaphore.

### Audio data
- **Always `int32_t` internally.** Wire serialization is big-endian (MSB first), done exclusively in `fd16_frame.c` (`abus_fd16_pack` / `abus_fd16_unpack`). Never memcpy native int32_t directly onto the wire.

### ISR safety
- PHY RX callbacks (ISR context) must only: copy the 72-byte frame to a static buffer, call the upper-layer callback, and signal a semaphore. All frame processing happens in the frame task.
- Frame task: `xTaskCreatePinnedToCore(..., core=1)` at `configMAX_PRIORITIES - 2`.
- RX re-queue: `parlio_rx_unit_receive()` is **not ISR-safe** (takes a mutex). A dedicated re-queue task re-arms the finished ping-pong buffer.

### PHY abstraction
- The FD16 PHY implements the `abus_phy_ops_t` vtable (see `audiobus_phy.h`).
- `set_direction` and `get_recovered_clock` are no-op stubs (full-duplex, no CDR).
- PHY uses ping-pong double buffering for DMA TX/RX (2 × 72-byte buffers each).

### Unused pins
- Assign `-1` for pins not used in a given configuration (e.g., `i2c_sda = -1` on slave nodes without Si5351A).

## Kconfig Options

| Symbol | Default | Notes |
|--------|---------|-------|
| `ABUS_FD16_ENABLE` | y | Enable FD16 profile |
| `ABUS_FD16_PIN_ROLE` | -1 | Role-select GPIO (1=master A, 0=slave B), read at startup |
| `ABUS_FD16_AUDIO_BUF_FRAMES` | 8 | Ring buffer depth, range 4–64 |
| `ABUS_FD16_CONCEALMENT_THRESHOLD` | 64 | Consecutive bad frames before mute, 0=never |
| `ABUS_FD16_PIN_TX_DATA` | -1 | PARLIO TX data[0] → SN65LVDS049 DIN1 |
| `ABUS_FD16_PIN_RX_DATA` | -1 | SN65LVDS049 ROUT1 → PARLIO RX data[0] |
| `ABUS_FD16_PIN_TX_CLK` | -1 | PARLIO TX clock output (optional) |
| `ABUS_FD16_PIN_CLK_IN` | -1 | 27.648 MHz external clock (TX+RX) |
| `ABUS_FD16_PIN_CLK_OUT` | -1 | SN65LVDS049 DIN2 (clock driver, master only) |
| `ABUS_FD16_PIN_I2C_SDA` | -1 | Si5351A I2C SDA (master only) |
| `ABUS_FD16_PIN_I2C_SCL` | -1 | Si5351A I2C SCL (master only) |

## Common Pitfalls

- **Audio always `int32_t`**: do not add format conversion outside `fd16_frame.c`.
- **27.648 MHz / 576 = 48 kHz invariant**: do not change frame length, line rate, or sample rate independently.
- **`parlio_rx_unit_receive()` is not ISR-safe**: never call it from an ISR; use the re-queue task pattern.
- **ISR budget**: the PHY RX ISR has a hard deadline of one frame period (~20.8 µs @ 48 kHz). Do not add non-ISR-safe operations inside `set_rx_callback` handlers.
- **`MALLOC_CAP_DMA` alignment**: ESP32-P4 DMA buffers must be 4-byte aligned and in internal SRAM.
- **No Si5351A driver exists**: the PHY reports the 27.648 MHz invariant but does not program the Si5351A; this is a hardware-integration task.
- **50 m is a validation target, not a guarantee**: TI does not guarantee the SN65LVDS049 at 50 m; validate with PRBS31/BERT (see `docs/FD16.md`).

## Design Brief

The full implementation brief is in [`docs/AudioBus_FD16_Cline_Implementation_Brief.docx`](docs/AudioBus_FD16_Cline_Implementation_Brief.docx).