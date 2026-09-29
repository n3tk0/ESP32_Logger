#pragma once
// Host shim for <ESPAsyncWebServer.h>: the type names that Globals.h
// declares (`extern AsyncWebServer server;`) and nothing else. No host test
// serves a request; code that does is not compiled here.
class AsyncWebServer;
class AsyncWebServerRequest;
