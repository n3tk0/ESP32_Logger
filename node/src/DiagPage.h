// ============================================================================
// node/src/DiagPage.h — GET /diag, the WiFi node's diagnostics page
//
// Reads /api/diag and /api/log every 5 s and lays them out in the setup
// page's colours. Hand-written and served as is (about 6 KB of the image's
// flash): the setup page is shared with the ESP-NOW node and built to a gzip
// budget (tools/build_node_portal.py) this one is kept out of.
//
// The language follows the setup page's choice (localStorage "np-lang").
// ES5, no template literals, like node_portal/app.js.
// ============================================================================
#pragma once

#include <Arduino.h>

static const char NODE_DIAG_HTML[] PROGMEM = R"NDIAG(<!DOCTYPE html>
<html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Node diagnostics</title>
<style>
:root{--bg:#f1f3f5;--panel:#fff;--panel-2:#f8f9fa;--border:#e1e4e8;--text:#111418;--text-3:#545c68;--accent:#0e7490;--ok:#15803d;--warn:#b45309;--err:#dc2626;--mono:ui-monospace,SFMono-Regular,Menlo,monospace}
@media (prefers-color-scheme:dark){:root{--bg:#0b0e12;--panel:#12161c;--panel-2:#161b22;--border:#242c36;--text:#e6edf3;--text-3:#9aa4b0;--accent:#22d3ee;--ok:#4ade80;--warn:#fbbf24;--err:#f87171}}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--text);font:14px/1.45 -apple-system,BlinkMacSystemFont,"Segoe UI",sans-serif}
.wrap{max-width:760px;margin:0 auto;padding:16px}
header{display:flex;align-items:center;gap:10px;flex-wrap:wrap;margin-bottom:14px}
h1{font-size:18px;margin:0;flex:1}
a{color:var(--accent)}
button{font:inherit;padding:6px 12px;border:1px solid var(--border);border-radius:6px;background:var(--panel);color:var(--text);cursor:pointer}
.card{background:var(--panel);border:1px solid var(--border);border-radius:6px;padding:14px 16px;margin-bottom:14px}
.card h2{font-size:13px;text-transform:uppercase;letter-spacing:.04em;color:var(--text-3);margin:0 0 10px}
.kv{display:grid;grid-template-columns:repeat(auto-fill,minmax(150px,1fr));gap:8px 14px}
.kv div{min-width:0}.kv span{display:block;font-size:12px;color:var(--text-3)}.kv b{font-family:var(--mono);font-weight:600;overflow-wrap:anywhere}
table{width:100%;border-collapse:collapse;font-size:13px}th,td{text-align:left;padding:5px 6px;border-bottom:1px solid var(--border)}th{color:var(--text-3);font-weight:500}
td{font-family:var(--mono)}
.ok{color:var(--ok)}.warn{color:var(--warn)}.err{color:var(--err)}
.verdict{margin-top:10px;padding:8px 10px;border-radius:6px;background:var(--panel-2);border-left:3px solid var(--accent)}
.verdict.err{border-left-color:var(--err)}.verdict.ok{border-left-color:var(--ok)}
pre{margin:0;max-height:360px;overflow:auto;font:12px/1.4 var(--mono);white-space:pre-wrap;word-break:break-all}
.muted{color:var(--text-3);font-size:12px}
</style></head><body><div class="wrap">
<header><h1 id="h"></h1><a href="/" id="back"></a><button id="cp"></button><button id="rf"></button></header>
<div id="out"></div>
<div class="card"><h2 id="lh"></h2><pre id="log"></pre></div>
<p class="muted" id="ts"></p>
</div><script>
var L={en:{h:"Node diagnostics",back:"Settings",cp:"Copy JSON",copied:"Copied",rf:"Refresh",sys:"System",wifi:"Wi-Fi",col:"Collector",sens:"Sensors",log:"Log (RAM, since boot)",
uptime:"Uptime",fw:"Firmware",reset:"Last reset",heap:"Free heap",block:"Largest block",frag:"Fragmentation",conn:"Connected",ssid:"Network",rssi:"Signal",ch:"Channel",ip:"IP",connects:"Connections",wfails:"Failed cycles",upFor:"Connected for",
host:"Address",posts:"Delivered",pfails:"Failed",code:"Last HTTP",lastOk:"Last delivery",lastFail:"Last failure",acc:"Last accepted / room",backlog:"Queued",dropped:"Lost (queue full)",
type:"Sensor",state:"State",reads:"Reads",empty:"Empty",lastRead:"Last value",ok:"ok",missing:"missing",
awake:"Awake",warmed:"Warmed up",pending:"Waiting",bytes:"Bytes received",frames:"Valid frames",bad:"Bad checksum",other:"Other frames",used:"Frames sent",gu:"Given up",woke:"Woken",frame:"Last frame",mode:"Reporting mode",period:"Working period",last:"Last sent",
yes:"yes",no:"no",never:"never",ago:" ago",noAns:"no answer",notAsked:"not asked",active:"active",query:"query",cont:"continuous",min:" min",
vNone:"Nothing at all is arriving on RX. Check the wiring: SDS011 TX to the node's RX pin, SDS011 RX to the TX pin, and 5 V to the sensor (the fan should be audible).",
vNoise:"Bytes arrive but no valid frames: a loose wire, noise or a wrong baud rate.",
vPeriod:"The sensor is in periodic mode ({p} min) and sends one frame per period. The node sets it back to continuous at start-up; restart the node.",
vQuery:"The sensor is in query mode and sends nothing on its own. The node sets it back to active at start-up; restart the node.",
vLate:"Frames arrive but none was fresh when a reading was due.",
vWarm:"Warming up: readings start once the fan has run its warm-up.",
vOk:"Working: frames arrive and readings are sent.",
err:"Could not read /api/diag"},
bg:{h:"Диагностика на node-а",back:"Настройки",cp:"Копирай JSON",copied:"Копирано",rf:"Обнови",sys:"Система",wifi:"Wi-Fi",col:"Collector",sens:"Сензори",log:"Лог (RAM, от старта)",
uptime:"Работи от",fw:"Фърмуер",reset:"Последен рестарт",heap:"Свободен heap",block:"Най-голям блок",frag:"Фрагментация",conn:"Свързан",ssid:"Мрежа",rssi:"Сигнал",ch:"Канал",ip:"IP",connects:"Свързвания",wfails:"Неуспешни цикли",upFor:"Свързан от",
host:"Адрес",posts:"Доставени",pfails:"Неуспешни",code:"Последен HTTP",lastOk:"Последна доставка",lastFail:"Последна грешка",acc:"Последно приети / място",backlog:"В опашката",dropped:"Изгубени (пълна опашка)",
type:"Сензор",state:"Състояние",reads:"Четения",empty:"Празни",lastRead:"Последна стойност",ok:"ok",missing:"липсва",
awake:"Буден",warmed:"Загрял",pending:"Чака",bytes:"Получени байтове",frames:"Валидни кадри",bad:"Грешен checksum",other:"Други кадри",used:"Изпратени кадри",gu:"Отказани",woke:"Събуден",frame:"Последен кадър",mode:"Режим на отчитане",period:"Работен период",last:"Последно изпратени",
yes:"да",no:"не",never:"никога",ago:" назад",noAns:"не отговаря",notAsked:"не е питан",active:"active",query:"query",cont:"непрекъснат",min:" мин",
vNone:"По RX не идва нищо. Провери окабеляването: TX на SDS011 към RX пина на node-а, RX на SDS011 към TX пина и 5 V към сензора (вентилаторът трябва да се чува).",
vNoise:"Идват байтове, но няма валидни кадри: хлабав проводник, смущения или грешна скорост.",
vPeriod:"Сензорът е в периодичен режим ({p} мин) и праща по един кадър на период. Node-ът го връща в непрекъснат при старт; рестартирай node-а.",
vQuery:"Сензорът е в query режим и не праща нищо сам. Node-ът го връща в active при старт; рестартирай node-а.",
vLate:"Идват кадри, но нито един не е бил пресен, когато е трябвало четене.",
vWarm:"Загрява: четенията започват след времето за загряване.",
vOk:"Работи: идват кадри и се изпращат стойности.",
err:"Не успях да прочета /api/diag"}};
var lang="en";try{lang=localStorage.getItem("np-lang")||""}catch(e){}
if(!L[lang])lang=/^bg/i.test(navigator.language||"")?"bg":"en";
function t(k){return L[lang][k]||L.en[k]||k}
function $(i){return document.getElementById(i)}
function esc(s){return String(s==null?"—":s).replace(/[&<>"]/g,function(c){return{"&":"&amp;","<":"&lt;",">":"&gt;",'"':"&quot;"}[c]})}
function dur(s){if(s==null)return t("never");s=+s;if(s<60)return s+" s";if(s<3600)return Math.floor(s/60)+" min "+(s%60)+" s";if(s<86400)return Math.floor(s/3600)+" h "+Math.floor(s%3600/60)+" min";return Math.floor(s/86400)+" d "+Math.floor(s%86400/3600)+" h"}
function ago(s){return s==null?t("never"):dur(s)+t("ago")}
function yn(b){return b?t("yes"):t("no")}
function kv(a){var h='<div class="kv">';for(var i=0;i<a.length;i++)h+='<div><span>'+esc(t(a[i][0]))+'</span><b'+(a[i][2]?' class="'+a[i][2]+'"':'')+'>'+esc(a[i][1])+'</b></div>';return h+'</div>'}
function card(k,b){return '<div class="card"><h2>'+esc(t(k))+'</h2>'+b+'</div>'}
function setting(v,f){return v==-2?t("notAsked"):v==-1?t("noAns"):f(v)}
function verdict(q){var v,c="";
if(q.bytes==0){v="vNone";c="err"}
else if(q.report_mode==1){v="vQuery";c="err"}
else if(q.period>0){v="vPeriod";c="err"}
else if(q.frames==0){v="vNoise";c="err"}
else if(q.used>0&&q.frame_s!=null&&q.frame_s<120){v="vOk";c="ok"}
else if(q.give_ups>0){v="vLate";c="err"}
else if(q.awake&&!q.warmed){v="vWarm"}
else v="vOk",c="ok";
return '<div class="verdict '+c+'">'+esc(t(v).replace("{p}",q.period))+'</div>'}
var J=null;
function render(d){J=d;var s=d.sys||{},w=d.wifi||{},c=d.collector,h="";
h+=card("sys",kv([["uptime",dur(s.uptime_s)],["fw",s.fw],["reset",s.reset],["heap",s.heap+" B",s.heap<8000?"warn":""],["block",s.max_block+" B"],["frag",s.frag+" %"]]));
var wk=[["conn",yn(w.connected),w.connected?"ok":"err"]];
if(w.connected)wk.push(["ssid",w.ssid],["rssi",w.rssi+" dBm",w.rssi<-80?"warn":""],["ch",w.ch],["ip",w.ip]);
wk.push(["connects",w.connects],["wfails",w.fails,w.fails?"warn":""],["upFor",w.up_s==null?null:dur(w.up_s)]);
h+=card("wifi",kv(wk));
if(c)h+=card("col",kv([["host",c.host+":"+c.port],["posts",c.posts],["pfails",c.fails,c.fails?"warn":""],["code",c.last_code,c.last_code==200?"ok":c.last_code?"err":""],["lastOk",ago(c.last_ok_s)],["lastFail",ago(c.last_fail_s)],["acc",(c.accepted==null?"—":c.accepted)+" / "+(c.room==null?"—":c.room)],["backlog",c.backlog+" / "+c.backlog_cap,c.backlog>c.backlog_cap/2?"warn":""],["dropped",c.dropped,c.dropped?"err":""]]));
var sn=d.sensors||[],tb='<table><tr><th>'+esc(t("type"))+'</th><th>'+esc(t("state"))+'</th><th>'+esc(t("reads"))+'</th><th>'+esc(t("empty"))+'</th><th>'+esc(t("lastRead"))+'</th></tr>';
for(var i=0;i<sn.length;i++){var x=sn[i];tb+='<tr><td>'+esc(x.type+(x.addr?"@0x"+x.addr.toString(16):"")+(x.found?" x"+x.found:""))+'</td><td class="'+(x.ok?"ok":"err")+'">'+esc(t(x.ok?"ok":"missing"))+'</td><td>'+esc(x.reads)+'</td><td'+(x.empty?' class="warn"':'')+'>'+esc(x.empty)+'</td><td>'+esc(ago(x.last_ok_s))+'</td></tr>'}
h+=card("sens",tb+'</table>');
var q=d.sds011;if(q){h+=card("SDS011",kv([["awake",yn(q.awake)],["warmed",yn(q.warmed)],["pending",yn(q.pending)],["bytes",q.bytes,q.bytes?"":"err"],["frames",q.frames,q.frames?"":"err"],["bad",q.bad_sum,q.bad_sum?"warn":""],["other",q.other],["used",q.used],["gu",q.give_ups,q.give_ups?"warn":""],["woke",ago(q.woke_s)],["frame",ago(q.frame_s)],
["mode",setting(q.report_mode,function(v){return v==0?t("active"):t("query")}),q.report_mode==1?"err":""],["period",setting(q.period,function(v){return v==0?t("cont"):v+t("min")}),q.period>0?"err":""],["last",(q.pm25==null?"—":q.pm25)+" / "+(q.pm10==null?"—":q.pm10)+" µg/m³"]])+verdict(q))}
$("out").innerHTML=h;$("ts").textContent=new Date().toLocaleTimeString()}
function get(u,cb){var x=new XMLHttpRequest();x.open("GET",u);x.timeout=6000;x.onload=function(){cb(x.status,x.responseText)};x.onerror=x.ontimeout=function(){cb(0,"")};x.send()}
function load(){get("/api/diag",function(st,b){var d=null;try{d=JSON.parse(b)}catch(e){}
if(st==200&&d)render(d);else $("out").innerHTML=card("sys",'<p class="err">'+esc(t("err"))+(st?" (HTTP "+st+")":"")+'</p>')});
get("/api/log",function(st,b){if(st==200){var p=$("log"),end=p.scrollTop+p.clientHeight>=p.scrollHeight-4;p.textContent=b;if(end)p.scrollTop=p.scrollHeight}})}
document.title=t("h");$("h").textContent=t("h");$("back").textContent=t("back");$("cp").textContent=t("cp");$("rf").textContent=t("rf");$("lh").textContent=t("log");
$("rf").onclick=load;
$("cp").onclick=function(){if(!J)return;var s=JSON.stringify(J,null,1);function done(){$("cp").textContent=t("copied");setTimeout(function(){$("cp").textContent=t("cp")},1500)}
if(navigator.clipboard&&window.isSecureContext)navigator.clipboard.writeText(s).then(done);else{var a=document.createElement("textarea");a.value=s;document.body.appendChild(a);a.select();try{document.execCommand("copy");done()}catch(e){}document.body.removeChild(a)}};
load();setInterval(load,5000);
</script></body></html>)NDIAG";
