/*
 * port/rando/rando_cosmetic.cpp — native cosmetic palette overrides.
 *
 * Also serves the general QoL tunic_color / heart_color settings, so the
 * colours work in normal play. Precedence: an active seed's own non-vanilla
 * pick wins; a vanilla (0) seed pick, or no seed, falls back to the general
 * setting.
 */

#include "rando/rando.h"
#include "port_runtime_config.h"

#include <stdint.h>
#include <stdio.h>

extern "C" uint16_t gPaletteBuffer[]; /* u16[0x200] — 32 rows x 16 colors */
extern "C" uint32_t gUsedPalettes;    /* FadeVBlank upload mask, 1 bit/row */

/* ---- vanilla fingerprints (identical palette data on USA and EU) -------- */
#define LINK_IDX_DARKEST 3
#define LINK_IDX_DARK 4
#define LINK_IDX_MAIN 5
#define LINK_IDX_LIGHT 7
#define LINK_VAN_DARKEST 0x15E2u
#define LINK_VAN_DARK 0x1688u
#define LINK_VAN_MAIN 0x07E2u
#define LINK_VAN_LIGHT 0x53F7u

#define HUD_IDX_WALLET_RED1 7
#define HUD_IDX_WALLET_RED2 8
#define HUD_IDX_YELLOW 10
#define HUD_VAN_WALLET_RED1 0x0CAFu
#define HUD_VAN_WALLET_RED2 0x10D7u
#define HUD_VAN_YELLOW 0x03DFu
#define HUD_IDX_HEART_FILL 9
#define HUD_VAN_HEART_FILL 0x195Fu /* the outline (index 14) is white already */

/* Tunic ramps {main, dark, darkest, light}; [0] is vanilla green. */
static const uint16_t kTunics[6][4] = {
    { LINK_VAN_MAIN, LINK_VAN_DARK, LINK_VAN_DARKEST, LINK_VAN_LIGHT },
    { 0x0014, 0x000F, 0x000A, 0x357A }, /* Red */
    { 0x7400, 0x5800, 0x4000, 0x7DF4 }, /* Blue */
    { 0x6412, 0x480C, 0x3408, 0x751C }, /* Purple */
    { 0x027E, 0x01B9, 0x0114, 0x4A7F }, /* Orange */
    { 0x4210, 0x318C, 0x2108, 0x5EF7 }, /* Grey */
};
/* Heart fills; [0] is vanilla red, and Rainbow (5) cycles kRainbow. */
static const uint16_t kHeartFills[5] = { HUD_VAN_HEART_FILL, 0x7400, 0x03E0, 0x03DF, 0x6412 };

static const uint16_t kRainbow[16] = {
    0x5749, 0x3F49, 0x3B49, 0x274A, 0x2753, 0x2759, 0x26BA, 0x261A,
    0x25DA, 0x257A, 0x253A, 0x3D3A, 0x613A, 0x61B6, 0x5E13, 0x5A90,
};
#define RAINBOW_FRAMES_PER_COLOR 12u

typedef struct {
    uint64_t state;
} SplitMix64;

static uint64_t SplitMix64_Next(SplitMix64* rng) {
    uint64_t z = (rng->state += 0x9e3779b97f4a7c15ull);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}

static uint32_t RngBounded(SplitMix64* rng, uint32_t bound) {
    if (bound <= 1) return 0;
    return (uint32_t)(SplitMix64_Next(rng) % bound);
}

typedef struct {
    bool valid;
    uint64_t seed;
    int tunic_pick, heart_pick; /* as chosen, 6 = Random */
    int tunic, heart;           /* resolved: kTunics row / kHeartFills row or 5 */
} CosmeticConfig;

static CosmeticConfig sCfg;
static bool sPainted; /* some non-vanilla pick has been applied this run */
static uint32_t sRainbowFrame;

static void RefreshConfig(uint64_t seed, int tunic_pick, int heart_pick) {
    sCfg.valid = true;
    sCfg.seed = seed;
    sCfg.tunic_pick = tunic_pick;
    sCfg.heart_pick = heart_pick;

    // 0 = Vanilla/Green, 1 = Red, 2 = Blue, 3 = Purple, 4 = Orange, 5 = Grey, 6 = Random
    if (tunic_pick == 6) {
        SplitMix64 rng;
        rng.state = seed ^ 0xbadc0deull;
        tunic_pick = (int)(RngBounded(&rng, 6));
    }
    sCfg.tunic = (tunic_pick >= 0 && tunic_pick < 6) ? tunic_pick : 0;

    // 0 = Red/Vanilla, 1 = Blue, 2 = Green, 3 = Yellow, 4 = Purple, 5 = Rainbow, 6 = Random
    if (heart_pick == 6) {
        SplitMix64 rng;
        rng.state = seed ^ 0xf00dcafeull;
        heart_pick = (int)(RngBounded(&rng, 6));
    }
    sCfg.heart = (heart_pick >= 0 && heart_pick < 6) ? heart_pick : 0;
}

/* Which kTunics ramp the row wears, or -1 when it is not Link's palette. */
static int MatchTunic(const uint16_t* row) {
    for (int i = 0; i < 6; ++i) {
        if (row[LINK_IDX_MAIN] == kTunics[i][0] && row[LINK_IDX_DARK] == kTunics[i][1] &&
            row[LINK_IDX_DARKEST] == kTunics[i][2] && row[LINK_IDX_LIGHT] == kTunics[i][3])
            return i;
    }
    return -1;
}

static bool IsHeartFill(uint16_t color) {
    for (uint16_t c : kHeartFills)
        if (color == c)
            return true;
    for (uint16_t c : kRainbow)
        if (color == c)
            return true;
    return false;
}

static inline void PutColor(uint16_t* row, uint32_t row_index, uint32_t idx, uint16_t color) {
    if (row[idx] != color) {
        row[idx] = color;
        gUsedPalettes |= 1u << row_index;
    }
}

extern "C" void Rando_Cosmetic_Tick(void) {
    uint64_t seed = 0;
    int tunic_pick = 0, heart_pick = 0;
    if (Rando_IsActive()) {
        const RandomizerSettings settings = Rando_GetSettings();
        seed = Rando_GetSeed64();
        tunic_pick = settings.tunic_color;
        heart_pick = settings.heart_color;
    }
    if (tunic_pick == 0)
        tunic_pick = Port_Config_GetTunicColor();
    if (heart_pick == 0)
        heart_pick = Port_Config_GetHeartColor();
    if (!sCfg.valid || sCfg.seed != seed || sCfg.tunic_pick != tunic_pick || sCfg.heart_pick != heart_pick) {
        RefreshConfig(seed, tunic_pick, heart_pick);
    }

    /* Rows are matched against every ramp we paint, not just vanilla, so a
     * changed pick (back to vanilla too) repaints rows wearing the old one —
     * including palette copies the game stashes and restores around menus
     * and cutscenes. Until something was painted there is nothing to undo. */
    if (sCfg.tunic != 0 || sCfg.heart != 0) {
        sPainted = true;
    }
    if (!sPainted) {
        return;
    }

    uint16_t heart_color;
    if (sCfg.heart == 5) {
        heart_color = kRainbow[(sRainbowFrame / RAINBOW_FRAMES_PER_COLOR) % 16u];
        sRainbowFrame++;
    } else {
        heart_color = kHeartFills[sCfg.heart];
    }

    for (uint32_t r = 0; r < 32; ++r) {
        uint16_t* row = &gPaletteBuffer[r * 16];

        const int tunic = MatchTunic(row);
        if (tunic >= 0) {
            if (tunic != sCfg.tunic) {
                PutColor(row, r, LINK_IDX_MAIN, kTunics[sCfg.tunic][0]);
                PutColor(row, r, LINK_IDX_DARK, kTunics[sCfg.tunic][1]);
                PutColor(row, r, LINK_IDX_DARKEST, kTunics[sCfg.tunic][2]);
                PutColor(row, r, LINK_IDX_LIGHT, kTunics[sCfg.tunic][3]);
            }
            continue;
        }

        if (row[HUD_IDX_WALLET_RED1] == HUD_VAN_WALLET_RED1 &&
            row[HUD_IDX_WALLET_RED2] == HUD_VAN_WALLET_RED2 &&
            row[HUD_IDX_YELLOW] == HUD_VAN_YELLOW && IsHeartFill(row[HUD_IDX_HEART_FILL])) {
            PutColor(row, r, HUD_IDX_HEART_FILL, heart_color);
        }
    }
}

extern "C" void Rando_Cosmetic_Apply(void) {
    sCfg.valid = false;
    Rando_Cosmetic_Tick();
}
