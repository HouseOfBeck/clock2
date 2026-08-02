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
    ".badge.ok{background:#173d33;color:#78edbd}.muted{color:#8494a8}.lost{color:#ff8d9d}.hidden{visibility:hidden}[hidden]{display:none!important}" \
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
    "<title>Clock 2 Status</title><style>" COMMON_STYLE
    ".refresh-controls{display:flex;align-items:center;flex-wrap:wrap;gap:8px;margin-bottom:7px}"
    ".refresh-controls select,.refresh-controls button{font:inherit;color:#e8eef7;background:#141d2a;border:1px solid #36475e;border-radius:7px;padding:6px 9px}"
    ".refresh-controls button{cursor:pointer}.refresh-controls button:hover{border-color:#38bdf8}"
    "</style></head><body>"
    "<header><div><h1>Clock 2</h1><p>GPS-Disciplined Stratum-1 NTP Server</p></div>"
    "<nav><a href=/ aria-current=page>Main</a><a href=/diagnostics>Diagnostics</a><a href=/settings>Settings</a></nav></header>"
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
    "</div></main><footer><div class=refresh-controls>"
    "<label for=refresh-rate>Auto-refresh:</label><select id=refresh-rate>"
    "<option value=0>Off</option><option value=2>2 seconds</option>"
    "<option value=5>5 seconds</option><option value=15>15 seconds</option>"
    "<option value=30>30 seconds</option><option value=60>60 seconds</option>"
    "</select><button id=refresh-now type=button>Refresh now</button></div>"
    "<div>Status reporting is independent of the NTP timing path.</div></footer><script>"
    COMMON_SCRIPT
    "function show(s){text('utc',s.time.valid?s.time.utc:'Timebase unavailable');mark('time-valid',s.time.valid);"
    "text('time-source',s.time.source);text('time-pps',s.time.pps_count+' · age '+age(s.time.pps_age_us));"
    "mark('gps-fix',s.gps.fix,'Fix','No fix');text('sats',s.gps.satellites);text('hdop',fixed(s.gps.hdop,3));"
    "text('lat',fixed(s.gps.latitude,7));text('lon',fixed(s.gps.longitude,7));text('alt',s.gps.altitude_m===null?'—':fixed(s.gps.altitude_m,3)+' m');text('nmea-age',age(s.gps.last_valid_age_us));"
    "mark('link',s.network.link,'Up','Down');text('ip',s.network.ip);text('mac',s.network.mac);text('host',s.network.hostname);text('mask',s.network.netmask);text('gw',s.network.gateway);"
    "mark('ntp-running',s.ntp.running,'Running','Stopped');text('ntp-ref','Stratum '+s.ntp.stratum+' · '+s.ntp.reference);text('replies',s.ntp.transmitted);text('requests',s.ntp.received+' / '+s.ntp.ignored);"
    "text('firmware',s.system.firmware);text('idf',s.system.idf);text('uptime',s.system.uptime_s+' s');text('heap',s.system.free_heap+' / '+s.system.minimum_free_heap+' B');}"
    "const refreshKey='clock2.main.refreshSeconds',refreshValues=['0','2','5','15','30','60'];let refreshTimer=null,refreshPending=false;"
    "function applyRefresh(value,save){if(!refreshValues.includes(value))value='15';$('refresh-rate').value=value;if(refreshTimer!==null){clearInterval(refreshTimer);refreshTimer=null}if(save){try{localStorage.setItem(refreshKey,value)}catch(e){}}if(value!=='0')refreshTimer=setInterval(refresh,Number(value)*1000)}"
    "async function refresh(){if(refreshPending)return;refreshPending=true;try{const r=await fetch('/api/status',{cache:'no-store'});if(!r.ok)throw 0;show(await r.json());$('lost').classList.add('hidden')}catch(e){$('lost').classList.remove('hidden')}finally{refreshPending=false}}"
    "let saved=null;try{saved=localStorage.getItem(refreshKey)}catch(e){}applyRefresh(refreshValues.includes(saved)?saved:'15',false);"
    "$('refresh-rate').addEventListener('change',e=>applyRefresh(e.target.value,true));$('refresh-now').addEventListener('click',refresh);refresh();"
    "</script></body></html>";

const char clock2_diagnostics_html[] =
    "<!doctype html><html lang=en><head><meta charset=utf-8>"
    "<meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>Clock 2 Diagnostics</title><style>" COMMON_STYLE "</style></head><body>"
    "<header><div><h1>Clock 2 Diagnostics</h1><p>Read-only engineering status</p></div>"
    "<nav><a href=/>Main</a><a href=/diagnostics aria-current=page>Diagnostics</a><a href=/settings>Settings</a></nav></header>"
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

const char clock2_settings_html[] =
    "<!doctype html><html lang=en><head><meta charset=utf-8>"
    "<meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>Clock 2 Settings</title><style>" COMMON_STYLE
    ".settings-card{max-width:680px}.field-label{display:block;color:#91a2b7;margin-bottom:7px}"
    ".hostname-entry{display:flex;align-items:center;gap:8px;margin-bottom:15px}"
    ".hostname-entry input{min-width:0;flex:1;font:inherit;color:#f5f8fc;background:#0b1018;border:1px solid #36475e;border-radius:7px;padding:9px 10px}"
    ".suffix{color:#91a2b7}.action{font:inherit;color:#071019;background:#79e1b1;border:0;border-radius:7px;padding:9px 13px;cursor:pointer;font-weight:650}"
    ".action.secondary{background:#f2b36f}.action:disabled{opacity:.55;cursor:wait}.message{min-height:22px;margin:12px 0;color:#79e1b1}.message.error{color:#ff8d9d}"
    ".notes{color:#91a2b7;line-height:1.5;margin-top:17px}.restart{margin-top:18px;padding-top:16px;border-top:1px solid #263448}.restart.hidden{display:none}.url{overflow-wrap:anywhere}"
    "</style></head><body>"
    "<header><div><h1>Clock 2 Settings</h1><p>Device configuration</p></div>"
    "<nav><a href=/>Main</a><a href=/diagnostics>Diagnostics</a><a href=/settings aria-current=page>Settings</a></nav></header>"
    "<main><div class=grid><section class='card wide settings-card'><h2>Network settings</h2>"
    "<form id=hostname-form><label class=field-label for=hostname>Device hostname</label>"
    "<div class=hostname-entry><input id=hostname name=hostname maxlength=63 autocomplete=off autocapitalize=none spellcheck=false required><span class=suffix>.local</span></div>"
    "<button id=save-hostname class=action type=submit>Save hostname</button></form>"
    "<div id=settings-message class=message aria-live=polite></div><div class=rows>"
    "<div class=row><span class=label>Configured hostname</span><b id=configured-host>—</b></div>"
    "<div class=row><span class=label>Active hostname</span><b id=active-host>—</b></div>"
    "<div class=row><span class=label>Resulting URL</span><b id=configured-url class=url>—</b></div>"
    "<div class=row><span class=label>Active Main page</span><a id=active-url href=/>—</a></div>"
    "<div class=row><span class=label>DHCP IPv4 fallback</span><b id=ip-fallback>—</b></div></div>"
    "<div id=restart-panel class='restart hidden'><p><b>Hostname saved.</b><br>Restart required.</p>"
    "<p>After restart:<br><b id=next-url class=url>—</b></p>"
    "<button id=restart-now class='action secondary' type=button>Restart now</button></div>"
    "<div class=notes>DHCP IPv4 access remains available if the new mDNS name is not immediately visible.<br>Bonjour/mDNS caches may take a short time to expire after restart.</div>"
    "</section></div></main><footer>Only the hostname and an explicit pending restart can be changed here.</footer><script>"
    COMMON_SCRIPT
    "let activeLabel='',configuredLabel='',fallbackIp='';"
    "const urlFor=h=>'http://'+h+'.local/';"
    "function validateHost(v){if(!v)return'Hostname is required.';if(v.length>63)return'Hostname must be 1 to 63 characters.';if(!/^[A-Za-z0-9-]+$/.test(v))return'Hostname must contain only letters, digits, and internal hyphens.';if(!/^[A-Za-z0-9]/.test(v)||!/[A-Za-z0-9]$/.test(v))return'Hostname must start and end with a letter or digit.';return''}"
    "function message(value,error=false){const e=$('settings-message');e.textContent=value;e.className='message'+(error?' error':'')}"
    "function render(active,configured,pending,ip){activeLabel=active;configuredLabel=configured;fallbackIp=ip||fallbackIp;$('hostname').value=configured;text('active-host',active+'.local');text('configured-host',configured+'.local');text('configured-url',urlFor(configured));const link=$('active-url');link.textContent=urlFor(active);link.href='/';text('ip-fallback',fallbackIp);text('next-url',urlFor(configured));$('restart-panel').classList.toggle('hidden',!pending)}"
    "async function load(){try{const r=await fetch('/api/status',{cache:'no-store'});if(!r.ok)throw 0;const s=await r.json();render(s.network.hostname_label,s.network.configured_hostname_label,s.network.hostname_restart_pending,s.network.ip)}catch(e){message('Unable to load settings.',true)}}"
    "$('hostname-form').addEventListener('submit',async e=>{e.preventDefault();const input=$('hostname'),save=$('save-hostname'),value=input.value,problem=validateHost(value);if(problem){message(problem,true);return}save.disabled=true;message('Saving…');try{const r=await fetch('/api/settings/hostname',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({hostname:value})});const data=await r.json();if(!r.ok||!data.ok)throw new Error(data.error||'Unable to save hostname.');render(data.active_hostname,data.configured_hostname,data.restart_required,fallbackIp);message(data.restart_required?'Hostname saved. Restart required.':'Hostname is unchanged.')}catch(error){message(error.message||'Unable to save hostname.',true)}finally{save.disabled=false}});"
    "$('restart-now').addEventListener('click',async()=>{const button=$('restart-now');button.disabled=true;message('Requesting restart…');try{const r=await fetch('/api/settings/restart',{method:'POST'});const data=await r.json();if(!r.ok||!data.ok)throw new Error(data.error||'Unable to restart Clock 2.');message(data.message+' Next URL: '+data.next_url+' DHCP fallback: '+fallbackIp)}catch(error){message(error.message||'Unable to restart Clock 2.',true);button.disabled=false}});load();"
    "</script></body></html>";
