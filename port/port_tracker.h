#pragma once
/*
 * port_tracker.h — check tracker: what is left to collect in the loaded save.
 *
 * Two lists, both read straight from gSave so they are right for any save,
 * including one made before the tracker existed or in an emulator:
 *
 *   checks   the randomizer's location table (chests, heart pieces, dojos,
 *            NPC gifts...), in vanilla play and rando alike. Chest and
 *            ground-item checks resolve to a room-local flag; a scripted
 *            check (shops, prizes, NPCs) resolves only where its save state
 *            is known, otherwise it is PORT_TRACKER_UNTRACKED.
 *   fusions  the 100 kinstone fusions: not fused, fused with its reward still
 *            out there (chest unopened, golden enemy alive, butterfly loose),
 *            or done.
 *
 * Nothing here writes to the save or keeps state of its own.
 */

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PORT_TRACKER_OPEN = 0,  /* not collected / not fused */
    PORT_TRACKER_PENDING,   /* fusions only: fused, reward not taken yet */
    PORT_TRACKER_DONE,
    PORT_TRACKER_UNTRACKED, /* checks only: no known save state to read */
} PortTrackerState;

#define PORT_TRACKER_FUSION_COUNT 100

/* A save file is loaded and in play; the lists mean nothing otherwise. */
bool Port_Tracker_Available(void);

size_t Port_Tracker_CheckCount(void);
/* "Area - Check", as the randomizer names it. */
const char* Port_Tracker_CheckName(size_t index);
PortTrackerState Port_Tracker_CheckState(size_t index);

/* Kinstone ids run 1..PORT_TRACKER_FUSION_COUNT. */
const char* Port_Tracker_FusionName(unsigned kinstone_id);
/* Area of the fusion's world event, or -1 for the gold fusions, which have
 * none of their own. */
int Port_Tracker_FusionArea(unsigned kinstone_id);
PortTrackerState Port_Tracker_FusionState(unsigned kinstone_id);

/* Who can fuse a kinstone id: every fuser whose fusion list names it. The
 * area is where a room's entity list places that fuser, or -1 when only a
 * script or room function spawns it. */
typedef struct {
    const char* name;
    int area;
    bool offering; /* the fuser's current offer is this fusion */
    bool locked;   /* the fuser does not fuse yet at this point in the story */
} PortTrackerFuser;

/* Writes up to `max` fusers; returns how many list the fusion. */
size_t Port_Tracker_FusionFusers(unsigned kinstone_id, PortTrackerFuser* out, size_t max);
/* One of the shared fusions any fuser may offer at random. */
bool Port_Tracker_FusionShared(unsigned kinstone_id);

#ifdef __cplusplus
}
#endif
