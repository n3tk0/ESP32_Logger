#pragma once
// src/web/StringResponse.h — AsyncBasicResponse, but it takes its String by
// move instead of copying it, so a small reply is in the heap once.
//
// A basic response writes its body straight from the String into the TCP
// send buffer: no 2872 B send buffer of its own, as the stream, chunked and
// callback responses have. The write path is the library's own
// (AsyncBasicResponse::write_send_buffs, ESPAsyncWebServer 3.x).
#include <ESPAsyncWebServer.h>

class StringResponse : public AsyncWebServerResponse {
public:
    StringResponse(int code, const char* type, String&& body) : _body(std::move(body)) {
        _code = code;
        _contentType = type;
        _contentLength = _body.length();
        addHeader("Connection", "close", false);
    }
    void _respond(AsyncWebServerRequest* request) override {
        _state = RESPONSE_HEADERS;
        _assembleHead(_head, request->version());
        _ack(request, 0, 0);
    }
    bool _sourceValid() const override { return true; }
    size_t _ack(AsyncWebServerRequest* request, size_t len, uint32_t) override {
        _ackedLength += len;
        size_t out = 0;
        if (_state == RESPONSE_HEADERS) {
            const size_t w = request->client()->add(_head.c_str() + _headDone, _head.length() - _headDone);
            _writtenLength += w;
            _headDone += w;
            if (_headDone < _head.length()) {
                if (!request->client()->send()) { request->client()->close(); return 0; }
                return w;
            }
            _state = RESPONSE_CONTENT;
            out += w;
            _head = String();
        }
        if (_state == RESPONSE_CONTENT) {
            const size_t w = request->client()->write(_body.c_str() + _sentLength, _body.length() - _sentLength);
            _writtenLength += w;
            _sentLength += w;
            out += w;
            if (_sentLength >= _body.length()) _state = RESPONSE_END;
        }
        if (_state == RESPONSE_WAIT_ACK) _state = RESPONSE_END;
        return out;
    }
private:
    String _body;
    String _head;
    size_t _headDone = 0;
};
