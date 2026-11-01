#include "ble_wire.h"
#include <string.h>

static bool utf8_valid(const unsigned char *s,size_t length)
{
    for(size_t i=0;i<length;) {
        unsigned c=s[i++],need=0,code=0,minimum=0;
        if(c<128)continue;
        if(c>=0xc2 && c<=0xdf) {need=1;code=c&31;minimum=0x80;}
        else if(c>=0xe0 && c<=0xef) {need=2;code=c&15;minimum=0x800;}
        else if(c>=0xf0 && c<=0xf4) {need=3;code=c&7;minimum=0x10000;}
        else return false;
        if(i+need>length)return false;
        while(need--) {c=s[i++];if((c&0xc0)!=0x80)return false;code=(code<<6)|(c&63);}
        if(code<minimum || code>0x10ffff || (code>=0xd800 && code<=0xdfff))return false;
    }
    return true;
}
int ble_message_feed(ble_message_t *m,const uint8_t *frame,size_t size,uint64_t now)
{
    if(m->received && now-m->last_ms>=BLE_FRAGMENT_TIMEOUT_MS) m->received=m->total=0;
    if(size<5) goto invalid;
    unsigned total=frame[0]|((unsigned)frame[1]<<8),offset=frame[2]|((unsigned)frame[3]<<8);
    size_t n=size-4;
    if(!total || total>BLE_MESSAGE_MAX || offset!=m->received || offset>total || n>total-offset
        || (offset && total!=m->total) || memchr(frame+4,0,n)) goto invalid;
    m->total=(uint16_t)total;
    memcpy(m->data+offset,frame+4,n);m->received=(uint16_t)(offset+n);m->last_ms=now;
    m->data[m->received]=0;
    if(m->received==m->total && !utf8_valid((const unsigned char *)m->data,m->total))goto invalid;
    return m->received==m->total?1:0;
invalid:
    m->received=m->total=0;return -1;
}
size_t ble_message_frame(const char *message,size_t total,size_t offset,unsigned mtu,uint8_t *out,size_t capacity)
{
    if(!total || total>BLE_MESSAGE_MAX || offset>=total || mtu<23 || capacity<5)return 0;
    size_t n=total-offset;
    if(n>mtu-7)n=mtu-7;
    if(n>capacity-4)n=capacity-4;
    out[0]=(uint8_t)total;out[1]=(uint8_t)(total>>8);out[2]=(uint8_t)offset;out[3]=(uint8_t)(offset>>8);
    memcpy(out+4,message+offset,n);return n+4;
}
