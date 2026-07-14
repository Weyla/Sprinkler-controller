# ESPHome Sprinkler Controller

An ESP32 sprinkler controller with device-owned schedules, relay safety,
Home Assistant integration, and an authenticated local web interface. It
supports 1-32 zones using either the included Dingtian shift-register driver or
ordinary ESPHome switches.

The current stable release is `v1.0.0`. Installed controllers should use a
versioned release tag; `master` contains the latest published code. Development
branches are kept private until they are ready to release.

> [!CAUTION]
> This software controls physical outputs. First test with pumps and valves
> disconnected, verify every pin and active level, and keep a local emergency
> shutoff. Never expose the HTTP server directly to the Internet.

## Install with ESPHome Device Builder

Users need only one main YAML in ESPHome Device Builder:

1. Copy [examples/dingtian-8-zone.yaml](examples/dingtian-8-zone.yaml) for the
   supported Dingtian-style board, or
   [examples/gpio-relays.yaml](examples/gpio-relays.yaml) for normal GPIO
   relays.
2. Add the keys from
   [examples/secrets.example.yaml](examples/secrets.example.yaml) to Device
   Builder's `secrets.yaml`.
3. Change the device name, board, network, location, relay pins, zones, and
   optional weather sources.
4. Select **Install**. Use USB for the first installation, or OTA when the
   existing firmware already accepts ESPHome OTA with the configured password.
5. Open `http://DEVICE_IP/` and sign in with the configured web credentials.

The examples use `sprinkler_version: v1.0.0`. That single value selects both
the remote package and external-component revision. To update later, change it
to a newer release tag and install again. Use `master` only when you explicitly
want the latest published code instead of an immutable release.

## Releases and updates

Releases use semantic version numbers:

- patch releases such as `v1.0.1` contain compatible fixes;
- minor releases such as `v1.1.0` add compatible features;
- major releases such as `v2.0.0` may require configuration or storage changes.

Git tags are immutable snapshots, so an ESPHome configuration pinned to
`v1.0.0` keeps building the same source. Read the release notes, replace
`sprinkler_version` with the desired newer tag, validate the configuration, and
select **Install** in ESPHome Device Builder. Keep a backup export before an
update that changes stored schedules or settings.

The Dingtian example loads `dtr0xx_io`, `dynamic_sprinkler`, and the pinned web
adapter. The GPIO example does not load `dtr0xx_io`. Any relay implementation
is valid if each valve is an internal ESPHome switch with a safe off restore
mode.

## What stays in the user's YAML

Installation-specific settings deliberately remain outside the remote package:

- device name, ESP32 board/variant/framework, logger, API, OTA, and Wi-Fi;
- IP settings, web port/authentication, time source, timezone, and coordinates;
- relay platform, pins, active level, and Dingtian configuration when used;
- the `dynamic_sprinkler.zones` list and optional Home Assistant weather
  sensors.

Give every zone a permanent unique `zone_id` from 1-32. Saved schedules refer
to that ID, so zones can be reordered safely. Do not reuse a retired ID for
different hardware while old schedules or backups may still reference it.

Weather inputs are optional and must already use these units:

| Input | Unit |
| --- | --- |
| Wind and gust | km/h |
| 24-hour rain | mm |
| Rain rate | mm/h |
| Temperature | °C |

The controller reads numeric values and does not convert Home Assistant units.
Missing, invalid, or readings older than 30 minutes fail open only for the
protection that needs them.

## Main features

- Up to 32 schedules, 32 rows per program, and three simultaneous zones per
  row.
- One to eight fixed-time, sunrise, or sunset triggers per schedule.
- One to ten rounds with total row duration preserved across rounds.
- Manual multi-row programs plus pause, resume, skip, and emergency stop.
- Optional rain, wind, gust, and temperature adjustments.
- Persistent zone names, settings, schedules, trigger claims, and 32 run-history
  entries.
- Versioned JSON export/import for schedules and controller settings.
- Local authenticated web UI with no CDN or runtime Internet dependency.
- Stable Home Assistant controls without creating entities per schedule.

Relays restore off at boot, and the boot hook issues an emergency stop. The
state machine starts simultaneous relays 250 ms apart and stops a run when it
detects an unexpected output, more than three active outputs, or an exceeded
row deadline. Wi-Fi recovery reboots the controller after 15 minutes offline;
boot safety turns outputs off again.

Current persistent records are versioned and CRC-checked. Firmware predating
the current dynamic storage format is not migrated; recreate those schedules.
Backups exclude credentials, network settings, relay mappings, coordinates,
weather entity IDs, history, and firmware. Import is sequential rather than an
atomic transaction, so retry the same file if the connection is interrupted.

## Repository layout

| Path | Purpose |
| --- | --- |
| `examples/` | Complete Device Builder configurations, secret template, and optional HA dashboards |
| `packages/` | Reusable remote ESPHome package and runtime entities |
| `components/` | Scheduler, Dingtian driver, and pinned ESPHome web adapter |
| `web/` | Web UI source embedded by `dynamic_sprinkler` |
| `development/` | This repository's local test-device configuration |
| `docs/` | Architecture and Dingtian pin reference |
| `tests/` | Browser behavior tests |

See [docs/architecture.md](docs/architecture.md) for storage, execution, and
HTTP API details.

## Development

Requirements are Python 3.11+, Git, and Node.js 18+. ESPHome is pinned in
`requirements.txt`.

```bash
git clone --branch master https://github.com/Weyla/Sprinkler-controller.git
cd Sprinkler-controller
python -m venv .venv
source .venv/bin/activate
python -m pip install -r requirements.txt
cp examples/secrets.example.yaml development/secrets.yaml
cp development/local.example.yaml development/local.yaml
```

Run the same checks as CI:

```bash
node --check web/dynamic_app.js
node --test tests/*.test.cjs
esphome config development/sprinkler-test.yaml
esphome compile development/sprinkler-test.yaml
```

After flashing a development build, test boot shutdown, row transitions,
one/three-zone runs, rounds and delays, pause/resume, skip, emergency stop,
fixed and solar starts, weather behavior, persistence, backup/import, and web
authentication on the physical controller.

## License

This is a personal project shared as-is. Code contributions are not currently
accepted.

Project-authored files are MIT-licensed. The modified ESPHome web adapter keeps
ESPHome's licensing split, including GPLv3 C++ runtime files. See
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). Distributed firmware combines
with ESPHome's runtime and is conveyed under GPLv3 with the matching repository
commit as corresponding source.
