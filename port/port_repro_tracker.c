/*
 * port_repro_tracker.c — headless check of the check tracker (port_tracker.c).
 *
 * Boots save slot 0 into gameplay and prints what the tracker reads from it:
 * per-state counts for checks and fusions, how many checks resolved through
 * a room's chest list versus a flag in the key, and every check and fusion
 * that is not done. Then, on a scratch copy of the save state, it sets each
 * resolved flag / scripted reward record / fused bit in turn and asserts the
 * state flips, restoring gSave afterwards. Exits 0 on PASS, 1 on FAIL.
 *
 * Enable: TMC_REPRO_TRACKER=1. The first slot in use in the tmc.sav next to
 * the binary is played; with none, a fresh synthetic save is.
 * TMC_REPRO_TRACKER_LIST=0 skips the per-row listing.
 *
 * TMC_REPRO_TRACKER_WARP=1 adds a slower pass that checks what a flag-keyed
 * check points at: it warps into each such check's room and looks for a live
 * object whose flag is that local flag. A miss is not necessarily wrong (dig
 * spots, pots and bushes only spawn their item when disturbed, and some
 * items need an event first), so the pass reports and never fails.
 *
 * TMC_REPRO_TRACKER_HOOKS=1 plays the DHC B2 King and Melari reward scenes
 * and checks the Simulation chest's data, reporting whether each one's rando
 * hook fires (see HookPassTick). Like the warp pass it never fails.
 *
 * TMC_REPRO_TRACKER_UI=1 opens the settings overlay and walks to the Tracker
 * group with synthetic key presses (the shipped config.json binds the D-pad
 * to the arrow keys and A to X), switches it to fusions and opens a header,
 * so the page is drawn with real data. It checks that nothing asserts; it
 * does not look at the pixels.
 */

#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "area.h"
#include "common.h"
#include "fileselect.h"
#include "flags.h"
#include "game.h"
#include "item.h"
#include "kinstone.h"
#include "main.h"
#include "npc.h"
#include "player.h"
#include "region.h"
#include "room.h"
#include "save.h"
#include "entity.h"
#include "port_debug_actions.h"
#include "port_debug_query.h"
#include "port_debug_menu.h"
#include "port_repro.h"
#include "port_runtime_config.h"
#include "port_tracker.h"
#include "rando/rando.h"
#include "rando/rando_keymap.h"
#include "rando/rando_runtime.h"

extern void SetActiveSave(u32 idx);

static const char* StateName(PortTrackerState st) {
    switch (st) {
        case PORT_TRACKER_OPEN:
            return "open";
        case PORT_TRACKER_PENDING:
            return "pending";
        case PORT_TRACKER_DONE:
            return "done";
        default:
            return "untracked";
    }
}

/* Write the save record the game writes on giving a scripted check's reward
 * (the writer is named where it is not obvious), or, for a check with none,
 * give its vanilla item. False when there is nothing to write. */
static bool MarkScriptedGiven(const RandoLocationDef* def) {
    const unsigned group = (def->key >> 24) & 0x7F, a = (def->key >> 16) & 0xFF, b = (def->key >> 8) & 0xFF;
    switch (group) {
        case RANDO_SCRIPTED_KEY_GORON_MERCHANT: {
            /* goronMerchantShopManager.c: buying slot b of the set on sale
             * (set a is on sale at restock level a) sets its sold flag. */
            static const u16 kLevels[] = { GORON_KAKERA_LV2, GORON_KAKERA_LV3, GORON_KAKERA_LV4, GORON_KAKERA_LV5 };
            if (a > 0)
                SetGlobalFlag(kLevels[a - 1]);
            SetGlobalFlag(GORON_KAKERA_L + b);
            return true;
        }
        case RANDO_SCRIPTED_KEY_CUCCO:
            /* cuccoMinigame.c: winning round a + 1 advances the level to it;
             * the tenth round sets ANJU_HEART. */
            if (a == 9) {
                SetGlobalFlag(ANJU_HEART);
            } else {
                static const u16 kBits[] = { ANJU_LV_BIT0, ANJU_LV_BIT1, ANJU_LV_BIT2, ANJU_LV_BIT3 };
                for (unsigned bit = 0; bit < 4; ++bit) {
                    if ((a + 1) >> bit & 1)
                        SetGlobalFlag(kBits[bit]);
                    else
                        ClearGlobalFlag(kBits[bit]);
                }
            }
            return true;
        case RANDO_SCRIPTED_KEY_STOCKWELL:
            switch (a) {
                case RANDO_STOCKWELL_SLOT_80:
                    SetLocalFlagByBank(FLAG_BANK_2, SHOP00_SAIFU);
                    return true;
                case RANDO_STOCKWELL_SLOT_600:
                    SetLocalFlagByBank(FLAG_BANK_2, SHOP00_YAZUTSU);
                    return true;
                case RANDO_STOCKWELL_SLOT_EXTRA_600:
                    SetLocalFlagByBank(FLAG_BANK_2, SHOP00_BOMBBAG);
                    return true;
            }
            break;
        case RANDO_SCRIPTED_KEY_SCRUB:
            if (a == RANDO_SCRUB_KEY_BOTTLE) {
                SetGlobalFlag(AKINDO_BOTTLE_SELL);
                return true;
            }
            break;
        case RANDO_SCRIPTED_KEY_SPECIAL:
            switch (a) {
                case RANDO_SPECIAL_KEY_BELL_HP:
                    SetLocalFlagByBankB(GetFlagBankOffset(AREA_HYRULE_TOWN), 0xD0);
                    return true;
                case RANDO_SPECIAL_KEY_MINISH_GREAT_FAIRY:
                    SetLocalFlagByBank(FLAG_BANK_2, IZUMI_01_FAIRY);
                    return true;
                case RANDO_SPECIAL_KEY_CRENEL_GREAT_FAIRY:
                    SetLocalFlagByBank(FLAG_BANK_2, IZUMI_02_FAIRY);
                    return true;
                case RANDO_SPECIAL_KEY_VALLEY_GREAT_FAIRY:
                    SetLocalFlagByBank(FLAG_BANK_2, IZUMI_00_FAIRY);
                    return true;
                case RANDO_SPECIAL_KEY_BOMB_MINISH_REMOTES:
                    SetLocalFlagByBank(FLAG_BANK_2, KHOUSE26_REMOCON);
                    return true;
                case RANDO_SPECIAL_KEY_CRYPT_PRIZE:
                    SetLocalFlagByBank(FLAG_BANK_3, OUBO_KAKERA);
                    return true;
                case RANDO_SPECIAL_KEY_DHC_KING:
                    SetLocalFlagByBank(FLAG_BANK_10, LV6_1d_KEYGET);
                    return true;
                case RANDO_SPECIAL_KEY_GREGAL_SHELLS:
                    SetLocalFlagByBank(FLAG_BANK_2, SORA_ELDER_TALK1ST);
                    return true;
                case RANDO_SPECIAL_KEY_BIGGORON:
                    /* script_BigGoronTalk sets both on taking the shield,
                     * script_BigGoronMirrorShield clears EXCHG on handing
                     * the mirror shield back. */
                    SetLocalFlagByBankB(FLAG_BANK_1, DAIGORON_SHIELD);
                    ClearLocalFlagByBankB(FLAG_BANK_1, DAIGORON_EXCHG);
                    return true;
                case RANDO_SPECIAL_KEY_MELARI:
                    SetInventoryValue(ITEM_QST_BROKEN_SWORD, 2);
                    return true;
                case RANDO_SPECIAL_KEY_CAFE_LADY:
                    SetLocalFlagByBankB(FLAG_BANK_1, MACHI_MES_60);
                    return true;
                case RANDO_SPECIAL_KEY_DOG_BOTTLE:
                    if (REGION_IS_EU)
                        SetInventoryValue(ITEM_QST_DOGFOOD, 2);
                    else
                        SetGlobalFlag(BIN_DOGFOOD);
                    return true;
            }
            break;
    }
    /* No record: vanilla play reads the check's own unique item. */
    if (def->vanilla_item != ITEM_NONE) {
        SetInventoryValue(def->vanilla_item, 1);
        return true;
    }
    return false;
}

static int RunChecks(void) {
    const char* list_env = getenv("TMC_REPRO_TRACKER_LIST");
    const int list = !(list_env && strcmp(list_env, "0") == 0);
    int fails = 0;

    fprintf(stderr, "[tracker] save: progress=%u fused=%u enemies=%u area=0x%02x room=0x%02x\n",
            (unsigned)gSave.global_progress, (unsigned)gSave.kinstones.fusedCount, (unsigned)gSave.enemies_killed,
            (unsigned)gRoomControls.area, (unsigned)gRoomControls.room);
    size_t n = Port_Tracker_CheckCount();
    int counts[4] = { 0 }, via_chest = 0, via_flag = 0, scripted = 0;
    for (size_t i = 0; i < n; ++i) {
        PortTrackerState st = Port_Tracker_CheckState(i);
        counts[st]++;
        uint32_t key = Rando_GetLocationDef((RandoLocationId)i)->key;
        if (key & 0x80000000u)
            scripted++;
        else if (Rando_GetChestLocalFlag((key >> 16) & 0xFF, (key >> 8) & 0xFF, key & 0xFF) != 0xFF)
            via_chest++;
        else
            via_flag++;
        if (list && st != PORT_TRACKER_DONE)
            fprintf(stderr, "[tracker] check %3u %-9s %s\n", (unsigned)i, StateName(st), Port_Tracker_CheckName(i));
    }
    fprintf(stderr, "[tracker] checks: %u total, open=%d done=%d untracked=%d | chest=%d flag=%d scripted=%d\n",
            (unsigned)n, counts[PORT_TRACKER_OPEN], counts[PORT_TRACKER_DONE], counts[PORT_TRACKER_UNTRACKED],
            via_chest, via_flag, scripted);

    int fcounts[4] = { 0 };
    for (unsigned k = 1; k <= PORT_TRACKER_FUSION_COUNT; ++k) {
        PortTrackerState st = Port_Tracker_FusionState(k);
        fcounts[st]++;
        if (list && st != PORT_TRACKER_DONE) {
            fprintf(stderr, "[tracker] fusion %3u %-7s area=%d %s\n", k, StateName(st), Port_Tracker_FusionArea(k),
                    Port_Tracker_FusionName(k));
            PortTrackerFuser fusers[8];
            size_t nf = Port_Tracker_FusionFusers(k, fusers, 8);
            for (size_t f = 0; f < nf && f < 8; ++f)
                fprintf(stderr, "[tracker]      fuser %s area=%d%s%s\n", fusers[f].name, fusers[f].area,
                        fusers[f].offering ? " offering" : "", fusers[f].locked ? " locked" : "");
            if (Port_Tracker_FusionShared(k))
                fprintf(stderr, "[tracker]      shared\n");
        }
    }
    fprintf(stderr, "[tracker] fusions: open=%d pending=%d done=%d\n", fcounts[PORT_TRACKER_OPEN],
            fcounts[PORT_TRACKER_PENDING], fcounts[PORT_TRACKER_DONE]);

    /* Flip tests on a scratch copy: every open plain check must turn done
     * when its resolved flag is set, every open scripted check when the
     * game's record of the reward is written, every unfused fusion must
     * leave open when its bit is set. */
    static SaveFile backup;
    memcpy(&backup, &gSave, sizeof(gSave));
    for (size_t i = 0; i < n; ++i) {
        uint32_t key = Rando_GetLocationDef((RandoLocationId)i)->key;
        if ((key & 0x80000000u) || Port_Tracker_CheckState(i) != PORT_TRACKER_OPEN)
            continue;
        unsigned area = (key >> 16) & 0xFF, room = (key >> 8) & 0xFF, low = key & 0xFF;
        unsigned bank = GetFlagBankOffset(area);
        unsigned chest = Rando_GetChestLocalFlag(area, room, low);
        if (chest != 0xFF)
            SetLocalFlagByBank(bank, chest);
        else
            SetLocalFlagByBankB(bank, low);
        if (Port_Tracker_CheckState(i) != PORT_TRACKER_DONE) {
            fprintf(stderr, "[tracker] FAIL: check %u (%s) did not flip\n", (unsigned)i, Port_Tracker_CheckName(i));
            fails++;
        }
    }
    /* Scripted checks: from the untouched save each time, since one write
     * (a Goron restock, a cucco level) can close several checks at once. */
    for (size_t i = 0; i < n; ++i) {
        const RandoLocationDef* def = Rando_GetLocationDef((RandoLocationId)i);
        memcpy(&gSave, &backup, sizeof(gSave));
        if ((def->key & 0x80000000u) == 0 || Port_Tracker_CheckState(i) != PORT_TRACKER_OPEN)
            continue;
        if (!MarkScriptedGiven(def)) {
            fprintf(stderr, "[tracker] FAIL: check %u (%s) is tracked but has no flip write\n", (unsigned)i,
                    Port_Tracker_CheckName(i));
            fails++;
        } else if (Port_Tracker_CheckState(i) != PORT_TRACKER_DONE) {
            fprintf(stderr, "[tracker] FAIL: check %u (%s) did not flip\n", (unsigned)i, Port_Tracker_CheckName(i));
            fails++;
        }
    }
    memcpy(&gSave, &backup, sizeof(gSave));
    for (unsigned k = 1; k <= PORT_TRACKER_FUSION_COUNT; ++k) {
        if (Port_Tracker_FusionState(k) != PORT_TRACKER_OPEN)
            continue;
        WriteBit(&gSave.kinstones.fusedKinstones, k);
        if (Port_Tracker_FusionState(k) == PORT_TRACKER_OPEN) {
            fprintf(stderr, "[tracker] FAIL: fusion %u did not flip\n", k);
            fails++;
        }
    }
    memcpy(&gSave, &backup, sizeof(gSave));
    return fails;
}

static void PressKey(SDL_Keycode key, bool down) {
    SDL_Event ev;
    SDL_zero(ev);
    ev.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    ev.key.key = key;
    ev.key.down = down;
    SDL_PushEvent(&ev);
}

/* Returns false when done. Presses are 8 frames apart, down then up. */
static bool UiPassTick(unsigned int frame) {
    /* Open the overlay, 8 x Down to Tracker (9th group), A to enter it,
     * A on the view button (a group opens with the cursor on its first
     * widget) for fusions, then Down x2 past "Hide done" + A on the first
     * header. */
    static const SDL_Keycode kSteps[] = {
        0,           SDLK_DOWN,  SDLK_DOWN,  SDLK_DOWN, SDLK_DOWN, SDLK_DOWN, SDLK_DOWN,
        SDLK_DOWN,   SDLK_DOWN,  SDLK_X,     0,          SDLK_X,    SDLK_DOWN,  SDLK_DOWN,
        SDLK_X,
    };
    static unsigned int start = 0;
    static bool done = false;
    if (done)
        return false;
    if (start == 0) {
        start = frame;
        Port_DebugMenu_Toggle();
        fprintf(stderr, "[tracker] ui: overlay open=%d\n", (int)Port_DebugMenu_IsOpen());
        return true;
    }
    unsigned int t = frame - start;
    unsigned int step = t / 8;
    const unsigned int count = sizeof(kSteps) / sizeof(kSteps[0]);
    if (step < count) {
        if (kSteps[step] != 0 && t % 8 == 0) {
            fprintf(stderr, "[tracker] ui: press %s\n", SDL_GetKeyName(kSteps[step]));
            PressKey(kSteps[step], true);
        }
        if (kSteps[step] != 0 && t % 8 == 3)
            PressKey(kSteps[step], false);
        return true;
    }
    if (t < count * 8 + 60)
        return true; /* let the page draw for a second */
    fprintf(stderr, "[tracker] ui: drove %u steps, overlay open=%d\n", count, (int)Port_DebugMenu_IsOpen());
    if (Port_DebugMenu_IsOpen())
        Port_DebugMenu_Toggle(); /* the overlay pauses the game */
    done = true;
    return false;
}

/* Is there a live object in the current room carrying local flag `flag`? */
static bool RoomHasObjectWithFlag(unsigned flag) {
    for (int l = 0; l < 9; ++l) {
        LinkedList* list = &gEntityLists[l];
        for (Entity* e = list->first; e != NULL && e != (Entity*)list; e = e->next) {
            if (e->kind != OBJECT)
                continue;
            u16 f = ((GenericEntity*)e)->field_0x86.HWORD;
            /* CheckFlags packing: type in bits 14-15, 0 = local. */
            if (f != 0 && (f & 0xC000) == 0 && (f & 0x3FF) == flag)
                return true;
        }
    }
    return false;
}

/* One check per step: warp, let the room load, look. Returns false when
 * every flag-keyed check has been visited. */
static bool WarpPassTick(unsigned int frame) {
    static size_t next = 0;
    static int phase = 0;
    static unsigned int warped_at = 0;
    static int seen = 0, found = 0;
    size_t n = Port_Tracker_CheckCount();

    /* Warps land in boss arenas and lava; a game over would end the pass. */
    gSave.stats.health = gSave.stats.maxHealth;
    gPlayerEntity.base.iframes = 30;

    if (phase == 1) {
        if (frame < warped_at + 45)
            return true;
        const RandoLocationDef* def = Rando_GetLocationDef((RandoLocationId)next);
        unsigned area = (def->key >> 16) & 0xFF, room = (def->key >> 8) & 0xFF;
#ifdef MULTI_REGION
        unsigned flag = Port_RemapBaselineLocalFlag(GetFlagBankOffset(area), def->key & 0xFF);
#else
        unsigned flag = def->key & 0xFF; /* single-region build: USA ordinals, no remap */
#endif
        bool here = gRoomControls.area == area && gRoomControls.room == room;
        bool hit = here && RoomHasObjectWithFlag(flag);
        seen++;
        found += hit;
        fprintf(stderr, "[tracker] warp %3u %-4s %s%s\n", (unsigned)next, hit ? "ok" : "miss",
                Port_Tracker_CheckName(next), here ? "" : " (warp did not arrive)");
        next++;
        phase = 0;
        return true;
    }

    for (; next < n; ++next) {
        const RandoLocationDef* def = Rando_GetLocationDef((RandoLocationId)next);
        uint32_t key = def->key;
        if (key & 0x80000000u)
            continue;
        unsigned area = (key >> 16) & 0xFF, room = (key >> 8) & 0xFF;
        if (Rando_GetChestLocalFlag(area, room, key & 0xFF) != 0xFF)
            continue;
        if (!Port_DebugAction_AreaIsWarpable((unsigned char)area))
            continue;
        /* A boss's heart container only exists after the fight, and warping
         * into some arenas ends gameplay. */
        if (strstr(def->name, "Boss") != NULL)
            continue;
        unsigned short x = 0, y = 0, w = 0, h = 0;
        unsigned char layer = 1;
        if (!Port_DebugAction_WarpSpawnOverride((unsigned char)area, (unsigned char)room, &x, &y, &layer)) {
            Port_DebugQuery_RoomDimensions((unsigned char)area, (unsigned char)room, &w, &h);
            x = w / 2;
            y = h / 2;
        }
        Port_DebugAction_Warp((unsigned char)area, (unsigned char)room, x, y, layer);
        warped_at = frame;
        phase = 1;
        return true;
    }
    fprintf(stderr, "[tracker] warp pass: %d of %d flag-keyed checks have a live object with their flag\n", found,
            seen);
    return false;
}

/* TMC_REPRO_TRACKER_SHOTS=1: for each fusion named only "Event", warp to its
 * world event unfused and fused and write shot_<id>_{0,1}.png, to see what
 * the fusion changes. */
static bool ShotPassTick(unsigned int frame) {
    static unsigned k = 10;
    static int phase = 0; /* 0 warp unfused, 1 shoot, 2 warp fused, 3 shoot */
    static unsigned int at = 0;
    extern int Port_CaptureBaseFramebufferPNG(const char* path);

    gSave.stats.health = gSave.stats.maxHealth;
    gPlayerEntity.base.iframes = 30;
    /* TMC_REPRO_TRACKER_SHOTS_IDS=11,27,... picks the fusions instead. */
    const char* ids = getenv("TMC_REPRO_TRACKER_SHOTS_IDS");
    for (; k <= PORT_TRACKER_FUSION_COUNT; ++k) {
        if (ids && *ids) {
            char want[8];
            snprintf(want, sizeof(want), ",%u,", k);
            char list[256];
            snprintf(list, sizeof(list), ",%s,", ids);
            if (strstr(list, want) != NULL)
                break;
        } else if (strncmp(Port_Tracker_FusionName(k), "Event", 5) == 0) {
            break;
        }
    }
    if (k > PORT_TRACKER_FUSION_COUNT)
        return false;
    const WorldEvent* ev = &GetWorldEvents()[GetFusionWorldEventId(k)];
    if (phase == 0 || phase == 2) {
        if (phase == 0)
            ClearBit(&gSave.kinstones.fusedKinstones, k);
        else
            WriteBit(&gSave.kinstones.fusedKinstones, k);
        Port_DebugAction_Warp(ev->area, ev->room, ev->x, (unsigned short)(ev->y + 24), 1);
        at = frame;
        phase++;
        return true;
    }
    if (frame < at + 180)
        return true;
    char path[64];
    snprintf(path, sizeof(path), "shot_%03u_%d.png", k, phase == 1 ? 0 : 1);
    Port_CaptureBaseFramebufferPNG(path);
    fprintf(stderr, "[tracker] shot %s area=%u room=%u type=%u here=%d\n", path, ev->area, ev->room, ev->type,
            gRoomControls.area == ev->area && gRoomControls.room == ev->room);
    if (phase == 3) {
        ClearBit(&gSave.kinstones.fusedKinstones, k);
        k++;
        phase = 0;
    } else {
        phase = 2;
    }
    return true;
}

/* TMC_REPRO_TRACKER_HOOKS=1: do the three rando hooks that were never seen
 * firing (issue #10) fire? Each scene is played for real: warp next to the
 * giver, force its talk interaction, press A through the text, and watch
 * which location key the reward hooks ask Rando_OverrideLocationKey about.
 * No seed is needed for that. Report only: it never fails the run. */
typedef struct {
    const char* name;
    unsigned char area, room;
    unsigned short x, y; /* where to stand: just below the giver */
    unsigned char npc;
    unsigned special; /* RANDO_SPECIAL_KEY_* the hook should build */
    int oyakata_demo; /* Melari: OYAKATA_DEMO set (as a rando file starts) or clear */
} HookScene;

static const HookScene kHookScenes[] = {
    { "DHC B2 King", 0x88, 0x39, 0x120, 0x78, KING_DALTUS, RANDO_SPECIAL_KEY_DHC_KING, -1 },
    { "Melari, OYAKATA_DEMO set (rando file)", 0x10, 0x00, 0x190, 0x160, MELARI, RANDO_SPECIAL_KEY_MELARI, 1 },
    { "Melari, OYAKATA_DEMO clear (control)", 0x10, 0x00, 0x190, 0x160, MELARI, RANDO_SPECIAL_KEY_MELARI, 0 },
};

static Entity* FindNpc(unsigned char id) {
    for (int l = 0; l < 9; ++l) {
        LinkedList* list = &gEntityLists[l];
        for (Entity* e = list->first; e != NULL && e != (Entity*)list; e = e->next) {
            if (e->kind == NPC && e->id == id)
                return e;
        }
    }
    return NULL;
}

/* Did the scene hand out its reward, whichever item the hook left it as? */
static bool HookSceneGave(const HookScene* s) {
    if (s->special == RANDO_SPECIAL_KEY_DHC_KING)
        return CheckLocalFlagByBank(FLAG_BANK_10, LV6_1d_KEYGET) != 0;
    return GetInventoryValue(ITEM_QST_BROKEN_SWORD) == 2;
}

/* The Simulation chest cannot be played here (area 0x44 is not warpable,
 * see kBrokenWarpAreas), so check the two hooks against the data instead:
 * the first-visit chest (roomInit.c loads entry 0xE of gUnk_080F0E1C) and
 * the key OpenSmallChest would build for it. */
static void SimulationChestProbe(void) {
    extern const TileEntity gUnk_080F0E1C[];
    const TileEntity* chest = &gUnk_080F0E1C[0xE];
    int ci = Rando_RoomChestIndex(0x44, 0x00, chest->localFlag);
    fprintf(stderr,
            "[tracker] hook Simulation chest: first-visit chest type=%u flag=%u item=0x%02x, room tile entities=%s, "
            "chest index=%d -> OpenSmallChest %s; the script hook only sees GivePlayerItem, and no Simulation "
            "script gives one\n",
            chest->type, chest->localFlag, chest->_2, GetRoomProperty(0x44, 0x00, 3) != NULL ? "yes" : "none", ci,
            ci >= 0 ? "builds a key" : "builds no key");
}

static bool HookPassTick(unsigned int frame) {
    static size_t next = 0;
    static int phase = 0;
    static unsigned int at = 0, gave_at = 0;
    static uint32_t count_before = 0;
    static bool fired = false;
    const size_t count = sizeof(kHookScenes) / sizeof(kHookScenes[0]);

    gSave.stats.health = gSave.stats.maxHealth;
    if (next >= count) {
        SimulationChestProbe();
        return false;
    }
    const HookScene* s = &kHookScenes[next];
    const uint32_t want = Rando_BuildScriptedKey(RANDO_SCRIPTED_KEY_SPECIAL, s->special, 0, 0);

    if (phase == 0) {
        if (s->special == RANDO_SPECIAL_KEY_DHC_KING) {
            /* Past the statue-breaking cutscene, key not yet given. */
            SetLocalFlagByBank(FLAG_BANK_10, LV6_39_KING);
            ClearLocalFlagByBank(FLAG_BANK_10, LV6_1d_KEYGET);
        } else {
            SetInventoryValue(ITEM_QST_BROKEN_SWORD, 1);
            SetInventoryValue(ITEM_GREEN_SWORD, 0);
            SetInventoryValue(ITEM_FIRE_ELEMENT, 0);
            ClearGlobalFlag(WHITE_SWORD_END);
            if (s->oyakata_demo)
                SetLocalFlagByBank(FLAG_BANK_2, OYAKATA_DEMO);
            else
                ClearLocalFlagByBank(FLAG_BANK_2, OYAKATA_DEMO);
        }
        Port_DebugAction_Warp(s->area, s->room, s->x, s->y, 1);
        Rando_LastQueriedLocationKey(&count_before);
        fired = false;
        gave_at = 0;
        at = frame;
        phase = 1;
        return true;
    }

    uint32_t calls = 0;
    if (Rando_LastQueriedLocationKey(&calls) == want && calls != count_before)
        fired = true;
    if (frame < at + 60)
        return true; /* room load */
    Entity* npc = FindNpc(s->npc);
    const bool gave = HookSceneGave(s);
    if (gave && gave_at == 0)
        gave_at = frame;
    if (!gave && npc != NULL && npc->interactType == INTERACTION_NONE && (frame & 0x3F) == 0)
        npc->interactType = 1; /* as if Link had talked to it */
    if ((frame & 0xF) == 0)
        Port_Config_TestForceEdge(PORT_INPUT_A);
    /* Done once the reward is out and its text is through, or after 40 s
     * (a scene that never gives its reward runs out the clock). */
    const bool here = gRoomControls.area == s->area && gRoomControls.room == s->room;
    if ((gave_at == 0 || frame < gave_at + 240) && frame < at + 2400)
        return true;
    fprintf(stderr, "[tracker] hook %s: npc=%s reward given=%s, hook %s (key 0x%08x)%s\n", s->name,
            npc != NULL ? "found" : "missing", gave ? "yes" : "no", fired ? "fired" : "did not fire", (unsigned)want,
            here ? "" : " (warp did not arrive)");
    next++;
    phase = 0;
    return true;
}

void Port_ReproTracker_Tick(unsigned int frame) {
    static int active = -1;
    static int booted = 0;
    static unsigned int in_game_since = 0;
    static unsigned int in_file_select_since = 0;

    if (active < 0) {
        const char* env = getenv("TMC_REPRO_TRACKER");
        active = (env && *env && strcmp(env, "0") != 0) ? 1 : 0;
        if (active)
            fprintf(stderr, "[tracker] repro active\n");
    }
    if (!active)
        return;

    if (!booted && gMain.task == TASK_TITLE && frame >= 30 && (frame & 0xF) < 3)
        Port_Config_TestForceEdge(PORT_INPUT_START);
    if (!booted && gMain.task == TASK_FILE_SELECT) {
        /* The slots are read over the first frames of file select. */
        if (in_file_select_since == 0)
            in_file_select_since = frame;
        if (frame < in_file_select_since + 90)
            return;
        int slot = -1;
        for (int i = 0; i < NUM_SAVE_SLOTS && slot < 0; ++i) {
            if (gFileSelectState.saveStatus[i] == 1) /* SAVE_VALID, private to fileselect.c */
                slot = i;
        }
        if (slot < 0) {
            SaveFile* sv = &gFileSelectState.saves[0];
            ResetSaveFile(0);
            sv->initialized = 1;
            sv->name[0] = 'A';
            gFileSelectState.saveStatus[0] = 1;
            slot = 0;
            fprintf(stderr, "[tracker] no save in use: synthetic save in slot 0\n");
        } else {
            fprintf(stderr, "[tracker] playing save slot %d\n", slot);
        }
        SetActiveSave((u32)slot);
        SetTask(TASK_GAME);
        booted = 1;
    }
    if (!booted)
        return;
    if (gMain.task != TASK_GAME) {
        if (in_game_since != 0) {
            fprintf(stderr, "[tracker] FAIL: left gameplay (task %u)\n", (unsigned)gMain.task);
            exit(1);
        }
        return;
    }
    if (in_game_since == 0)
        in_game_since = frame;
    if (frame < in_game_since + 60)
        return;

    static int fails = -1;
    if (fails < 0) {
        fails = RunChecks();
    }
    static int ui = -1;
    if (ui < 0) {
        const char* e = getenv("TMC_REPRO_TRACKER_UI");
        ui = (e && *e && strcmp(e, "0") != 0) ? 1 : 0;
    }
    if (ui && UiPassTick(frame))
        return;
    const char* shots = getenv("TMC_REPRO_TRACKER_SHOTS");
    if (shots && *shots && strcmp(shots, "0") != 0 && ShotPassTick(frame))
        return;
    const char* hooks = getenv("TMC_REPRO_TRACKER_HOOKS");
    if (hooks && *hooks && strcmp(hooks, "0") != 0 && HookPassTick(frame))
        return;
    const char* warp = getenv("TMC_REPRO_TRACKER_WARP");
    if (warp && *warp && strcmp(warp, "0") != 0 && WarpPassTick(frame))
        return;
    fprintf(stderr, "[tracker] %s\n", fails == 0 ? "PASS" : "FAIL");
    fflush(stderr);
    exit(fails == 0 ? 0 : 1);
}
