// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 SciLor
// Pure logic of the airplane-mode nag patch (no libnx) so it can be unit tested on the host.
//
// Target: nifm's IRequest::GetAppletInfo implementation. Its start looks like
//     bl   <get request result>          ; w0 = result of the request
//     cbz  w0, <err: "no applet info">   ; <- hook
//     mov  wN, w0
//     ... (0-4 unrelated instructions, depends on firmware)
//     and  xM, xN, #0x3fffff
// The patch redirects the cbz into a small code cave that also takes the <err> exit when
// the result is 2110-1111 (0x8AE6E, airplane mode), so the game never gets the netConnect
// applet id and therefore never shows the "please disable airplane mode" popup.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#define AN_RESULT_AIRPLANE 0x8AE6Eu        // 2110-1111
#define AN_CAVE_WORDS 5
#define AN_CAVE_BYTES (AN_CAVE_WORDS * 4)

static inline uint32_t rd32(const uint8_t* p) { uint32_t v; memcpy(&v, p, 4); return v; }
static inline void wr32(uint8_t* p, uint32_t v) { memcpy(p, &v, 4); }

// ---- the few AArch64 encodings the cave needs ----
static inline uint32_t a64_b(uint64_t src, uint64_t dst)      { return 0x14000000u | (uint32_t)(((int64_t)(dst - src) >> 2) & 0x3ffffff); }
static inline uint32_t a64_cbz_w0(uint64_t src, uint64_t dst) { return 0x34000000u | ((uint32_t)(((int64_t)(dst - src) >> 2) & 0x7ffff) << 5); }
static inline uint32_t a64_beq(uint64_t src, uint64_t dst)    { return 0x54000000u | ((uint32_t)(((int64_t)(dst - src) >> 2) & 0x7ffff) << 5); }
// sub w16, w0, #0x8a, lsl #12   (x16 = IP0, free to clobber at this point)
#define A64_SUB_W16_W0_8A000 (0x51000000u | (1u << 22) | (0x8au << 10) | (0u << 5) | 16u)
// cmp w16, #0xe6e   (0x8a000 + 0xe6e == AN_RESULT_AIRPLANE)
#define A64_CMP_W16_E6E (0x7100001fu | (0xe6eu << 10) | (16u << 5))

static inline int64_t sext(uint64_t v, int bits) { return (int64_t)(v << (64 - bits)) >> (64 - bits); }
static inline uint64_t cbz_target(uint32_t w, uint64_t at) { return at + (uint64_t)(sext((w >> 5) & 0x7ffff, 19) * 4); }
static inline uint64_t b_target(uint32_t w, uint64_t at)   { return at + (uint64_t)(sext(w & 0x3ffffff, 26) * 4); }

static inline int is_bl(uint32_t w)     { return (w >> 26) == 0x25; }
static inline int is_b(uint32_t w)      { return (w >> 26) == 0x05; }
static inline int is_cbz_w0(uint32_t w) { return (w & 0xff00001fu) == 0x34000000u; }

// fills out[AN_CAVE_WORDS] for a cave at cave_off, hook at hook_off, with err = the original cbz target
static inline void build_cave(uint32_t out[AN_CAVE_WORDS], uint64_t cave_off, uint64_t hook_off, uint64_t err_off) {
    out[0] = a64_cbz_w0(cave_off, err_off);              // original: result == 0 -> err
    out[1] = A64_SUB_W16_W0_8A000;
    out[2] = A64_CMP_W16_E6E;
    out[3] = a64_beq(cave_off + 12, err_off);            // airplane mode -> err
    out[4] = a64_b(cave_off + 16, hook_off + 4);         // continue the original code
}

// cbz / b.cond only reach +-1 MiB and the encoders truncate silently: check that a built cave really
// goes where it should before writing it (matters only if .text ever grows past 1 MiB)
static inline int an_cave_ok(const uint32_t w[AN_CAVE_WORDS], uint64_t cave_off, uint64_t hook_off, uint64_t err_off) {
    return cbz_target(w[0], cave_off) == err_off && cbz_target(w[3], cave_off + 12) == err_off
        && b_target(w[4], cave_off + 16) == hook_off + 4;
}

typedef struct {
    uint64_t hook_off;   // offset of the cbz / b instruction
    int      patched;    // 1 if hook is already a branch to a valid cave
    uint64_t err_off;    // original cbz target
    uint64_t cave_off;   // valid if patched
} AnHook;

// Find the (single) hook in code[0..size). Returns the number of matches (want exactly 1).
// A patched hook is recognised by its cave contents, so state survives without extra bookkeeping.
static inline int an_find_hook(const uint8_t* code, size_t size, AnHook* out) {
    int found = 0;
    for (size_t i = 0; i + 36 <= size; i += 4) {
        uint32_t w0 = rd32(code + i), w1 = rd32(code + i + 4), w2 = rd32(code + i + 8);
        if (!is_bl(w0)) continue;
        if ((w2 & 0xffffffe0u) != 0x2a0003e0u) continue;                 // mov wN, w0
        int has_and = 0;                                                 // and xM, xN, #0x3fffff within 5 instrs
        for (int k = 3; k < 8 && !has_and; k++) {
            uint32_t w = rd32(code + i + 4 * k);
            has_and = (w & 0xfffffc00u) == 0x92405400u && ((w >> 5) & 31) == (w2 & 31);
        }
        if (!has_and) continue;
        AnHook h = {i + 4, 0, 0, 0};
        if (is_cbz_w0(w1)) {
            h.err_off = cbz_target(w1, h.hook_off);
        } else if (is_b(w1)) {
            uint64_t cave = b_target(w1, h.hook_off);
            // overflow-safe: a backward branch to before the buffer wraps to a huge cave value
            if (cave > size || size - cave < AN_CAVE_BYTES) continue;
            uint32_t c0 = rd32(code + cave);
            if (!is_cbz_w0(c0)) continue;
            uint32_t want[AN_CAVE_WORDS];
            build_cave(want, cave, h.hook_off, cbz_target(c0, cave));
            // a branch into something that is not our cave is not "patched" and not "original": ignore it
            // (can only be caused by an external poke; a nifm restart reloads clean code)
            if (memcmp(want, code + cave, AN_CAVE_BYTES)) continue;
            h.patched = 1; h.cave_off = cave; h.err_off = cbz_target(c0, cave);
        } else continue;
        if (!found) *out = h;
        found++;
    }
    return found;
}

// LAST 4-byte aligned run of >= AN_CAVE_BYTES zero bytes (unused function padding near the end of
// .text, never the module header at the start). Returns offset or (size_t)-1.
static inline size_t an_find_cave(const uint8_t* code, size_t size) {
    size_t run = 0, best = (size_t)-1;
    for (size_t i = 0; i + 4 <= size; i += 4) {
        if (rd32(code + i) == 0) { run += 4; if (run >= AN_CAVE_BYTES) best = i + 4 - run; }
        else run = 0;
    }
    return best;
}
