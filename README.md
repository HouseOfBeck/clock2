# Clock 2 hardware test

Clock 2 is an ESP32-S3 GPS-disciplined NTP appliance using W5500 Ethernet.

## Web interface

With the factory hostname, the embedded web pages are available at:

- Main: `http://clock2.local/`
- Diagnostics: `http://clock2.local/diagnostics`
- Settings: `http://clock2.local/settings`

The default hostname is `clock2`. The Settings page can save a different
single-label hostname in NVS. A saved hostname becomes active only after an
explicit restart; for example, `office-clock` becomes
`http://office-clock.local/` after restart.

The DHCP IPv4 address remains a fallback when mDNS is unavailable or while
Bonjour/mDNS caches expire. The Main page auto-refresh preference is stored
only in each browser's local storage. It is not firmware configuration and is
not stored in ESP32 NVS.
