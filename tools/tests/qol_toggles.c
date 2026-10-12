/* Drive port/port_qol.c (issue #3 QoL toggles) against hand-set game state. */
#include "port/port_qol.c"
#include <assert.h>

Main gMain;
UI gUI;
PlayerState gPlayerState;
PlayerEntity gPlayerEntity;
u8 _gMenuSharedStorage[0x40];

static bool cfgItemText, cfgShortAnims, cfgSkip;
bool Port_Config_GetSkipRepeatItemText(void) { return cfgItemText; }
bool Port_Config_GetShortAnimations(void) { return cfgShortAnims; }
bool Port_Config_GetSkipCutscenes(void) { return cfgSkip; }
float Port_Config_FastForwardSpeed(void) { return 0.0f; }

#define KEYS_NONE 0x03FF
#define KEYS_B (KEYS_NONE & ~B_BUTTON)

static void Ticks(unsigned n, uint16_t keys) {
    while (n--)
        Port_Qol_Tick(keys, false);
}

static bool Turbo(float* speed) {
    *speed = -1.0f;
    return Port_Qol_Turbo(speed);
}

int main(void) {
    float speed;
    gMain.task = TASK_GAME;
    gMain.substate = GAMEMAIN_UPDATE;

    /* Defaults off: nothing changes, whatever the game is doing. */
    gPlayerState.controlMode = CONTROL_DISABLED;
    gPlayerEntity.base.action = PLAYER_USEPORTAL;
    gPlayerEntity.base.subAction = 5;
    Ticks(60, KEYS_B);
    assert(!Turbo(&speed) && !Port_Qol_SkippingCutscene());
    assert(!Port_Qol_SkipItemText(0x0401) && !Port_Qol_SkipItemText(0x0401));

    /* Item text: off records, on skips only what was already shown. */
    cfgItemText = true;
    assert(Port_Qol_SkipItemText(0x0401));
    assert(!Port_Qol_SkipItemText(0x0402));
    assert(Port_Qol_SkipItemText(0x0402));
    assert(!Port_Qol_SkipItemText(0xFFFF) && Port_Qol_SkipItemText(0xFFFF));

    /* Short animations: the shrink runs at 3x; standing on the portal does not. */
    cfgShortAnims = true;
    gPlayerState.controlMode = CONTROL_ENABLED;
    Ticks(1, KEYS_NONE);
    assert(Turbo(&speed) && speed == 3.0f);
    gPlayerEntity.base.subAction = 1;
    Ticks(1, KEYS_NONE);
    assert(!Turbo(&speed));
    gPlayerEntity.base.action = PLAYER_MINISH;
    gPlayerEntity.base.subAction = 4; /* growing */
    Ticks(1, KEYS_NONE);
    assert(Turbo(&speed) && speed == 3.0f);
    Port_Qol_Tick(KEYS_NONE, true); /* a settings overlay drops it */
    assert(!Turbo(&speed));
    gPlayerEntity.base.subAction = 7; /* hopping off: player-driven */
    Ticks(1, KEYS_NONE);
    assert(!Turbo(&speed));

    /* Kinstone menu: browsing the bag is normal speed, the fusion is not. */
    gMain.substate = GAMEMAIN_SUBTASK;
    gUI.nextToLoad = 2;
    gUI.lastState = SUBTASK_KINSTONEMENU;
    gMenu.menuType = 1;
    Ticks(1, KEYS_NONE);
    assert(!Turbo(&speed));
    gMenu.menuType = 3;
    Ticks(1, KEYS_NONE);
    assert(Turbo(&speed));
    gMenu.menuType = 5;
    gMenu.overlayType = 1; /* "fused" text box */
    Ticks(1, KEYS_NONE);
    assert(!Turbo(&speed));
    gMenu.overlayType = 2;
    Ticks(1, KEYS_NONE);
    assert(Turbo(&speed));
    cfgShortAnims = false;
    Ticks(1, KEYS_NONE);
    assert(!Turbo(&speed));

    /* Cutscene skip: needs the toggle, a cutscene and B held for 20 ticks. */
    gMain.substate = GAMEMAIN_UPDATE;
    gPlayerEntity.base.action = PLAYER_NORMAL;
    gPlayerState.controlMode = CONTROL_DISABLED;
    Ticks(60, KEYS_B);
    assert(!Port_Qol_SkippingCutscene()); /* toggle off */
    cfgSkip = true;
    Ticks(60, KEYS_B);
    assert(!Port_Qol_SkippingCutscene()); /* B was already held: let go first */
    Ticks(1, KEYS_NONE);
    Ticks(19, KEYS_B);
    assert(!Port_Qol_SkippingCutscene()); /* a page-turning B press is not a skip */
    Ticks(1, KEYS_B);
    assert(Port_Qol_SkippingCutscene() && Turbo(&speed) && speed == 0.0f);
    /* Control handed back for a tick between script steps: keep going. */
    gPlayerState.controlMode = CONTROL_ENABLED;
    Ticks(QOL_SKIP_GRACE_TICKS, KEYS_B);
    assert(Port_Qol_SkippingCutscene());
    gPlayerState.controlMode = CONTROL_DISABLED;
    Ticks(1, KEYS_B);
    assert(Port_Qol_SkippingCutscene());
    /* The scene ends: stop, and the held B does not restart a skip. */
    gPlayerState.controlMode = CONTROL_ENABLED;
    Ticks(QOL_SKIP_GRACE_TICKS + 1, KEYS_B);
    assert(!Port_Qol_SkippingCutscene() && !Turbo(&speed));
    gPlayerState.controlMode = CONTROL_DISABLED;
    Ticks(60, KEYS_B);
    assert(!Port_Qol_SkippingCutscene());
    /* Releasing B stops a skip at once. */
    Ticks(1, KEYS_NONE);
    Ticks(20, KEYS_B);
    assert(Port_Qol_SkippingCutscene());
    Ticks(1, KEYS_NONE);
    assert(!Port_Qol_SkippingCutscene() && !Turbo(&speed));
    /* B held into a cutscene from gameplay (a charging sword) never skips. */
    gPlayerState.controlMode = CONTROL_ENABLED;
    Ticks(5, KEYS_B);
    gPlayerState.controlMode = CONTROL_DISABLED;
    Ticks(60, KEYS_B);
    assert(!Port_Qol_SkippingCutscene());
    /* Cutscene subtasks count; menus do not. */
    Ticks(1, KEYS_NONE);
    gPlayerState.controlMode = CONTROL_ENABLED;
    gMain.substate = GAMEMAIN_SUBTASK;
    gUI.lastState = SUBTASK_AUXCUTSCENE;
    Ticks(20, KEYS_B);
    assert(Port_Qol_SkippingCutscene());
    Ticks(1, KEYS_NONE);
    gUI.lastState = SUBTASK_PAUSEMENU;
    Ticks(60, KEYS_B);
    assert(!Port_Qol_SkippingCutscene());
    return 0;
}
