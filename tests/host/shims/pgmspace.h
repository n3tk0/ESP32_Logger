#pragma once
// Host shim for <pgmspace.h>: on the host, flash and RAM are one address
// space, so PROGMEM means nothing. Generated headers such as
// src/modules/ModuleSchemas.h include this directly.
#ifndef PROGMEM
#define PROGMEM
#endif
