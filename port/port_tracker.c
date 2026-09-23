/*
 * port_tracker.c — check tracker state, read from gSave (see port_tracker.h).
 */

#include "port_tracker.h"

#include "area.h"
#include "common.h"
#include "flags.h"
#include "game.h"
#include "item.h"
#include "kinstone.h"
#include "main.h"
#include "player.h"
#include "rando/rando.h"
#include "rando/rando_keymap.h"
#include "rando/rando_runtime.h"
#include "entity.h"
#include "port_debug_query.h"
#include "port_region_data.h"
#include "port_rom.h"
#include "room.h"
#include "save.h"

#include "port_tracker_fusions.inc"

bool Port_Tracker_Available(void) {
    return gMain.task == TASK_GAME;
}

size_t Port_Tracker_CheckCount(void) {
    return Rando_GetLocationCount();
}

const char* Port_Tracker_CheckName(size_t index) {
    const RandoLocationDef* def = Rando_GetLocationDef((RandoLocationId)index);
    return def != NULL ? def->name : "";
}

/* Chest and ground-item keys are area << 16 | room << 8 | low, where low is
 * the chest's index among the room's chests (see Rando_RoomChestIndex) or,
 * for a ground item or heart container, its room-local flag. Chest indices
 * are small and ground-item flags are not, so a low byte that names no chest
 * is a flag. The chest's flag comes from this region's room data; a flag in
 * the key is a USA ordinal and goes through the baseline remap. */
static bool PlainKeyCollected(uint32_t key) {
    unsigned area = (key >> 16) & 0xFF;
    unsigned room = (key >> 8) & 0xFF;
    unsigned low = key & 0xFF;
    unsigned bank = GetFlagBankOffset(area);
    unsigned chest_flag = Rando_GetChestLocalFlag(area, room, low);
    if (chest_flag != 0xFF)
        return CheckLocalFlagByBank(bank, chest_flag) != 0;
    return CheckLocalFlagByBankB(bank, low) != 0;
}

/* Vanilla play only: the check's own item is unique, so holding it means the
 * check was taken. A randomizer seed puts something else there. */
static bool IsUniqueItem(uint16_t item) {
    switch (item) {
        case ITEM_EARTH_ELEMENT:
        case ITEM_FIRE_ELEMENT:
        case ITEM_WATER_ELEMENT:
        case ITEM_WIND_ELEMENT:
        case ITEM_OCARINA:
        case ITEM_FLIPPERS:
        case ITEM_PEGASUS_BOOTS:
        case ITEM_GRIP_RING:
        case ITEM_LIGHT_ARROW:
        case ITEM_JABBERNUT:
        case ITEM_QST_BOOK1:
        case ITEM_QST_BOOK2:
        case ITEM_QST_BOOK3:
        case ITEM_QST_CARLOV_MEDAL:
        case ITEM_QST_TINGLE_TROPHY:
        case ITEM_QST_MUSHROOM:
        case ITEM_QST_GRAVEYARD_KEY:
        case ITEM_SKILL_SPIN_ATTACK:
        case ITEM_SKILL_ROLL_ATTACK:
        case ITEM_SKILL_DASH_ATTACK:
        case ITEM_SKILL_ROCK_BREAKER:
        case ITEM_SKILL_SWORD_BEAM:
        case ITEM_SKILL_GREAT_SPIN:
        case ITEM_SKILL_DOWN_THRUST:
        case ITEM_SKILL_PERIL_BEAM:
        case ITEM_SKILL_FAST_SPIN:
        case ITEM_SKILL_FAST_SPLIT:
        case ITEM_SKILL_LONG_SPIN:
            return true;
        default:
            return false;
    }
}

/* Goron merchant restock level, 0..4: the highest of LV2..LV5 set, as
 * goronMerchant.c's GoronMerchant_GetRestockLevel reads it (a randomizer
 * file can start with LV5 alone). */
static unsigned GoronLevel(void) {
    static const u16 kLevels[] = { GORON_KAKERA_LV2, GORON_KAKERA_LV3, GORON_KAKERA_LV4, GORON_KAKERA_LV5 };
    unsigned level = 0;
    for (size_t i = 0; i < sizeof(kLevels) / sizeof(kLevels[0]); ++i) {
        if (CheckGlobalFlag(kLevels[i]))
            level = (unsigned)i + 1;
    }
    return level;
}

/* Scripted checks with a save record of the reward having been given: 1 given,
 * 0 not yet, -1 no such record. Each is the flag the game sets on giving the
 * reward (or reads to stop giving it twice); Hyrule Town and Veil Falls are
 * bank 1, whose numbering differs by region, hence the B variants there. */
static int ScriptedCheckDone(uint32_t key) {
    const unsigned group = (key >> 24) & 0x7F, a = (key >> 16) & 0xFF, b = (key >> 8) & 0xFF;
    switch (group) {
        case RANDO_SCRIPTED_KEY_GORON_MERCHANT: {
            /* Set a (0..4), slot b: sold once the merchant has restocked past
             * the set, or this slot's sold flag is up (cleared on restock). */
            const unsigned level = GoronLevel();
            return level > a || (level == a && CheckGlobalFlag(GORON_KAKERA_L + b));
        }
        case RANDO_SCRIPTED_KEY_CUCCO: {
            /* Rounds 1..9 advance the 4-bit level; round 10 caps it and sets
             * ANJU_HEART instead. */
            if (a == 9)
                return CheckGlobalFlag(ANJU_HEART) != 0;
            const unsigned level = CheckGlobalFlag(ANJU_LV_BIT0) | CheckGlobalFlag(ANJU_LV_BIT1) << 1 |
                                   CheckGlobalFlag(ANJU_LV_BIT2) << 2 | CheckGlobalFlag(ANJU_LV_BIT3) << 3;
            return level > a;
        }
        case RANDO_SCRIPTED_KEY_STOCKWELL:
            switch (a) {
                case RANDO_STOCKWELL_SLOT_80:
                    return CheckLocalFlagByBank(FLAG_BANK_2, SHOP00_SAIFU) != 0;
                case RANDO_STOCKWELL_SLOT_600:
                    return CheckLocalFlagByBank(FLAG_BANK_2, SHOP00_YAZUTSU) != 0;
                case RANDO_STOCKWELL_SLOT_EXTRA_600:
                    return CheckLocalFlagByBank(FLAG_BANK_2, SHOP00_BOMBBAG) != 0;
                default:
                    /* The 300 slot (boomerang) and the dog food set nothing;
                     * they stay on sale until you own the vanilla item. */
                    return -1;
            }
        case RANDO_SCRIPTED_KEY_SCRUB:
            return a == RANDO_SCRUB_KEY_BOTTLE ? CheckGlobalFlag(AKINDO_BOTTLE_SELL) != 0 : -1;
        case RANDO_SCRIPTED_KEY_SPECIAL:
            break;
        default:
            return -1;
    }
    switch (a) {
        case RANDO_SPECIAL_KEY_BELL_HP:
            /* graveyardKey.c type 1 sets local flag 0xD0 (USA) in Hyrule Town. */
            return CheckLocalFlagByBankB(GetFlagBankOffset(AREA_HYRULE_TOWN), 0xD0) != 0;
        case RANDO_SPECIAL_KEY_MINISH_GREAT_FAIRY:
            return CheckLocalFlagByBank(FLAG_BANK_2, IZUMI_01_FAIRY) != 0;
        case RANDO_SPECIAL_KEY_CRENEL_GREAT_FAIRY:
            return CheckLocalFlagByBank(FLAG_BANK_2, IZUMI_02_FAIRY) != 0;
        case RANDO_SPECIAL_KEY_VALLEY_GREAT_FAIRY:
            return CheckLocalFlagByBank(FLAG_BANK_2, IZUMI_00_FAIRY) != 0;
        case RANDO_SPECIAL_KEY_BOMB_MINISH_REMOTES:
            return CheckLocalFlagByBank(FLAG_BANK_2, KHOUSE26_REMOCON) != 0;
        case RANDO_SPECIAL_KEY_CRYPT_PRIZE:
            return CheckLocalFlagByBank(FLAG_BANK_3, OUBO_KAKERA) != 0;
        case RANDO_SPECIAL_KEY_DHC_KING:
            return CheckLocalFlagByBank(FLAG_BANK_10, LV6_1d_KEYGET) != 0;
        case RANDO_SPECIAL_KEY_GREGAL_SHELLS:
            return CheckLocalFlagByBank(FLAG_BANK_2, SORA_ELDER_TALK1ST) != 0;
        case RANDO_SPECIAL_KEY_BIGGORON:
            /* Handing over the shield sets both; the mirror shield clears
             * EXCHG. A second shield handed over reads as not given yet. */
            return CheckLocalFlagByBankB(FLAG_BANK_1, DAIGORON_SHIELD) &&
                   !CheckLocalFlagByBankB(FLAG_BANK_1, DAIGORON_EXCHG);
        case RANDO_SPECIAL_KEY_MELARI:
            /* melari.c is the only place the broken sword becomes 2. */
            return GetInventoryValue(ITEM_QST_BROKEN_SWORD) == 2;
        case RANDO_SPECIAL_KEY_CAFE_LADY:
            /* An open-world rando file starts with MACHI_MES_60 set. */
            if (Rando_IsActive() && Rando_GetSettings().open_world)
                return -1;
            return CheckLocalFlagByBankB(FLAG_BANK_1, MACHI_MES_60) != 0;
        case RANDO_SPECIAL_KEY_DOG_BOTTLE:
            /* EU never sets BIN_DOGFOOD; the dog food goes to 2 there. */
            if (REGION_IS_EU)
                return GetInventoryValue(ITEM_QST_DOGFOOD) == 2;
            return CheckGlobalFlag(BIN_DOGFOOD) != 0;
        default:
            return -1;
    }
}

PortTrackerState Port_Tracker_CheckState(size_t index) {
    const RandoLocationDef* def = Rando_GetLocationDef((RandoLocationId)index);
    if (def == NULL)
        return PORT_TRACKER_UNTRACKED;

    uint32_t key = def->key;
    if ((key & 0x80000000u) == 0)
        return PlainKeyCollected(key) ? PORT_TRACKER_DONE : PORT_TRACKER_OPEN;

    int done = ScriptedCheckDone(key);
    if (done >= 0)
        return done ? PORT_TRACKER_DONE : PORT_TRACKER_OPEN;

    if (!Rando_IsActive() && IsUniqueItem(def->vanilla_item))
        return GetInventoryValue(def->vanilla_item) != 0 ? PORT_TRACKER_DONE : PORT_TRACKER_OPEN;

    return PORT_TRACKER_UNTRACKED;
}

const char* Port_Tracker_FusionName(unsigned kinstone_id) {
    if (kinstone_id < 1 || kinstone_id > PORT_TRACKER_FUSION_COUNT)
        return "";
    return kTrackerFusionNames[kinstone_id];
}

int Port_Tracker_FusionArea(unsigned kinstone_id) {
    if (kinstone_id < 10 || kinstone_id > PORT_TRACKER_FUSION_COUNT)
        return -1;
    return GetWorldEvents()[GetFusionWorldEventId(kinstone_id)].area;
}

PortTrackerState Port_Tracker_FusionState(unsigned kinstone_id) {
    if (kinstone_id < 1 || kinstone_id > PORT_TRACKER_FUSION_COUNT)
        return PORT_TRACKER_OPEN;
    if (!CheckKinstoneFused(kinstone_id))
        return PORT_TRACKER_OPEN;
    /* Gold fusions and those whose world event has no end condition (a
     * tree door, a bridge) are done once fused. */
    if (kinstone_id < 10 || GetWorldEvents()[GetFusionWorldEventId(kinstone_id)].condition == CND_0)
        return PORT_TRACKER_DONE;
    return CheckFusionEventDone(kinstone_id) ? PORT_TRACKER_DONE : PORT_TRACKER_PENDING;
}

/* ------------------------------------------------------------------ */
/*   Fusers                                                            */
/* ------------------------------------------------------------------ */
/* The ROM's fuser tables map an NPC or enemy (id, type, type2) to a fuser
 * id; the fuser id indexes both its fusion list (Port_GetFuserFusionData:
 * 5-byte header -- [0] story progress it starts at, [1] how often it
 * offers -- then up to six kinstone ids, 0-terminated, 0xFF for a random
 * shared one) and gSave.kinstones.fuserOffers / fuserProgress. Where a fuser
 * stands comes from scanning every room's entity lists once. */

#define TRACKER_FUSER_COUNT 120

typedef struct {
    bool known;
    u8 kind, id;
    int area;
} TrackerFuser;

static TrackerFuser sFusers[TRACKER_FUSER_COUNT];
static bool sFusersBuilt = false;

static void BuildFusers(void) {
    static const u8 kinds[] = { NPC, ENEMY };
    for (size_t k = 0; k < sizeof(kinds); ++k) {
        u8 id, type, type2, fuser;
        for (u32 i = 1; Port_GetFuserTableEntry(kinds[k], i, &id, &type, &type2, &fuser); ++i) {
            if (fuser < TRACKER_FUSER_COUNT && !sFusers[fuser].known) {
                sFusers[fuser].known = true;
                sFusers[fuser].kind = kinds[k];
                sFusers[fuser].id = id;
                sFusers[fuser].area = -1;
            }
        }
    }
    for (u32 area = 0; area < 0x90; ++area) {
        const int rooms = Port_DebugQuery_AreaRoomCount((unsigned char)area);
        for (int room = 0; room < rooms; ++room) {
            for (u32 prop = 0; prop < 2; ++prop) {
                const EntityData* e = (const EntityData*)Port_ResolveRegionData(GetRoomProperty(area, room, prop));
                for (int n = 0; e != NULL && n < 256 && e->kind != 0xFF; ++n, ++e) {
                    u32 kind = e->kind & 0xF;
                    if (kind != NPC && kind != ENEMY)
                        continue;
                    u64 data = Port_GetEntityFuserData(kind, e->id, e->type, (u8)e->type2);
                    u32 fuser = (u32)(data & 0xFF);
                    if (data != 0 && fuser < TRACKER_FUSER_COUNT && sFusers[fuser].area < 0)
                        sFusers[fuser].area = (int)area;
                }
            }
        }
    }
    sFusersBuilt = true;
}

static const char* FuserName(const TrackerFuser* f) {
    if (f->kind == NPC)
        return f->id < sizeof(kTrackerNpcNames) / sizeof(kTrackerNpcNames[0]) ? kTrackerNpcNames[f->id] : "Someone";
    return f->id < sizeof(kTrackerEnemyNames) / sizeof(kTrackerEnemyNames[0]) ? kTrackerEnemyNames[f->id]
                                                                                : "Someone";
}

size_t Port_Tracker_FusionFusers(unsigned kinstone_id, PortTrackerFuser* out, size_t max) {
    if (kinstone_id < 1 || kinstone_id > PORT_TRACKER_FUSION_COUNT)
        return 0;
    if (!sFusersBuilt)
        BuildFusers();
    size_t n = 0;
    for (u32 f = 0; f < TRACKER_FUSER_COUNT; ++f) {
        const u8* data = (const u8*)Port_GetFuserFusionData(f);
        if (data == NULL || !sFusers[f].known)
            continue;
        bool listed = false;
        for (int i = 5; i < 11 && data[i] != KINSTONE_NONE; ++i) {
            if (data[i] == kinstone_id)
                listed = true;
        }
        if (!listed)
            continue;
        if (n < max) {
            out[n].name = FuserName(&sFusers[f]);
            out[n].area = sFusers[f].area;
            out[n].offering = gSave.kinstones.fuserOffers[f] == kinstone_id;
            out[n].locked = data[0] > gSave.global_progress;
        }
        n++;
    }
    return n;
}

bool Port_Tracker_FusionShared(unsigned kinstone_id) {
    extern const u8 SharedFusions[];
    for (int i = 0; i < 18; ++i) {
        if (SharedFusions[i] == kinstone_id)
            return true;
    }
    return false;
}
