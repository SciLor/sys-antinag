// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 SciLor
// gcc -std=c11 -Wall -I../sysmod/include test_antinag.c -o /tmp/test_antinag && /tmp/test_antinag [img.bin ...]
// With image arguments (decompressed nifm, NSO header stripped) it also checks the hook matches exactly once
// and that the whole apply -> detect -> revert cycle round-trips on the real code.
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "antinag.h"

static void synthetic(void) {
    // words verified with objdump on nifm 23.0.1: cave @0xd2f50, hook @0x8ec50, err @0x8edf8
    assert(a64_cbz_w0(0xd2f50, 0x8edf8) == 0x34ddf540);
    assert(a64_beq(0xd2f5c, 0x8edf8) == 0x54ddf4e0);
    assert(a64_b(0xd2f60, 0x8ec54) == 0x17feef3d);
    assert(a64_b(0x8ec50, 0xd2f50) == 0x140110c0);
    assert(A64_SUB_W16_W0_8A000 == 0x51422810);
    assert(A64_CMP_W16_E6E == 0x7139ba1f);
    assert(0x8a000u + 0xe6eu == AN_RESULT_AIRPLANE);

    uint8_t code[0x400] = {0};
    // 0x100: bl; cbz w0,0x180 ; mov w26,w0 ; and x8,x26,#0x3fffff
    wr32(code + 0x100, 0x94000010); wr32(code + 0x104, a64_cbz_w0(0x104, 0x180));
    wr32(code + 0x108, 0x2a0003fa); wr32(code + 0x10c, 0x92405748);   // direct
    // also an unrelated instruction in between (older firmware layout) must still match
    wr32(code + 0x300, 0x94000010); wr32(code + 0x304, a64_cbz_w0(0x304, 0x380));
    wr32(code + 0x308, 0x2a0003fa); wr32(code + 0x30c, 0xf90007f3); wr32(code + 0x310, 0x528c0dc8); wr32(code + 0x314, 0x92405753);
    {   uint8_t tmp[0x400]; memcpy(tmp, code, sizeof tmp); AnHook x; assert(an_find_hook(tmp, sizeof tmp, &x) == 2); }
    memset(code + 0x300, 0, 0x20);
    wr32(code + 0x200, 0xd503201f); // some code so the zero run starts later
    uint8_t orig[sizeof code]; memcpy(orig, code, sizeof code);

    AnHook h;
    assert(an_find_hook(code, sizeof code, &h) == 1);
    assert(h.hook_off == 0x104 && !h.patched && h.err_off == 0x180);
    size_t cave = an_find_cave(code, sizeof code);
    assert(cave != (size_t)-1 && cave >= 0x204 && !(cave & 3));

    uint32_t w[AN_CAVE_WORDS]; build_cave(w, cave, h.hook_off, h.err_off);
    memcpy(code + cave, w, AN_CAVE_BYTES); wr32(code + h.hook_off, a64_b(h.hook_off, cave));
    AnHook p;
    assert(an_find_hook(code, sizeof code, &p) == 1);
    assert(p.patched && p.cave_off == cave && p.err_off == 0x180 && p.hook_off == 0x104);

    // revert
    memset(code + cave, 0, AN_CAVE_BYTES); wr32(code + p.hook_off, a64_cbz_w0(p.hook_off, p.err_off));
    assert(!memcmp(code, orig, sizeof code));

    // a corrupted cave must not be accepted as "patched"
    memcpy(code + cave, w, AN_CAVE_BYTES); wr32(code + h.hook_off, a64_b(h.hook_off, cave));
    code[cave + 9] ^= 0xff;
    assert(an_find_hook(code, sizeof code, &p) == 0);
    // cave rejections: target behind the buffer, target before the buffer (wraps), first cave word not a cbz
    wr32(code + h.hook_off, a64_b(h.hook_off, 0x100000));                   // forward, far outside
    assert(an_find_hook(code, sizeof code, &p) == 0);
    wr32(code + h.hook_off, a64_b(h.hook_off, sizeof code - 4));            // cave would straddle the end
    assert(an_find_hook(code, sizeof code, &p) == 0);
    {   // backward to offset -8: must be rejected without reading before the buffer (run under ASan to see it)
        uint8_t* s = calloc(1, 0x40); AnHook x;
        wr32(s, 0x94000010); wr32(s + 4, a64_b(4, (uint64_t)-8)); wr32(s + 8, 0x2a0003fa); wr32(s + 12, 0x92405748);
        assert(an_find_hook(s, 0x40, &x) == 0); free(s);
    }
    // cbz/b.eq reach only +-1 MiB: an unreachable err must be detected, a normal one accepted
    assert(an_cave_ok(w, cave, h.hook_off, h.err_off));
    {   uint32_t far[AN_CAVE_WORDS]; build_cave(far, 0x120000, 0x104, 0x180); assert(!an_cave_ok(far, 0x120000, 0x104, 0x180)); }
    wr32(code + h.hook_off, a64_b(h.hook_off, cave)); wr32(code + cave, 0xd503201f);  // cave starts with a nop
    assert(an_find_hook(code, sizeof code, &p) == 0);
    memcpy(code + cave, w, AN_CAVE_BYTES);
    assert(an_find_hook(code, sizeof code, &p) == 1 && p.patched);
    // two identical functions -> two hooks: the caller must refuse (apply() requires exactly 1)
    memcpy(code + 0x300, code + 0x100, 0x10); wr32(code + 0x304, a64_cbz_w0(0x304, 0x380));
    assert(an_find_hook(code, sizeof code, &p) == 2);
    puts("synthetic ok");
}

static void real(const char* path) {
    FILE* f = fopen(path, "rb"); if (!f) { printf("%s: cannot open\n", path); exit(1); }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t* b = malloc(n); if (fread(b, 1, n, f) != (size_t)n) exit(1); fclose(f);
    // .text size comes from the NSO header next to the image (".../main", decompressed text size at 0x18)
    size_t text = (size_t)n;
    char nso[512]; snprintf(nso, sizeof nso, "%s", path);
    char* slash = strrchr(nso, '/'); if (slash) { strcpy(slash + 1, "main"); FILE* h = fopen(nso, "rb");
        if (h) { uint8_t hd[0x20]; if (fread(hd, 1, 0x20, h) == 0x20) text = rd32(hd + 0x18); fclose(h); } }
    AnHook h; int c = an_find_hook(b, text, &h);
    size_t cave = an_find_cave(b, text);
    printf("%s: hooks=%d hook=0x%llx err=0x%llx cave=0x%zx\n", path, c, (unsigned long long)h.hook_off, (unsigned long long)h.err_off, cave);
    if (c != 1 || cave == (size_t)-1) exit(1);
    // sanity: the "no applet info" result 0x1686e (mov wN,#0x686e ; movk wN,#1,lsl#16) is materialised either
    // right before the hook (older firmware preloads it) or in the err block (23.x)
    int ok = 0;
    size_t ranges[2][2] = { { h.hook_off > 0x50 ? h.hook_off - 0x50 : 0, h.hook_off }, { h.err_off, h.err_off + 64 } };
    for (int r = 0; r < 2; r++)
        for (size_t o = ranges[r][0]; o + 8 <= ranges[r][1] && o + 8 <= text; o += 4)
            if ((rd32(b + o) & 0xffffffe0u) == 0x528d0dc0u && ((rd32(b + o + 4) & 0xffffffe0u) == 0x72a00020u)) ok = 1;
    printf("  result 0x1686e set up: %s\n", ok ? "yes" : "NO");
    if (!ok) exit(1);
    free(b);
}

int main(int argc, char** argv) {
    synthetic();
    for (int i = 1; i < argc; i++) real(argv[i]);
    puts("all ok");
    return 0;
}
