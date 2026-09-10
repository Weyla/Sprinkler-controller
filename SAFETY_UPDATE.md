# Controller safety and watering update

- Each valve receives its own actual ON-time budget. Startup staggering and
  pauses no longer extend the first valve's watering duration. Activity history
  records each ON interval; aggregate watered seconds now sum valve ON time.
- Enabled rain/wind/gust protections are checked every second during automatic
  rows, including startup. Valves close before waiting. Each continuous weather
  interruption has the configured maximum wait; zero means skip immediately.
  Weather is rechecked every five seconds while waiting. Clearing weather resumes
  the remaining valve budgets. Quick manual programs retain their weather bypass.
- Rainfall adjustments recompute the row's total target and subtract watering
  already delivered to each valve. The target cannot increase again during that
  row occurrence. Existing rounds, temperature adjustments and one-second minimum
  occurrence duration remain in effect.
- Pause and Skip are visible only while running; Resume only while paused.
  Emergency stop is visible throughout an active program, including startup,
  weather wait and inter-row delay. The API rejects Skip outside RUNNING.
- Backup import uses an owner token and a persistent controller-side start lock.
  An interrupted import leaves watering locked, including after reboot. Retry the
  complete import to recover; another browser can take over after 60 seconds of
  inactivity. Import is not a transactional rollback of the previous backup.
  A valid device clock is required to complete import. Already-due trigger
  occurrences in the grace window are persisted as claimed before unlocking.
- Timezone changes now apply after the select publishes its new value. The
  reusable package turns valves off during controlled shutdown.

Validation:

```
node --test tests/web_ui.test.cjs
python3 tests/runtime.test.py
python3 tests/import.test.py
```

Host tests execute production C++ functions against relay, clock and storage
stubs. ESPHome 2026.6.5 firmware builds are also checked. These tests do not verify
physical valve response or weather-station accuracy. Existing stale-weather
fallback remains unchanged: unavailable readings do not enforce that protection.
