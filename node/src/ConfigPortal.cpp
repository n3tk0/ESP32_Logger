#include "ConfigPortal.h"

// The NODE_SENSOR_* selection decides which pin fields the form shows.
#include "node_config.h"
// The pin table and the "D6 is GPIO12" parser, shared with tests/host.
#include "NodePins.h"

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <DNSServer.h>
#include <ArduinoJson.h>

// The sync web server is the right shape here: the portal is a handful of
// requests from one phone, it runs only while nothing else is happening, and
// blocking inside a handler costs nothing when there is no sensor loop to
// stall.
static ESP8266WebServer s_http(80);
static DNSServer        s_dns;
static NodeSettings*    s_target = nullptr;
static bool             s_saved  = false;

/// True while the portal is being served on the STA interface — the home LAN —
/// rather than on the node's own access point.
///
/// THE DIFFERENCE IS WHO CAN REACH IT. To open the AP portal you have to be
/// associated with the node's access point, standing next to it, during a
/// window it opens only after repeated WiFi failures. The background portal is
/// on the LAN, for the node's whole uptime, reachable by anything on the
/// network. The same page cannot be safe in both places, so it is not the same
/// page: secrets are rendered on one and blanked on the other.
static bool s_background = false;

/// Handlers are registered once for the life of the process.
///
/// ESP8266WebServer::on() appends to a list and never deduplicates, and
/// portalStartBackground() is called on every transition back to connected —
/// so a node whose router reboots nightly grew three dead handler entries a
/// night, walked on every request, until it ran out of heap.
static bool s_routesBound = false;

/// Basic auth on the background portal, using the credentials the node already
/// holds for the collector.
///
/// Reusing them is not elegant, but the alternative was worse in both
/// directions: a new setting nobody would fill in, or — as shipped — a form on
/// the LAN that anyone could POST to, repointing the node at their own
/// collector and restarting it. The AP portal is not gated, because reaching it
/// already requires the AP's own password.
static bool backgroundAuthOk() {
    if (!s_background) return true;
    const NodeSettings& s = *s_target;
    if (s_http.authenticate(s.basicUser, s.basicPass)) return true;
    s_http.requestAuthentication();
    return false;
}

bool portalButtonHeld() {
    pinMode(PORTAL_TRIGGER_PIN, INPUT_PULLUP);
    // One read can catch a floating pin mid-transition; require the button to
    // still be down after a debounce interval.
    if (digitalRead(PORTAL_TRIGGER_PIN) != LOW) return false;
    delay(50);
    return digitalRead(PORTAL_TRIGGER_PIN) == LOW;
}

// ---------------------------------------------------------------------------
// The pin table lives in NodePins.h, next to the host test that checks it.
// These are the names this file uses it under.
// ---------------------------------------------------------------------------
using NodePins::BOARDS;
using NodePins::BOARD_COUNT;
using NodePins::FACTS;
using NodePins::LABELS;
using NodePins::LABEL_COUNT;
using NodePins::MAX_GPIO;
using NodePins::RISK_NEVER;

// ---------------------------------------------------------------------------
// Chunked output
// ---------------------------------------------------------------------------
//
// The page is now ~14 KB of HTML, CSS and JS. Building that in a String costs
// more than the ESP8266 can spare while WiFi is up — and the old page already
// reserved 4 KB for a much smaller form. Everything is streamed instead: text
// accumulates in a buffer that flushes at a kilobyte, so the page can grow
// without the free heap noticing.
class Chunked {
public:
    void begin() {
        s_http.setContentLength(CONTENT_LENGTH_UNKNOWN);
        s_http.send(200, "text/html", "");
    }
    Chunked& operator+=(const __FlashStringHelper* v) { _b += v; _maybe(); return *this; }
    Chunked& operator+=(const char* v)                { _b += v; _maybe(); return *this; }
    Chunked& operator+=(const String& v)              { _b += v; _maybe(); return *this; }
    Chunked& operator+=(int v)                        { _b += v; _maybe(); return *this; }
    void end() {
        if (_b.length()) s_http.sendContent(_b);
        _b = "";
        s_http.sendContent(String());   // terminates the chunked response
    }
private:
    void _maybe() {
        if (_b.length() >= 1024) { s_http.sendContent(_b); _b = ""; }
    }
    String _b;
};

// ---------------------------------------------------------------------------
// A value going into an HTML attribute has to be escaped. A WiFi passphrase
// containing a double quote is legal and not rare, and without this it would
// break out of the value attribute and silently truncate the field — the kind
// of bug that looks like "the portal corrupts my password".
// ---------------------------------------------------------------------------
static String esc(const char* raw) {
    String out;
    for (const char* p = raw; *p; p++) {
        switch (*p) {
            case '&':  out += F("&amp;");  break;
            case '<':  out += F("&lt;");   break;
            case '>':  out += F("&gt;");   break;
            case '"':  out += F("&quot;"); break;
            case '\'': out += F("&#39;");  break;
            default:   out += *p;          break;
        }
    }
    return out;
}

static void row(Chunked& p, const char* id, const char* label,
                const String& value, const char* type, const char* hint) {
    p += F("<label for=\""); p += id; p += F("\">"); p += label;
    p += F("</label><input id=\""); p += id;
    p += F("\" name=\""); p += id;
    p += F("\" type=\""); p += type;
    p += F("\" value=\""); p += value;
    p += F("\">");
    if (hint && *hint) { p += F("<p class=\"h\">"); p += hint; p += F("</p>"); }
}

/// A pin field: the same input, plus the live resolver underneath it. The
/// `pin` class is what the JS hooks; `data-role` names the wire so the board
/// diagram can badge it.
static void pinRow(Chunked& p, const char* id, const char* label,
                   uint8_t gpio, const char* role, const char* hint) {
    p += F("<label for=\""); p += id; p += F("\">"); p += label;
    p += F("</label><input id=\""); p += id;
    p += F("\" name=\""); p += id;
    p += F("\" class=\"pin\" data-role=\""); p += role;
    p += F("\" type=\"text\" inputmode=\"text\" autocomplete=\"off\" value=\"");
    p += (int)gpio;
    p += F("\"><p class=\"pinout\" id=\"o_"); p += id; p += F("\"></p>");
    if (hint && *hint) { p += F("<p class=\"h\">"); p += hint; p += F("</p>"); }
}

// ---------------------------------------------------------------------------
// The page
// ---------------------------------------------------------------------------
static void handleRoot() {
    if (!backgroundAuthOk()) return;
    const NodeSettings& s = *s_target;

    Chunked p;
    p.begin();

    p += F("<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
           "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
           "<title>Sensor node setup</title><style>"
           "body{font-family:system-ui,-apple-system,sans-serif;max-width:32rem;"
           "margin:0 auto;padding:1rem;background:#f6f7f9;color:#111}"
           "h1{font-size:1.3rem;margin:0 0 .2rem}"
           "h2{font-size:.8rem;text-transform:uppercase;letter-spacing:.08em;"
           "color:#666;margin:1.2rem 0 .5rem;border-bottom:1px solid #ddd;"
           "padding-bottom:.3rem}"
           "label{display:block;font-weight:600;font-size:.9rem;margin-top:.8rem}"
           "input,select{width:100%;padding:.5rem;font-size:1rem;"
           "border:1px solid #bbb;border-radius:6px;box-sizing:border-box;"
           "background:#fff}"
           ".h{font-size:.8rem;color:#666;margin:.25rem 0 0}"
           "button{margin-top:1.5rem;width:100%;padding:.7rem;font-size:1rem;"
           "font-weight:600;border:0;border-radius:6px;background:#275673;"
           "color:#fff}"
           "button.sec{background:#e3e6ea;color:#243}"
           "button.mini{margin-top:.4rem;width:auto;padding:.35rem .7rem;"
           "font-size:.85rem;background:#e3e6ea;color:#243}"
           ".nav{display:flex;gap:.6rem}"
           ".dots{display:flex;gap:.35rem;margin:.6rem 0 1rem}"
           ".dot{height:4px;flex:1;border-radius:2px;background:#d7dbe0}"
           ".dot.on{background:#275673}"
           ".step{display:none}.step.on{display:block}"
           ".sub{color:#666;font-size:.85rem;margin:.1rem 0 0}"
           ".pinout{font-size:.8rem;margin:.3rem 0 0;font-weight:600}"
           ".ok{color:#166534}.warn{color:#92400e}.bad{color:#b91c1c}"
           ".nets{margin:.5rem 0 0;max-height:11rem;overflow:auto;"
           "border:1px solid #ddd;border-radius:6px;background:#fff}"
           ".net{display:flex;justify-content:space-between;gap:.5rem;"
           "padding:.5rem .6rem;border-bottom:1px solid #eee;cursor:pointer}"
           ".net:last-child{border-bottom:0}"
           ".net b{font-weight:600}.net span{color:#666;font-size:.8rem}"
           ".board{display:flex;gap:.4rem;justify-content:center;margin:.6rem 0;"
           "background:#fff;border:1px solid #ddd;border-radius:10px;"
           "padding:.6rem .4rem}"
           ".col{display:flex;flex-direction:column;gap:.2rem;flex:1}"
           ".col.r{align-items:flex-end}"
           ".pad{font-size:.72rem;font-family:ui-monospace,monospace;"
           "padding:.15rem .35rem;border-radius:4px;border:1px solid #d7dbe0;"
           "background:#f2f4f6;white-space:nowrap}"
           ".pad.free{background:#e7f6ec;border-color:#b7e0c4}"
           ".pad.caution{background:#fdf3e3;border-color:#f0d9a8}"
           ".pad.never{background:#fdeaea;border-color:#f2bcbc;color:#7f1d1d}"
           ".pad.used{outline:2px solid #275673;font-weight:700}"
           ".mid{flex:0 0 4.5rem;background:#31404d;border-radius:6px;"
           "color:#cfd8e0;font-size:.7rem;text-align:center;padding:.4rem .2rem}"
           ".legend{font-size:.72rem;color:#666;text-align:center;margin:.2rem 0 0}"
           ".err{background:#fdeaea;border:1px solid #f2bcbc;color:#7f1d1d;"
           "padding:.5rem .6rem;border-radius:6px;font-size:.85rem;margin:.6rem 0}"
           "</style></head><body>"
           "<h1>Sensor node setup</h1>");

    p += F("<p class=\"sub\" id=\"sub\">Step 1 of 4 &middot; Network</p>"
           "<div class=\"dots\"><i class=\"dot on\"></i><i class=\"dot\"></i>"
           "<i class=\"dot\"></i><i class=\"dot\"></i></div>"
           "<form method=\"POST\" action=\"/save\" id=\"f\">");

    // ── Step 1 — the network ────────────────────────────────────────────────
    p += F("<section class=\"step on\" data-step=\"1\"><h2>WiFi</h2>");
    row(p, "ssid", "Network name", esc(s.ssid), "text", "");
    p += F("<button type=\"button\" class=\"mini\" id=\"scan\">Scan for networks"
           "</button><div class=\"nets\" id=\"nets\" style=\"display:none\"></div>"
           "<p class=\"h\" id=\"scanmsg\"></p>");
    // Rendered ONLY on the access-point portal, where seeing it is a
    // convenience for whoever is standing in front of the node and had to know
    // the AP password to get here. On the LAN portal the field is blank and an
    // empty submission keeps what is stored: that page is reachable by
    // everything on the network for as long as the node is up, and putting the
    // home WiFi passphrase in its HTML source would hand it to all of them.
    if (s_background) {
        row(p, "pass", "Password", String(), "password",
            s.pass[0] ? "Leave empty to keep the saved password."
                      : "Required.");
    } else {
        row(p, "pass", "Password", esc(s.pass), "password",
            "<input type=\"checkbox\" style=\"width:auto;display:inline-block;margin-right:5px;vertical-align:middle\" "
            "onclick=\"document.getElementById('pass').type = this.checked ? 'text' : 'password'\">"
            "<span style=\"vertical-align:middle\">Show password</span>");
    }
    p += F("</section>");

    // ── Step 2 — where the readings go ──────────────────────────────────────
    p += F("<section class=\"step\" data-step=\"2\"><h2>Collector</h2>");
    row(p, "host", "IP address", esc(s.host), "text",
        "The ESP32 collector, on THIS network — the node posts to it. "
        "Give it a DHCP reservation on your router.");
    row(p, "port", "Port", String(s.port), "number", "");
    // Same rule as the passphrase: a shared secret, shown where only someone
    // already on the node's own AP can see it, blank on the LAN.
    if (s_background) {
        row(p, "token", "Ingest token", String(), "password",
            s.token[0] ? "Leave empty to keep the saved token."
                       : "Must match INGEST_TOKEN on the collector.");
    } else {
        row(p, "token", "Ingest token", esc(s.token), "text",
            "Must match INGEST_TOKEN on the collector.");
    }
    row(p, "buser", "Basic auth user", esc(s.basicUser), "text",
        "Two jobs, one password: sent to the collector when it was built with "
        "WEB_BASIC_AUTH_ENABLED, AND required to open this page over the LAN. "
        "Leave both empty and the LAN portal does not start at all.");
    row(p, "bpass", "Basic auth password", String(), "password",
        s.basicPass[0] ? "Leave empty to keep the saved password." : "");
    p += F("</section>");

    // ── Step 3 — the board and its pins ─────────────────────────────────────
    p += F("<section class=\"step\" data-step=\"3\"><h2>Board</h2>"
           "<label for=\"board\">Which board is this</label>"
           "<select id=\"board\" name=\"board\">");
    for (uint8_t i = 0; i < BOARD_COUNT; i++) {
        p += F("<option value=\""); p += (int)i; p += F("\"");
        if (s.board == i) p += F(" selected");
        p += F(">"); p += BOARDS[i].name; p += F("</option>");
    }
    p += F("</select>"
           "<div class=\"board\" id=\"diagram\"></div>"
           "<p class=\"legend\">green: free &middot; amber: usable with care "
           "&middot; red: SPI flash, never</p>");

    // Only sensors actually compiled into this build get pin fields. Offering
    // a 1-Wire pin on a build with no DS18B20 driver would be a control that
    // does nothing, which is worse than no control at all.
#if defined(NODE_SENSOR_BMX280) || defined(NODE_SENSOR_BME688) || \
    defined(NODE_SENSOR_BH1750) || defined(NODE_SENSOR_DS18B20) || \
    defined(NODE_SENSOR_PULSE)  || defined(NODE_SENSOR_SDS011)
    p += F("<h2>Sensor pins</h2>"
           "<p class=\"h\">Type either form — <b>D6</b> or <b>12</b>. The line "
           "under each field says which pin that resolves to.</p>");
#endif

#if defined(NODE_SENSOR_BMX280) || defined(NODE_SENSOR_BME688) || \
    defined(NODE_SENSOR_BH1750)
    pinRow(p, "sda", "I2C SDA", s.i2cSda, "SDA",
           "Shared by every I2C sensor on this node.");
    pinRow(p, "scl", "I2C SCL", s.i2cScl, "SCL", "");
#endif
#if defined(NODE_SENSOR_DS18B20)
    pinRow(p, "ow", "1-Wire data", s.oneWirePin, "1-W",
           "Needs a 4.7k pull-up to 3V3.");
#endif
#if defined(NODE_SENSOR_PULSE)
    pinRow(p, "pulse", "Pulse input", s.pulsePin, "PULSE",
           PULSE_MODE_RAIN ? "Rain gauge reed switch, to GND."
                           : "Hall flow sensor signal line.");
#endif
#if defined(NODE_SENSOR_SDS011)
    pinRow(p, "sdsrx", "SDS011 RX", s.sdsRx, "RX",
           "Wire the sensor's TXD here. D0/GPIO16 will not work — it cannot "
           "raise interrupts.");
    pinRow(p, "sdstx", "SDS011 TX", s.sdsTx, "TX", "");
#endif
    p += F("</section>");

    // ── Step 4 — identity, and what the collector listens for ───────────────
    p += F("<section class=\"step\" data-step=\"4\"><h2>This node</h2>");
    row(p, "nodeid", "Node id", esc(s.nodeId), "text",
        "The collector pairs on THIS STRING, not on an address: it must match "
        "the <b>node</b> field of a sensor of type <b>remote</b> there, "
        "exactly, up to 16 characters.");
    row(p, "interval", "Post interval (seconds)",
        String(s.intervalMs / 1000UL), "number", "");
    row(p, "alt", "Altitude (m)", String(s.altitudeM, 1), "number",
        "Used to report sea-level pressure. 0 sends station pressure only.");
    p += F("<h2>Summary</h2><div id=\"sum\" class=\"h\"></div></section>");

    p += F("<div class=\"nav\">"
           "<button type=\"button\" class=\"sec\" id=\"back\">Back</button>"
           "<button type=\"button\" id=\"next\">Next</button>"
           "<button type=\"submit\" id=\"save\" style=\"display:none\">"
           "Save and restart</button></div></form>");

    // ── The tables the page needs, straight from the C++ ones ───────────────
    //
    // Emitted rather than duplicated in JS: the validation that refuses a save
    // and the colours on the diagram have to agree, and the way to guarantee
    // that is to have one table. It is a few hundred bytes on the wire.
    p += F("<script>var RISK={");
    for (uint8_t g = 0; g <= MAX_GPIO; g++) {
        if (g) p += F(",");
        p += (int)g; p += F(":[");
        p += (int)FACTS[g].risk;
        p += F(",\"");
        p += FPSTR(FACTS[g].why);
        p += F("\"]");
    }
    p += F("};var DL={");
    for (uint8_t i = 0; i < LABEL_COUNT; i++) {
        if (i) p += F(",");
        p += F("\""); p += LABELS[i].label; p += F("\":");
        p += (int)LABELS[i].gpio;
    }
    p += F("};var BOARDS=[");
    for (uint8_t i = 0; i < BOARD_COUNT; i++) {
        if (i) p += F(",");
        p += F("{n:\""); p += BOARDS[i].name;
        p += F("\",l:\""); p += BOARDS[i].left;
        p += F("\",r:\""); p += BOARDS[i].right;
        p += F("\"}");
    }
    p += F("];");

    p += F(
      // ── steps ──────────────────────────────────────────────────────────
      "var st=1,MAX=4;"
      "function show(){"
        "var names=['Network','Collector','Board & pins','This node'];"
        "[].forEach.call(document.querySelectorAll('.step'),function(e){"
          "e.classList.toggle('on',+e.dataset.step===st);});"
        "[].forEach.call(document.querySelectorAll('.dot'),function(e,i){"
          "e.classList.toggle('on',i<st);});"
        "document.getElementById('sub').textContent="
          "'Step '+st+' of '+MAX+' \\u00b7 '+names[st-1];"
        "document.getElementById('back').style.visibility=st>1?'visible':'hidden';"
        "document.getElementById('next').style.display=st<MAX?'':'none';"
        "document.getElementById('save').style.display=st<MAX?'none':'';"
        "if(st===4)summary();"
        "window.scrollTo(0,0);}"
      "function need(id,msg){var v=(document.getElementById(id)||{}).value||'';"
        "if(v.trim())return true;alert(msg);return false;}"
      "function stepOk(){"
        "if(st===1)return need('ssid','Pick or type a network name first.');"
        "if(st===2)return need('host','The collector\\u2019s IP address is required.');"
        "if(st===3)return pinsOk();"
        "return true;}"
      "document.getElementById('next').onclick=function(){"
        "if(!stepOk())return;if(st<MAX){st++;show();}};"
      "document.getElementById('back').onclick=function(){"
        "if(st>1){st--;show();}};"
      // ── pins ───────────────────────────────────────────────────────────
      "function dName(g){for(var k in DL){if(DL[k]===g&&k.charAt(0)==='D'"
        "&&k.length===2)return k;}return '';}"
      "function resolve(v){v=(v||'').trim().toUpperCase();if(!v)return -1;"
        "if(DL.hasOwnProperty(v))return DL[v];"
        "if(v.indexOf('GPIO')===0)v=v.slice(4);"
        "if(!/^[0-9]+$/.test(v))return -1;var n=+v;"
        "return (n>=0&&n<=16)?n:-1;}"
      "function pinFields(){return document.querySelectorAll('input.pin');}"
      "function paintPin(el){"
        "var out=document.getElementById('o_'+el.id);if(!out)return -1;"
        "var g=resolve(el.value);"
        "if(g<0){out.className='pinout bad';"
          "out.textContent='not a pin on this chip \\u2014 use D0\\u2013D8 or a GPIO number 0\\u201316';"
          "return -1;}"
        "var r=RISK[g],d=dName(g),tag='GPIO'+g+(d?' ('+d+')':'');"
        "if(r[0]===2){out.className='pinout bad';out.textContent=tag+' \\u2014 '+r[1];}"
        "else if(r[0]===1){out.className='pinout warn';out.textContent=tag+' \\u2014 '+r[1];}"
        "else{out.className='pinout ok';out.textContent=tag+' \\u2014 free';}"
        "return g;}"
      "function pinsOk(){var bad=[];"
        "[].forEach.call(pinFields(),function(el){var g=paintPin(el);"
          "if(g<0||RISK[g][0]===2)bad.push(el.previousElementSibling"
            "?el.previousElementSibling.textContent:el.id);});"
        "if(bad.length){alert('These pins cannot be used: '+bad.join(', ')+"
          "'.\\nRed means the SPI flash bus \\u2014 the node would boot-loop.');"
          "return false;}"
        "var seen={},dup=[];"
        "[].forEach.call(pinFields(),function(el){var g=resolve(el.value);"
          "if(seen[g])dup.push('GPIO'+g);seen[g]=1;});"
        "if(dup.length){return confirm('Two sensors share '+dup.join(', ')+"
          "'. Save anyway?');}"
        "return true;}"
      // ── the board diagram ──────────────────────────────────────────────
      "function pads(names){var used={};"
        "[].forEach.call(pinFields(),function(el){var g=resolve(el.value);"
          "if(g>=0)used[g]=(used[g]?used[g]+'/':'')+el.dataset.role;});"
        "return names.split(',').map(function(n){"
          "var g=DL.hasOwnProperty(n)?DL[n]:(/^GPIO[0-9]+$/.test(n)?+n.slice(4):-1);"
          "var cls='pad',txt=n;"
          "if(g>=0){var r=RISK[g][0];"
            "cls+=r===2?' never':(r===1?' caution':' free');"
            "txt=n+(/^GPIO/.test(n)?'':' \\u00b7 '+g);"
            "if(used[g]){cls+=' used';txt+=' \\u2190 '+used[g];}}"
          "return '<span class=\"'+cls+'\" title=\"'+(g>=0?RISK[g][1]:'')+'\">'"
            "+txt+'</span>';}).join('');}"
      "function diagram(){var b=BOARDS[+document.getElementById('board').value||0];"
        "document.getElementById('diagram').innerHTML="
          "'<div class=\"col\">'+pads(b.l)+'</div>'"
          "+'<div class=\"mid\">'+b.n+'</div>'"
          "+'<div class=\"col r\">'+pads(b.r)+'</div>';}"
      "document.getElementById('board').onchange=diagram;"
      "[].forEach.call(pinFields(),function(el){"
        "el.addEventListener('input',function(){paintPin(el);diagram();});"
        "paintPin(el);});"
      "diagram();"
      // ── network scan ───────────────────────────────────────────────────
      "var tries=0;"
      "function poll(){fetch('/scan').then(function(r){return r.json();})"
        ".then(function(d){"
          "var msg=document.getElementById('scanmsg');"
          "if(d.scanning){if(++tries<15){setTimeout(poll,700);}"
            "else{msg.textContent='Scan timed out.';}return;}"
          "if(d.error){msg.textContent=d.error;return;}"
          "var box=document.getElementById('nets');"
          "if(!d.networks||!d.networks.length){msg.textContent="
            "'No networks found. Type the name by hand.';return;}"
          "msg.textContent='Tap one to use it.';"
          "box.style.display='';"
          // An SSID is 32 arbitrary bytes chosen by whoever is in radio
          // range, and this page renders the ingest token and the WiFi
          // passphrase into its own fields. Dropping a name straight into
          // innerHTML would let a neighbouring AP named
          // "<img src=x onerror=...>" run script in the portal's origin the
          // moment somebody presses Scan. Escaped for BOTH destinations —
          // the attribute and the text — not just the attribute.
          "function esc(t){return String(t).replace(/[&<>\"']/g,function(c){"
            "return {'&':'&amp;','<':'&lt;','>':'&gt;','\"':'&quot;',"
            "\"'\":'&#39;'}[c];});}"
          "box.innerHTML=d.networks.map(function(n){"
            "return '<div class=\"net\" data-s=\"'+esc(n.ssid)"
              "+'\"><b>'+esc(n.ssid)+'</b><span>'+(n.secure?'\\ud83d\\udd12 ':'')"
              "+(+n.rssi)+' dBm</span></div>';}).join('');"
          "[].forEach.call(box.querySelectorAll('.net'),function(e){"
            "e.onclick=function(){document.getElementById('ssid').value=e.dataset.s;"
              "document.getElementById('pass').focus();};});})"
        ".catch(function(){document.getElementById('scanmsg').textContent="
          "'Scan failed.';});}"
      "document.getElementById('scan').onclick=function(){"
        "tries=0;document.getElementById('scanmsg').textContent='Scanning\\u2026';"
        "fetch('/scan_start').then(function(){setTimeout(poll,900);})"
        ".catch(function(){document.getElementById('scanmsg').textContent="
          "'Scan failed to start.';});};"
      // ── summary ────────────────────────────────────────────────────────
      "function summary(){var g=function(i){var e=document.getElementById(i);"
          "return e?e.value:'';};"
        "var rows=[['Network',g('ssid')],['Collector',g('host')+':'+g('port')],"
          "['Node id',g('nodeid')||'(required)'],"
          "['Posts every',g('interval')+' s']];"
        "[].forEach.call(pinFields(),function(el){"
          "var gp=resolve(el.value);"
          "rows.push([el.dataset.role,gp<0?'invalid':'GPIO'+gp"
            "+(dName(gp)?' ('+dName(gp)+')':'')]);});"
        "document.getElementById('sum').innerHTML=rows.map(function(r){"
          "return '<div><b>'+r[0]+':</b> '+String(r[1]).replace(/</g,'&lt;')"
            "+'</div>';}).join('');}"
      "show();"
      "</script></body></html>");

    p.end();
}

// ---------------------------------------------------------------------------
// GET /scan_start, GET /scan — the network picker
// ---------------------------------------------------------------------------
//
// Two endpoints, not one, for the same reason the collector splits them: an
// ESP8266 scan takes two seconds or more, and doing it inside the request
// would hold the only web-server thread the portal has — on the AP, that is
// the phone's own connection timing out. scanNetworks(true) returns
// immediately and /scan reports progress.
/// True while the scan has the radio widened from AP to AP+STA, so the mode
/// can be put back. The ESP8266 has ONE radio: the softAP and the station
/// share a channel, and a station that associates drags the AP onto its
/// channel — disassociating the phone that is standing in the portal. Leaving
/// AP_STA on for the rest of the session leaves that trap armed.
static bool s_widenedForScan = false;

static void handleScanStart() {
    if (!backgroundAuthOk()) return;
    // Widen AP → AP_STA so the access point the phone is sitting on stays up
    // while the radio scans. In the background portal the node is already in
    // STA and can scan as it is.
    if (WiFi.getMode() == WIFI_AP) {
        WiFi.mode(WIFI_AP_STA);
        s_widenedForScan = true;
        // Scanning does not need an association, and an association is the
        // thing that would move the AP's channel. portalRun() can be reached
        // from setup() before anything calls WiFi.disconnect(), so the SDK
        // may still hold credentials from flash and auto-connect on its own
        // the moment the station interface comes up.
        WiFi.disconnect(false);
    }
    WiFi.scanDelete();
    WiFi.scanNetworks(true);
    s_http.send(200, "text/plain", "OK");
}

/// Put the radio back the way the portal found it, once the scan is over.
static void narrowAfterScan() {
    if (!s_widenedForScan) return;
    s_widenedForScan = false;
    WiFi.mode(WIFI_AP);
}

static void handleScanResult() {
    if (!backgroundAuthOk()) return;

    JsonDocument doc;
    const int n = WiFi.scanComplete();
    if (n == WIFI_SCAN_RUNNING) {
        doc["scanning"] = true;
    } else if (n == WIFI_SCAN_FAILED) {
        doc["error"] = "Scan failed.";
        narrowAfterScan();
    } else {
        JsonArray nets = doc["networks"].to<JsonArray>();
        // Twelve is more than any hallway shows on a phone, and the JSON is
        // built in RAM on a part that has little of it.
        for (int i = 0; i < n && i < 12; i++) {
            JsonObject net = nets.add<JsonObject>();
            net["ssid"]   = WiFi.SSID(i);
            net["rssi"]   = WiFi.RSSI(i);
            net["secure"] = WiFi.encryptionType(i) != ENC_TYPE_NONE;
        }
        WiFi.scanDelete();
        narrowAfterScan();
    }

    String out;
    serializeJson(doc, out);
    s_http.send(200, "application/json", out);
}

// ---------------------------------------------------------------------------
static void refuse(const String& why) {
    String p = F("<!DOCTYPE html><meta charset=\"utf-8\">"
                 "<body style=\"font-family:sans-serif;max-width:32rem;"
                 "margin:2rem auto;padding:1rem\">"
                 "<p style=\"background:#fdeaea;border:1px solid #f2bcbc;"
                 "color:#7f1d1d;padding:.6rem;border-radius:6px\">");
    p += why;
    p += F("</p><p><a href=\"/\">Back to the form</a> &mdash; nothing was saved.</p>");
    s_http.send(400, "text/html", p);
}

static void handleSave() {
    // Checked HERE too, not only on the form. A POST does not have to come from
    // a page this device served, and this is the handler that rewrites the
    // node's collector address and then restarts it.
    if (!backgroundAuthOk()) return;

    // Build into a COPY. Writing straight into the caller's settings would
    // leave them corrupted on the validation and write-failure paths below:
    // portalRun() promises to return with `s` unmodified, and a node that
    // fell back to retrying its saved network would instead be running
    // against a half-applied config from RAM until the next power cycle,
    // even though flash still holds a good one.
    NodeSettings s = *s_target;

    auto text = [&](const char* field, char* dst, size_t len) {
        if (!s_http.hasArg(field)) return;
        strncpy(dst, s_http.arg(field).c_str(), len - 1);
        dst[len - 1] = '\0';
    };
    // An empty password field means "keep the saved one", so it must not
    // overwrite. Clearing a password therefore needs an erase, which is the
    // right trade: accidentally blanking WiFi credentials from a phone
    // keyboard is far likelier than deliberately wanting an open network.
    auto secret = [&](const char* field, char* dst, size_t len) {
        if (!s_http.hasArg(field)) return;
        const String v = s_http.arg(field);
        if (v.length() == 0) return;
        strncpy(dst, v.c_str(), len - 1);
        dst[len - 1] = '\0';
    };

    text  ("ssid",   s.ssid,      sizeof(s.ssid));
    // secret(), not text(). The LAN form serves these fields blank, so a plain
    // copy would wipe the passphrase and the token on every save from it. On
    // the AP form the fields arrive pre-filled, so "empty means keep" only
    // costs the ability to blank them there — which an erase already covers,
    // and which is far rarer than clearing a field by accident on a phone.
    secret("pass",   s.pass,      sizeof(s.pass));
    text  ("host",   s.host,      sizeof(s.host));
    secret("token",  s.token,     sizeof(s.token));
    text  ("buser",  s.basicUser, sizeof(s.basicUser));
    secret("bpass",  s.basicPass, sizeof(s.basicPass));
    text  ("nodeid", s.nodeId,    sizeof(s.nodeId));

    if (s_http.hasArg("port")) {
        const long v = s_http.arg("port").toInt();
        s.port = (v > 0 && v <= 65535) ? (uint16_t)v : 80;
    }
    if (s_http.hasArg("interval")) {
        long secs = s_http.arg("interval").toInt();
        // A node posting every second would hit the collector's rate limiter
        // and fill its ingest table with churn; an hour is past the point
        // where a "live" outdoor reading means anything.
        if (secs < 10)   secs = 10;
        if (secs > 3600) secs = 3600;
        s.intervalMs = (uint32_t)secs * 1000UL;
    }
    if (s_http.hasArg("alt")) {
        s.altitudeM = s_http.arg("alt").toFloat();
    }
    if (s_http.hasArg("board")) {
        const long v = s_http.arg("board").toInt();
        if (v >= 0 && v < BOARD_COUNT) s.board = (uint8_t)v;
    }

    // GPIO numbers — the silkscreen accepted as well, and the flash bus
    // refused outright.
    //
    // The refusal is the point of this whole pass. The form used to take any
    // number from 0 to 16 and write it, so "D6" typed as 6 put I2C on the
    // flash clock and the node boot-looped on a watchdog reset with nothing
    // readable on the serial line. There is no wiring that makes GPIO6-11
    // work, so there is no "are you sure" for it either: the save is rejected
    // and the settings on flash stay as they were.
    const char* badField = nullptr;
    int         badPin   = -1;
    auto pin = [&](const char* field, uint8_t& dst) {
        if (badField) return;                      // first refusal wins
        if (!s_http.hasArg(field)) return;
        const int g = NodePins::resolve(s_http.arg(field));
        if (g < 0 || NodePins::riskOf(g) == RISK_NEVER) {
            badField = field;
            badPin   = g;
            return;
        }
        dst = (uint8_t)g;
    };
    pin("sda",   s.i2cSda);
    pin("scl",   s.i2cScl);
    pin("ow",    s.oneWirePin);
    pin("pulse", s.pulsePin);
    pin("sdsrx", s.sdsRx);
    pin("sdstx", s.sdsTx);

    if (badField) {
        String why = F("<b>");
        why += badField;
        why += F("</b>: ");
        if (badPin < 0) {
            why += F("not a pin on this chip. Use a silkscreen label "
                     "(D0&ndash;D8) or a GPIO number from 0 to 16.");
        } else {
            why += F("GPIO");
            why += badPin;
            why += F(" is the ");
            why += FPSTR(FACTS[badPin].why);
            why += F(". Note that <b>D6 is GPIO12</b>, not GPIO6 &mdash; the "
                     "silkscreen and the GPIO numbers are different scales.");
        }
        Serial.printf("[portal] refused %s = \"%s\"\n",
                      badField, s_http.arg(badField).c_str());
        refuse(why);
        return;
    }

    if (!s.isComplete()) {
        s_http.send(400, "text/html",
                    F("<!DOCTYPE html><meta charset=\"utf-8\">"
                      "<p style=\"font-family:sans-serif\">A network name and a "
                      "collector address are both required. "
                      "<a href=\"/\">Back</a></p>"));
        return;
    }

    if (!settingsSave(s)) {
        s_http.send(500, "text/html",
                    F("<!DOCTYPE html><meta charset=\"utf-8\">"
                      "<p style=\"font-family:sans-serif\">Could not write to "
                      "the filesystem. <a href=\"/\">Back</a></p>"));
        return;
    }

    *s_target = s;
    s_http.send(200, "text/html",
                F("<!DOCTYPE html><meta charset=\"utf-8\">"
                  "<p style=\"font-family:sans-serif\">Saved. Restarting &mdash; "
                  "this network will disappear.</p>"));
    s_saved = true;
}

/// Both portals answer the same four routes. Registered once — see
/// s_routesBound above for what re-registering used to cost.
static void bindRoutes() {
    if (s_routesBound) return;
    s_http.on("/", handleRoot);
    s_http.on("/save", HTTP_POST, handleSave);
    s_http.on("/scan_start", handleScanStart);
    s_http.on("/scan", handleScanResult);
    s_http.onNotFound(handleRoot);
    s_routesBound = true;
}

bool portalRun(NodeSettings& s, uint32_t timeoutMs) {
    s_target     = &s;
    s_saved      = false;
    s_background = false;   // the AP portal: secrets shown, no auth gate

    char ap[32];
    // The MAC suffix keeps two nodes in one house apart on the air.
    snprintf(ap, sizeof(ap), "esp-node-%04X",
             (uint16_t)(ESP.getChipId() & 0xFFFF));

    WiFi.mode(WIFI_AP);
    WiFi.softAP(ap, PORTAL_AP_PASS);
    delay(100);

    const IPAddress ip = WiFi.softAPIP();
    Serial.printf("[portal] \"%s\" up at http://%s  (pass: %s)\n",
                  ap, ip.toString().c_str(), PORTAL_AP_PASS);
    if (timeoutMs) Serial.printf("[portal] closing in %lu s\n",
                                 (unsigned long)(timeoutMs / 1000));

    // Answer every DNS query with our own address so phones show the
    // "sign in to network" prompt instead of leaving the user to discover
    // 192.168.4.1 on their own.
    s_dns.setErrorReplyCode(DNSReplyCode::NoError);
    s_dns.start(53, "*", ip);

    bindRoutes();
    s_http.begin();

    uint32_t start     = millis();
    bool     hadClient = false;
    while (true) {
        s_dns.processNextRequest();
        s_http.handleClient();

        if (s_saved) break;

        // THE CLOCK STOPS WHILE SOMEBODY IS CONNECTED, and restarts when they
        // leave. Both halves matter, and an earlier rewrite of this file kept
        // the comment while dropping the assignments — which is worse than
        // never having had them, because the behaviour it describes is the
        // reason the timeout is survivable.
        //
        // The timeout exists so a node does not sit in AP mode forever after a
        // 3 am router reboot nobody witnessed. A station associated to the
        // softAP is the opposite of that: direct evidence a human is standing
        // there, mid-configuration. Without the reset the window closes under
        // them — joining the AP, waiting out the phone's "no internet, stay
        // connected?" prompt and working through four steps is comfortably
        // more than five minutes from boot, and a phone that roams off the
        // no-internet AP for one loop iteration would take the portal with it.
        const bool client = WiFi.softAPgetStationNum() > 0;
        if (client) {
            start = millis();          // hold the window open
            hadClient = true;
        } else if (hadClient) {
            // They left without saving. A fresh window rather than a stale
            // count, so a reconnect gets the whole five minutes again.
            hadClient = false;
            start = millis();
        }

        if (timeoutMs && !client && (millis() - start) > timeoutMs) {
            Serial.println("[portal] timed out, retrying the saved network");
            break;
        }
        delay(5);   // feeds the softAP task; yield() alone starves it here
    }

    if (s_saved) {
        // Let the browser collect the confirmation page before the AP drops.
        const uint32_t until = millis() + 1500;
        while ((int32_t)(millis() - until) < 0) {
            s_http.handleClient();
            delay(5);
        }
    }

    s_http.stop();
    s_dns.stop();
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_OFF);

    return s_saved;
}

bool portalStartBackground(NodeSettings& s) {
    // FAILS CLOSED. Without credentials there is nothing between this form and
    // everything on the network, and the form rewrites the node's collector
    // address and restarts it. Refusing is a feature the user has to turn on by
    // setting a password, which is the right way round; serving it anyway would
    // be a remote-configuration interface nobody asked to expose.
    if (s.basicUser[0] == '\0' || s.basicPass[0] == '\0') {
        Serial.println(F("[portal] background server NOT started: set a basic-auth "
                         "user and password in the setup portal to enable "
                         "configuration over the LAN"));
        return false;
    }

    s_target     = &s;
    s_saved      = false;
    s_background = true;

    bindRoutes();
    s_http.begin();
    Serial.printf("[portal] background configuration server on http://%s/ "
                  "(password protected)\n", WiFi.localIP().toString().c_str());
    return true;
}

void portalHandleClient() {
    s_http.handleClient();
    if (s_saved) {
        // Let the browser collect the confirmation page
        const uint32_t until = millis() + 1500;
        while ((int32_t)(millis() - until) < 0) {
            s_http.handleClient();
            delay(5);
        }
        Serial.println("[portal] Settings saved via background server, restarting");
        ESP.restart();
    }
}
