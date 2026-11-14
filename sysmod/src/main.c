// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 SciLor
// sys-antinag: removes the "please disable airplane mode" nag shown by games.
//
// Patches nifm in memory (see antinag.h). Runs for the whole session, re-applies after nifm restarts and
// follows /config/sys-antinag/config.ini live (the overlay edits it).
//
// Debug mode (config: debug=1) adds file triggers in /config/sys-antinag/debug/ - drop a file over FTP/WiFi:
//   peek.txt   lines "<hexoff> <len>"        read nifm .text   (offsets relative to the start of .text)
//   poke.txt   lines "<hexoff> <hexbytes>"   write nifm .text  (reverted by a reboot or by disabling antinag + nifm restart)
//   dump       writes all non-code regions of nifm to debug/dump_*.bin
//   probe      one nifm request incl. GetAppletInfo from this process, result goes to the log
//   probe_auto the same every ~4 s while the file exists
// Everything is logged to /config/sys-antinag/log.txt
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <switch.h>
#include "antinag.h"

#define DIR_CFG   "sdmc:/config/sys-antinag"
#define DIR_DEBUG DIR_CFG "/debug"
#define F_CFG     DIR_CFG "/config.ini"
#define F_LOG     DIR_CFG "/log.txt"
#define F_STATUS  DIR_CFG "/status.txt"
#ifndef ANTINAG_VERSION
#define ANTINAG_VERSION "dev"
#endif
#define NIFM_TID  0x010000000000000FULL
#define TEXT_MAX  0x140000   // nifm .text is ~0xd7000; refuse anything unexpectedly large

u32 __nx_applet_type = AppletType_None;
u32 __nx_fs_num_sessions = 1;

void __libnx_initheap(void) {
    static char inner_heap[0x20000];
    extern void* fake_heap_start;
    extern void* fake_heap_end;
    fake_heap_start = inner_heap;
    fake_heap_end = inner_heap + sizeof(inner_heap);
}

void __appInit(void) {
    if (R_FAILED(smInitialize())) fatalThrow(0xA001);
    // libnx picks command ids by firmware version (e.g. pmdmntGetProcessId), so it must be told
    if (R_SUCCEEDED(setsysInitialize())) {
        SetSysFirmwareVersion fw;
        if (R_SUCCEEDED(setsysGetFirmwareVersion(&fw))) hosversionSet(MAKEHOSVERSION(fw.major, fw.minor, fw.micro));
        setsysExit();
    }
    if (R_FAILED(fsInitialize())) fatalThrow(0xA002);
    if (R_FAILED(pmdmntInitialize())) fatalThrow(0xA003);
    fsdevMountSdmc();
    smExit();
}

void __appExit(void) {
    fsdevUnmountAll();
    pmdmntExit();
    fsExit();
}

// ---------------------------------------------------------------- log / status / config
static void logf_(const char* fmt, ...) {
    FILE* f = fopen(F_LOG, "a");
    if (!f) return;
    va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
    fclose(f);
}

static void set_status(const char* state, u64 text_base, const AnHook* h) {
    FILE* f = fopen(F_STATUS, "w");
    if (!f) return;
    fprintf(f, "state=%s\nversion=%s\n", state, ANTINAG_VERSION);
    if (h) fprintf(f, "hook=0x%llx\nerr=0x%llx\ncave=0x%llx\n", (unsigned long long)h->hook_off, (unsigned long long)h->err_off, (unsigned long long)h->cave_off);
    if (text_base) fprintf(f, "text_base=0x%llx\n", (unsigned long long)text_base);
    fclose(f);
}

typedef struct { int enabled; int debug; } Config;

static void load_config(Config* c) {
    c->enabled = 1; c->debug = 0;
    FILE* f = fopen(F_CFG, "r");
    if (!f) return;
    char ln[128];
    while (fgets(ln, sizeof ln, f)) {
        int v;
        if (sscanf(ln, " enabled = %d", &v) == 1) c->enabled = v != 0;
        else if (sscanf(ln, " debug = %d", &v) == 1) c->debug = v != 0;
    }
    fclose(f);
}

static void write_default_config(void) {
    if (access(F_CFG, F_OK) == 0) return;
    FILE* f = fopen(F_CFG, "w");
    if (!f) return;
    fputs("[antinag]\nenabled=1\ndebug=0\nshow_debug=0\n", f);
    fclose(f);
}

static time_t mtime(const char* p) { struct stat st; return stat(p, &st) == 0 ? st.st_mtime : 0; }

// ---------------------------------------------------------------- nifm access
static u8 g_text[TEXT_MAX];

typedef struct { Handle h; u64 base; u64 size; } Proc;

static int proc_open(Proc* p) {
    u64 pid; Result rc;
    if (R_FAILED(rc = pmdmntGetProcessId(&pid, NIFM_TID))) { logf_("pmdmntGetProcessId rc=0x%x\n", rc); return 0; }
    memset(p, 0, sizeof *p);
    DebugEventInfo ev;
    if (R_FAILED(rc = svcDebugActiveProcess(&p->h, pid))) { logf_("svcDebugActiveProcess(pid %llu) rc=0x%x\n", (unsigned long long)pid, rc); return 0; }
    svcGetDebugEvent(&ev, p->h);
    MemoryInfo mi; u32 pi; u64 addr = 0;
    while (R_SUCCEEDED(svcQueryDebugProcessMemory(&mi, &pi, p->h, addr))) {
        addr = mi.addr + mi.size;
        if ((mi.type & 0xFF) == MemType_CodeStatic && (mi.perm & Perm_Rx) == Perm_Rx) { p->base = mi.addr; p->size = mi.size; return 1; }
        if (!addr || !mi.size) break;
    }
    logf_("no executable code region found in nifm (pid %llu)\n", (unsigned long long)pid);
    svcCloseHandle(p->h);
    return 0;
}

static void proc_close(Proc* p) { svcCloseHandle(p->h); }

static int proc_read(Proc* p, void* dst, u64 off, u64 n) {
    for (u64 o = 0; o < n; o += 0x10000) {
        u64 c = n - o < 0x10000 ? n - o : 0x10000;
        if (R_FAILED(svcReadDebugProcessMemory((u8*)dst + o, p->h, p->base + off + o, c))) return 0;
    }
    return 1;
}

static int proc_write(Proc* p, const void* src, u64 off, u64 n) {
    return R_SUCCEEDED(svcWriteDebugProcessMemory(p->h, src, p->base + off, n));
}

// ---------------------------------------------------------------- the patch
// returns 1 when the requested state is in place (or was reached), 0 on failure
static int apply(Proc* p, int want_patched) {
    if (p->size > TEXT_MAX || !proc_read(p, g_text, 0, p->size)) { logf_("cannot read nifm .text\n"); set_status("error_read", p->base, NULL); return 0; }
    AnHook h;
    int n = an_find_hook(g_text, p->size, &h);
    if (n != 1) { logf_("hook matches=%d (need 1): this nifm build is not supported\n", n); set_status("unsupported", p->base, NULL); return 0; }

    if (want_patched == h.patched) { set_status(h.patched ? "patched" : "original", p->base, &h); return 1; }

    if (want_patched) {
        size_t cave = an_find_cave(g_text, p->size);
        if (cave == (size_t)-1) { logf_("no code cave found\n"); set_status("no_cave", p->base, &h); return 0; }
        uint32_t w[AN_CAVE_WORDS]; build_cave(w, cave, h.hook_off, h.err_off);
        if (!an_cave_ok(w, cave, h.hook_off, h.err_off)) { logf_("cave 0x%zx out of branch range of err 0x%llx\n", cave, (unsigned long long)h.err_off); set_status("no_cave", p->base, &h); return 0; }
        uint32_t hook = a64_b(h.hook_off, cave);
        // cave first, then the hook that makes it reachable
        if (!proc_write(p, w, cave, AN_CAVE_BYTES) || !proc_write(p, &hook, h.hook_off, 4)) { logf_("write failed\n"); set_status("error_write", p->base, &h); return 0; }
        h.patched = 1; h.cave_off = cave;
        logf_("patched: hook=0x%llx cave=0x%zx err=0x%llx\n", (unsigned long long)h.hook_off, cave, (unsigned long long)h.err_off);
        set_status("patched", p->base, &h);
    } else {
        uint32_t cbz = a64_cbz_w0(h.hook_off, h.err_off);
        static const uint8_t zero[AN_CAVE_BYTES];
        if (!proc_write(p, &cbz, h.hook_off, 4) || !proc_write(p, zero, h.cave_off, AN_CAVE_BYTES)) { logf_("revert failed\n"); set_status("error_write", p->base, &h); return 0; }
        logf_("reverted\n");
        h.patched = 0;
        set_status("original", p->base, &h);
    }
    return 1;
}

// ---------------------------------------------------------------- debug helpers
static void dbg_peek_poke(Proc* p, const char* path, int write) {
    FILE* f = fopen(path, "r");
    if (!f) return;
    char ln[256];
    while (fgets(ln, sizeof ln, f)) {
        unsigned long long off = 0; unsigned len = 0; char hex[200] = {0}; u8 b[64];
        if (write) {
            if (sscanf(ln, "%llx %199s", &off, hex) != 2) continue;
            if (strlen(hex) % 2 || hex[strspn(hex, "0123456789abcdefABCDEF")]) { logf_("poke 0x%llx: need an even number of hex digits, ignored\n", off); continue; }
            len = strlen(hex) / 2; if (len > sizeof b) len = sizeof b;
            if (off > p->size || len > p->size - off) { logf_("poke 0x%llx len %u outside .text (size 0x%llx), ignored\n", off, len, (unsigned long long)p->size); continue; }
            for (unsigned k = 0; k < len; k++) { unsigned v = 0; sscanf(hex + 2 * k, "%2x", &v); b[k] = (u8)v; }
            logf_("poke 0x%llx len %u -> %s\n", off, len, proc_write(p, b, off, len) ? "ok" : "FAILED");
        } else {
            if (sscanf(ln, "%llx %u", &off, &len) != 2) continue;
            if (len > sizeof b) len = sizeof b;
            if (off > p->size || len > p->size - off) { logf_("peek 0x%llx len %u outside .text (size 0x%llx), ignored\n", off, len, (unsigned long long)p->size); continue; }
            if (!proc_read(p, b, off, len)) { logf_("peek 0x%llx FAILED\n", off); continue; }
            FILE* l = fopen(F_LOG, "a");
            if (l) { fprintf(l, "peek 0x%llx:", off); for (unsigned k = 0; k < len; k++) fprintf(l, " %02x", b[k]); fputc('\n', l); fclose(l); }
        }
    }
    fclose(f);
}

static void dbg_dump(Proc* p) {
    static u8 buf[0x10000];
    char path[160]; MemoryInfo mi; u32 pi; u64 addr = 0;
    while (R_SUCCEEDED(svcQueryDebugProcessMemory(&mi, &pi, p->h, addr))) {
        addr = mi.addr + mi.size;
        if (!mi.size) break;
        if ((mi.perm & Perm_R) && !(mi.perm & Perm_X) && (mi.type & 0xFF) != MemType_CodeStatic && mi.type != MemType_Unmapped && mi.size <= 0x2000000) {
            snprintf(path, sizeof path, DIR_DEBUG "/dump_%010llx_%llx_t%02x.bin", (unsigned long long)mi.addr, (unsigned long long)mi.size, mi.type & 0xFF);
            FILE* f = fopen(path, "wb");
            if (f) {
                for (u64 o = 0; o < mi.size; o += sizeof buf) {
                    u64 c = mi.size - o < sizeof buf ? mi.size - o : sizeof buf;
                    if (R_FAILED(svcReadDebugProcessMemory(buf, p->h, mi.addr + o, c))) break;
                    fwrite(buf, 1, c, f);
                }
                fclose(f);
            }
        }
        if (!addr) break;
    }
    logf_("dump done (text_base=0x%llx)\n", (unsigned long long)p->base);
}

// one real nifm request, as a game does it. Returns after logging; verdict (for the overlay test) in /config/sys-antinag/test.txt
static void probe(int write_test) {
    Result res = 0, rc = 0; u32 id = 0, mode = 0, sz = 0; const char* verdict = "error: could not talk to nifm";
    if (R_SUCCEEDED(smInitialize())) {
        if (R_SUCCEEDED(nifmInitialize(NifmServiceType_User))) {
            NifmRequest r;
            if (R_SUCCEEDED(nifmCreateRequest(&r, true))) {
                static u8 buf[0x1000];
                nifmRequestSubmitAndWait(&r);
                res = nifmGetResult(&r);
                rc = nifmRequestGetAppletInfo(&r, 0, buf, sizeof buf, &id, &mode, &sz);
                nifmRequestClose(&r);
                if (res != AN_RESULT_AIRPLANE) verdict = "inconclusive: turn WiFi off first (airplane mode must block the connection)";
                else if (R_SUCCEEDED(rc) && id == 0xF) verdict = "FAIL: popup would still be shown (netConnect applet returned)";
                else verdict = "OK: popup suppressed";
            }
            nifmExit();
        }
        smExit();
    }
    logf_("probe: GetResult=0x%x GetAppletInfo rc=0x%x applet=0x%x mode=%u size=%u -> %s\n", res, rc, id, mode, sz, verdict);
    if (write_test) {
        FILE* f = fopen(DIR_CFG "/test.txt", "w");
        if (f) { fprintf(f, "verdict=%s\nresult=0x%x\nappletinfo_rc=0x%x\napplet=0x%x\n", verdict, res, rc, id); fclose(f); }
    }
}

static int take(const char* name) {   // consume a one-shot trigger file
    char p[128]; snprintf(p, sizeof p, DIR_DEBUG "/%s", name);
    if (access(p, F_OK)) return 0;
    remove(p);
    return 1;
}

// ---------------------------------------------------------------- main loop
int main(void) {
    mkdir("sdmc:/config", 0777); mkdir(DIR_CFG, 0777); mkdir(DIR_DEBUG, 0777);
    remove(F_LOG);
    write_default_config();
    logf_("sys-antinag %s started\n", ANTINAG_VERSION);

    Config cfg = {1, 0}; time_t cfg_time = 0; u64 last_pid = 0; int need = 1; unsigned tick = 0;
    for (;;) {
        time_t t = mtime(F_CFG);
        if (t != cfg_time) { cfg_time = t; load_config(&cfg); logf_("config: enabled=%d debug=%d\n", cfg.enabled, cfg.debug); need = 1; }

        u64 pid = 0;
        Result prc = pmdmntGetProcessId(&pid, NIFM_TID);
        if (R_FAILED(prc)) { if (last_pid) { logf_("nifm not running (rc=0x%x)\n", prc); last_pid = 0; } set_status("nifm_missing", 0, NULL); need = 1; svcSleepThread(1000000000ULL); continue; }
        if (pid != last_pid) { last_pid = pid; need = 1; }

        int dbg = cfg.debug && (access(DIR_DEBUG "/peek.txt", F_OK) == 0 || access(DIR_DEBUG "/poke.txt", F_OK) == 0 || access(DIR_DEBUG "/dump", F_OK) == 0);
        if (need || dbg) {
            Proc p;
            static int fails;
            if (proc_open(&p)) {
                fails = 0;
                if (need) { apply(&p, cfg.enabled); need = 0; }   // retry only on config change / nifm restart
                if (cfg.debug) {
                    char f[128];
                    snprintf(f, sizeof f, DIR_DEBUG "/peek.txt"); if (access(f, F_OK) == 0) { logf_("peek.txt\n"); dbg_peek_poke(&p, f, 0); remove(f); }
                    snprintf(f, sizeof f, DIR_DEBUG "/poke.txt"); if (access(f, F_OK) == 0) { logf_("poke.txt\n"); dbg_peek_poke(&p, f, 1); remove(f); }
                    if (take("dump")) dbg_dump(&p);
                }
                proc_close(&p);
            } else { if (fails++ < 5) logf_("cannot attach to nifm (attempt %d)\n", fails); need = 1; }
        }
        if (access(DIR_CFG "/test.req", F_OK) == 0) { remove(DIR_CFG "/test.req"); probe(1); }   // from the overlay
        if (cfg.debug) {
            if (take("probe")) probe(0);
            if (access(DIR_DEBUG "/probe_auto", F_OK) == 0 && (++tick % 4) == 0) probe(0);
        }
        svcSleepThread(1000000000ULL);
    }
}
