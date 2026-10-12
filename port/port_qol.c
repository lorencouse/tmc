/*
 * port/port_qol.c -- per-frame state behind the issue #3 quality-of-life
 * toggles. See port_qol.h. Read-only against game state: it only decides
 * whether the engine should tick faster and whether a text box is shown.
 *
 * Shorter animations and the cutscene skip reuse the fast-forward path in
 * port_bios.c (Port_Qol_Turbo), so the vanilla logic runs unchanged, just
 * more ticks per second. Nothing is cut out of a sequence, and every flag
 * and script step it sets still happens.
 */
#include "port_qol.h"

#include "game.h"
#include "main.h"
#include "menu.h"
#include "player.h"
#include "subtask.h"

#include "port_runtime_config.h"

#include <string.h>

/* Engine speed for the shortened animations: 3x, about a third the time. */
#define QOL_ANIM_SPEED 3.0f
/* B has to be held this many ticks before a cutscene skip starts, so a
 * plain B press to page text never kicks it off (about a third of a second). */
#define QOL_SKIP_HOLD_TICKS 20
/* Ticks a running skip survives with control briefly handed back. */
#define QOL_SKIP_GRACE_TICKS 8

static bool sAnimTurbo;
static bool sSkipping;
static bool sSkipBlocked;
static unsigned sSkipHeld;
static unsigned sSkipGrace;
/* Item-get messages already shown this session, one bit per message index. */
static uint8_t sSeenItemText[0x10000 / 8];

/* The subtask running right now, or -1 outside one. */
static int ActiveSubtask(void) {
    if (gMain.task != TASK_GAME || gMain.substate != GAMEMAIN_SUBTASK || gUI.nextToLoad != 2)
        return -1;
    return gUI.lastState;
}

/* Kinstone fusion, portal and shrink / grow sequences: the parts that only
 * play out, never the parts that wait on the player (bag browsing, the
 * "fused" text box, standing on a portal). */
static bool InShortenableAnimation(void) {
    const int subtask = ActiveSubtask();
    if (subtask == SUBTASK_KINSTONEMENU) {
        /* 3: halves slide together, 4: mismatched halves slide apart,
         * 5 from overlay 2 on: the joined stone flies off and sparks. */
        return gMenu.menuType == 3 || gMenu.menuType == 4 || (gMenu.menuType == 5 && gMenu.overlayType >= 2);
    }
    if (subtask == SUBTASK_PORTALCUTSCENE || subtask == SUBTASK_WORLDEVENT)
        return true;
    if (subtask >= 0 || gMain.task != TASK_GAME)
        return false;
    if (gMain.substate == GAMEMAIN_MINISHPORTAL)
        return true;
    if (gMain.substate != GAMEMAIN_UPDATE)
        return false;
    switch (gPlayerEntity.base.action) {
        case PLAYER_USEPORTAL: /* 2..: Ezlo activates the portal, shrink, fall in */
            return gPlayerEntity.base.subAction >= 2;
        case PLAYER_MINISH: /* 2..6: walk to the portal centre and grow */
            return gPlayerEntity.base.subAction >= 2 && gPlayerEntity.base.subAction <= 6;
        default:
            return false;
    }
}

/* A scripted scene with the player's control taken, or a subtask that is
 * itself a cutscene. */
static bool InCutscene(void) {
    const int subtask = ActiveSubtask();
    if (subtask >= 0)
        return subtask == SUBTASK_AUXCUTSCENE || subtask == SUBTASK_PORTALCUTSCENE || subtask == SUBTASK_WORLDEVENT;
    return gMain.task == TASK_GAME && gMain.substate == GAMEMAIN_UPDATE &&
           gPlayerState.controlMode == CONTROL_DISABLED;
}

void Port_Qol_Tick(uint16_t keyinput, bool overlay) {
    const bool bHeld = (keyinput & B_BUTTON) == 0; /* KEYINPUT is active low */
    const bool eligible = !overlay && Port_Config_GetSkipCutscenes() && InCutscene();

    sAnimTurbo = !overlay && Port_Config_GetShortAnimations() && InShortenableAnimation();

    if (!bHeld) {
        sSkipping = false;
        sSkipHeld = 0;
        sSkipGrace = 0;
        sSkipBlocked = false;
        return;
    }
    if (!eligible) {
        /* Scripts hand control back for a tick or two between steps; ride
         * those out. Past that, the scene is over: stop, and make this B be
         * let go before it can skip again, so a B held from gameplay (a
         * charging sword) into a scene never starts a skip by itself. */
        if (sSkipping && ++sSkipGrace <= QOL_SKIP_GRACE_TICKS)
            return;
        sSkipping = false;
        sSkipHeld = 0;
        sSkipBlocked = true;
        return;
    }
    sSkipGrace = 0;
    if (sSkipBlocked || sSkipping)
        return;
    if (++sSkipHeld >= QOL_SKIP_HOLD_TICKS)
        sSkipping = true;
}

bool Port_Qol_Turbo(float* speed) {
    if (sSkipping) {
        *speed = Port_Config_FastForwardSpeed();
        return true;
    }
    if (sAnimTurbo) {
        *speed = QOL_ANIM_SPEED;
        return true;
    }
    return false;
}

bool Port_Qol_SkippingCutscene(void) {
    return sSkipping;
}

bool Port_Qol_SkipItemText(uint32_t messageIndex) {
    const uint32_t i = messageIndex & 0xFFFFu;
    const uint8_t bit = (uint8_t)(1u << (i & 7u));
    const bool seen = (sSeenItemText[i >> 3] & bit) != 0;
    sSeenItemText[i >> 3] |= bit;
    return seen && Port_Config_GetSkipRepeatItemText();
}
