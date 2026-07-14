# Sprinkler Controller Pinout

## Active Relay Outputs

The Dingtian Device Builder example configures these eight relay outputs. The scheduler can
accept 1-32 outputs; additional hardware and mappings are installer-specific.
They are internal ESPHome switches, not exposed as raw Home Assistant controls.
The table documents the supplied single-register Dingtian board only.

| Zone | ESPHome relay id | Dingtian output pin |
| --- | --- | --- |
| 1 | `relay_1` | 7 |
| 2 | `relay_2` | 6 |
| 3 | `relay_3` | 5 |
| 4 | `relay_4` | 4 |
| 5 | `relay_5` | 3 |
| 6 | `relay_6` | 2 |
| 7 | `relay_7` | 1 |
| 8 | `relay_8` | 0 |

## Expansion

`dtr0xx_io.sr_count` declares how many compatible eight-bit registers are
physically chained: 1, 2, 3, or 4 can address up to 8, 16, 24, or 32 outputs.
Increasing the value does not add physical relays. Define every additional
internal relay switch and append its matching `dynamic_sprinkler.zones` entry
in the main YAML. Other ESPHome switch platforms can be used instead of the
Dingtian driver without changing scheduler code.

## Documented Former Inputs

The controller previously exposed these as diagnostic GPIO binary sensors. They
are not active in the current ESPHome config. The mapping is kept here so the
wiring is not lost.

All former inputs used `inverted: true`.

| Former input | Dingtian input pin |
| --- | --- |
| Input 1 | 7 |
| Input 2 | 6 |
| Input 3 | 5 |
| Input 4 | 4 |
| Input 5 | 3 |
| Input 6 | 2 |
| Input 7 | 1 |
| Input 8 | 0 |
