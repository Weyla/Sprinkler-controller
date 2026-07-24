# Architecture

`dynamic_sprinkler` owns schedule storage, execution, relay safety, history,
and the authenticated HTTP API. Home Assistant supplies optional weather values
and stable controls but is not required for schedule execution. All scheduler
and relay mutations run in ESPHome's main loop; asynchronous HTTP handlers only
queue copied commands or return cached snapshots.

## Package boundary

`packages/sprinkler-core.yaml` owns reusable runtime entities, boot safety, web
behavior, and project metadata. The user's YAML owns the ESP device/framework,
logger, API, OTA, Wi-Fi, time/location, web authentication, relay hardware,
zones, and weather sources.

Both the package and selected external components use `sprinkler_version` as
their Git ref. The Dingtian example selects all three custom components; the
GPIO example selects only `dynamic_sprinkler` and `web_server_idf`.

The component embeds `web/style.css` and `web/dynamic_app.js` from its Git
checkout into ESPHome's authenticated `/0.css` and `/0.js` routes. Users do not
need separate asset files or a runtime Internet connection.

## Limits and persistence

| Item | Limit |
| --- | ---: |
| Configured zones | 32 |
| Saved schedules | 32 |
| Rows per program | 32 |
| Start triggers per schedule | 8 |
| Rounds per schedule | 10 |
| Simultaneous zones per row | 3 |
| Run-history entries | 32 |
| Detailed zone-activity intervals | 64 |

Fixed arrays bound long-running memory use. Records are encoded field by field
with a magic value, schema version, payload length, and CRC32. Separate NVS
keys store metadata, schedules, history, detailed zone activity, zone names,
weather settings, and the last claimed automatic trigger. Zone start/stop
events remain in RAM during a run and the rolling activity trace is committed
once with the completed run, avoiding a flash write per relay transition.
Status polling and browser countdowns do not write flash.

Schedule IDs are monotonic, and revisions reject stale browser updates. Zone
records use stable `zone_id` values rather than YAML positions. Only the current
storage schema is loaded; legacy records are intentionally ignored.

## Execution

The non-blocking state sequence is:

`IDLE → WEATHER_WAIT → STARTING → RUNNING → DELAY → IDLE`

`PAUSED` is entered only from `RUNNING`; resume continues the same row with its
saved remaining time.

- Automatic schedules may wait for weather once per minute within their
  configured limit. Run-now schedules skip blocked rows; manual programs bypass
  weather protections.
- `STARTING` enables relays one at a time with 250 ms separation.
- `DELAY` keeps all relays off between rows and rounds. The final delay is
  omitted after the final round.
- Duration is divided across rounds in milliseconds without losing the total.
- Actual watered time excludes pauses, weather waits, and row delays.
- The relay guard stops unexpected outputs, more than three active relays, or
  a row exceeding its deadline tolerance.
- Boot, normal completion, and emergency stop issue an unconditional final OFF
  command to every configured relay, even when software already reports it off.

Clock triggers store local minute-of-day. Solar triggers store sunrise/sunset
plus a signed offset from -360 to +360 minutes. Adjacent dates are checked for
offsets crossing midnight. A trigger may start up to five minutes late after
boot or time synchronization, while its persisted claim prevents duplicate
runs.

## Weather order

Configured inputs have independent persisted enable switches and a 30-minute
freshness limit:

1. active rain and row wind/gust limits can wait or skip;
2. meeting the 24-hour rain target skips the row;
3. partial rain reduces duration proportionally;
4. temperature applies the schedule's percentage-per-degree adjustment;
5. final duration is clamped to 1-3600 seconds before round division.

Invalid or stale data disables only the decision that requires that input.

## HTTP API

All routes use ESPHome web-server Basic Authentication. POST requests also
require `X-Sprinkler-Request: 1`.

| Method | Route | Purpose |
| --- | --- | --- |
| GET | `/sprinkler/api/schedules` | List schedules |
| GET | `/sprinkler/api/schedule?id=ID` | Read one schedule |
| POST | `/sprinkler/api/save` | Create or update a schedule |
| POST | `/sprinkler/api/validate` | Validate without writing |
| POST | `/sprinkler/api/delete` | Delete a schedule |
| POST | `/sprinkler/api/run` | Run a saved schedule |
| POST | `/sprinkler/api/manual` | Run a one-off program |
| GET | `/sprinkler/api/status` | Cached live controller state |
| GET | `/sprinkler/api/solar` | Resolve a solar preview |
| POST | `/sprinkler/api/control` | Pause, resume, skip, or stop |
| GET | `/sprinkler/api/history?offset=N&limit=N` | Read paginated run summaries |
| GET | `/sprinkler/api/activity?offset=N&limit=N` | Read paginated zone activity |
| GET | `/sprinkler/api/system` | Read health and settings |
| POST | `/sprinkler/api/settings` | Change controller settings |

History and activity responses are limited to ten records per page so they
remain below ESPHome's 5120-byte JSON serialization ceiling. The browser polls
status every ten seconds when idle and every three seconds while a run is
active, with exponential backoff after errors. It animates timers from
monotonic device samples and updates runtime DOM nodes only when displayed data
changes.

Backup export is assembled schedule by schedule in the browser. Import validates
all incoming schedules before replacement and refuses to run while watering.
Replacement is sequential, not atomic; retry the same file after an interrupted
import.
