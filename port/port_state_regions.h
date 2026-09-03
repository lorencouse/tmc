/*
 * port_state_regions.h — the list of host globals that make up a save-state.
 *
 * On the GBA a savestate was a dump of EWRAM/IWRAM/VRAM/IO: every piece of
 * game state lived in one of those four arrays. The PC port lifted most of
 * that state out into ordinary C globals (port_linked_stubs.c), so a snapshot
 * that copies only the GBA arrays plus a hand-picked few structs restores the
 * entity *bodies* while leaving the list heads, counters, textbox, fade and
 * script state at whatever they were at load time. That mismatch is exactly
 * what strands the player: a state taken while a manager had control
 * disabled comes back with no manager to re-enable it.
 *
 * So the table is defined next to the globals themselves (bottom of
 * port_linked_stubs.c), where every type is already complete and a rename
 * fails to compile rather than silently dropping a region. port_quicksave.c
 * just iterates it.
 */
#ifndef PORT_STATE_REGIONS_H
#define PORT_STATE_REGIONS_H

#include <stddef.h>

typedef struct {
    void* ptr;
    size_t size;
    const char* name;
} PortStateRegion;

extern const PortStateRegion gPortStateRegions[];
extern const size_t gPortStateRegionCount;

#endif /* PORT_STATE_REGIONS_H */
