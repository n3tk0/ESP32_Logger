// ============================================================================
// src/web/KindleChartBmp.h — the 24-hour trend chart, as a 4-bit greyscale BMP
//
// SEPARATED FROM KindleDashboard.cpp FOR THE SAME REASON NodeTable.h IS
// SEPARATED FROM EspNowIngest.cpp: there is no Arduino in here, no web server
// and no filesystem — only arithmetic over a TrendRing snapshot and bytes
// written into a caller's buffer. That makes it reachable from
// tests/host/test_kindle_chart_bmp.cpp, and the parts of this feature most
// likely to be wrong are exactly the parts a host can check: the header
// layout, the bottom-up row order, the 4-bit packing, and the guarantee that
// the same image comes out no matter how the transport chops it up.
//
// It matters because the streamer serves this over the network a chunk at a
// time, and the first version of that code wrote a 118-byte header into
// whatever buffer it was handed without looking at the size.
// ============================================================================
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>

#include "../pipeline/TrendRing.h"

// ============================================================================
// 4-bit grayscale BMP streaming for /kindle/graph.bmp
// ============================================================================
// BMP format: 14-byte file header + 40-byte info header + 64-byte palette
// (16 entries × 4 bytes) + pixel data (bottom-up, 4-bit packed).
//
// The ESP32 cannot hold the full image in RAM (even at 560×200 it would be
// 56 KB), so we stream it chunk by chunk using AsyncWebServer's chunked
// response. Each chunk renders a few rows into a small stack buffer.

// 16-shade greyscale palette, matched to the CSS values in the HTML dashboard.
// Index 0 = black (#000), index 15 = white (#fff).
constexpr uint8_t BMP_PALETTE[16] = {
    0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
    0x88, 0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF
};

// Map a CSS hex grey like 0xD5 to a palette index.
inline uint8_t cssGrey(uint8_t hex) { return hex >> 4; }

/// 14-byte file header + 40-byte info header + 16 palette entries of 4 bytes.
/// Named because the streaming filler indexes into it and a literal 118 in
/// three places is how an off-by-one gets in.
constexpr size_t KD_BMP_HEADER_SIZE = 14 + 40 + 64;

/// The widest row the streamer has to cache: 1000 px at two pixels per byte.
constexpr size_t KD_BMP_MAX_ROW_BYTES = 500;

inline size_t writeBmpHeader(uint8_t* buf, uint16_t w, uint16_t h) {
    const uint16_t rowBytes = w / 2;  // 4-bit, w is always even
    const uint32_t pixelSize = (uint32_t)rowBytes * h;
    const uint32_t headerSize = KD_BMP_HEADER_SIZE;
    const uint32_t fileSize = headerSize + pixelSize;

    // BITMAPFILEHEADER (14 bytes)
    buf[0] = 'B'; buf[1] = 'M';
    memcpy(buf + 2, &fileSize, 4);
    memset(buf + 6, 0, 4);  // reserved
    memcpy(buf + 10, &headerSize, 4);

    // BITMAPINFOHEADER (40 bytes)
    uint32_t infoSize = 40;
    memcpy(buf + 14, &infoSize, 4);
    int32_t sw = w, sh = h;  // signed for BMP
    memcpy(buf + 18, &sw, 4);
    memcpy(buf + 22, &sh, 4);  // positive = bottom-up
    uint16_t planes = 1;
    memcpy(buf + 26, &planes, 2);
    uint16_t bpp = 4;
    memcpy(buf + 28, &bpp, 2);
    memset(buf + 30, 0, 24);  // compression=0, rest zeros

    // Palette: 16 greyscale entries (BGRA)
    for (int i = 0; i < 16; i++) {
        uint8_t v = BMP_PALETTE[i];
        buf[54 + i*4 + 0] = v;  // B
        buf[54 + i*4 + 1] = v;  // G
        buf[54 + i*4 + 2] = v;  // R
        buf[54 + i*4 + 3] = 0;  // A
    }
    return headerSize;  // 118 bytes
}

// Rendering context for the BMP chart, computed once and shared across chunks.
struct ChartBmpCtx {
    uint16_t W, H;
    uint16_t rowBytes;
    int L, R, T, B;
    float dx;
    float lo, hi, span;
    TrendRing::Hour tOut[TrendRing::HOURS];
    TrendRing::Hour tIn[TrendRing::HOURS];
    bool haveOut, haveIn;
    float yScale;  // (B - T) / span

    // Precomputed X positions for each hour
    int hourX[TrendRing::HOURS];
    // Precomputed outdoor band Y (min/max) and mean Y for each hour
    int outMinY[TrendRing::HOURS];
    int outMaxY[TrendRing::HOURS];
    int outMeanY[TrendRing::HOURS];
    int inMeanY[TrendRing::HOURS];
    bool hourValid[TrendRing::HOURS];    // outdoor has data
    bool inHourValid[TrendRing::HOURS];  // indoor has data

    // Grid line Y positions (5 lines)
    //
    // THE LINES HAVE NO LABELS, and that is a real gap rather than an
    // oversight in this struct. A gridLabel[5][8] used to be computed here and
    // was never read: nothing in renderRow() draws it, and drawing text into a
    // 4-bit BMP would need a bitmap font on the ESP32 that this firmware does
    // not carry. The Kindle draws every other string on the dashboard itself
    // with FBInk, so if the axis is to be labelled, the values belong in
    // /kindle/data next to the rest of the text and the script should place
    // them — not here. Removed rather than left in place, because a populated
    // array that no code reads reads as a feature that works.
    int gridY[5];

    void init(uint16_t width, uint16_t height) {
        W = width;
        H = height;
        rowBytes = W / 2;
        L = W * 40 / 560;   // scale margins proportionally
        R = W - W * 4 / 560;
        T = H * 10 / 200;
        B = H - H * 26 / 200;
        dx = (float)(R - L) / (float)(TrendRing::HOURS - 1);

        // Compute Y scale
        lo = 1e9f; hi = -1e9f;
        for (int i = 0; i < TrendRing::HOURS; i++) {
            if (haveOut && tOut[i].count) {
                if (tOut[i].min < lo) lo = tOut[i].min;
                if (tOut[i].max > hi) hi = tOut[i].max;
            }
            if (haveIn && tIn[i].count) {
                if (tIn[i].min < lo) lo = tIn[i].min;
                if (tIn[i].max > hi) hi = tIn[i].max;
            }
        }
        float pad = (hi - lo) * 0.06f;
        if (pad < 0.4f) pad = 0.4f;
        lo -= pad; hi += pad;
        span = hi - lo;
        if (span < 0.001f) span = 1.0f;  // safety
        yScale = (float)(B - T) / span;

        // Precompute positions
        for (int i = 0; i < TrendRing::HOURS; i++) {
            hourX[i] = L + (int)(dx * (float)i);
            hourValid[i] = haveOut && tOut[i].count > 0;
            inHourValid[i] = haveIn && tIn[i].count > 0;
            if (hourValid[i]) {
                outMinY[i] = T + (int)((hi - tOut[i].min) * yScale);
                outMaxY[i] = T + (int)((hi - tOut[i].max) * yScale);
                outMeanY[i] = T + (int)((hi - tOut[i].sum / tOut[i].count) * yScale);
            }
            if (inHourValid[i]) {
                inMeanY[i] = T + (int)((hi - tIn[i].sum / tIn[i].count) * yScale);
            }
        }

        // Grid lines
        for (int k = 0; k <= 4; k++)
            gridY[k] = T + (int)((float)(B - T) * (float)k / 4.0f);
    }

    // Render one row of pixels. `y` is in image coordinates (0=top).
    // `bmpY` is the BMP row (bottom-up: bmpY = H-1-y).
    void renderRow(uint8_t* row, int y) const {
        // Fill with white (palette index 15)
        memset(row, 0xFF, rowBytes);

        // ── Vertical grid lines (every 3h + "now") ──
        if (y >= T && y <= B) {
            uint8_t vgridGrey = cssGrey(0xD5);  // #d5d5d5 → palette ~13
            for (int i = 0; i < TrendRing::HOURS; i += 3)
                setPixel4(row, hourX[i], vgridGrey);
            setPixel4(row, hourX[TrendRing::HOURS - 1], vgridGrey);
        }

        // ── Horizontal grid lines ──
        for (int k = 0; k <= 4; k++) {
            if (y == gridY[k]) {
                uint8_t c = (k == 4) ? cssGrey(0x77) : cssGrey(0xC4);
                for (int x = L; x <= R; x++)
                    setPixel4(row, x, c);
            }
        }

        // ── Outdoor min-max band ──
        if (haveOut) {
            for (int i = 0; i < TrendRing::HOURS - 1; i++) {
                if (!hourValid[i] || !hourValid[i+1]) continue;
                // Interpolate band for this row between hour i and i+1
                int x0 = hourX[i], x1 = hourX[i+1];
                for (int x = x0; x <= x1; x++) {
                    float t = (x1 > x0) ? (float)(x - x0) / (float)(x1 - x0) : 0;
                    int minY = outMinY[i] + (int)(t * (outMinY[i+1] - outMinY[i]));
                    int maxY = outMaxY[i] + (int)(t * (outMaxY[i+1] - outMaxY[i]));
                    if (y >= maxY && y <= minY) {
                        // Inside band: fill with #d8d8d8
                        setPixel4(row, x, cssGrey(0xD8));
                    }
                    // Band outline (#8f8f8f)
                    if (y == maxY || y == minY) {
                        setPixel4(row, x, cssGrey(0x8F));
                    }
                }
            }
        }

        // ── Outdoor mean line (3px wide, #000) ──
        if (haveOut) {
            for (int i = 0; i < TrendRing::HOURS - 1; i++) {
                if (!hourValid[i] || !hourValid[i+1]) continue;
                int x0 = hourX[i], x1 = hourX[i+1];
                for (int x = x0; x <= x1; x++) {
                    float t = (x1 > x0) ? (float)(x - x0) / (float)(x1 - x0) : 0;
                    int my = outMeanY[i] + (int)(t * (outMeanY[i+1] - outMeanY[i]));
                    // 3px thick: draw if within ±1 of mean
                    if (y >= my - 1 && y <= my + 1)
                        setPixel4(row, x, 0);  // black
                }
            }
        }

        // ── Indoor mean line (2px wide, #777, dashed 7-5) ──
        if (haveIn) {
            for (int i = 0; i < TrendRing::HOURS - 1; i++) {
                if (!inHourValid[i] || !inHourValid[i+1]) continue;
                int x0 = hourX[i], x1 = hourX[i+1];
                for (int x = x0; x <= x1; x++) {
                    // Dash pattern: 7px on, 5px off
                    int phase = (x - x0) % 12;
                    if (phase >= 7) continue;  // gap
                    float t = (x1 > x0) ? (float)(x - x0) / (float)(x1 - x0) : 0;
                    int my = inMeanY[i] + (int)(t * (inMeanY[i+1] - inMeanY[i]));
                    if (y >= my && y <= my + 1)
                        setPixel4(row, x, cssGrey(0x77));
                }
            }
        }
    }

    // Set a single pixel in a 4-bit packed row buffer.
    void setPixel4(uint8_t* row, int x, uint8_t palIdx) const {
        if (x < 0 || x >= W) return;
        int byteIdx = x / 2;
        if (x & 1)
            row[byteIdx] = (row[byteIdx] & 0xF0) | (palIdx & 0x0F);
        else
            row[byteIdx] = (palIdx << 4) | (row[byteIdx] & 0x0F);
    }
};

// ---------------------------------------------------------------------------
// Serving the image a piece at a time
// ---------------------------------------------------------------------------

/// The whole file — header and pixels — as a byte sequence a caller can read
/// any slice of, in order.
///
/// This exists as a type of its own rather than as a lambda in the web handler
/// because the transport decides the slice sizes and the transport is not
/// available to a test. `maxLen` on the real path is whatever is left of the
/// TCP window after the response headers: usually about 1.4 kB, occasionally a
/// handful of bytes, and by construction smallest when the connection is
/// already under pressure. Code that assumed otherwise wrote a 118-byte header
/// into a buffer it never measured.
///
/// One rendered row is cached, so a row split across several reads is drawn
/// once. Reads must advance — this is a stream, not random access.
struct ChartBmpReader {
    ChartBmpCtx ctx;
    uint8_t     header[KD_BMP_HEADER_SIZE];
    uint32_t    sent  = 0;
    uint32_t    total = 0;

    int         cachedRow = -1;
    uint8_t     row[KD_BMP_MAX_ROW_BYTES];

    /// Lay out the header and compute the total size. Call after ctx.init().
    void begin() {
        writeBmpHeader(header, ctx.W, ctx.H);
        total     = (uint32_t)KD_BMP_HEADER_SIZE + (uint32_t)ctx.rowBytes * ctx.H;
        sent      = 0;
        cachedRow = -1;
    }

    bool done() const { return sent >= total; }

    /// Copy up to `maxLen` more bytes into `out`. Returns how many, which is 0
    /// only at the end of the image. Correct for every `maxLen >= 1`.
    size_t read(uint8_t* out, size_t maxLen) {
        size_t n = 0;
        while (n < maxLen && sent < total) {
            size_t take;
            if (sent < KD_BMP_HEADER_SIZE) {
                take = KD_BMP_HEADER_SIZE - sent;
                if (take > maxLen - n) take = maxLen - n;
                memcpy(out + n, header + sent, take);
            } else {
                const uint32_t px  = sent - (uint32_t)KD_BMP_HEADER_SIZE;
                const int      r   = (int)(px / ctx.rowBytes);
                const uint32_t off = px % ctx.rowBytes;
                if (cachedRow != r) {
                    // BMP rows run bottom-up; the renderer thinks top-down.
                    ctx.renderRow(row, ctx.H - 1 - r);
                    cachedRow = r;
                }
                take = ctx.rowBytes - off;
                if (take > maxLen - n) take = maxLen - n;
                memcpy(out + n, row + off, take);
            }
            n    += take;
            sent += (uint32_t)take;
        }
        return n;
    }
};
