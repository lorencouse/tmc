/*
 * port/port_qol.h -- quality-of-life toggles that need per-frame state:
 * skip repeat item-get text, shorter animations, hold-B cutscene skip.
 * (Fast Pegasus charge is a plain config read at its call site.)
 *
 * All default off (see port_runtime_config.h); with every toggle off this
 * module changes nothing: no text is skipped and no turbo is requested.
 */
#ifndef PORT_QOL_H
#define PORT_QOL_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Once per engine tick from Port_UpdateInput, after KEYINPUT is stored.
 * keyinput is the GBA register value (active low). overlay is true when a
 * settings overlay owns input; it drops any turbo. */
void Port_Qol_Tick(uint16_t keyinput, bool overlay);

/* Pacing: true when a QoL toggle wants the engine to run fast this tick,
 * as fast-forward does. *speed gets the tick-rate multiple, 0 = uncapped. */
bool Port_Qol_Turbo(float* speed);

/* True while a hold-B cutscene skip is running. Text pages advance on
 * their own and draw at once; choice prompts still wait for the player. */
bool Port_Qol_SkippingCutscene(void);

/* Item-get text: true when skip_repeat_item_text is on and this message was
 * already shown on this save this session, so the caller should not show it
 * again. With the toggle on, records the message as seen; off, does nothing. */
bool Port_Qol_SkipItemText(uint32_t messageIndex);

/* Forget which item-get messages were shown. Called when a file is loaded
 * or a new game starts from the file screen, and on every save-state
 * restore. */
void Port_Qol_ResetItemText(void);

#ifdef __cplusplus
}
#endif

#endif /* PORT_QOL_H */
