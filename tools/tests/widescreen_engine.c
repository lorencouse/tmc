/* Small fixture for the unchanged production helper bodies extracted by the runner. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
typedef int16_t s16;
typedef int32_t s32;
typedef uint16_t u16;
typedef uint32_t u32;
#define MODE1_GBA_BG_CLIP_X 240
#define MODE1_WS_SHADOW_COLS 32
#define MODE1_WS_SHADOW_ROWS 32
#define MODE1_TALL_SHADOW_ROWS 36
#define MODE1_TALL_SHADOW_COLS 54
static struct { int scroll_x, scroll_y, origin_x, origin_y, width, height, scrollAction, area, room; } gRoomControls;
static struct { unsigned active, xPos, yPos, width, height; } gCurrentWindow;
static int virtuappu_mode1_ws_shadow_base_tile[4];
static u16* virtuappu_mode1_ws_shadow[4];
static u16* virtuappu_mode1_tall_shadow[4];
static int virtuappu_mode1_tall_hud_split;
static int virtuappu_mode1_bg_stretch[4];
static u16 sTallShadowBottom[MODE1_TALL_SHADOW_ROWS * MODE1_TALL_SHADOW_COLS];
static u16 sTallShadowTop[MODE1_TALL_SHADOW_ROWS * MODE1_TALL_SHADOW_COLS];
static int tallH = 160;
static int Port_Widescreen_TargetViewHeight(void) { return tallH; }
static u32 sWsContentKey;
static int sWsContentPx;
static int enabled = 1;
static int Port_Config_WidescreenEnabled(void) { return enabled; }
static int Port_Widescreen_TargetViewWidth(void) { return 384; }
static int Port_WidescreenEffectiveTarget(void) { return gRoomControls.width < 384 ? gRoomControls.width : 384; }
static u32 Port_WidescreenRoomKey(void) { return 1; }
int Port_Widescreen_FallbackNative(void);
static int Port_Widescreen_EffectiveViewWidth(void) { return Port_Widescreen_FallbackNative() ? 240 : Port_WidescreenEffectiveTarget(); }
/* Only unrelated task/HUD dependencies are stubbed; shadow publication runs
 * the production UpdateShadows body, including its area and BG gates. */
enum { TASK_GAME = 2, AREA_MINISH_WOODS = 0x0b, AREA_HYRULE_FIELD = 0x0a,
       AREA_CAVE_OF_FLAMES = 0x60, MANAGER = 9, ENTER_ROOM_TEXTBOX_MANAGER = 1, LIGHT_RAY_MANAGER = 1 };
#define MODE1_GBA_BG_COUNT 4
#define DISPCNT_BG0_ON 0x100
#define DISPCNT_BG3_ON 0x800
static struct { int task; } gMain;
typedef struct { u32 control; int yOffset; } TestBg;
static struct { TestBg bg0, bg1, bg2, bg3; struct { int displayControl; } lcd; } gScreen;
static struct { TestBg* bgSettings; } gMapBottom, gMapTop;
static u16 gMapDataBottomSpecial[128*128], gMapDataTopSpecial[128*128];
static u16 sWsShadowBG1[32*32], sWsShadowBG2[32*32], sWsShadowOverlay[32*32];
static u16 vram[0x18000/2];
#define gVram ((unsigned char*)vram)
static int virtuappu_mode1_ws_hud_right_anchor, virtuappu_mode1_ws_msg_shift;
static int virtuappu_mode1_ws_msg_x0, virtuappu_mode1_ws_msg_x1;
static int virtuappu_mode1_ws_msg_y0, virtuappu_mode1_ws_msg_y1;
static int Port_WidescreenScanContentPx(void) { return 1024; }
static int Port_Widescreen_IsActive(void) { return gMain.task == TASK_GAME && !Port_Widescreen_FallbackNative(); }
static int Port_Widescreen_EffectiveViewHeight(void) { return Port_Widescreen_IsActive() ? tallH : 160; }
static int Port_Widescreen_HudRightAnchor(void) { return 0; }
typedef struct { int action; } Entity;
static Entity* FindEntityByID(int kind, int id, int list) { (void)kind; (void)id; (void)list; return NULL; }
typedef struct { Entity base; int unk_21; } LightRayManager;
static LightRayManager sLightRay;
static int sLightRayPresent;
static Entity* DeepFindEntityByID(int kind, int id) {
    return (kind == MANAGER && id == LIGHT_RAY_MANAGER && sLightRayPresent) ? &sLightRay.base : NULL;
}
#include "widescreen_helpers.h"
static int errors;
#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #cond); ++errors; } } while (0)
int main(void) {
    gRoomControls.width = 1024;
    gRoomControls.height = 1024;
    static u16 map[128*128], shadow[MODE1_WS_SHADOW_COLS*32];
    for (int i=0; i<128*128; ++i) map[i] = (u16)(i+1);
    gRoomControls.scroll_y = 16;
    for (int phase=0; phase<16; ++phase) {
        gRoomControls.scroll_x = 64 + phase;
        Port_WidescreenShadow_Populate(1, map, shadow);
        for (int shake=-3; shake<=3; ++shake) {
            for (int x=240; x<384; ++x) {
                int col = ((x+phase+shake)&255)/8;
                int index = (col-virtuappu_mode1_ws_shadow_base_tile[1]+32)%32;
                int expectedCol = (64+phase+shake+x)/8;
                if (index >= MODE1_WS_SHADOW_COLS || shadow[index] != map[128+expectedCol]) {
                    fprintf(stderr,"FAIL shadow phase=%d shake=%d x=%d index=%d\n",phase,shake,x,index);
                    ++errors; break;
                }
            }
        }
    }
    for (int action=0; action<=5; ++action) {
        gRoomControls.scrollAction = action;
        CHECK(Port_Widescreen_EffectiveViewWidth() == ((action==2 || action==4 || action==5) ? 240 : 384));
        CHECK(Port_Widescreen_CameraRestX(1000) == ((action==2 || action==4 || action==5) ? 784 : 640));
    }
    /* At the top edge the native fill duplicates map row zero into its
     * leading padding row. Upward shake exposes that row at the seam. */
    gRoomControls.scroll_x = 0;
    for (int y = 0; y < 8; ++y) {
        gRoomControls.scroll_y = y;
        Port_WidescreenShadow_Populate(1, map, shadow);
        for (int shake = -3; shake < 0; ++shake) {
            int row = (8 + y + shake) / 8;
            CHECK(shadow[row * 32 + 1] == map[30]);
        }
    }
    gRoomControls.scroll_y = 16;
    /* Room-edge padding stays transparent even when the backing map is stale. */
    gRoomControls.scroll_x = 0;
    gRoomControls.width = 256;
    Port_WidescreenShadow_Populate(1, map, shadow);
    CHECK(shadow[2] == map[128+31]);
    CHECK(shadow[3] == 0);
    gRoomControls.scrollAction = 1;
    CHECK(Port_Widescreen_EffectiveViewWidth() == 256);
    enabled = 0;
    CHECK(Port_Widescreen_EffectiveViewWidth() == 240);
    enabled = 1;
    gRoomControls.width = 240;
    CHECK(Port_Widescreen_EffectiveViewWidth() == 240);
    /* Overlay is a complete wrapping screenblock, independent of camera. */
    static u16 screen[32 * 32];
    for (int i = 0; i < 32 * 32; ++i) screen[i] = (u16)(i + 7);
    Port_WidescreenShadow_PopulateOverlay(screen, shadow);
    CHECK(virtuappu_mode1_ws_shadow[3] == shadow);
    CHECK(virtuappu_mode1_ws_shadow_base_tile[3] == 0);
    for (int row = 0; row < 32; ++row)
        for (int col = 0; col < MODE1_WS_SHADOW_COLS; ++col)
            CHECK(shadow[row * MODE1_WS_SHADOW_COLS + col] == screen[row * 32 + (col & 31)]);
    /* All three repeating overlays must publish the complete screenblock;
     * disabled overlays, unrelated canvases and native mode must not. */
    memcpy(gVram + 0xf000, screen, sizeof(screen));
    gMain.task = TASK_GAME;
    gRoomControls.width = 1024;
    const struct { int area, control, enabled, wide, expected; } overlays[] = {
        { AREA_MINISH_WOODS, 0x1e04, 1, 1, 1 },
        { AREA_HYRULE_FIELD, 0x1e05, 1, 1, 1 },
        { AREA_CAVE_OF_FLAMES, 0x1e04, 1, 1, 1 },
        { AREA_HYRULE_FIELD, 0x1e05, 0, 1, 0 },
        { AREA_HYRULE_FIELD, 0x1e05, 1, 0, 0 },
        { AREA_HYRULE_FIELD, 0x1d05, 1, 1, 0 },
        { 0, 0x1e04, 1, 1, 0 },
    };
    for (unsigned i = 0; i < sizeof(overlays)/sizeof(overlays[0]); ++i) {
        gRoomControls.area = overlays[i].area;
        gScreen.bg3.control = overlays[i].control;
        gScreen.lcd.displayControl = overlays[i].enabled ? DISPCNT_BG3_ON : 0;
        enabled = overlays[i].wide;
        Port_Widescreen_UpdateShadows();
        CHECK((virtuappu_mode1_ws_shadow[3] != NULL) == overlays[i].expected);
        if (overlays[i].expected && virtuappu_mode1_ws_shadow[3])
            CHECK(memcmp(virtuappu_mode1_ws_shadow[3], screen, sizeof(screen)) == 0);
    }
    /* LightRayManager: the drifting ray texture (states 1-2) wraps like fog;
     * the screen-pinned sunshine (state 4, fading out in 3) is stretched. */
    gRoomControls.area = AREA_MINISH_WOODS;
    gScreen.bg3.control = 0x1e04;
    gScreen.lcd.displayControl = DISPCNT_BG3_ON;
    enabled = 1;
    sLightRayPresent = 1;
    for (int state = 0; state <= 4; ++state) {
        int pinned = state == 3 || state == 4;
        sLightRay.unk_21 = state;
        Port_Widescreen_UpdateShadows();
        CHECK(virtuappu_mode1_bg_stretch[3] == pinned);
        CHECK((virtuappu_mode1_ws_shadow[3] != NULL) == !pinned);
    }
    gRoomControls.area = 0; /* the barrel house: stretched in any area */
    sLightRay.unk_21 = 4;
    Port_Widescreen_UpdateShadows();
    CHECK(virtuappu_mode1_bg_stretch[3] == 1);
    sLightRayPresent = 0;
    Port_Widescreen_UpdateShadows();
    CHECK(virtuappu_mode1_bg_stretch[3] == 0);
    enabled = 1;
    int x,y,w,h;
    gCurrentWindow.active=0;
    CHECK(!Message_GetWindowRect(&x,&y,&w,&h));
    gCurrentWindow.active=1; gCurrentWindow.xPos=5; gCurrentWindow.yPos=1;
    gCurrentWindow.width=18; gCurrentWindow.height=2;
    CHECK(Message_GetWindowRect(&x,&y,&w,&h));
    CHECK(x==40 && y==8 && w==160 && h==32);
    /* Closing animation uses the smaller currently drawn frame. */
    gCurrentWindow.xPos=13; gCurrentWindow.yPos=3;
    gCurrentWindow.width=2; gCurrentWindow.height=0;
    CHECK(Message_GetWindowRect(&x,&y,&w,&h));
    CHECK(x==104 && y==24 && w==32 && h==16);
    gCurrentWindow.active=0;
    CHECK(!Message_GetWindowRect(&x,&y,&w,&h));
    /* Tall view: all-or-nothing fallback, and the whole-view shadow maps
     * cell [r][c] to map (2*row16 - 1 + r, 2*col16 - 1 + c). */
    tallH = 240;
    gRoomControls.scrollAction = 0;
    gRoomControls.origin_x = 0; gRoomControls.origin_y = 0;
    gRoomControls.width = 1024; gRoomControls.height = 1024;
    CHECK(!Port_Widescreen_FallbackNative());
    gRoomControls.height = 176;
    CHECK(Port_Widescreen_FallbackNative());  /* room shorter than 240 */
    gRoomControls.height = 1024; gRoomControls.width = 320;
    CHECK(Port_Widescreen_FallbackNative());  /* room narrower than the 384 target */
    gRoomControls.width = 1024;
    for (int ydiff = 0; ydiff < 40; ydiff += 3) {
        for (int xdiff = 0; xdiff < 40; xdiff += 5) {
            gRoomControls.scroll_x = xdiff; gRoomControls.scroll_y = ydiff;
            Port_TallShadow_Populate(1, map, sTallShadowBottom);
            CHECK(virtuappu_mode1_tall_shadow[1] == sTallShadowBottom);
            for (int r = 0; r < MODE1_TALL_SHADOW_ROWS; ++r) {
                for (int c = 0; c < MODE1_TALL_SHADOW_COLS; ++c) {
                    int row = 2 * (ydiff >> 4) - 1 + r, col = 2 * (xdiff >> 4) - 1 + c;
                    if (ydiff < 8 && r == 0) row = 0;
                    u16 want = (row >= 0 && col >= 0 && col < 128) ? map[row * 128 + col] : 0;
                    CHECK(sTallShadowBottom[r * MODE1_TALL_SHADOW_COLS + c] == want);
                }
            }
        }
    }
    /* Tall view: a 256x256 BG that is neither a map nor a known overlay
     * (a parallax sky, a boss backdrop) is stretched; maps, 512-wide
     * canvases, disabled BGs and the 160-line view are not. */
    gRoomControls.area = 0;
    gScreen.bg1.control = 0x1c01; gScreen.bg2.control = 0x1d02; gScreen.bg3.control = 0x1e07;
    gMapBottom.bgSettings = &gScreen.bg2; gMapTop.bgSettings = &gScreen.bg1;
    gScreen.lcd.displayControl = 0x0e00;
    Port_Widescreen_UpdateShadows();
    CHECK(virtuappu_mode1_bg_stretch[3] == 1);
    CHECK(virtuappu_mode1_bg_stretch[1] == 0 && virtuappu_mode1_bg_stretch[2] == 0);
    gScreen.bg3.control = 0x5e07;
    Port_Widescreen_UpdateShadows();
    CHECK(virtuappu_mode1_bg_stretch[3] == 0);
    gScreen.bg3.control = 0x1e07; gScreen.lcd.displayControl = 0x0600;
    Port_Widescreen_UpdateShadows();
    CHECK(virtuappu_mode1_bg_stretch[3] == 0);
    gScreen.lcd.displayControl = 0x0e00; tallH = 160;
    Port_Widescreen_UpdateShadows();
    CHECK(virtuappu_mode1_bg_stretch[3] == 0);
    tallH = 240;
    gMapBottom.bgSettings = NULL; gMapTop.bgSettings = NULL; gScreen.lcd.displayControl = 0;
    gRoomControls.height = 200; gRoomControls.scroll_x = 16; gRoomControls.scroll_y = 32;
    Port_TallShadow_Populate(1, map, sTallShadowBottom);
    CHECK(sTallShadowBottom[(MODE1_TALL_SHADOW_ROWS - 1) * MODE1_TALL_SHADOW_COLS + 3] == 0); /* below the room */
    tallH = 160;
    if (!errors) puts("widescreen engine: shake continuity, iris width/camera, live message window, tall view passed");
    return errors != 0;
}
