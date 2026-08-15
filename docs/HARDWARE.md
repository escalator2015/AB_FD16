# AudioBus FD16 — Hardware Reference

Connection diagrams for the SN65LVDS049 and Si5351A with the ESP32-P4, plus
the cable/termination strategy for the 50 m Cat6A link.

---

## 1. Topology overview

FD16 is a **point-to-point** link between exactly two nodes, **A** and **B**.
Both nodes are electrically identical ESP32-P4 + SN65LVDS049 boards; the only
difference is the **clock source** (see §4).

```
                    CAT6A (4 twisted pairs, 50 m target)
   Node A (master)  ─────────────────────────────────────  Node B (slave)
   +----------------+                                      +----------------+
   | ESP32-P4       |                                      | ESP32-P4       |
   |                |  Pair 1: DATA A→B  ────────────────► |                |
   | PARLIO TX ─────┼──► SN65LVDS049 DIN1 ──► DOUT1 ──────┼──► RIN1 ──► ROUT1 ──► PARLIO RX
   | PARLIO RX ◄────┼──◄ SN65LVDS049 ROUT1 ◄── RIN1 ◄─────┼──◄ DOUT1 ◄── DIN1 ◄── PARLIO TX
   |                |  Pair 2: DATA B→A  ◄──────────────── |                |
   |                |                                      |                |
   | Si5351A ───────┼──► 27.648 MHz                        |                |
   |  (clock src)   |     │                                |                |
   |                |     ├──► PARLIO clk_in (TX+RX)       |                |
   |                |     └──► SN65LVDS049 DIN2            |                |
   |                |  Pair 3: CLOCK A→B  ────────────────► |                |
   |                |          DOUT2 ──────────────────────┼──► RIN2 ──► ROUT2 ──► PARLIO clk_in (TX+RX)
   |                |                                      |                |
   |                |  Pair 4: REF / shield                |                |
   +----------------+                                      +----------------+
```

**Key point:** Node B has **no local clock source**. It receives the 27.648 MHz
clock from A over Pair 3 and uses it as the external PARLIO clock for both its
TX and RX. This guarantees zero frequency offset between the two endpoints.

---

## 2. Node A (master / clock source) — connection diagram

```
ESP32-P4 (Node A)                          SN65LVDS049 (U1)
┌──────────────────────┐                   ┌──────────────────────┐
│                      │                   │                      │
│  GPIO (PARLIO        │                   │  DIN1 ◄── PARLIO TX data[0]
│   TX data[0]) ───────┼───────────────────┼──► DOUT1 ──► Pair 1 (DATA A→B)
│                      │                   │                      │
│  GPIO (PARLIO        │                   │  ROUT1 ──► PARLIO RX data[0]
│   RX data[0]) ◄──────┼───────────────────┼──◄ RIN1 ◄── Pair 2 (DATA B→A)
│                      │                   │                      │
│  GPIO (PARLIO        │                   │  DIN2 ◄── 27.648 MHz (from Si5351A)
│   clk_in) ◄──────────┼── 27.648 MHz ─────┼──► DOUT2 ──► Pair 3 (CLOCK A→B)
│                      │                   │                      │
│  GPIO (PARLIO        │                   │  ROUT2 (unused on A)  │
│   clk_out, optional) ┼──► (unused)       │  RIN2  (unused on A)  │
│                      │                   │                      │
│  I2C SDA ────────────┼──► Si5351A SDA    │                      │
│  I2C SCL ────────────┼──► Si5351A SCL    │                      │
└──────────────────────┘                   └──────────────────────┘
        │
        ▼
   Si5351A (U2) — 27.648 MHz clock generator
   ┌──────────────────────┐
   │  CLK0 ──► 27.648 MHz │──► PARLIO clk_in (ESP32-P4)
   │                      │──► SN65LVDS049 DIN2 (clock driver)
   │  SDA ◄── ESP32-P4    │
   │  SCL ◄── ESP32-P4    │
   │  VDD = 3.3 V         │
   │  XTAL = 25 MHz       │
   └──────────────────────┘
```

**Node A pin summary (via menuconfig):**

| Kconfig symbol | ESP32-P4 GPIO | Connected to |
|----------------|---------------|--------------|
| `ABUS_FD16_PIN_ROLE` | GPIO _r_ | Pull-up to 3.3 V (HIGH = master) |
| `ABUS_FD16_PIN_TX_DATA` | GPIO _x_ | SN65LVDS049 DIN1 |
| `ABUS_FD16_PIN_RX_DATA` | GPIO _y_ | SN65LVDS049 ROUT1 |
| `ABUS_FD16_PIN_CLK_IN` | GPIO _z_ | Si5351A CLK0 (27.648 MHz) |
| `ABUS_FD16_PIN_CLK_OUT` | GPIO _w_ | SN65LVDS049 DIN2 (clock driver) |
| `ABUS_FD16_PIN_I2C_SDA` | GPIO _s_ | Si5351A SDA |
| `ABUS_FD16_PIN_I2C_SCL` | GPIO _c_ | Si5351A SCL |
| `ABUS_FD16_PIN_TX_CLK` | -1 (optional) | PARLIO TX clock output (unused) |

---

## 3. Node B (slave / clock receiver) — connection diagram

```
ESP32-P4 (Node B)                          SN65LVDS049 (U1)
┌──────────────────────┐                   ┌──────────────────────┐
│                      │                   │                      │
│  GPIO (PARLIO        │                   │  DIN1 ◄── PARLIO TX data[0]
│   TX data[0]) ───────┼───────────────────┼──► DOUT1 ──► Pair 2 (DATA B→A)
│                      │                   │                      │
│  GPIO (PARLIO        │                   │  ROUT1 ──► PARLIO RX data[0]
│   RX data[0]) ◄──────┼───────────────────┼──◄ RIN1 ◄── Pair 1 (DATA A→B)
│                      │                   │                      │
│  GPIO (PARLIO        │                   │  ROUT2 ──► 27.648 MHz (received clock)
│   clk_in) ◄──────────┼── 27.648 MHz ─────┼──◄ RIN2 ◄── Pair 3 (CLOCK A→B)
│                      │                   │                      │
│  GPIO (PARLIO        │                   │  DIN2 (unused on B)   │
│   clk_out, optional) ┼──► (unused)       │  DOUT2 (unused on B)  │
│                      │                   │                      │
│  (no Si5351A!)       │                   │                      │
└──────────────────────┘                   └──────────────────────┘
```

**Node B pin summary (via menuconfig):**

| Kconfig symbol | ESP32-P4 GPIO | Connected to |
|----------------|---------------|--------------|
| `ABUS_FD16_PIN_ROLE` | GPIO _r_ | Pull-down to GND (LOW = slave) |
| `ABUS_FD16_PIN_TX_DATA` | GPIO _x_ | SN65LVDS049 DIN1 |
| `ABUS_FD16_PIN_RX_DATA` | GPIO _y_ | SN65LVDS049 ROUT1 |
| `ABUS_FD16_PIN_CLK_IN` | GPIO _z_ | SN65LVDS049 ROUT2 (received 27.648 MHz) |
| `ABUS_FD16_PIN_CLK_OUT` | -1 | Not used on B |
| `ABUS_FD16_PIN_I2C_SDA` | -1 | Not used on B |
| `ABUS_FD16_PIN_I2C_SCL` | -1 | Not used on B |
| `ABUS_FD16_PIN_TX_CLK` | -1 (optional) | PARLIO TX clock output (unused) |

---

## 4. Si5351A — ¿en todos los nodos o en uno solo?

**Solo en el nodo A (master).** El nodo B **no** lleva Si5351A.

Razón: el diseño FD16 elimina deliberadamente la necesidad de un reloj local en
B. A genera el reloj de transporte de 27.648 MHz con el Si5351A, lo envía por
el **Par 3** del cable Cat6A como reloj LVDS dedicado, y B lo recibe y lo usa
directamente como reloj externo de su PARLIO (TX y RX). Esto:

- Elimina la deriva de frecuencia entre dos cristales independientes (mismatch).
- Elimina la necesidad de CDR (recuperación de reloj desde los datos).
- Garantiza que TX y RX de B estén enganchados en fase a A con offset cero.

Si ambos nodos llevaran Si5351A, habría que sincronizarlos (PLL software, etc.),
lo que reintroduce complejidad que FD16 evita por diseño.

---

## 5. ¿Hay distinción Master/Slave en FD16?

**Sí, pero solo en el reloj.** No hay distinción en el flujo de datos.

| Aspecto | Master (A) | Slave (B) |
|---------|-----------|-----------|
| Reloj de transporte | Genera 27.648 MHz (Si5351A) | Recibe 27.648 MHz (Par 3) |
| Si5351A | Sí | No |
| PARLIO TX | 16 canales → B | 16 canales → A |
| PARLIO RX | 16 canales ← B | 16 canales ← A |
| Dirección switching | No (full-duplex) | No (full-duplex) |
| Discovery / slot-map | No | No |
| 8b10b | No | No |
| CDR | No | No |

Ambos nodos ejecutan el **mismo** código de datos: transmiten 16 canales y
reciben 16 canales simultáneamente. La única diferencia es el **estado del pin
de selección de rol** (`ABUS_FD16_PIN_ROLE`) leído al arrancar:

- **A (master):** pin de rol = **HIGH (1)** → con Si5351A y `clk_out` asignado.
- **B (slave):** pin de rol = **LOW (0)** → sin Si5351A, `clk_in` conectado a ROUT2.

El mismo firmware se flashea en ambos nodos; el rol se decide por hardware
(puente/jumper o pull-up/pull-down en el pin de rol), no por menuconfig.

---

## 6. Cable pair assignment (Cat6A, 4 pairs)

| Pair | Signal | Direction | Notes |
|------|--------|-----------|-------|
| 1 | DATA A→B | A to B | 100 Ω LVDS, receiver termination at B |
| 2 | DATA B→A | B to A | 100 Ω LVDS, receiver termination at A |
| 3 | CLOCK A→B | A to B | 27.648 MHz LVDS, receiver termination at B |
| 4 | REF / shield | — | No power; shield/chassis/0 V strategy defined by system |

---

## 7. Termination

- **100 Ω receiver termination** across each LVDS differential pair, placed
  physically close to the receiving SN65LVDS049 pins.
- Pair 1 (DATA A→B): termination at **B**.
- Pair 2 (DATA B→A): termination at **A**.
- Pair 3 (CLOCK A→B): termination at **B**.

---

## 8. Power and grounding

- SN65LVDS049: 3.3 V supply.
- Si5351A: 3.3 V supply, 25 MHz crystal (or 27 MHz — verify PLL config).
- The Cat6A shield is **not** a DC common-mode reference. Verify the A/B ground
  offset in the actual power architecture; the LVDS receiver common-mode range
  is finite.
- Use low-capacitance ESD protection at the external cable interface only after
  checking its effect on the eye opening.

---

## 9. PCB layout guidelines

- 100 Ω controlled-impedance differential routing for all LVDS paths.
- Keep PCB differential pairs short, symmetric, referenced to a continuous ground.
- Minimize vias and stubs between the SN65LVDS049 and the cable connector.
- Place the 100 Ω receiver termination close to the receiving SN65LVDS049 pins.

---

## 10. Distance disclaimer

50 m at 27.648 Mb/s is a **validation target**, not a guarantee. TI specifies
the SN65LVDS049 at up to 400 Mbps point-to-point over ~100 Ω media, but notes
ultimate distance depends on attenuation, coupled noise, and application
conditions. Validate with PRBS31/BERT (see `docs/FD16.md` § "Link validation
strategy").