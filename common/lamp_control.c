#include "lamp_control.h"
#define LAMP_WHITE_MIN_K 2000.0f
#define LAMP_WHITE_MAX_K 6500.0f
#include <math.h>
#include <stdlib.h>

static float clamp01(float n) { return fminf(1.0f, fmaxf(0.0f, n)); }

float lamp_normalize(int raw, int low, int high)
{
    if (high <= low) return 0.0f;
    float v = clamp01((float)(raw - low) / (high - low));
    // End stops remain reachable despite ADC and mechanical tolerances.
    return v < 0.01f ? 0.0f : v > 0.99f ? 1.0f : v;
}

bool lamp_debounce(lamp_debounce_t *s, bool value, uint32_t now)
{
    if (value != s->candidate) { s->candidate = value; s->since_ms = now; }
    if ((uint32_t)(now - s->since_ms) >= 40) s->stable = s->candidate;
    return s->stable;
}

static lamp_xy_t white_xy(float k)
{
    // Planckian-locus approximation (1667..25000 K); use 2000..6500 K here.
    k = fminf(6500.0f, fmaxf(2000.0f, k));
    float x = k <= 4000.0f
        ? -0.2661239e9f/(k*k*k) - 0.2343589e6f/(k*k) + 0.8776956e3f/k + 0.179910f
        : -3.0258469e9f/(k*k*k) + 2.1070379e6f/(k*k) + 0.2226347e3f/k + 0.240390f;
    float y = k <= 2222.0f ? -1.1063814f*x*x*x - 1.34811020f*x*x + 2.18555832f*x - 0.20219683f
        : k <= 4000.0f ? -0.9549476f*x*x*x - 1.37418593f*x*x + 2.09137015f*x - 0.16748867f
        : 3.0817580f*x*x*x - 5.87338670f*x*x + 3.75112997f*x - 0.37001483f;
    return (lamp_xy_t){x, y};
}

static lamp_xy_t hue_xy(float hue)
{
    hue = clamp01(hue) * 6.0f;
    int sector = (int)hue;
    float f = hue - sector, r = 0, g = 0, b = 0;
    switch (sector % 6) {
    case 0: r = 1; g = f; break;
    case 1: r = 1-f; g = 1; break;
    case 2: g = 1; b = f; break;
    case 3: g = 1-f; b = 1; break;
    case 4: r = f; b = 1; break;
    case 5: r = 1; b = 1-f; break;
    }
    // Linear sRGB primaries define a reproducible hue wheel, independent of model.
    // ponytail: no model-specific gamut; calibrate primaries when the bulb is known.
    float X = 0.4124564f*r + 0.3575761f*g + 0.1804375f*b;
    float Y = 0.2126729f*r + 0.7151522f*g + 0.0721750f*b;
    float Z = 0.0193339f*r + 0.1191920f*g + 0.9503041f*b;
    return (lamp_xy_t){X/(X+Y+Z), Y/(X+Y+Z)};
}

lamp_xy_t lamp_mix(float kelvin, float hue, float saturation)
{
    lamp_xy_t w = white_xy(kelvin), c = hue_xy(hue);
    float s = clamp01(saturation);
    // Mix XYZ with equal Y=1, then convert back to xy. Brightness is independent.
    float X = (1-s)*w.x/w.y + s*c.x/c.y;
    float Z = (1-s)*(1-w.x-w.y)/w.y + s*(1-c.x-c.y)/c.y;
    return (lamp_xy_t){X/(X+1+Z), 1/(X+1+Z)};
}

lamp_command_t lamp_command(bool on, const float p[4], bool was_white)
{
    float k = LAMP_WHITE_MIN_K + clamp01(p[1])*(LAMP_WHITE_MAX_K-LAMP_WHITE_MIN_K);
    float saturation = clamp01(p[3]);
    // Hysteresis prevents repeated CT/xy mode changes near the white end stop.
    bool white = saturation <= (was_white ? 0.025f : 0.015f);
    lamp_xy_t xy = lamp_mix(k, p[2], saturation);
    return (lamp_command_t){
        .on = on, .white = white,
        .level = (uint8_t)lroundf(1 + 253*clamp01(p[0])),
        .mired = (uint16_t)lroundf(1000000.0f/k),
        .x = (uint16_t)lroundf(xy.x*65536.0f),
        .y = (uint16_t)lroundf(xy.y*65536.0f),
    };
}

bool lamp_color_changed(lamp_command_t a, lamp_command_t b)
{
    if (a.white != b.white) return true;
    if (a.white) return abs((int)a.mired - b.mired) >= 2;
    return abs((int)a.x - b.x) >= 100 || abs((int)a.y - b.y) >= 100;
}

void lamp_xy_rgb(float x, float y, float brightness, uint8_t rgb[3])
{
    if (y < 0.0001f) { rgb[0] = rgb[1] = rgb[2] = 0; return; }
    float X = x/y, Z = (1-x-y)/y;
    float c[3] = {3.2404542f*X - 1.5371385f - 0.4985314f*Z,
        -0.9692660f*X + 1.8760108f + 0.0415560f*Z,
        0.0556434f*X - 0.2040259f + 1.0572252f*Z};
    float max = fmaxf(c[0], fmaxf(c[1], c[2]));
    for (int i = 0; i < 3; i++) {
        float v = clamp01(c[i]/fmaxf(max, 0.0001f));
        v = v <= 0.0031308f ? 12.92f*v : 1.055f*powf(v, 1/2.4f)-0.055f;
        rgb[i] = (uint8_t)lroundf(255*v*clamp01(brightness));
    }
}

void lamp_hsv_rgb(float hue, float saturation, uint8_t rgb[3])
{
    float h=clamp01(hue)*6, s=clamp01(saturation), f=h-floorf(h);
    float a=1-s, b=1-s*f, c=1-s*(1-f);
    float sectors[6][3]={{1,c,a},{b,1,a},{a,1,c},{a,b,1},{c,a,1},{1,a,b}};
    for (int i=0;i<3;i++) rgb[i]=(uint8_t)lroundf(255*sectors[(int)h%6][i]);
}

void lamp_native_hs(float kelvin, float hue, float saturation, uint8_t *h, uint8_t *s)
{
    float mix=clamp01(saturation);
    // Exact saturated endpoint: independent of temperature and the sRGB gamut.
    if (mix==1) { *h=(uint8_t)lroundf(clamp01(hue)*254); *s=254; return; }
    uint8_t white[3],color[3];
    lamp_xy_t xy=white_xy(kelvin);
    lamp_xy_rgb(xy.x,xy.y,1,white); lamp_hsv_rgb(hue,1,color);
    // A perceptual warm/cool white blend, not independent control of LED channels.
    // The bulb maps this HS colour into its native gamut. Pure white uses CT instead.
    float r=(1-mix)*white[0]+mix*color[0], g=(1-mix)*white[1]+mix*color[1], b=(1-mix)*white[2]+mix*color[2];
    float hi=fmaxf(r,fmaxf(g,b)),lo=fminf(r,fminf(g,b)),d=hi-lo,angle=0;
    if (d>0.0001f) {
        if (hi==r) angle=(g-b)/d;
        else if (hi==g) angle=2+(b-r)/d;
        else angle=4+(r-g)/d;
        if (angle<0) angle+=6;
    }
    *h=(uint8_t)lroundf(angle*254/6);
    *s=(uint8_t)lroundf(hi>0?254*d/hi:0);
}
