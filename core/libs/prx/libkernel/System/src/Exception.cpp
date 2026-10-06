#include <cstdint>
#include <cstddef>
#include <atomic>
#include <map>
#include "SceTypes.hpp"
#include "prx/libc/include/General.hpp"

// Exception handler storage - maps signal number to handler address
// The handler is recorded but never invoked: host exceptions are not forwarded to guest handlers.
static std::atomic<uint64_t> g_exceptionHandlers[32] = {};
static std::atomic<int> g_exceptionHandlerIdCounter{1};

extern "C" {

int APS5_VABI sceKernelInstallExceptionHandler(int signum, void* handler) {
    if (signum < 0 || signum >= 32) {
        return -1; // Invalid signum
    }
    if (!handler) {
        return -1; // Invalid handler
    }
    
    // Store the handler and return a positive ID (like traditional Unix)
    g_exceptionHandlers[signum].store(reinterpret_cast<uint64_t>(handler), std::memory_order_relaxed);
    APS5_LOG_OUT("sceKernelInstallExceptionHandler: signum=%d handler=%p", signum, handler);
    
    // Return a unique positive ID for this handler installation
    return g_exceptionHandlerIdCounter.fetch_add(1, std::memory_order_relaxed);
}

int APS5_VABI sceKernelRemoveExceptionHandler(int signum) {
    if (signum < 0 || signum >= 32) {
        return -1; // Invalid signum
    }
    
    g_exceptionHandlers[signum].store(0, std::memory_order_relaxed);
    APS5_LOG_OUT("sceKernelRemoveExceptionHandler: signum=%d", signum);
    
    return 0; // Success
}

int APS5_VABI sceKernelRaiseException(Pthread thread, int signum) {
    (void)thread;
    
    if (signum < 0 || signum >= 32) {
        return -1; // Invalid signum
    }
    
    uint64_t handler = g_exceptionHandlers[signum].load(std::memory_order_relaxed);
    if (handler) {
        APS5_LOG_OUT("sceKernelRaiseException: thread=%p signum=%d handler=%p (would invoke)", 
                     (void*)thread, signum, (void*)handler);
        // Note: Handler is recorded but never actually invoked in this emulator
        // Host exceptions are not forwarded to guest handlers
    } else {
        APS5_LOG_OUT("sceKernelRaiseException: thread=%p signum=%d (no handler installed)", 
                     (void*)thread, signum);
    }
    
    return 0; // Success
}

void APS5_VABI sceKernelDebugRaiseException(int c1, int c2) {
  APS5_LOG_OUT("sceKernelDebugRaiseException c1=%d c2=%d", c1, c2);
}

void APS5_VABI sceKernelDebugRaiseExceptionOnReleaseMode(int c1, int c2) {
  APS5_LOG_OUT("sceKernelDebugRaiseExceptionOnReleaseMode c1=%d c2=%d", c1, c2);
}

}
