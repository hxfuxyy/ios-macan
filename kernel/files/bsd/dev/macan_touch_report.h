#ifndef MACAN_TOUCH_REPORT_H
#define MACAN_TOUCH_REPORT_H
#include <stdint.h>
struct macan_contact { uint16_t x,y; uint8_t active; };
static inline unsigned macan_le16(const uint8_t *p) {return p[0]|((unsigned)p[1]<<8);}
static inline int macan_parse_contacts(const uint8_t *p,unsigned n,unsigned maxx,unsigned maxy,struct macan_contact out[10]) {
    if(n!=50 || !maxx || !maxy)return 0;
    unsigned seen=0;
    for(unsigned i=0;i<10;i++)out[i]=(struct macan_contact){0};
    for(unsigned i=0;i<10;i++,p+=5) {
        unsigned id=p[0]&15,classification=p[0]>>4;
        if(id>=10 || (seen&(1u<<id)) || classification>3)return 0;
        seen|=1u<<id;
        unsigned x=macan_le16(p+1),y=macan_le16(p+3);
        if(classification==1 || classification==2) {
            if(x>maxx || y>maxy)return 0;
            out[id].active=1;out[id].x=(uint16_t)((uint64_t)x*65535/maxx);out[id].y=(uint16_t)((uint64_t)y*65535/maxy);
        }
    }
    return 1;
}
#endif
