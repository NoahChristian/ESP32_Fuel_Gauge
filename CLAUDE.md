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

Single-threaded `loop()`, but unlike the sibling power-controller project
this one **does block**, deliberately, to drive LED sweep animations:

1. If `f_SoC` changed since the last redraw, recompute how many of the 40
   LEDs should be lit (`blue = 40 * f_SoC / 100`) and redraw: unlit
   portion `DarkRed`, lit portion `MidnightBlue`.
2. If `currentState == 1` (set by `"Charging"`), run a ~1.6s blocking
   forward sweep (LED 0→39, `Yellow`, 20ms per LED) — matches the
   README's "charging = yellow, sweeps upward."
3. If `currentState == -1` (set by `"Discharging"`), run the same sweep
   backward (39→0, `Amethyst`) — README's "discharging = purple, sweeps
   downward."
4. `currentState == 0` ("Idle") runs no animation — matches the README's
   "no activity, no animation."
5. A trailing `while` loop polls MQTT for ~500ms before the next `loop()`
   iteration.
6. If `f_bright` changed, update `FastLED.setBrightness()`.

Net effect: whenever the battery is actively charging or discharging (the
common case, not an edge case, in a solar+battery+grid-arbitrage setup),
each `loop()` pass takes roughly 1.6s (animation) + 500ms (poll) ≈ up to
~2.1s; `mqttClient.poll()` is only called during the trailing 500ms
window, so incoming MQTT messages can sit unprocessed for up to ~1.6s
mid-animation. Not long enough to trip a typical broker's keepalive on
its own, but worth knowing if this device ever seems slow to reflect a
fresh Home Assistant update.

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
