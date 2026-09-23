// The shared sensor layer (node_common/NodeSensors.cpp), compiled into this
// firmware. PlatformIO builds only what is under src/, and node_common/ is
// shared with node/, which compiles it through a wrapper of its own.
// Reached through the project's -I.. (the repository root).
//
// Wire is named here as well because PlatformIO's library finder scans src/
// and does not follow an include out of the project: without it the shared
// file fails with "Wire.h: No such file or directory".
#include <Wire.h>
#include "node_common/NodeSensors.cpp"
