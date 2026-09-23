/* Expected-pixel regressions for the real CPU PPU; no ROM or SDL required. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "cpu/mode1.h"
#include "virtuappu.h"
uint32_t virtuappu_frame_buffer[VIRTUAPPU_FRAME_BUFFER_SIZE];
static uint8_t io[0x400], vram[0x18000];
static uint16_t bgpal[256], objpal[256], oam[512];
static int failures;
static void expect(const char* name, uint32_t got, uint32_t want) {
    if (got != want) {
        fprintf(stderr, "%s: got %08x, expected %08x\n", name, got, want);
        failures++;
    }
}
int main(void) {
    VirtuaPPUMode1GbaMemory mem = {io, vram, bgpal, objpal, oam};
    virtuappu_mode1_bind_gba_memory(&mem);
    for (int i = 0; i < 128; i++) oam[i * 4] = 0x200;
    io[1] = 0x10; /* OBJ enabled; BGs transparent. */
    bgpal[0] = 0x03e0;
    objpal[1] = 0x001f;
    memset(vram + 0x10000, 0x11, 32);
    oam[0] = 16; oam[1] = 236; oam[2] = 0;
    PPUMemory p = {0};
    p.mode = 1; p.frame_width = 284; p.frame_pitch = MODE1_GBA_WIDTH;
    virtuappu_mode1_render_frame(&p);
    expect("sprite left of seam", virtuappu_frame_buffer[16 * MODE1_GBA_WIDTH + 239], 0xff0000f8);
    expect("sprite right of seam", virtuappu_frame_buffer[16 * MODE1_GBA_WIDTH + 240], 0xff0000f8);
    expect("backdrop right of seam", virtuappu_frame_buffer[240], 0xff00f800);
    /* Window-hidden layers must still reveal the real backdrop. */
    io[1] |= 0x20; /* WIN0 enabled, zero window/outside controls. */
    virtuappu_mode1_render_frame(&p);
    expect("window masks sprite", virtuappu_frame_buffer[16 * MODE1_GBA_WIDTH + 240], 0xff00f800);
    /* Native fallback must not touch padding in the wider-pitch buffer. */
    p.frame_width = 240;
    for (int i = 0; i < MODE1_GBA_WIDTH * 160; i++) virtuappu_frame_buffer[i] = 0x12345678;
    virtuappu_mode1_render_frame(&p);
    expect("native fallback padding", virtuappu_frame_buffer[240], 0x12345678);
    /* Woods fog is a repeating screenblock, including scanline-varying HOFS.
     * Its shadow must represent every tile column, not just the camera reveal. */
    static uint16_t overlay[32 * MODE1_WS_SHADOW_COLS];
    memset(io, 0, sizeof(io));
    io[1] = 8; /* BG3 only. */
    io[0xe] = 4; io[0xf] = 0x1e; /* char base 1, screen base 30 */
    bgpal[1] = 0x001f; bgpal[2] = 0x03e0;
    memset(vram + 0x4020, 0x11, 32);
    memset(vram + 0x4040, 0x22, 32);
    uint16_t* screen = (uint16_t*)(vram + 0xf000);
    for (int row = 0; row < 32; ++row) {
        for (int col = 0; col < 32; ++col) {
            uint16_t entry = 1 + (col & 1);
            screen[row * 32 + col] = entry;
            if (col < MODE1_WS_SHADOW_COLS)
                overlay[row * MODE1_WS_SHADOW_COLS + col] = entry;
        }
    }
    virtuappu_mode1_ws_shadow[3] = overlay;
    virtuappu_mode1_ws_shadow_base_tile[3] = 0;
    p.frame_width = 384;
    for (int offset = 0; offset < 256; offset += 7) {
        io[0x1c] = offset;
        virtuappu_mode1_render_frame(&p);
        for (int x = 240; x < 384; ++x) {
            uint32_t color = (((x + offset) / 8) & 1) ? 0xff00f800 : 0xff0000f8;
            expect("repeating fog past native edge", virtuappu_frame_buffer[x], color);
            if (failures) break;
        }
        if (failures) break;
    }
    /* Tall view: a message box across the HUD split at line 80 moves whole
     * with the half holding most of it. BG0 rows 56..103 are the box. */
    virtuappu_mode1_ws_shadow[3] = NULL;
    memset(io, 0, sizeof(io));
    memset(vram + 0xf000, 0, 0x800);
    io[1] = 1; /* BG0 only */
    io[8] = 4; io[9] = 0x1e;
    for (int row = 7; row < 13; ++row)
        for (int col = 0; col < 32; ++col) screen[row * 32 + col] = 1;
    p.frame_width = 320; p.frame_height = 240;
    virtuappu_mode1_tall_hud_split = 1;
    virtuappu_mode1_ws_msg_shift = 40;
    virtuappu_mode1_ws_msg_x0 = 0; virtuappu_mode1_ws_msg_x1 = 240;
    const struct { int y0, y1, red_from, red_to; } boxes[] = {
        { 56, 104, 136, 184 }, /* mostly below 80: all of it at the bottom */
        { 56, 96, 56, 96 },    /* mostly above: kept at the top */
    };
    for (int b = 0; b < 2; ++b) {
        virtuappu_mode1_ws_msg_y0 = boxes[b].y0; virtuappu_mode1_ws_msg_y1 = boxes[b].y1;
        virtuappu_mode1_render_frame(&p);
        for (int line = 0; line < 240; ++line) {
            int red = (line >= boxes[b].red_from && line < boxes[b].red_to) ||
                      (b == 1 && line >= 176 && line < 184); /* rows 96..103, outside the box */
            expect(b ? "box kept at top" : "box moved down",
                   virtuappu_frame_buffer[line * MODE1_GBA_WIDTH + 100], red ? 0xff0000f8 : 0xff00f800);
            if (failures) { fprintf(stderr, "  line %d\n", line); break; }
        }
    }
    virtuappu_mode1_tall_hud_split = 0; virtuappu_mode1_ws_msg_shift = 0;
    if (!failures) puts("widescreen CPU PPU: PASS");
    return failures != 0;
}
