// ============================================================================
// src/core/LazySlots.h
//
// LazySlots<T, N> — up to N records of T, each allocated the first time one is
// needed and then kept for good.
//
// For tables sized for the most a collector could ever have (16 node configs,
// 24 rollout targets) where a real one has a few: a static T[N] costs the
// whole table in RAM from boot, which on a C3 is heap nobody can use. Here
// the table is N pointers, and a record costs its size only once something
// claims it. A record that falls out of use is not freed — it waits for the
// next claim — so a node coming and going does not fragment the heap.
//
// Not locked: the caller's own lock covers it, as it covered the array.
// ============================================================================
#pragma once

#include <stddef.h>
#include <new>

template <typename T, size_t N>
class LazySlots {
public:
    LazySlots() = default;
    LazySlots(const LazySlots&) = delete;
    LazySlots& operator=(const LazySlots&) = delete;
    /// Never runs on the device (a static table lives as long as the
    /// firmware); it keeps a host test's leak check clean.
    ~LazySlots() { for (T* p : _p) delete p; }

    /// Every record allocated so far, in use or not, like walking the array
    /// it replaces: the caller tests its own "used" field.
    class It {
    public:
        It(T* const* p, T* const* end) : _p(p), _end(end) { skip(); }
        T& operator*() const { return **_p; }
        It& operator++() { ++_p; skip(); return *this; }
        bool operator!=(const It& o) const { return _p != o._p; }
    private:
        void skip() { while (_p != _end && !*_p) ++_p; }
        T* const* _p;
        T* const* _end;
    };
    It begin() const { return It(_p, _p + N); }
    It end()   const { return It(_p + N, _p + N); }

    /// A record for a new use: one already allocated that `isFree(record)`
    /// says is free, else a new, value-initialised one. nullptr when all N are
    /// allocated and in use, or the heap has no room for another.
    template <typename F>
    T* claim(F isFree) {
        for (T* p : _p)
            if (p && isFree(*p)) return p;
        for (T*& p : _p)
            if (!p) return p = new (std::nothrow) T();
        return nullptr;
    }

    /// How many records have been allocated (in use or not).
    size_t allocated() const {
        size_t n = 0;
        for (T* p : _p) n += p != nullptr;
        return n;
    }

    static constexpr size_t capacity() { return N; }

private:
    T* _p[N] = {};
};
