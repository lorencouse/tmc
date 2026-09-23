#include <stdio.h>
#include "src/npc/dampe.c"

RoomControls gRoomControls;
static int viewWidth, viewHeight = 160;
int Port_Widescreen_EffectiveViewWidth(void) { return viewWidth; }
int Port_Widescreen_EffectiveViewHeight(void) { return viewHeight; }

int main(void) {
    const int widths[] = { 240, 284, 384 };
    int failures = 0;
    gRoomControls.scroll_x = 400;
    gRoomControls.scroll_y = 300;
    /* 320x240 is the tall (zoomed-out) view. */
    const int heights[] = { 160, 160, 160, 240 };
    for (unsigned i = 0; i < sizeof(heights) / sizeof(heights[0]); ++i) {
        viewWidth = i < 3 ? widths[i] : 320;
        viewHeight = heights[i];
#if MODE1_GBA_WIDTH > 240
        int width = viewWidth, height = viewHeight;
#else
        int width = 240, height = 160;
#endif
        const int xs[] = { -17, -16, 0, 255, 256, width + 15, width + 16 };
        const int ys[] = { -25, -24, 0, 183, 184, height + 23, height + 24 };
        for (unsigned x = 0; x < sizeof(xs) / sizeof(xs[0]); ++x) {
            for (unsigned y = 0; y < sizeof(ys) / sizeof(ys[0]); ++y) {
                Entity entity = { 0 };
                ScriptExecutionContext context = { 0 };
                entity.x.HALF.HI = gRoomControls.scroll_x + xs[x];
                entity.y.HALF.HI = gRoomControls.scroll_y + ys[y];
                int expected = xs[x] >= -16 && xs[x] < width + 16 && ys[y] >= -24 && ys[y] < height + 24;
                sub_0806BF44(&entity, &context);
                if (context.condition != expected) {
                    fprintf(stderr, "Dampe width=%d x=%d y=%d: got %d expected %d\n",
                            width, xs[x], ys[y], context.condition, expected);
                    failures++;
                }
            }
        }
    }
    return failures != 0;
}
