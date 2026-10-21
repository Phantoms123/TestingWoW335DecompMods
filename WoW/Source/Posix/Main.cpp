#include "WoW/Source/Client.h"
#include "WoW/Source/Native/Startup.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <SDL.h>
#if defined(__linux__)
#include <dlfcn.h>
#include <execinfo.h>
#include <signal.h>
#include <ucontext.h>
#include <unistd.h>
#endif

#if defined(__linux__)
// CHANGED: not in the original client. On a crash, the faulting address and the call stack go to the terminal as
// module + offset (matched to source with addr2line on the unstripped build); the default action (a core dump)
// follows.
static void PrintFrame(int index, void* address) {
    Dl_info info{};

    if (dladdr(address, &info) && info.dli_fname) {
        const char* module = std::strrchr(info.dli_fname, '/');
        std::fprintf(stderr, "  #%d %s+0x%zx\n", index, module ? module + 1 : info.dli_fname, static_cast<size_t>(static_cast<char*>(address) - static_cast<char*>(info.dli_fbase)));
    } else {
        std::fprintf(stderr, "  #%d %p\n", index, address);
    }
}

static void CrashHandler(int signal, siginfo_t* info, void* context) {
    std::fprintf(stderr, "Wow: crashed (signal %d, address %p)\n", signal, info ? info->si_addr : nullptr);
#if defined(__x86_64__)
    if (context) {
        PrintFrame(0, reinterpret_cast<void*>(static_cast<ucontext_t*>(context)->uc_mcontext.gregs[REG_RIP]));
    }
#endif
    void* frames[48];
    int count = backtrace(frames, 48);
    std::fprintf(stderr, "Call stack:\n");

    for (int i = 1; i < count; i++) {
        PrintFrame(i, frames[i]);
    }

    std::fflush(stderr);
    struct sigaction action {};
    action.sa_handler = SIG_DFL;
    sigaction(signal, &action, nullptr);
    raise(signal);
}

static void InstallCrashHandler() {
    static char s_altStack[64 * 1024];
    stack_t stack{};
    stack.ss_sp = s_altStack;
    stack.ss_size = sizeof(s_altStack);
    sigaltstack(&stack, nullptr);

    struct sigaction action {};
    action.sa_sigaction = CrashHandler;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&action.sa_mask);
    int signals[] = { SIGSEGV, SIGBUS, SIGFPE, SIGILL };

    for (int sig : signals) {
        sigaction(sig, &action, nullptr);
    }
}
#endif

int main() {
    if (!NativeStartupEnsureStack()) {
        std::perror("Wow: cannot provide 8 MiB main-thread stack");
        return 1;
    }

#if defined(__linux__)
    InstallCrashHandler();
#endif
    StormRtlInitialize();
    if (!NativeStartupInitializeFloatingPoint()) {
        std::perror("Wow: unsupported floating-point CPU");
        return 1;
    }

#if defined(__linux__)
    const char* wayland = SDL_getenv("WAYLAND_DISPLAY");
    if (wayland && *wayland && !SDL_getenv("SDL_VIDEODRIVER") && !SDL_getenv("SDL_VIDEO_DRIVER")) {
        SDL_SetHint(SDL_HINT_VIDEODRIVER, "wayland");
    }
#endif
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) {
        std::fprintf(stderr, "Wow: SDL initialization failed: %s\n", SDL_GetError());
        return 1;
    }
    struct SdlLifetime { ~SdlLifetime() { SDL_Quit(); } } sdl;
    return CommonMain();
}
