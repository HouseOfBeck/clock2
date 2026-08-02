#include "web_assets.h"

#define COMMON_STYLE \
    ":root{color-scheme:dark;font-family:system-ui,-apple-system,sans-serif;background:#0b1018;color:#e8eef7}" \
    "*{box-sizing:border-box}body{margin:0;min-height:100vh;background:linear-gradient(160deg,#101827,#080c12);font-size:16px}" \
    "header,main,footer{width:min(1100px,94vw);margin:auto}header{padding:24px 0 16px;display:flex;align-items:end;justify-content:space-between;gap:16px}" \
    "h1{font-size:clamp(28px,5vw,44px);margin:0;color:#fff}header p{margin:4px 0 0;color:#9fb1c8}" \
    "nav a{color:#8bd5ff;text-decoration:none;margin-left:18px}nav a[aria-current=page]{color:#fff;border-bottom:2px solid #38bdf8}" \
    ".grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(260px,1fr));gap:14px;padding-bottom:22px}" \
    ".card{background:#141d2a;border:1px solid #263448;border-radius:12px;padding:17px;box-shadow:0 5px 22px #0005}" \
    ".wide{grid-column:1/-1}.card h2{font-size:14px;text-transform:uppercase;letter-spacing:.12em;color:#8fa6c0;margin:0 0 13px}" \
    ".hero{font-size:clamp(24px,4vw,38px);font-variant-numeric:tabular-nums;color:#79e1b1;overflow-wrap:anywhere}" \
    ".rows{display:grid;gap:9px}.row,.pair{display:flex;justify-content:space-between;gap:14px;border-bottom:1px solid #263448;padding-bottom:7px}" \
    ".pair{justify-content:flex-start;flex-wrap:wrap;gap:12px 30px}.label{color:#91a2b7}.value,b{font-variant-numeric:tabular-nums;color:#f5f8fc}" \
    ".badge{display:inline-block;border-radius:999px;padding:3px 9px;font-size:13px;background:#3b2630;color:#ff9dab}" \
    ".badge.ok{background:#173d33;color:#78edbd}.muted{color:#8494a8}.lost{color:#ff8d9d}.hidden{visibility:hidden}" \
    "footer{color:#718299;font-size:13px;padding:2px 0 22px}@media(max-width:620px){header{align-items:start;flex-direction:column}nav a{margin:0 18px 0 0}.card{padding:14px}}"

#define COMMON_SCRIPT \
    "const $=id=>document.getElementById(id);" \
    "const text=(id,v)=>{const e=$(id);if(e)e.textContent=(v===null||v===undefined)?'—':v};" \
    "const age=v=>v<0?'—':v<1000?v+' µs':v<1000000?(v/1000).toFixed(1)+' ms':(v/1000000).toFixed(2)+' s';" \
    "const fixed=(v,n=3)=>v===null?'—':Number(v).toFixed(n);" \
    "const mark=(id,ok,yes='Valid',no='Invalid')=>{const e=$(id);if(!e)return;e.textContent=ok?yes:no;e.className='badge '+(ok?'ok':'');};"

const char clock2_status_html[] =
    "<!doctype html><html lang=en><head><meta charset=utf-8>"
    "<meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>Clock 2 Status</title><style>" COMMON_STYLE "</style></head><body>"
    "<header><div><h1>Clock 2</h1><p>GPS-Disciplined Stratum-1 NTP Server</p></div>"
    "<nav><a href=/ aria-current=page>Status</a><a href=/advanced>Advanced</a></nav></header>"
    "<main><div id=lost class='lost hidden'>Connection lost — showing last received values</div><div class=grid>"
    "<section class='card wide'><h2>Time</h2><div id=utc class=hero>Waiting for GNSS time…</div>"
    "<div class=pair><span><span class=label>State </span><span id=time-valid class=badge>Invalid</span></span>"
    "<span><span class=label>Source </span><b id=time-source>—</b></span>"
    "<span><span class=label>PPS </span><b id=time-pps>—</b></span></div></section>"
    "<section class=card><h2>GPS</h2><div class=rows>"
    "<div class=row><span class=label>Fix</span><span id=gps-fix class=badge>Unknown</span></div>"
    "<div class=row><span class=label>Satellites</span><b id=sats>—</b></div>"
    "<div class=row><span class=label>HDOP</span><b id=hdop>—</b></div>"
    "<div class=row><span class=label>Latitude</span><b id=lat>—</b></div>"
    "<div class=row><span class=label>Longitude</span><b id=lon>—</b></div>"
    "<div class=row><span class=label>Altitude</span><b id=alt>—</b></div>"
    "<div class=row><span class=label>Last valid NMEA</span><b id=nmea-age>—</b></div></div></section>"
    "<section class=card><h2>Network</h2><div class=rows>"
    "<div class=row><span class=label>Link</span><span id=link class=badge>Down</span></div>"
    "<div class=pair><span><span class=label>IP </span><b id=ip>—</b></span>"
    "<span><span class=label>MAC </span><b id=mac>—</b></span></div>"
    "<div class=row><span class=label>Hostname</span><b id=host>clock2.local</b></div>"
    "<div class=row><span class=label>Netmask</span><b id=mask>—</b></div>"
    "<div class=row><span class=label>Gateway</span><b id=gw>—</b></div></div></section>"
    "<section class=card><h2>NTP</h2><div class=rows>"
    "<div class=row><span class=label>Server</span><span id=ntp-running class=badge>Stopped</span></div>"
    "<div class=row><span class=label>Stratum / Reference</span><b id=ntp-ref>—</b></div>"
    "<div class=row><span class=label>Replies</span><b id=replies>0</b></div>"
    "<div class=row><span class=label>Requests / Ignored</span><b id=requests>0 / 0</b></div></div></section>"
    "<section class=card><h2>System</h2><div class=rows>"
    "<div class=row><span class=label>Firmware</span><b id=firmware>—</b></div>"
    "<div class=row><span class=label>ESP-IDF</span><b id=idf>—</b></div>"
    "<div class=row><span class=label>Uptime</span><b id=uptime>—</b></div>"
    "<div class=row><span class=label>Free / minimum heap</span><b id=heap>—</b></div></div></section>"
    "</div></main><footer>Read-only status · refreshes every 2 seconds</footer><script>"
    COMMON_SCRIPT
    "function show(s){text('utc',s.time.valid?s.time.utc:'Timebase unavailable');mark('time-valid',s.time.valid);"
    "text('time-source',s.time.source);text('time-pps',s.time.pps_count+' · age '+age(s.time.pps_age_us));"
    "mark('gps-fix',s.gps.fix,'Fix','No fix');text('sats',s.gps.satellites);text('hdop',fixed(s.gps.hdop,3));"
    "text('lat',fixed(s.gps.latitude,7));text('lon',fixed(s.gps.longitude,7));text('alt',s.gps.altitude_m===null?'—':fixed(s.gps.altitude_m,3)+' m');text('nmea-age',age(s.gps.last_valid_age_us));"
    "mark('link',s.network.link,'Up','Down');text('ip',s.network.ip);text('mac',s.network.mac);text('host',s.network.hostname);text('mask',s.network.netmask);text('gw',s.network.gateway);"
    "mark('ntp-running',s.ntp.running,'Running','Stopped');text('ntp-ref','Stratum '+s.ntp.stratum+' · '+s.ntp.reference);text('replies',s.ntp.transmitted);text('requests',s.ntp.received+' / '+s.ntp.ignored);"
    "text('firmware',s.system.firmware);text('idf',s.system.idf);text('uptime',s.system.uptime_s+' s');text('heap',s.system.free_heap+' / '+s.system.minimum_free_heap+' B');}"
    "async function refresh(){try{const r=await fetch('/api/status',{cache:'no-store'});if(!r.ok)throw 0;show(await r.json());$('lost').classList.add('hidden')}catch(e){$('lost').classList.remove('hidden')}}refresh();setInterval(refresh,2000);"
    "</script></body></html>";

const char clock2_advanced_html[] =
    "<!doctype html><html lang=en><head><meta charset=utf-8>"
    "<meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>Clock 2 Advanced</title><style>" COMMON_STYLE "</style></head><body>"
    "<header><div><h1>Clock 2 Advanced</h1><p>Advanced diagnostics — read only</p></div>"
    "<nav><a href=/>Status</a><a href=/advanced aria-current=page>Advanced</a></nav></header>"
    "<main><div id=lost class='lost hidden'>Connection lost — showing last received values</div><div class=grid>"
    "<section class=card><h2>PPS</h2><div id=pps class=rows></div></section>"
    "<section class=card><h2>NMEA timing</h2><div id=nmea class=rows></div></section>"
    "<section class=card><h2>Timebase</h2><div id=tb class=rows></div></section>"
    "<section class=card><h2>NTP / Network</h2><div id=net class=rows></div></section>"
    "<section class=card><h2>Build / System</h2><div id=sys class=rows></div></section>"
    "<section class=card><h2>Hardware configuration</h2><div id=cfg class=rows></div></section>"
    "</div></main><footer>No controls or settings are exposed.</footer><script>"
    COMMON_SCRIPT
    "const row=(k,v)=>'<div class=row><span class=label>'+k+'</span><b>'+v+'</b></div>';"
    "const timing=x=>x.n?('n='+x.n+' mean='+x.mean_us+' µs min='+x.min_us+' max='+x.max_us):'n=0';"
    "function show(s){$('pps').innerHTML=row('Valid / count',s.pps.valid+' / '+s.pps.count)+row('Age',age(s.pps.age_us))+row('Last interval',s.pps.last_interval_us+' µs')+row('Interval n / mean',s.pps.interval_samples+' / '+s.pps.mean_interval_us+' µs')+row('Interval min / max',s.pps.min_interval_us+' / '+s.pps.max_interval_us+' µs')+row('Selected edge',s.pps.selected_edge)+row('Pulse width',s.pps.pulse_width_us===null?'unavailable':s.pps.pulse_width_us+' µs');"
    "$('nmea').innerHTML=row('GGA',timing(s.gps.timing.gga))+row('RMC',timing(s.gps.timing.rmc))+row('ZDA',timing(s.gps.timing.zda))+row('Last valid age',age(s.gps.last_valid_age_us));"
    "$('tb').innerHTML=row('Valid / UTC',s.time.valid+' / '+(s.time.utc||'—'))+row('Source',s.time.source)+row('PPS / label age',age(s.time.pps_age_us)+' / '+age(s.time.gnss_age_us))+row('Accepted / rejected',s.time.accepted+' / '+s.time.rejected)+row('Association window',s.config.association_min_us+'–'+s.config.association_max_us+' µs')+row('PPS / GNSS timeout',s.config.pps_timeout_us+' / '+s.config.gnss_timeout_us+' µs');"
    "$('net').innerHTML=row('NTP received / sent',s.ntp.received+' / '+s.ntp.transmitted)+row('Ignored / invalid-timebase',s.ntp.ignored+' / '+s.ntp.invalid_timebase)+row('NTP port / precision',s.ntp.port+' / '+s.ntp.precision)+row('Ethernet',s.network.link?'link up':'link down')+row('IP / MAC',s.network.ip+' / '+s.network.mac)+row('Netmask / gateway',s.network.netmask+' / '+s.network.gateway);"
    "$('sys').innerHTML=row('Project / version',s.system.project+' / '+s.system.firmware)+row('Build',s.system.build_date+' '+s.system.build_time)+row('ESP-IDF',s.system.idf)+row('Chip',s.system.chip)+row('Reset reason',s.system.reset_reason)+row('Flash',s.system.flash_size+' B')+row('Uptime',s.system.uptime_s+' s')+row('Heap free / minimum',s.system.free_heap+' / '+s.system.minimum_free_heap+' B')+row('NTP path diagnostics',s.diagnostics.ntp_path?'enabled':'disabled')+row('PPS diagnostics',s.diagnostics.pps_path?'enabled':'disabled');"
    "$('cfg').innerHTML=row('GPS UART',s.config.uart+' GPIO'+s.config.gps_rx_gpio+' @ '+s.config.gps_baud+' baud')+row('PPS GPIO','GPIO'+s.config.pps_gpio)+row('W5500 SPI','MOSI '+s.config.eth_mosi+' MISO '+s.config.eth_miso+' SCLK '+s.config.eth_sclk+' CS '+s.config.eth_cs)+row('W5500 INT / RESET',s.config.eth_int+' / '+s.config.eth_reset)+row('W5500 SPI clock',s.config.eth_spi_clock_hz+' Hz')+row('Raw NMEA dump',s.diagnostics.raw_nmea?'enabled':'disabled');}"
    "async function refresh(){try{const r=await fetch('/api/status',{cache:'no-store'});if(!r.ok)throw 0;show(await r.json());$('lost').classList.add('hidden')}catch(e){$('lost').classList.remove('hidden')}}refresh();setInterval(refresh,2000);"
    "</script></body></html>";
