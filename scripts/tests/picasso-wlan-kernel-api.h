// SPDX-License-Identifier: BSD-2-Clause-Patent
// Explicit host-owned API stand-ins; not an XNU SDK or hardware qualification.
#include <stdint.h>
#include <stddef.h>
extern "C" int printf(const char *, ...) __attribute__((format(printf, 1, 2)));
extern "C" bool PE_parse_boot_argn(const char *, void *, int);
class IOMemoryMap { public: uintptr_t getVirtualAddress(); void release(); };
class IODeviceMemory {
public:
    static IODeviceMemory *withRange(uint64_t, uint64_t);
    IOMemoryMap *map(unsigned);
    void release();
};
enum { kIOMapAnywhere = 1, kIOMapInhibitCache = 2, OSNoRetain = 0 };
template<class T> class OSSharedPtr {
    T *pointer;
public:
    OSSharedPtr() : pointer(nullptr) {}
    OSSharedPtr(T *value, int) : pointer(value) {}
    explicit operator bool() const { return pointer != nullptr; }
    T *operator->() const { return pointer; }
};

// Stand-ins for the <kern/thread.h> and IOKit logging declarations stripped
// from WlanProbe.cpp before the freestanding ARM64 compile. Kept in lockstep
// with the manual stage-1 boot-arg wiring; -Werror forbids implicit decls.
typedef struct thread *thread_t;
typedef int wait_result_t;
typedef int kern_return_t;
enum { KERN_SUCCESS = 0 };
extern "C" kern_return_t kernel_thread_start(void (*)(void *, wait_result_t), void *, thread_t *);
extern "C" void thread_deallocate(thread_t);
extern "C" void IOLog(const char *, ...) __attribute__((format(printf, 1, 2)));
extern "C" void IOSleep(unsigned);
