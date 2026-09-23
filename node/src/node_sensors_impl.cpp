// The shared sensor layer (node_common/NodeSensors.cpp), compiled into this
// firmware. PlatformIO builds only what is under src/, and node_common/ is
// shared with node_espnow/, which compiles it through a wrapper of its own.
// Reached through the project's -I.. (the repository root).
//
// The two library headers are named here as well because PlatformIO's
// library finder scans src/ and does not follow an include out of the
// project: without them it never adds Wire and SoftwareSerial to the build,
// and the shared file fails with "Wire.h: No such file or directory".
#include <Wire.h>
#include <SoftwareSerial.h>
#include "node_common/NodeSensors.cpp"

// ---------------------------------------------------------------------------
// The table-reading nodecfg calls live in this translation unit too — see
// NodeCfgTables.h for why.
// ---------------------------------------------------------------------------
#include "NodeCfgTables.h"
#include "src/nodecfg/NodeConfigJson.h"

bool nodeValidate(const nodecfg::NodeConfig& c, nodecfg::Validation& v) {
    return nodecfg::validate(c, v);
}

void nodeEncodeCaps(JsonObject out) {
    nodecfg::encodeCaps(nodecfg::Transport::Wifi, nodecfg::Hw::Esp8266, out);
}
