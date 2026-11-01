#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include "lamp_control.h"

static int close_to(float a, float b) { return fabsf(a-b) < 0.0001f; }

int main(void)
{
    assert(lamp_normalize(-10, 0, 3600) == 0);
    assert(lamp_normalize(4095, 0, 3600) == 1);
    assert(lamp_normalize(1800, 0, 3600) == 0.5f);
    assert(lamp_normalize(100, 200, 100) == 0);
    lamp_debounce_t button = {0};
    assert(!lamp_debounce(&button, true, 10));
    assert(!lamp_debounce(&button, false, 20));
    assert(!lamp_debounce(&button, true, 30));
    assert(!lamp_debounce(&button, true, 69));
    assert(lamp_debounce(&button, true, 70));
    button = (lamp_debounce_t){.candidate = true, .since_ms = UINT32_MAX-20};
    assert(lamp_debounce(&button, true, 25)); // millisecond counter rollover

    lamp_xy_t warm = lamp_mix(2000, 0, 0), cool = lamp_mix(6500, 0, 0);
    for (int h=0;h<=360;h++) for (int k=2000;k<=6500;k+=100) {
        uint8_t zh,zs; lamp_native_hs((float)k,h/360.0f,1,&zh,&zs);
        assert(zs==254 && zh==(uint8_t)lroundf(h*254.0f/360));
        uint8_t rgb[3];lamp_hsv_rgb(h/360.0f,1,rgb);
        assert(rgb[0]==0 || rgb[1]==0 || rgb[2]==0);
        uint8_t near_h,near_s;lamp_native_hs((float)k,h/360.0f,.999f,&near_h,&near_s);
        int delta=abs((int)near_h-zh);assert(delta<=1 || delta>=253);assert(near_s>=253);
        for (int s=0;s<100;s++) {
            lamp_native_hs((float)k,h/360.0f,s/100.0f,&zh,&zs);assert(zh<=254 && zs<=254);
        }
    }
    uint8_t wh,ws,ch,cs;
    lamp_native_hs(2000,.66f,.5f,&wh,&ws);lamp_native_hs(6500,.66f,.5f,&ch,&cs);
    assert(wh!=ch || ws!=cs);
    assert(fabsf(warm.x-0.5269f) < 0.001f);
    assert(fabsf(cool.x-0.3135f) < 0.001f);
    lamp_xy_t red = lamp_mix(2000, 0, 1), blue = lamp_mix(6500, 2.0f/3, 1);
    assert(fabsf(red.x-0.64f) < 0.001f && fabsf(red.y-0.33f) < 0.001f);
    assert(fabsf(blue.x-0.15f) < 0.001f && fabsf(blue.y-0.06f) < 0.001f);
    lamp_xy_t wrapped = lamp_mix(2000, 1, 1);
    assert(close_to(red.x, wrapped.x) && close_to(red.y, wrapped.y));
    for (int k = 2000; k <= 6500; k += 100) {
        for (int h = 0; h <= 100; h++) {
            lamp_xy_t w = lamp_mix((float)k, h/100.0f, 0);
            lamp_xy_t c = lamp_mix((float)k, h/100.0f, 1);
            lamp_xy_t c2 = lamp_mix(6500, h/100.0f, 1);
            assert(close_to(c.x, c2.x) && close_to(c.y, c2.y));
            float previous_distance = -1;
            for (int s = 0; s <= 100; s++) {
                lamp_xy_t xy = lamp_mix((float)k, h/100.0f, s/100.0f);
                assert(isfinite(xy.x) && isfinite(xy.y));
                assert(xy.x > 0 && xy.y > 0 && xy.x+xy.y < 1);
                assert(xy.x >= fminf(w.x,c.x)-0.0001f && xy.x <= fmaxf(w.x,c.x)+0.0001f);
                float distance = (xy.x-w.x)*(xy.x-w.x) + (xy.y-w.y)*(xy.y-w.y);
                assert(distance+0.000001f >= previous_distance);
                previous_distance = distance;
            }
        }
    }
    float pots[4] = {0, 0, 0, 0};
    lamp_command_t command = lamp_command(false, pots, true);
    assert(!command.on && command.white && command.level == 1 && command.mired == 500);
    pots[0] = 1; pots[1] = 1; pots[3] = 1;
    command = lamp_command(true, pots, true);
    assert(command.on && !command.white && command.level == 254 && command.mired == 154);
    pots[3] = 0.02f;
    assert(lamp_command(true, pots, true).white);
    assert(!lamp_command(true, pots, false).white);
    pots[3] = 0;
    command = lamp_command(true, pots, false);
    assert(command.white);
    lamp_command_t changed = command;
    changed.x += 1000; // Hue changes must not transmit a colour while white mode is active.
    assert(!lamp_color_changed(command, changed));
    changed.mired += 3;
    assert(lamp_color_changed(command, changed));
    changed = command; changed.white = false;
    assert(lamp_color_changed(command, changed));
    puts("PASS: colour mixing (469246 points), endpoints, modes, debounce and ADC mapping");
    return 0;
}
