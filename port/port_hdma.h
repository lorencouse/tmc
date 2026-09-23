#ifndef PORT_HDMA_H
#define PORT_HDMA_H

/*
 * Software simulation of GBA HBlank-triggered DMA channels.
 *
 * On hardware, a DMA configured with DMA_START_HBLANK | DMA_REPEAT transfers
 * `count` units from src to dest at every HBlank. TMC uses this for the iris
 * circle / window effects (per-scanline WIN0H).
 *
 * The host PPU renders frames as a single batch, so we drive HDMA from the
 * VirtuaPPU mode-1 pre-line callback: one transfer per scanline.
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void port_hdma_register(int channel, const void* src, void* dest, uint16_t cnt_h, uint16_t count);
int port_hdma_has_active_channels(void);
/* 1 if any ACTIVE channel's per-line destination range overlaps [lo, hi).
 * Used by the PPU glue to strobe the affine BG2X/BG2Y reference latch: a
 * write EVENT must reload the internal reference even when the written
 * value is unchanged (constant-value HDMA pins the layer on hardware). */
int port_hdma_dest_overlaps(const void* lo, const void* hi);
void port_hdma_unregister(int channel);
void port_hdma_step_line(int line);
/* Lines the next armed HBlank table holds (160 unless its writer covers the
 * tall view). Lines past it keep the last transfer's registers. Set after
 * SetVBlankDMA, which resets it to 160; latched when the channel is armed. */
void port_hdma_set_table_lines(int lines);
void port_hdma_vblank_reset(void);

/* WIN0 spans at frame resolution for the circle/iris WIN0H tables, whose
 * 8-bit x and 160 entries can't describe a wider or taller view. The writer
 * fills the buffer returned for its table ([entry][0] = left, [1] = right,
 * PORT_HDMA_SPAN_LINES entries, zeroed); SetVBlankDMA arms it when that same
 * table is sent to WIN0H, and port_hdma_win0_spans returns it while channel
 * 0 plays that table (NULL otherwise, including when nothing refilled it). */
#define PORT_HDMA_SPAN_LINES 240
int16_t (*port_hdma_win0_spans_fill(const void* table))[2];
void port_hdma_win0_spans_commit(const void* src, int dest_is_win0h);
const int16_t (*port_hdma_win0_spans(void))[2];

#ifdef __cplusplus
}
#endif

#endif
