# ESP32 Fuel Gauge

## Purpose

Firmware for an ESP32 that drives a 40-LED WS2812 strip as a physical
battery state-of-charge (SoC) gauge for a home battery system. It has no
sensors of its own — it's a pure MQTT-to-LED display driven by Home
Assistant, which is the actual source of battery SoC/charging-state data
(likely from a BMS integration). This is a companion device to the
`Giga_YH_Controller_LVGL` power controller in the same home-automation
setup (same author, same MQTT broker, same `V1.0/Home/...` topic
convention, same `arduino_secrets.h` credential pattern) but is otherwise
independent — it doesn't control anything, only displays.

Single-file Arduino sketch (`ESP32_Fuel_Gauge.ino`, ~317 lines). No build
system, package manifest, or test suite — the `.ino` is the entire
codebase.

## Hardware / topology

- **MCU**: ESP32 (any WROOM-class board with `WiFi.h`/station mode).
- **LED strip**: 40x WS2812 (`FastLED`), single data line on pin 33
  (`DATA_PIN`), 5V powered, no level shifting (3.3V data is sufficient
  for WS2812).
- **MQTT** (via `ArduinoMqttClient`) to the same Home Assistant broker
  used by `Giga_YH_Controller_LVGL`. Subscribe-only — this device never
  publishes.
- Credentials (`SECRET_SSID`, `SECRET_PASS`, `HOME_ASSISTANT_IP`,
  `MQTT_USERNAME`, `MQTT_PASSWORD`) live in a gitignored
  `arduino_secrets.h`, same convention as the sibling project.

## MQTT topics (subscribe-only)

| Topic | Payload | Meaning |
|---|---|---|
| `V1.0/Home/Battery/SoC` | float string, 0–100 | State of charge percentage |
| `V1.0/Home/Battery/Action` | `"Charging"` / `"Discharging"` / `"Idle"` | Current battery action |
| `V1.0/Home/Battery/Brightness` | float string, 0–100 | LED strip brightness |

**These topics are a reusable data source.** Anything else on the same
broker (e.g. `Giga_YH_Controller_LVGL`'s planned display work) can
subscribe to `Battery/SoC` and `Battery/Action` directly rather than
needing a new sensor — this device's existence is the proof that real
battery SoC/charging-state data is already flowing through MQTT.

## Runtime model

Single-threaded `loop()`, fully non-blocking as of the 2026-07-13
animation rewrite (see below) — no `delay()` longer than 1ms anywhere in
the steady-state loop:

1. If `f_SoC` changed since the last redraw, recompute how many of the
   `NUM_LEDS` LEDs should be lit and redraw: unlit portion `DarkRed`, lit
   portion `MidnightBlue` (via `baseColorFor(i)`), skipping whichever LED
   is currently mid-highlight from the sweep animation below so it isn't
   stomped.
2. If `currentState == 1` (`"Charging"`) or `== -1` (`"Discharging"`), a
   small state machine (`anim_i`/`anim_step`/`anim_last_ms`) advances the
   sweep by one phase (light the current LED, or restore it and move to
   the next) every `ANIM_STEP_MS` (20ms) — matches the original's
   per-LED timing exactly, just non-blocking. Forward (0→`NUM_LEDS`-1,
   `Yellow`) while charging, backward (`NUM_LEDS`-1→0, `Amethyst`) while
   discharging, matching the README. A direction flip or a transition to
   Idle mid-sweep restores whatever LED was highlighted before resetting.
3. `mqttClient.poll()` runs every single `loop()` pass, unconditionally.
4. If `f_bright` changed, update `FastLED.setBrightness()`.
5. A 1ms `delay()` at the end of `loop()` is the only throttling — purely
   a yield, not a rate limiter.

Previously (pre-2026-07-13) the charge/discharge sweep used nested
`delay(20)` calls and blocked `loop()` for up to ~1.6s per full sweep,
during which `mqttClient.poll()` never ran — since charging/discharging
is the common case in a solar+battery+grid-arbitrage setup, not an edge
case, MQTT updates could sit unprocessed for the majority of the device's
runtime. The rewrite produces the same visual animation with `loop()`
returning every pass instead.

## Known issues found and fixed here

- **`f_SoC` was overloaded with two different units.** It's set from
  MQTT as a 0–100 percentage in `onMqttMessage`, but `loop()` used to
  overwrite it with `blue` (a 0–40 *LED count*) right after each redraw
  (`f_SoC = (float) blue;`). This happened to self-correct on the next
  real MQTT update (which resets it back to a true percentage), so there
  was no visible symptom — but any future code reading `f_SoC` expecting
  a percentage (a "low battery" threshold check, a debug print, a
  republish) would silently see an LED-count value instead most of the
  time. Fixed by tracking the last-drawn LED count in its own variable
  and leaving `f_SoC` alone.
- **`onMqttMessage`'s `tbuf[256]` fill loops had no bounds check** on any
  of the three subscribed topics — the exact same unbounded-copy pattern
  (down to the identical `//TODO` comment) found and fixed in the sibling
  `Giga_YH_Controller_LVGL` project. Fixed the same way: bound the copy
  loop and drain any excess bytes so an oversized message can't overflow
  the buffer or desync the next one.
- **Inline comments on the charging/discharging branches were swapped**
  relative to the actual `currentState` assignment (`if (currentState ==
  1){ //discharging` when `1` is actually set by `"Charging"`, and vice
  versa). The animation *behavior* was already correct and matched the
  README — only the comments were backwards. Fixed the comments; no
  behavior change.
- Dropped a handful of declared-but-unused globals left over from
  prototyping (`interval`/`previousMillis`, `count`, `hue`, `lastMillis`)
  and dead commented-out lines — same style of cleanup done on the
  sibling project, and likely from the same original boilerplate given
  the identical unused-variable names.
- **Critical (found in a 2026-07-13 security pass): out-of-bounds write
  to `leds[]` from an unclamped `Battery/SoC` value.** `f_SoC` was set
  straight from the MQTT payload with no range check, and `lit_leds`
  (`= 40 * f_SoC/100`) was used as a loop bound with no check against
  `NUM_LEDS` either. A single published value outside 0–100 (e.g. `"1000"`
  → `lit_leds` wraps to `144` after the `uint8_t` cast) made the fill
  loop write up to 104 elements past the end of the real 40-element
  `leds[]` array — memory corruption triggerable by one MQTT message, no
  authentication beyond the broker connection itself. Fixed with clamps
  at both the trust boundary (`f_SoC` constrained to `[0,100]` in
  `onMqttMessage`) and the point of use (`lit_leds` constrained to
  `[0,NUM_LEDS]` in `loop()`, defense in depth in case the first clamp is
  ever bypassed by a future edit).
- The `lit_leds` calculation also hardcoded a literal `40` instead of
  referencing `NUM_LEDS` — harmless today since they match, but would
  silently go stale if `NUM_LEDS` (and the physical strip length) is ever
  changed without updating that literal too. Confirmed the strip really
  is 40 LEDs (matches `NUM_LEDS` and the README), so `NUM_LEDS` itself
  wasn't wrong — just made the math derive from it instead of duplicating
  the value, so changing `NUM_LEDS` alone is enough to support a
  different strip length in the future.

## Known rough edges still open (not fixed here)

- **No WiFi/MQTT reconnect logic.** `connectToWiFi()` tries for
  `WIFI_TIMEOUT` (15s), and `setup()` proceeds regardless of the result
  straight into the MQTT connect attempt, which will fail without WiFi
  and hang forever (`while (1);`). If the connection drops after
  `setup()` completes successfully, nothing in `loop()` detects or
  recovers from it. Same class of gap as `Giga_YH_Controller_LVGL`.
- **String comparisons in `onMqttMessage` are case- and
  whitespace-sensitive** (`strcmp(tbuf, "Discharging")` etc.) — an
  unexpected casing or trailing character from the HA side would silently
  fail to match and leave `currentState` stale rather than erroring
  visibly.
- **No bounds clamping on `f_bright`** before `FastLED.setBrightness((uint8_t) f_bright*2.55)`
  — relies on `FastLED`'s own internal clamping for out-of-range input
  rather than validating at the boundary.
