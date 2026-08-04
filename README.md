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

## OLED display

The stacked Waveshare Pico-OLED-1.3 uses the SH1107 in a logical 128x64
landscape orientation on SPI3. It presents four read-only pages for clock
status, GPS, network, and NTP/system status. Pages rotate automatically every
8 seconds by default.

- KEY0 cycles display brightness through 100%, 35%, off, and back to 100%.
- KEY1 pauses or resumes automatic page rotation.

Button state, brightness, and the current page are runtime-only and are not
stored in NVS. OLED failure is nonfatal: the display is presentation-only and
is independent of PPS capture, UTC discipline, and NTP timestamp generation.
