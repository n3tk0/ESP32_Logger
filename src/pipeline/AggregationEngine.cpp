#include "AggregationEngine.h"
#include <math.h>

// ---------------------------------------------------------------------------
// LTTB — Steinarsson 2013, O(n) after bucket grouping
// Reference: https://skemman.is/bitstream/1946/15343/3/SS_MSthesis.pdf
// ---------------------------------------------------------------------------
double AggregationEngine::_triangleArea(const SensorReading& a,
                                         const SensorReading& b,
                                         const SensorReading& c)
{
    // Area of triangle formed by three (timestamp, value) points
    double ax = (double)a.timestamp, ay = (double)a.value;
    double bx = (double)b.timestamp, by = (double)b.value;
    double cx = (double)c.timestamp, cy = (double)c.value;
    return fabs((ax - cx) * (by - ay) - (ax - bx) * (cy - ay)) * 0.5;
}

size_t AggregationEngine::lttb(const SensorReading* in,  size_t inLen,
                                      SensorReading* out, size_t maxPoints)
{
    if (inLen == 0 || maxPoints == 0) return 0;
    if (inLen <= maxPoints) {
        memcpy(out, in, inLen * sizeof(SensorReading));
        return inLen;
    }
    if (maxPoints == 1) {
        out[0] = in[0];
        return 1;
    }

    size_t sampled = 0;

    // Always include first and last point
    out[sampled++] = in[0];

    double bucketSize = (double)(inLen - 2) / (double)(maxPoints - 2);

    size_t a = 0; // index of last selected point

    for (size_t i = 0; i < maxPoints - 2; i++) {
        // Next bucket range
        size_t nextBucketStart = (size_t)((i + 1) * bucketSize) + 1;
        size_t nextBucketEnd   = (size_t)((i + 2) * bucketSize) + 1;
        if (nextBucketEnd >= inLen) nextBucketEnd = inLen - 1;

        // Average of next bucket (the "C" reference point)
        double avgX = 0, avgY = 0;
        size_t avgLen = nextBucketEnd - nextBucketStart;
        for (size_t j = nextBucketStart; j < nextBucketEnd; j++) {
            avgX += in[j].timestamp;
            avgY += in[j].value;
        }
        if (avgLen > 0) { avgX /= avgLen; avgY /= avgLen; }

        SensorReading avgPoint = in[nextBucketStart];
        avgPoint.timestamp = (uint32_t)avgX;
        avgPoint.value     = (float)avgY;

        // Current bucket range
        size_t rangeStart = (size_t)(i * bucketSize) + 1;
        size_t rangeEnd   = nextBucketStart;

        // Find point in current bucket with max triangle area
        double maxArea = -1.0;
        size_t maxIdx  = rangeStart;
        for (size_t j = rangeStart; j < rangeEnd; j++) {
            double area = _triangleArea(in[a], in[j], avgPoint);
            if (area > maxArea) { maxArea = area; maxIdx = j; }
        }

        out[sampled++] = in[maxIdx];
        a = maxIdx;
    }

    out[sampled++] = in[inLen - 1];
    return sampled;
}

// ---------------------------------------------------------------------------
SensorReading AggregationEngine::_reduceWindow(const SensorReading* w,
                                                size_t wLen,
                                                AggMode mode)
{
    if (wLen == 0) return SensorReading{};
    if (wLen == 1 || mode == AGG_RAW) return w[0];

    SensorReading result = w[0]; // copy metadata from first

    switch (mode) {
        case AGG_AVG: {
            double sum = 0;
            for (size_t i = 0; i < wLen; i++) sum += w[i].value;
            result.value = (float)(sum / wLen);
            // Timestamp = midpoint
            result.timestamp = (w[0].timestamp + w[wLen-1].timestamp) / 2;
            break;
        }
        case AGG_MIN: {
            float mn = w[0].value;
            for (size_t i = 1; i < wLen; i++) if (w[i].value < mn) mn = w[i].value;
            result.value = mn;
            break;
        }
        case AGG_MAX: {
            float mx = w[0].value;
            for (size_t i = 1; i < wLen; i++) if (w[i].value > mx) mx = w[i].value;
            result.value = mx;
            break;
        }
        case AGG_SUM: {
            float s = 0;
            for (size_t i = 0; i < wLen; i++) s += w[i].value;
            result.value     = s;
            result.timestamp = w[wLen-1].timestamp; // end of window
            break;
        }
        case AGG_LTTB: {
            // Can't LTTB a window without context; fall back to AVG
            double sum = 0;
            for (size_t i = 0; i < wLen; i++) sum += w[i].value;
            result.value = (float)(sum / wLen);
            break;
        }
        default:
            break;
    }
    return result;
}

// ---------------------------------------------------------------------------
size_t AggregationEngine::bucket(const SensorReading* in,  size_t inLen,
                                        SensorReading* out, size_t outMaxLen,
                                        TimeBucket bucketMins, AggMode mode)
{
    if (inLen == 0 || outMaxLen == 0) return 0;
    if (bucketMins == BUCKET_RAW || mode == AGG_RAW) {
        size_t n = inLen < outMaxLen ? inLen : outMaxLen;
        memcpy(out, in, n * sizeof(SensorReading));
        return n;
    }

    uint32_t bucketSecs = (uint32_t)bucketMins * 60u;
    size_t   outCount   = 0;

    // Use a small stack-allocated window buffer.
    // Max readings per bucket: even at 1-sec readings, 1-day bucket = 86400.
    // We handle large windows by iterating once with a running accumulator.

    uint32_t windowStart = 0;
    size_t   windowBegin = 0;

    // Find initial bucket boundary
    if (inLen > 0) {
        windowStart = (in[0].timestamp / bucketSecs) * bucketSecs;
    }

    for (size_t i = 0; i <= inLen && outCount < outMaxLen; i++) {
        bool endOfData   = (i == inLen);
        bool newBucket   = !endOfData &&
                           (in[i].timestamp >= windowStart + bucketSecs);

        if ((newBucket || endOfData) && i > windowBegin) {
            // Reduce [windowBegin, i)
            size_t wLen = i - windowBegin;

            // For large windows, stream through accumulator instead of
            // passing a pointer (avoids stack overflow with huge arrays).
            SensorReading reduced;
            if (mode == AGG_AVG || mode == AGG_LTTB) {
                double sum = 0;
                for (size_t j = windowBegin; j < i; j++) sum += in[j].value;
                reduced           = in[windowBegin];
                reduced.value     = (float)(sum / wLen);
                reduced.timestamp = windowStart + bucketSecs / 2;
            } else {
                reduced = _reduceWindow(in + windowBegin, wLen, mode);
            }
            out[outCount++] = reduced;
            windowBegin = i;
        }

        if (!endOfData && newBucket) {
            windowStart = (in[i].timestamp / bucketSecs) * bucketSecs;
        }
    }
    return outCount;
}

// ---------------------------------------------------------------------------
size_t AggregationEngine::aggregate(const SensorReading* in,  size_t inLen,
                                          SensorReading* out, size_t outMaxLen,
                                          TimeBucket bucketMins,
                                          AggMode    mode,
                                          size_t     maxPoints)
{
    if (inLen == 0 || outMaxLen == 0) return 0;

    // Phase 1: bucket
    // Allocate intermediate on heap if needed (large inLen)
    size_t bucketedMax = outMaxLen;
    SensorReading* bucketed = out; // Reuse output buffer for first pass if safe

    // If mode is LTTB we need a separate intermediate buffer
    SensorReading* tmpBuf = nullptr;
    if (mode == AGG_LTTB && bucketMins != BUCKET_RAW) {
        tmpBuf  = new SensorReading[outMaxLen];
        if (!tmpBuf) {
            // Fallback: no intermediate, skip LTTB
            return bucket(in, inLen, out, outMaxLen, bucketMins, AGG_AVG);
        }
        bucketed    = tmpBuf;
        bucketedMax = outMaxLen;
    }

    size_t n = bucket(in, inLen, bucketed, bucketedMax, bucketMins,
                      (mode == AGG_LTTB) ? AGG_AVG : mode);

    // Phase 2: LTTB if needed
    size_t result = n;
    if (mode == AGG_LTTB && n > maxPoints) {
        result = lttb(bucketed, n, out, maxPoints);
    } else if (mode == AGG_LTTB && tmpBuf) {
        // Copy tmpBuf → out
        size_t copy = n < outMaxLen ? n : outMaxLen;
        memcpy(out, tmpBuf, copy * sizeof(SensorReading));
        result = copy;
    }

    delete[] tmpBuf;
    return result;
}
