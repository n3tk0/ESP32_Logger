// src/web/FormArgs.cpp — see FormArgs.h.
#include "FormArgs.h"

#include <string.h>

String queryOr(AsyncWebServerRequest* r, const char* key, const String& def) {
    const String* v = queryArg(r, key);
    return v ? *v : def;
}

bool formCopy(AsyncWebServerRequest* r, const char* key, char* dst, size_t n) {
    const String* v = formArg(r, key);
    if (!v || n == 0) return false;
    strncpy(dst, v->c_str(), n - 1);
    dst[n - 1] = '\0';
    return true;
}

const char* formParam(void* ctx, const char* key) {
    const String* v = formArg(static_cast<AsyncWebServerRequest*>(ctx), key);
    return v ? v->c_str() : nullptr;
}
