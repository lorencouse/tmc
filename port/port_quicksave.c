/*
 * port_quicksave.c — multi-slot save-states with disk persistence + auto-save.
 *
 * Snapshots a curated set of game-state regions into an in-memory slot
 * on save, restores them on load. Slots also get serialized to disk as
 * `state_<slot>.bin` next to the binary so they survive restart.
 *
 * Slot layout:
 *   slot 0-19: manual slots. One of them is "selected" (persisted as
 *              savestate_slot in config.json); the rebindable
 *              PORT_INPUT_STATE_SAVE / _LOAD / _NEXT / _PREV actions act
 *              on the selection, so a handful of bindings reach all of
 *              them. "Save to a new slot" counts up and wraps back to the
 *              first once all 20 are filled, so repeated presses leave a
 *              rolling history rather than overwriting one state.
 *              Slot 0 doubles as the legacy single-slot quicksave API,
 *              and F1..F4 still address slots 1-4 directly (Shift = save).
 *   slot 20-22: auto-save ring (Port_QuickSave_AutoTick cycles through
 *              these). Not selectable — the ring would overwrite a
 *              hand-made state parked there.
 *
 * File format (disk persistence):
 *   magic     "TMCS"                          (4 bytes)
 *   version   PORT_QUICKSAVE_VERSION          (u32 LE)
 *   total     sum of all region sizes         (u32 LE)
 *   saved_at  unix seconds                    (u64 LE)
 *   session   id of the process that wrote it (u64 LE, see ResumeFromSnapshot)
 *   region    ROM region tag                  (u32 LE)
 *   data      concatenated region bytes       (gPortStateRegions[] order)
 *
 * On load, if magic/version/size don't match, the file is rejected
 * silently — the in-memory snapshot (if any) stays untouched. This is
 * defensive against schema changes between builds; saves are best-effort,
 * not a contract.
 *
 * Coverage: every mutable game global -- emulated GBA memory, the save
 * file, player, entities *and their list heads/counters*, room/area/map
 * state, scripts, textbox, menus, fades, palettes. The list lives in
 * port_linked_stubs.c (gPortStateRegions) next to the definitions; see
 * port_state_regions.h for what is deliberately left out and why.
 *
 * Cross-session loads: the snapshot is full of host pointers (entity links,
 * script contexts, the text renderer's cursor into an asset buffer), valid
 * only in the process that wrote them. A state file from an earlier run is
 * therefore not memcpy'd back; instead its save file is restored and the
 * engine re-enters the same room at the same position through the path the
 * game's own continue uses (ResumeFromSnapshot). Inventory, flags, health
 * and position survive; room state (enemies, cutscene progress) starts
 * fresh. Within one session loads are exact.
 *
 * Caveats:
 *  - Snapshotting mid-frame is supported but the visible result is
 *    "next frame" — entity logic that ran this frame may have already
 *    written to OAM, which is not snapshotted.
 *  - Save-states are not the same as the game's in-engine save file
 *    (`tmc.sav`). The game's own save still goes through its file-select
 *    flow. Save-states capture transient runtime state including
 *    mid-cutscene positions.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

#include <SDL3/SDL.h>

#include "structures.h"
#include "save.h"
#include "main.h"
#include "entity.h"
#include "message.h" /* gMessage — save-on-textbox test hook */
#include "player.h"
#include "port_gba_mem.h"
#include "port_runtime_config.h"
#include "region.h" /* REGION_IS_EU/JP — per-region savestate isolation (#21) */
#include "virtuappu.h" /* virtuappu_frame_buffer — slot thumbnails */

#include "port_state_regions.h"

extern u32 gRand; /* port_linked_stubs.c; restored on a cross-session resume */

/* The region table is owned by port_linked_stubs.c; these keep the rest of
 * this file reading the way it always has. */
#define sRegions gPortStateRegions
#define NUM_REGIONS gPortStateRegionCount
/* 20 manual slots. A rolling quick-save is only worth as much as how far
 * back it lets you rewind, and the cost here is small: ~409 KB of state plus
 * a 38 KB thumbnail per slot, so a full set is ~9 MB on a card with tens of
 * gigabytes free. Change this one constant to resize the ring. */
#define NUM_MANUAL_SLOTS 20
#define NUM_AUTO_SLOTS 3
#define AUTO_SLOT_BASE NUM_MANUAL_SLOTS
#define NUM_SLOTS (NUM_MANUAL_SLOTS + NUM_AUTO_SLOTS)
#define MAGIC 0x53434D54u /* "TMCS" little-endian */
#define VERSION                                         \
    7u /* v2: header carries gEntities base address for \
        * cross-process pointer-fixup on restore.       \
        * v3: gRand added to region list so RNG         \
        * state round-trips (GBA had it in IWRAM).      \
        * v4: gPracticeFrame added so speedrun IGT      \
        * timer rewinds with state.                     \
        * v5: ROM region tag — USA state restored     \
        * into a JP session contaminates tmc_jp.sav     \
        * (#21); cross-region loads are refused.        \
        * v6: entity subclass layouts changed; older    \
        * snapshots are rejected.                       \
        * v7: full host-global coverage (list heads,    \
        * textbox, scripts, ...); the header carries a  \
        * session id instead of the gEntities base, and \
        * cross-session loads resume via the engine.    */

typedef struct {
    u8* snapshot; /* heap, NULL if slot empty */
    size_t bytes;
    int valid;
    u64 saved_at_unix;  /* clock_gettime CLOCK_REALTIME seconds */
    u64 saved_session; /* process that wrote it; 0 = this one (in-RAM slot) */
} Slot;

/* Slot thumbnails. A save-state picker is unusable without them -- five
 * timestamps tell you nothing about which one is the fight you wanted. Half
 * the GBA frame (240x160 -> 120x80) by nearest sampling, which for pixel art
 * is exact 2x1:1 decimation rather than a resample.
 *
 * Static rather than heap: AGENTS.md asks for fixed pools over allocation in
 * frame paths, and 8 x 120x80x4 is 300 KB against a 400 KB snapshot per slot.
 * RGBA8 byte order matches both virtuappu_frame_buffer (ABGR LE, so byte 0 is
 * R) and ImTextureFormat_RGBA32, so the copy only has to force alpha. */
#define THUMB_W 120
#define THUMB_H 80
#define THUMB_BYTES (THUMB_W * THUMB_H * 4)
#define THUMB_MAGIC 0x54434D54u /* "TMCT" little-endian */

static u8 sThumbs[NUM_SLOTS][THUMB_BYTES];
static int sThumbValid[NUM_SLOTS];
/* Bumped whenever a slot's thumbnail changes, so the menu can tell that its
 * cached GPU texture is stale without comparing 38 KB of pixels. */
static u32 sThumbGeneration[NUM_SLOTS];

/* Disk-probe cache.
 *
 * HasSlot() and SlotTimestamp() fall back to opening the state file when a
 * slot is not in memory, and the Saves tab calls both for every row, every
 * frame. At 23 slots that is thousands of fopen()s a second against an SD
 * card for information that only changes when we ourselves write a slot. So
 * probe once and remember; a save refreshes the entry, and after a save the
 * in-memory copy answers anyway.
 *
 * A slot deleted out from under us mid-session goes stale, which is no worse
 * than the in-memory path already was, and nothing writes these files but us. */
static int sDiskProbed[NUM_SLOTS];
static int sDiskExists[NUM_SLOTS];
static u64 sDiskSavedAt[NUM_SLOTS];
/* Separate from the above: a state can exist with no .thumb sidecar (written
 * by an older build), and we must not re-open the missing file every frame. */
static int sThumbProbed[NUM_SLOTS];

static Slot sSlots[NUM_SLOTS];
static int sAutoNextSlot = AUTO_SLOT_BASE; /* round-robin cursor */
static u64 sAutoLastSaveTicksMs = 0;
static int sAutoEnabled = 1;        /* on by default — the F8
                                       toggle (and config.json)
                                       can flip it off. */
static u32 sAutoIntervalMs = 60000; /* 60 seconds default */

/* Area-change auto-save (independent of the interval timer). Tracks
 * the last-observed (area, room) and saves to the ring whenever it
 * changes. Helps with the crash-on-load-then-lose-an-hour case Jester
 * flagged. */
static int sAutoOnAreaChange = 1;
static u8 sLastSeenArea = 0xFF;
static u8 sLastSeenRoom = 0xFF;

static size_t TotalRegionBytes(void) {
    size_t total = 0;
    for (size_t i = 0; i < NUM_REGIONS; i++) {
        total += sRegions[i].size;
    }
    return total;
}

static int Snapshot_Capture(Slot* s) {
    const size_t total = TotalRegionBytes();
    if (s->snapshot == NULL || s->bytes != total) {
        free(s->snapshot);
        s->snapshot = (u8*)malloc(total);
        if (s->snapshot == NULL) {
            s->bytes = 0;
            s->valid = 0;
            fprintf(stderr, "[quicksave] alloc failed (%zu bytes)\n", total);
            return 0;
        }
        s->bytes = total;
    }
    u8* dst = s->snapshot;
    for (size_t i = 0; i < NUM_REGIONS; i++) {
        memcpy(dst, sRegions[i].ptr, sRegions[i].size);
        dst += sRegions[i].size;
    }
    s->valid = 1;
    s->saved_at_unix = (u64)time(NULL);
    s->saved_session = 0; /* ours: an exact restore is safe */
    return 1;
}

static int Snapshot_MatchesCurrent(const Slot* s, const char** region, size_t* offset, u8* expected, u8* actual) {
    const u8* src;
    size_t i;

    if (!s->valid || s->snapshot == NULL || s->bytes != TotalRegionBytes()) {
        *region = "snapshot";
        *offset = 0;
        *expected = 0;
        *actual = 0;
        return 0;
    }

    src = s->snapshot;
    for (i = 0; i < NUM_REGIONS; i++) {
        const u8* current = (const u8*)sRegions[i].ptr;
        size_t j;

        if (memcmp(src, current, sRegions[i].size) == 0) {
            src += sRegions[i].size;
            continue;
        }
        for (j = 0; j < sRegions[i].size && src[j] == current[j]; j++) {}
        *region = sRegions[i].name;
        *offset = j;
        *expected = src[j];
        *actual = current[j];
        return 0;
    }
    return 1;
}

/* Identity of this process, written into every state file so a load can tell
 * "written by me" (exact restore is safe) from "written by an earlier run"
 * (pointers are stale; resume through the engine instead). */
static u64 SessionId(void) {
    static u64 id = 0;
    if (id == 0) {
        id = ((u64)time(NULL) << 32) ^ SDL_GetPerformanceCounter() ^ (u64)(uintptr_t)&id;
        if (id == 0)
            id = 1;
    }
    return id;
}

/* Pointer to one region's bytes inside a snapshot, by name. */
static const u8* SnapshotRegion(const Slot* s, const char* name) {
    const u8* p = s->snapshot;
    for (size_t i = 0; i < NUM_REGIONS; i++) {
        if (strcmp(sRegions[i].name, name) == 0)
            return p;
        p += sRegions[i].size;
    }
    return NULL;
}

/* A state written by an earlier process. Its entity, script and text state
 * is full of pointers into that process's heap, so instead of copying it
 * back we take what is pointer-free and durable -- the save file -- and
 * re-enter the room where the state was taken, at the same spot, the way
 * the game's own continue does (sub_08053250 + SetTask(TASK_GAME)). */
static int ResumeFromSnapshot(const Slot* s) {
    const SaveFile* save = (const SaveFile*)SnapshotRegion(s, "gSave");
    const RoomControls* rc = (const RoomControls*)SnapshotRegion(s, "gRoomControls");
    const PlayerEntity* pl = (const PlayerEntity*)SnapshotRegion(s, "gPlayerEntity");
    const u32* rnd = (const u32*)SnapshotRegion(s, "gRand");
    if (save == NULL || rc == NULL || pl == NULL)
        return 0;
    memcpy(&gSave, save, sizeof(gSave));
    if (rnd != NULL)
        gRand = *rnd;
    gRoomTransition.player_status.spawn_type = PL_SPAWN_DEFAULT;
    gRoomTransition.player_status.start_pos_x = (s16)(pl->base.x.HALF.HI - rc->origin_x);
    gRoomTransition.player_status.start_pos_y = (s16)(pl->base.y.HALF.HI - rc->origin_y);
    gRoomTransition.player_status.start_anim = pl->base.animationState;
    gRoomTransition.player_status.layer = pl->base.collisionLayer;
    gRoomTransition.player_status.area_next = rc->area;
    gRoomTransition.player_status.room_next = rc->room;
    memcpy(&gSave.saved_status, &gRoomTransition.player_status, sizeof(gRoomTransition.player_status));
    SetTask(TASK_GAME);
    fprintf(stderr, "[quicksave] state is from an earlier session: resuming area=%u room=%u at (%d,%d) via the engine\n",
            rc->area, rc->room, pl->base.x.HALF.HI, pl->base.y.HALF.HI);
    return 2;
}

/* Returns 1 for an exact restore, 2 for a cross-session resume, 0 on failure. */
static int Snapshot_Restore(const Slot* s) {
    if (!s->valid || s->snapshot == NULL || s->bytes != TotalRegionBytes()) {
        return 0;
    }
    if (s->saved_session != 0 && s->saved_session != SessionId())
        return ResumeFromSnapshot(s);
    const u8* src = s->snapshot;
    for (size_t i = 0; i < NUM_REGIONS; i++) {
        memcpy(sRegions[i].ptr, src, sRegions[i].size);
        src += sRegions[i].size;
    }
    return 1;
}

/* Headless deterministic restore/replay gate. With no external input, advance
 * the same 120 engine frames twice from one snapshot and require every saved
 * byte to agree. Runs here so capture/restore stays on a frame boundary. */
static int QuickSaveReplayTestTick(void) {
    enum {
        SETTLE_FRAMES = 30,
        DEFAULT_REPLAY_FRAMES = 120,
        MAX_REPLAY_FRAMES = 3600,
    };
    static int enabled = -1;
    static int phase = 0;
    static unsigned int phaseFrame = 0;
    static unsigned int replayFrames = DEFAULT_REPLAY_FRAMES;
    static Slot start;
    static Slot expectedEnd;
    const char* region;
    size_t offset;
    u8 expected;
    u8 actual;

    if (enabled < 0) {
        const char* env = getenv("TMC_REPRO_QUICKSAVE_ROUNDTRIP");
        enabled = env != NULL && env[0] != '\0' && strcmp(env, "0") != 0;
        if (enabled) {
            const char* frames = getenv("TMC_REPRO_QUICKSAVE_FRAMES");
            char* end;
            unsigned long requested = frames != NULL ? strtoul(frames, &end, 10) : 0;
            if (frames != NULL && requested > 0 && requested <= MAX_REPLAY_FRAMES && end != frames && *end == '\0') {
                replayFrames = (unsigned int)requested;
            }
            fprintf(stderr, "[quicksave-test] enabled: settle=%d replay=%u frames\n", SETTLE_FRAMES, replayFrames);
        }
    }
    if (!enabled) {
        return 0;
    }

    if (phase == 0 && gMain.task != TASK_GAME) {
        phaseFrame = 0;
        return 1;
    }

    phaseFrame++;
    if (phase == 0 && phaseFrame >= SETTLE_FRAMES) {
        if (!Snapshot_Capture(&start)) {
            fprintf(stderr, "[quicksave-test] FAIL: initial capture failed\n");
            fflush(stderr);
            _Exit(1);
        }
        fprintf(stderr, "[quicksave-test] captured start (%zu bytes)\n", start.bytes);
        phase = 1;
        phaseFrame = 0;
    } else if (phase == 1 && phaseFrame >= replayFrames) {
        if (!Snapshot_Capture(&expectedEnd) || !Snapshot_Restore(&start)) {
            fprintf(stderr, "[quicksave-test] FAIL: endpoint capture or restore failed\n");
            fflush(stderr);
            _Exit(1);
        }
        if (!Snapshot_MatchesCurrent(&start, &region, &offset, &expected, &actual)) {
            fprintf(stderr, "[quicksave-test] FAIL: restore mismatch %s+0x%zx expected=%02x actual=%02x\n", region,
                    offset, (unsigned)expected, (unsigned)actual);
            fflush(stderr);
            _Exit(1);
        }
        fprintf(stderr, "[quicksave-test] restored start; replaying\n");
        phase = 2;
        phaseFrame = 0;
    } else if (phase == 2 && phaseFrame >= replayFrames) {
        if (!Snapshot_MatchesCurrent(&expectedEnd, &region, &offset, &expected, &actual)) {
            fprintf(stderr, "[quicksave-test] FAIL: replay mismatch %s+0x%zx expected=%02x actual=%02x\n", region,
                    offset, (unsigned)expected, (unsigned)actual);
            fflush(stderr);
            _Exit(1);
        }
        fprintf(stderr, "[quicksave-test] PASS: %u-frame replay byte-identical (%zu bytes)\n", replayFrames,
                expectedEnd.bytes);
        fflush(stderr);
        _Exit(0);
    }

    return 1;
}

/* Region tag for the state header + per-region state filenames. EU/JP get
 * their own state files (like tmc_eu.sav / tmc_jp.sav) so sessions never
 * even see another region's states; USA keeps the legacy names. */
static u32 ActiveRegionTag(void) {
    if (REGION_IS_EU)
        return 2;
    if (REGION_IS_JP)
        return 3;
    return 1; /* USA */
}

static void SlotFilename(int slot, char* out, size_t cap) {
    const char* prefix = REGION_IS_EU ? "state_eu" : REGION_IS_JP ? "state_jp" : "state";
    if (slot >= AUTO_SLOT_BASE) {
        snprintf(out, cap, "%s_auto_%d.bin", prefix, slot - AUTO_SLOT_BASE);
    } else if (slot == 0) {
        snprintf(out, cap, "%s_quick.bin", prefix);
    } else {
        snprintf(out, cap, "%s_%d.bin", prefix, slot);
    }
}

/* Framebuffer stride is the build's frame width, not the buffer's max width
 * -- reading at the wrong stride shears the capture. Same rule as
 * port_bugreport.cpp's kFrameW. */
#define FB_STRIDE MODE1_GBA_WIDTH

/* Grab the frame that is on screen right now into the slot's thumbnail.
 * Called from the save path, so it runs once per save, not per frame. */
static void CaptureThumbnail(int slot) {
    if (slot < 0 || slot >= NUM_SLOTS)
        return;
    u8* dst = sThumbs[slot];
    for (int y = 0; y < THUMB_H; ++y) {
        const u32* src = &virtuappu_frame_buffer[(y * 2) * FB_STRIDE];
        for (int x = 0; x < THUMB_W; ++x) {
            const u32 px = src[x * 2];
            *dst++ = (u8)(px & 0xFF);         /* R (ABGR LE: byte 0 is R) */
            *dst++ = (u8)((px >> 8) & 0xFF);  /* G */
            *dst++ = (u8)((px >> 16) & 0xFF); /* B */
            *dst++ = 0xFF;                    /* A -- the frame has no alpha */
        }
    }
    sThumbValid[slot] = 1;
    sThumbGeneration[slot]++;
}

/* Thumbnails live in a sidecar rather than inside the state file so that
 * adding them did not have to bump VERSION -- a bump rejects every existing
 * save on disk, and losing someone's states to gain a preview is a bad
 * trade. A state without a sidecar simply shows no picture. */
static void ThumbFilename(int slot, char* out, size_t cap) {
    char base[64];
    SlotFilename(slot, base, sizeof(base));
    const char* dot = strrchr(base, '.');
    const int stem = dot ? (int)(dot - base) : (int)strlen(base);
    snprintf(out, cap, "%.*s.thumb", stem, base);
}

static void WriteThumbToDisk(int slot) {
    if (slot < 0 || slot >= NUM_SLOTS || !sThumbValid[slot])
        return;
    char path[80];
    ThumbFilename(slot, path, sizeof(path));
    FILE* f = fopen(path, "wb");
    if (!f)
        return; /* best-effort: a missing preview is not worth a failed save */
    const u32 magic = THUMB_MAGIC;
    const u16 w = THUMB_W, h = THUMB_H;
    if (fwrite(&magic, sizeof(magic), 1, f) == 1 && fwrite(&w, sizeof(w), 1, f) == 1 &&
        fwrite(&h, sizeof(h), 1, f) == 1) {
        fwrite(sThumbs[slot], 1, THUMB_BYTES, f);
    }
    fclose(f);
}

static int ReadThumbFromDisk(int slot) {
    if (slot < 0 || slot >= NUM_SLOTS)
        return 0;
    char path[80];
    ThumbFilename(slot, path, sizeof(path));
    FILE* f = fopen(path, "rb");
    if (!f)
        return 0;
    u32 magic = 0;
    u16 w = 0, h = 0;
    int ok = fread(&magic, sizeof(magic), 1, f) == 1 && fread(&w, sizeof(w), 1, f) == 1 &&
             fread(&h, sizeof(h), 1, f) == 1 && magic == THUMB_MAGIC && w == THUMB_W && h == THUMB_H &&
             fread(sThumbs[slot], 1, THUMB_BYTES, f) == THUMB_BYTES;
    fclose(f);
    if (ok) {
        sThumbValid[slot] = 1;
        sThumbGeneration[slot]++;
    }
    return ok;
}

static int WriteSlotToDisk(int slot) {
    if (slot < 0 || slot >= NUM_SLOTS)
        return 0;
    Slot* s = &sSlots[slot];
    if (!s->valid || s->snapshot == NULL)
        return 0;

    char path[64];
    SlotFilename(slot, path, sizeof(path));
    FILE* f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "[quicksave] open %s for write failed\n", path);
        return 0;
    }
    const u32 magic = MAGIC;
    const u32 version = VERSION;
    const u32 total = (u32)s->bytes;
    const u64 saved_at = s->saved_at_unix;
    /* Executable image range at save time. A later process loads at a
     * different ASLR base, and every host pointer in the snapshot (entity
     * links, list heads, script contexts, .rodata hitboxes) must be shifted
     * by the difference -- see RelocatePointers. */
    const u64 session = SessionId();
    const u32 region_tag = ActiveRegionTag();
    if (fwrite(&magic, sizeof(magic), 1, f) != 1 || fwrite(&version, sizeof(version), 1, f) != 1 ||
        fwrite(&total, sizeof(total), 1, f) != 1 || fwrite(&saved_at, sizeof(saved_at), 1, f) != 1 ||
        fwrite(&session, sizeof(session), 1, f) != 1 || fwrite(&region_tag, sizeof(region_tag), 1, f) != 1) {
        fprintf(stderr, "[quicksave] header write failed for %s\n", path);
        fclose(f);
        return 0;
    }
    const size_t written = fwrite(s->snapshot, 1, s->bytes, f);
    fclose(f);
    if (written != s->bytes) {
        fprintf(stderr, "[quicksave] short write %s (%zu/%zu)\n", path, written, s->bytes);
        return 0;
    }
    return 1;
}

/* Reads the fixed 4-field slot header (magic, version, total, saved_at).
 * Returns 1 if all four fields read, 0 on short read. Field values are
 * not validated and no diagnostics are emitted — callers decide. */
static int ReadSlotHeader(FILE* f, u32* magic, u32* version, u32* total, u64* saved_at) {
    return fread(magic, sizeof(*magic), 1, f) == 1 && fread(version, sizeof(*version), 1, f) == 1 &&
           fread(total, sizeof(*total), 1, f) == 1 && fread(saved_at, sizeof(*saved_at), 1, f) == 1;
}

static int ReadSlotFromDisk(int slot) {
    if (slot < 0 || slot >= NUM_SLOTS)
        return 0;
    char path[64];
    SlotFilename(slot, path, sizeof(path));
    FILE* f = fopen(path, "rb");
    if (!f)
        return 0;
    u32 magic = 0, version = 0, total = 0;
    u64 saved_at = 0;
    u64 session = 0;
    Slot* s = &sSlots[slot];
    if (!ReadSlotHeader(f, &magic, &version, &total, &saved_at)) {
        fprintf(stderr, "[quicksave] short read on %s header, ignoring slot file\n", path);
        fclose(f);
        return 0;
    }
    if (magic != MAGIC || version != VERSION || total != (u32)TotalRegionBytes()) {
        fclose(f);
        return 0;
    }
    if (fread(&session, sizeof(session), 1, f) != 1) {
        fprintf(stderr, "[quicksave] short read on %s session header, ignoring slot file\n", path);
        fclose(f);
        return 0;
    }
    {
        u32 region_tag = 0;
        if (fread(&region_tag, sizeof(region_tag), 1, f) != 1) {
            fprintf(stderr, "[quicksave] short read on %s region header, ignoring slot file\n", path);
            fclose(f);
            return 0;
        }
        if (region_tag != ActiveRegionTag()) {
            fprintf(stderr, "[quicksave] %s was saved in a different ROM region (%u != %u) — refusing load\n", path,
                    region_tag, ActiveRegionTag());
            fclose(f);
            return 0;
        }
    }
    if (s->snapshot == NULL || s->bytes != total) {
        free(s->snapshot);
        s->snapshot = (u8*)malloc(total);
        if (!s->snapshot) {
            s->bytes = 0;
            s->valid = 0;
            fclose(f);
            return 0;
        }
        s->bytes = total;
    }
    const size_t got = fread(s->snapshot, 1, total, f);
    fclose(f);
    if (got != total) {
        fprintf(stderr, "[quicksave] short read on %s (%zu/%u bytes), ignoring slot file\n", path, got, total);
        s->valid = 0;
        return 0;
    }
    s->valid = 1;
    s->saved_at_unix = saved_at;
    s->saved_session = session;
    return 1;
}

/* ============================================================
 *   Public API
 * ============================================================ */

int Port_QuickSave_SaveSlot(int slot) {
    if (slot < 0 || slot >= NUM_SLOTS)
        return 0;
    if (!Snapshot_Capture(&sSlots[slot]))
        return 0;
    CaptureThumbnail(slot);
    /* Best-effort disk persistence — failure is non-fatal, the in-memory
     * snapshot still works for the session. */
    WriteSlotToDisk(slot);
    WriteThumbToDisk(slot);
    /* The files just changed under the probe cache; re-seed it rather than
     * invalidating, since we already know both answers. */
    sDiskProbed[slot] = 1;
    sDiskExists[slot] = 1;
    sDiskSavedAt[slot] = sSlots[slot].saved_at_unix;
    sThumbProbed[slot] = 1;
    fprintf(stderr, "[quicksave] slot %d saved (%zu bytes)\n", slot, sSlots[slot].bytes);
    return 1;
}

int Port_QuickSave_LoadSlot(int slot) {
    /* Refuse any state restore under Console-Parity — covers the menu/imgui
     * load buttons too, not just the F-key hotkeys. */
    if (Port_Config_GetConsoleParity())
        return 0;
    if (slot < 0 || slot >= NUM_SLOTS)
        return 0;
    if (!sSlots[slot].valid) {
        /* Try loading from disk first — handles "fresh launch, never
         * saved this session but slot exists on disk from last run". */
        if (!ReadSlotFromDisk(slot)) {
            fprintf(stderr, "[quicksave] slot %d empty\n", slot);
            return 0;
        }
    }
    const int how = Snapshot_Restore(&sSlots[slot]);
    if (how == 0) {
        fprintf(stderr, "[quicksave] slot %d restore failed\n", slot);
        return 0;
    }
    fprintf(stderr, "[quicksave] slot %d %s\n", slot, how == 2 ? "resumed (room re-entered)" : "restored");
    {
        /* Tell the Reborn-parity layer a resume just happened so it
         * can swallow the next queued Ezlo hint (if that toggle is on). */
        extern void Port_Reborn_NotifyJustResumed(void);
        Port_Reborn_NotifyJustResumed();
    }
    return how; /* 1 exact, 2 resumed -- callers word the toast accordingly */
}

/* Reads the on-disk header once per slot and caches existence + timestamp. */
static void ProbeSlotOnDisk(int slot) {
    if (slot < 0 || slot >= NUM_SLOTS || sDiskProbed[slot])
        return;
    sDiskProbed[slot] = 1;
    sDiskExists[slot] = 0;
    sDiskSavedAt[slot] = 0;

    char path[64];
    SlotFilename(slot, path, sizeof(path));
    FILE* f = fopen(path, "rb");
    if (!f)
        return;
    u32 magic = 0, version = 0, total = 0;
    u64 saved_at = 0;
    /* A file from an older build (different version or region layout) would
     * be refused by ReadSlotFromDisk, so present it as empty rather than as a
     * slot whose Load button silently does nothing. */
    if (ReadSlotHeader(f, &magic, &version, &total, &saved_at) && magic == MAGIC && version == VERSION &&
        total == (u32)TotalRegionBytes()) {
        sDiskExists[slot] = 1;
        sDiskSavedAt[slot] = saved_at;
    }
    fclose(f);
}

int Port_QuickSave_HasSlot(int slot) {
    if (slot < 0 || slot >= NUM_SLOTS)
        return 0;
    if (sSlots[slot].valid)
        return 1;
    /* Probe disk so the menu can label populated-on-disk slots correctly
     * before the user touches them. */
    ProbeSlotOnDisk(slot);
    return sDiskExists[slot];
}

/* ---- Practice point -------------------------------------------------- *
 * A dedicated in-memory snapshot for the speedrun practice mode, kept
 * separate from the F1..F5 user slots so practising a segment never clobbers
 * a manual save. In-process only (no disk, no pointer relocation needed
 * within one run), so set/reload is a sub-ms memcpy.
 * Driven from port_practice.c via the Port_Practice_SetPoint/LoadPoint API. */
static Slot sPracticeSlot;

int Port_QuickSave_SavePractice(void) {
    if (!Snapshot_Capture(&sPracticeSlot))
        return 0;
    fprintf(stderr, "[quicksave] practice point set (%zu bytes)\n", sPracticeSlot.bytes);
    return 1;
}

int Port_QuickSave_LoadPractice(void) {
    if (Port_Config_GetConsoleParity())
        return 0;
    if (!sPracticeSlot.valid) {
        fprintf(stderr, "[quicksave] practice point empty\n");
        return 0;
    }
    if (!Snapshot_Restore(&sPracticeSlot)) {
        fprintf(stderr, "[quicksave] practice point restore failed\n");
        return 0;
    }
    {
        extern void Port_Reborn_NotifyJustResumed(void);
        Port_Reborn_NotifyJustResumed();
    }
    return 1;
}

int Port_QuickSave_HasPractice(void) {
    return sPracticeSlot.valid ? 1 : 0;
}

u64 Port_QuickSave_SlotTimestamp(int slot) {
    if (slot < 0 || slot >= NUM_SLOTS)
        return 0;
    if (sSlots[slot].valid)
        return sSlots[slot].saved_at_unix;
    /* Probe the disk file's timestamp header so the menu can show
     * "last saved" even for slots that haven't been loaded into memory. */
    ProbeSlotOnDisk(slot);
    return sDiskSavedAt[slot];
}

/* Legacy single-slot API — slot 0 is the F5/F6 quicksave. */
int Port_QuickSave(void) {
    return Port_QuickSave_SaveSlot(0);
}
int Port_QuickLoad(void) {
    return Port_QuickSave_LoadSlot(0);
}
int Port_QuickSave_HasSnapshot(void) {
    return Port_QuickSave_HasSlot(0);
}

/* Auto-save — call once per frame from VBlankIntrWait. Saves to the
 * next slot in the auto-save ring if enabled and the configured
 * interval has elapsed since the last auto-save. */
static void TakeAutoSnapshot(const char* reason) {
    const int slot = sAutoNextSlot;
    sAutoNextSlot++;
    if (sAutoNextSlot >= AUTO_SLOT_BASE + NUM_AUTO_SLOTS) {
        sAutoNextSlot = AUTO_SLOT_BASE;
    }
    if (Port_QuickSave_SaveSlot(slot)) {
        fprintf(stderr, "[autosave] saved to ring slot %d (%s)\n", slot, reason);
    }
}

/* Headless cross-process save-state check, two halves run as two processes:
 *
 *   TMC_REPRO_SAVE_SLOT_ON_MSG=N  the first frame a textbox is open while the
 *                                 player has no control (cutscene dialogue,
 *                                 area-name banner), save slot N and exit.
 *                                 That is the moment a snapshot with partial
 *                                 coverage strands the player.
 *   TMC_REPRO_LOAD_SLOT=N         once gameplay is reached, load slot N (left
 *                                 on disk by the first process), then report
 *                                 position and control mode every second. A
 *                                 good load shows ctrl returning to 0 and,
 *                                 with the harness's TMC_REPRO_MASH_A=1 and
 *                                 TMC_REPRO_HOLD_DIR=l, the player moving.
 *
 * Both need TMC_AUTOPLAY=1 and rely on port_repro_npc_talk.c to bootstrap
 * into a room (TMC_REPRO_NPC_TALK=1 for the first, automatic for the second).
 * Input has to be forced from that harness, not from here: this runs after
 * Port_UpdateInput() has already sampled the frame. */
static void ReproLoadSlotTick(void) {
    static int pending = -2;
    static int saveOnMsg = -1;
    static unsigned int frames = 0;
    static int reports = 0;
    if (pending == -2) {
        const char* env = getenv("TMC_REPRO_LOAD_SLOT");
        pending = (env != NULL && env[0] != '\0') ? atoi(env) : -1;
        env = getenv("TMC_REPRO_SAVE_SLOT_ON_MSG");
        saveOnMsg = (env != NULL && env[0] != '\0') ? atoi(env) : -1;
    }
    if (gMain.task != TASK_GAME)
        return;
    if (saveOnMsg >= 0 && (gMessage.state & 0x7f) != 0 && gPlayerState.controlMode != CONTROL_ENABLED) {
        fprintf(stderr, "[quicksave-test] textbox open with ctrl=%d at x=%d y=%d area=%u room=%u -- saving slot %d\n",
                gPlayerState.controlMode, gPlayerEntity.base.x.HALF.HI, gPlayerEntity.base.y.HALF.HI,
                gRoomControls.area, gRoomControls.room, saveOnMsg);
        const int ok = Port_QuickSave_SaveSlot(saveOnMsg);
        fprintf(stderr, "[quicksave-test] %s\n", ok ? "saved; exiting" : "FAIL: save failed");
        fflush(stderr);
        _Exit(ok ? 0 : 1);
    }
    if (pending == -1)
        return;
    frames++;
    if (pending >= 0) {
        if (frames < 60)
            return;
        fprintf(stderr, "[quicksave-test] before load: player x=%d y=%d ctrl=%d\n", gPlayerEntity.base.x.HALF.HI,
                gPlayerEntity.base.y.HALF.HI, gPlayerState.controlMode);
        fprintf(stderr, "[quicksave-test] loading slot %d\n", pending);
        if (!Port_QuickSave_LoadSlot(pending))
            fprintf(stderr, "[quicksave-test] FAIL: slot %d did not load\n", pending);
        pending = -3; /* loaded; now just report */
        frames = 0;
        return;
    }
    if (frames % 60 == 0 && reports < 120) {
        reports++;
        fprintf(stderr, "[quicksave-test] t+%us: player x=%d y=%d ctrl=%d area=%u room=%u\n", frames / 60,
                gPlayerEntity.base.x.HALF.HI, gPlayerEntity.base.y.HALF.HI, gPlayerState.controlMode,
                gRoomControls.area, gRoomControls.room);
    }
}

void Port_QuickSave_AutoTick(void) {
    ReproLoadSlotTick();
    if (QuickSaveReplayTestTick() || !sAutoEnabled)
        return;
    const u64 now = SDL_GetTicks();

    /* Area-change trigger. gRoomControls is the engine's source-of-
     * truth for the current area/room; we just compare against the
     * last value we observed and fire a snapshot on transition. The
     * first poll seeds the cache without saving (sLastSeen* both
     * 0xFF) so we don't double-save on boot. */
    if (sAutoOnAreaChange) {
        /* gRoomControls is declared in include/room.h, already in scope
         * via include/save.h above. */
        const u8 area = gRoomControls.area;
        const u8 room = gRoomControls.room;
        if (sLastSeenArea == 0xFF && sLastSeenRoom == 0xFF) {
            sLastSeenArea = area;
            sLastSeenRoom = room;
        } else if (area != sLastSeenArea || room != sLastSeenRoom) {
            sLastSeenArea = area;
            sLastSeenRoom = room;
            sAutoLastSaveTicksMs = now;
            TakeAutoSnapshot("area-change");
            return;
        }
    }

    /* Interval trigger. */
    if (sAutoLastSaveTicksMs == 0) {
        sAutoLastSaveTicksMs = now;
        return;
    }
    if (now - sAutoLastSaveTicksMs < sAutoIntervalMs)
        return;
    sAutoLastSaveTicksMs = now;
    TakeAutoSnapshot("interval");
}

int Port_QuickSave_AutoOnAreaChangeEnabled(void) {
    return sAutoOnAreaChange;
}
void Port_QuickSave_SetAutoOnAreaChange(int on) {
    sAutoOnAreaChange = on ? 1 : 0;
    /* Reset the seen-area cache when toggled on so the next change
     * doesn't fire spuriously against pre-toggle history. */
    if (on) {
        sLastSeenArea = 0xFF;
        sLastSeenRoom = 0xFF;
    }
}

void Port_QuickSave_SetAutoEnabled(int enabled) {
    sAutoEnabled = enabled ? 1 : 0;
    if (enabled)
        sAutoLastSaveTicksMs = SDL_GetTicks();
}

int Port_QuickSave_AutoEnabled(void) {
    return sAutoEnabled;
}

void Port_QuickSave_SetAutoIntervalMs(u32 ms) {
    if (ms < 5000)
        ms = 5000; /* clamp to 5s minimum — anything
                      faster would thrash on busy
                      frames and risk visible hitches. */
    if (ms > 600000)
        ms = 600000; /* 10 minute cap */
    sAutoIntervalMs = ms;
}

u32 Port_QuickSave_AutoIntervalMs(void) {
    return sAutoIntervalMs;
}

int Port_QuickSave_SlotCount(void) {
    return NUM_SLOTS;
}

/* Manual slots are 0..AUTO_SLOT_BASE-1. The auto ring above them is
 * excluded from everything the user selects: the ring cursor overwrites
 * those slots on its own schedule, so parking a hand-made state there
 * would silently lose it. They stay loadable from the Saves tab. */
int Port_QuickSave_ManualSlotCount(void) {
    return NUM_MANUAL_SLOTS;
}

int Port_QuickSave_SelectedSlot(void) {
    int slot = Port_Config_SaveStateSlot();
    if (slot < 0 || slot >= AUTO_SLOT_BASE)
        slot = 0;
    return slot;
}

void Port_QuickSave_SetSelectedSlot(int slot) {
    if (slot < 0 || slot >= AUTO_SLOT_BASE)
        return;
    Port_Config_SetSaveStateSlot(slot);
}

/* Wraps at both ends so a single bound button can walk the whole ring. */
void Port_QuickSave_CycleSelectedSlot(int direction) {
    int slot = Port_QuickSave_SelectedSlot() + (direction >= 0 ? 1 : -1);
    if (slot < 0)
        slot = AUTO_SLOT_BASE - 1;
    else if (slot >= AUTO_SLOT_BASE)
        slot = 0;
    Port_Config_SetSaveStateSlot(slot);
}

int Port_QuickSave_SaveSelected(void) {
    return Port_QuickSave_SaveSlot(Port_QuickSave_SelectedSlot());
}

/* One-button "save to a new slot": advance the cursor first, then save, so
 * repeated presses lay down a rolling history across the manual slots instead
 * of clobbering one. The cursor lands on what was just written, which is also
 * what a plain load-selected then restores -- press save twice and load, and
 * you get the second save, not a surprise.
 *
 * It wraps, so the oldest manual slot is eventually overwritten. That is the
 * point: it is a quick-save, and a state you want kept can be parked in a slot
 * the cursor is not walking, or simply re-saved. */
int Port_QuickSave_SaveToNewSlot(void) {
    Port_QuickSave_CycleSelectedSlot(+1);
    return Port_QuickSave_SaveSlot(Port_QuickSave_SelectedSlot());
}

int Port_QuickSave_LoadSelected(void) {
    return Port_QuickSave_LoadSlot(Port_QuickSave_SelectedSlot());
}
int Port_QuickSave_AutoSlotBase(void) {
    return AUTO_SLOT_BASE;
}
int Port_QuickSave_AutoSlotCount(void) {
    return NUM_AUTO_SLOTS;
}


/* ------------------------------------------------------------------ */
/*  Slot thumbnails (menu-facing)                                     */
/* ------------------------------------------------------------------ */

void Port_QuickSave_ThumbnailSize(int* w, int* h) {
    if (w)
        *w = THUMB_W;
    if (h)
        *h = THUMB_H;
}

/* RGBA8, THUMB_W x THUMB_H, or NULL when the slot has no preview. Slots
 * written by an older build (or restored from a backup without the sidecar)
 * legitimately have none. Reads the sidecar on first ask so previews show up
 * for states that exist on disk but have not been loaded this session. */
const unsigned char* Port_QuickSave_SlotThumbnail(int slot) {
    if (slot < 0 || slot >= NUM_SLOTS)
        return NULL;
    if (!sThumbValid[slot] && !sThumbProbed[slot] && Port_QuickSave_HasSlot(slot)) {
        sThumbProbed[slot] = 1;
        ReadThumbFromDisk(slot);
    }
    return sThumbValid[slot] ? sThumbs[slot] : NULL;
}

/* Changes whenever the slot's pixels change. The menu caches one GPU texture
 * per slot and re-uploads only when this moves. */
unsigned int Port_QuickSave_SlotThumbnailGeneration(int slot) {
    if (slot < 0 || slot >= NUM_SLOTS)
        return 0;
    return sThumbGeneration[slot];
}
