// NoThrowNew.cpp — operator new that returns NULL when the heap is out,
// instead of throwing std::bad_alloc.
//
// Nothing in this firmware catches an exception (platformio.ini,
// -fno-exceptions), so libstdc++'s throwing new turned every failed `new`
// into std::terminate -> abort() -> PANIC reboot. Seen on a C3 during an
// Overview load, 2026-10-16: a JSON reply buffer larger than the largest free
// block, five PANIC reboots in a row (sendJsonResponse now checks the block
// first). Much of the code that calls new was written for NULL — cbuf::resize,
// which grows every web response stream, keeps its old buffer and the stream
// sends what fit. Not all of it: cbuf's constructor does not test its buffer,
// so a big up-front size still has to be checked by whoever asks for it.
//
// Same trick as IdfTrims.c: our objects link before libstdc++.a, so these
// definitions satisfy the references and the throwing ones are never pulled
// in. Needs -fcheck-new (platformio.ini) so GCC keeps the callers' null
// checks; the nothrow and delete forms stay libstdc++'s (malloc/free).

#include <new>
#include <stdlib.h>

void* operator new(size_t n)   { return malloc(n ? n : 1); }
void* operator new[](size_t n) { return malloc(n ? n : 1); }
