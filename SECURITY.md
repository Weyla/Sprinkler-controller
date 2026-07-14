# Security and safety

This controller operates physical outputs. Treat firmware changes, OTA access,
the native ESPHome API, and the web UI as safety-sensitive.

- Use unique API, OTA, and web-server credentials.
- Keep the controller on a trusted local network; the web server is HTTP, not
  HTTPS, and should not be exposed directly to the Internet.
- Keep relay entities internal and configured to restore off.
- Test changes on disconnected hardware before attaching valves or pumps.
- Keep a serial recovery path available when changing partitions, boot logic,
  GPIO mappings, or the relay driver.

Please report security-sensitive issues privately through GitHub's repository
security advisory feature rather than a public issue. Include the affected
commit, ESPHome version, board, reproduction steps, and relevant logs with all
credentials removed.
