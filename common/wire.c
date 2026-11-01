#include "wire.h"
#include <stdio.h>
#include <string.h>
uint32_t wire_crc(const void *data, size_t size)
{
    const uint8_t *bytes = data;
    uint32_t crc = UINT32_MAX;
    while (size--) {
        crc ^= *bytes++;
        for (unsigned i=0;i<8;i++) crc=(crc>>1)^(0xedb88320u & (0u-(crc&1u)));
    }
    return ~crc;
}
size_t wire_encode(const char *json, char *out, size_t capacity)
{
    size_t n=strlen(json);
    if (n+11>capacity || n+10>=WIRE_CAP || strchr(json,'\n') || strchr(json,'\r')) return 0;
    memcpy(out,json,n);
    snprintf(out+n,capacity-n,"*%08lx\n",(unsigned long)wire_crc(json,n));
    return n+10;
}
bool wire_feed(wire_rx_t *rx, uint8_t byte)
{
    if (byte=='\n') {
        size_t n=rx->used;
        bool dropped=rx->dropping;
        rx->used=0; rx->dropping=false;
        if (dropped || n<11 || rx->data[n-9]!='*') return false;
        uint32_t expected=0;
        for (size_t i=n-8;i<n;i++) {
            unsigned char c=(unsigned char)rx->data[i];
            unsigned v=c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:c>='A'&&c<='F'?c-'A'+10:16;
            if (v>15) return false;
            expected=(expected<<4)|v;
        }
        if (wire_crc(rx->data,n-9)!=expected) return false;
        rx->data[n-9]=0;
        return true;
    }
    if (byte==0 || byte=='\r' || rx->used>=WIRE_CAP-1) rx->dropping=true;
    if (!rx->dropping) rx->data[rx->used++]=(char)byte;
    return false;
}
