// Lab sampling profiler (command: prof <seconds>). Every ~0.5 ms it suspends the game's main thread and its render
// thread in turn, walks each stack with the x64 unwind tables and writes the return addresses to
// AILimitVR\prof_<n>.txt. games\AILimitVR\tools\prof_report.py turns that into per-function times using Unity's public
// UnityPlayer symbols and the IL2CPP dump. Lab only: it never runs unless asked.
#include <psapi.h>
#include <atomic>

static DWORD g_mainTid, g_renderTid;
static std::atomic<bool> g_profBusy{false};
static void __stdcall OnProfTidEvent(int) { g_renderTid = GetCurrentThreadId(); }

static int WalkThread(HANDLE th, uint64_t* out, int maxFrames) {
    CONTEXT ctx{}; ctx.ContextFlags = CONTEXT_FULL;
    if (SuspendThread(th) == (DWORD)-1) return 0;
    int n = 0;
    if (GetThreadContext(th, &ctx)) {
        for (; n < maxFrames && ctx.Rip; n++) {
            out[n] = ctx.Rip;
            DWORD64 base = 0; PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(ctx.Rip, &base, nullptr);
            if (!fn) {  // leaf function: the return address is on top of the stack
                if (IsBadReadPtr((void*)ctx.Rsp, 8)) break;
                ctx.Rip = *(DWORD64*)ctx.Rsp; ctx.Rsp += 8;
            } else {
                void* hd = nullptr; DWORD64 est = 0;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, ctx.Rip, fn, &ctx, &hd, &est, nullptr);
            }
        }
    }
    ResumeThread(th);
    return n;
}

static DWORD WINAPI ProfThread(void* arg) {
    double seconds = *(double*)arg; delete (double*)arg;
    static int runNo = 0; runNo++;
    for (int i = 0; i < 50 && !g_renderTid; i++) Sleep(10);
    HANDLE thr[2] = {
        g_mainTid ? OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, g_mainTid) : nullptr,
        g_renderTid ? OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, g_renderTid) : nullptr};
    wchar_t path[MAX_PATH]; swprintf_s(path, L"%sAILimitVR\\prof_%d.txt", g_gameDir.c_str(), runNo);
    FILE* f = _wfopen(path, L"w");
    if (!f) { Log("PROF could not write the profile"); g_profBusy = false; return 0; }
    HMODULE mods[512]; DWORD need = 0;
    if (EnumProcessModules(GetCurrentProcess(), mods, sizeof mods, &need)) {
        for (DWORD i = 0; i < need / sizeof(HMODULE); i++) {
            MODULEINFO mi{}; char nm[MAX_PATH] = {};
            GetModuleInformation(GetCurrentProcess(), mods[i], &mi, sizeof mi); GetModuleFileNameA(mods[i], nm, MAX_PATH);
            fprintf(f, "MOD %llx %lx %s\n", (unsigned long long)mi.lpBaseOfDll, mi.SizeOfImage, nm);
        }
    }
    timeBeginPeriod(1);
    uint64_t st[48]; int samples[2] = {0, 0};
    double t0 = Now();
    while (Now() - t0 < seconds) {
        for (int k = 0; k < 2; k++) {
            if (!thr[k]) continue;
            int n = WalkThread(thr[k], st, 48);
            if (!n) continue;
            fputc(k ? 'R' : 'M', f);
            for (int i = 0; i < n; i++) fprintf(f, " %llx", (unsigned long long)st[i]);
            fputc('\n', f); samples[k]++;
        }
        Sleep(1);
    }
    timeEndPeriod(1);
    fclose(f);
    for (HANDLE h : thr) if (h) CloseHandle(h);
    Log("PROF %d: %.0f s, main thread %d samples, render thread %d samples, written to prof_%d.txt", runNo, seconds, samples[0], samples[1], runNo);
    g_profBusy = false;
    return 0;
}
static void StartProfile(double seconds) {
    if (g_profBusy.exchange(true)) { Log("PROF already running"); return; }
    if (!g_renderTid) g_issue((void*)&OnProfTidEvent, 7);
    Log("PROF start %.0f s: main thread %lu, render thread %lu", seconds, g_mainTid, g_renderTid);
    CreateThread(nullptr, 0, ProfThread, new double(seconds), 0, nullptr);
}
