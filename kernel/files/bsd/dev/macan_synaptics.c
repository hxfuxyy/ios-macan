#include <kern/clock.h>
#include <kern/thread.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/systm.h>
#include <sys/macan_input.h>
#include <sys/macan_dsp.h>
#include "macan_touch_report.h"
extern kern_return_t thread_terminate(thread_t);
extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
extern void loader_log_putc(char);
static vm_offset_t pins;
static volatile uint32_t stopping;
static int pins_owned;
static uint32_t reset_cfg,reset_io,irq_cfg;
static vm_offset_t avdd_channel;
static uint8_t avdd_saved[7];
static int avdd_changed;
static void restore_avdd(void);
static void vddi_restore(void);
static unsigned max_x,max_y;
static int diagnostic;
static struct macan_contact contacts[10];
static uint32_t saved_cfg[4], saved_io[4];
static uint32_t rd(unsigned pin,unsigned offset) { return *(volatile uint32_t *)(pins+pin*0x1000+offset); }
static void wr(unsigned pin,unsigned offset,uint32_t v) { *(volatile uint32_t *)(pins+pin*0x1000+offset)=v; __asm__ volatile("dsb sy"); }
static void say(const char *fmt,...) {char b[256];va_list ap;va_start(ap,fmt);vsnprintf(b,sizeof(b),fmt,ap);va_end(ap);for(char *p=b;*p;p++)loader_log_putc(*p);printf("%s",b);}
static void level(unsigned pin,int high) {wr(pin,4,high?2:0);}
static void pause_edge(void) {delay_for_interval(2,NSEC_PER_USEC);}
static void spi_bytes(const uint8_t *tx,uint8_t *out,unsigned n) {
    for(unsigned i=0;i<n;i++) {uint8_t v=0;for(int bit=7;bit>=0;bit--) {level(49,tx?((tx[i]>>bit)&1):1);pause_edge();level(50,1);pause_edge();v=(uint8_t)((v<<1)|(rd(48,4)&1));level(50,0);}if(out)out[i]=v;}
}
static void transfer_spi(const uint8_t *tx,uint8_t *out,unsigned n) {
    level(51,0);pause_edge();spi_bytes(tx,out,n);
    pause_edge();level(51,1);pause_edge();
}
static int packet(uint8_t *code,uint8_t *payload,unsigned *length) {
    uint8_t frame[1029];
    level(51,0);pause_edge();spi_bytes(NULL,frame,4);
    unsigned n=macan_le16(frame+2);
    int valid=frame[0]==0xa5&&n<=1024;
    if(valid&&n)spi_bytes(NULL,frame+4,n+1);
    pause_edge();level(51,1);pause_edge();
    static unsigned samples;
    if(diagnostic&&samples++<8)say("macan-touch: RX %02x %02x %02x %02x irq=%u\n",frame[0],frame[1],frame[2],frame[3],rd(55,4)&1);
    if(!valid)return 0;
    if(n&&frame[n+4]!=0x5a){if(diagnostic)say("macan-touch: padding mismatch n=%u byte=%x\n",n,frame[n+4]);return 0;}
    if(n)memcpy(payload,frame+4,n);
    *code=frame[1];*length=n;return 1;
}
static int command(uint8_t cmd,const uint8_t *data,unsigned n,uint8_t *response,unsigned *length) {
    uint8_t tx[1027],code;unsigned size;
    if(n>1024||stopping)return 0;
    tx[0]=cmd;tx[1]=(uint8_t)n;tx[2]=(uint8_t)(n>>8);if(n)memcpy(tx+3,data,n);
    transfer_spi(tx,NULL,n+3);
    for(unsigned tries=0;tries<100 && !stopping;tries++) {
        delay_for_interval(5,NSEC_PER_MSEC);
        if(packet(&code,response,&size) && code<0x10 && code!=0 && code!=3 && code!=0xff){if(diagnostic)say("macan-touch: CMD %x response=%x len=%u\n",cmd,code,size);*length=size;return code==1;}
    }
    return 0;
}
static void restore_pins(void) {
    if(!pins_owned){restore_avdd();vddi_restore();return;}
    level(51,1);
    for(unsigned i=0;i<4;i++){wr(48+i,4,saved_io[i]&2);wr(48+i,0,saved_cfg[i]);}
    wr(161,4,reset_io&2);wr(161,0,reset_cfg);wr(55,0,irq_cfg);
    pins_owned=0;restore_avdd();vddi_restore();
}
void macan_synaptics_quiesce(void) {stopping=1;restore_pins();}
static void report_loop(void) {
    uint8_t response[1024];unsigned n=0;
    uint8_t startup=0;
    if(!packet(&startup,response,&n)||startup!=0x10||n<2){say("macan-touch: startup identify unavailable; no report writes\n");return;}
    say("macan-touch: startup identify version=%u mode=%x len=%u\n",response[0],response[1],n);
    if(!command(0x20,NULL,0,response,&n) || n<40 || macan_le16(response+2)!=0) {say("macan-touch: application info unavailable; no report writes\n");return;}
    unsigned config_size=macan_le16(response+12);
    max_x=macan_le16(response+32);max_y=macan_le16(response+34);
    if(config_size<11||config_size>1024||macan_le16(response+36)!=10||!max_x||!max_y){say("macan-touch: incompatible application limits\n");return;}
    say("macan-touch: application x=%u y=%u objects=10 config=%u\n",max_x,max_y,config_size);
    uint8_t config[1024]={2,6,4,7,4,8,16,9,16,3,0};
    if(!command(0x26,config,config_size,response,&n)){say("macan-touch: report config refused\n");return;}
    if(!command(0x25,NULL,0,response,&n) || n<11 || memcmp(response,config,11)!=0) {say("macan-touch: custom report readback mismatch\n");return;}
    uint8_t report=0x11;
    if(!command(5,&report,1,response,&n)){say("macan-touch: enable report refused\n");return;}
    say("macan-touch: report polling started\n");
    unsigned failures=0,frames=0;
    while(!stopping && failures<100) {
        if(rd(55,4)&1){delay_for_interval(2,NSEC_PER_MSEC);continue;}
        uint8_t code;struct macan_contact next[10];
        if(!packet(&code,response,&n)){failures++;delay_for_interval(5,NSEC_PER_MSEC);continue;}
        if(code==0x10){say("macan-touch: controller reset; ending poll\n");break;}
        if(code!=0x11)continue;
        if(!macan_parse_contacts(response,n,max_x,max_y,next)){failures++;continue;}
        failures=0;
        for(unsigned i=0;i<10;i++) {
            if(next[i].active)redwood_touch_post(contacts[i].active?REDWOOD_TOUCH_MOVE:REDWOOD_TOUCH_DOWN,i,next[i].x,next[i].y);
            else if(contacts[i].active)redwood_touch_post(REDWOOD_TOUCH_UP,i,contacts[i].x,contacts[i].y);
            contacts[i]=next[i];
        }
        ++frames;
        if(diagnostic) {
            for(unsigned i=0;i<10;i++)if(next[i].active)
                say("TOUCH frame=%u id=%u x=%u y=%u\n",frames,i,next[i].x,next[i].y);
        } else if(frames<=3)say("macan-touch: frame %u delivered\n",frames);
    }
    for(unsigned i=0;i<10;i++)if(contacts[i].active)redwood_touch_post(REDWOOD_TOUCH_UP,i,contacts[i].x,contacts[i].y);
    say("macan-touch: polling ended frames=%u failures=%u\n",frames,failures);
}
static int avdd_write(unsigned reg,uint8_t value) {
    if(!avdd_channel || reg<0x40 || reg>0x46)return 0;
    *(volatile uint32_t *)(avdd_channel+0x10)=value;
    __asm__ volatile("dsb sy");
    *(volatile uint32_t *)avdd_channel=reg<<4;
    unsigned tries=0;uint32_t status;
    do{status=*(volatile uint32_t *)(avdd_channel+8);delay_for_interval(1,NSEC_PER_USEC);}while(!(status&1)&&++tries<1000);
    if(status!=1)say("macan-touch: AVDD write reg%x status=%x\n",reg,status);
    return status==1;
}
static void restore_avdd(void) {
    if(!avdd_changed)return;
    avdd_write(0x44,avdd_saved[4]);
    avdd_write(0x40,avdd_saved[0]);avdd_write(0x41,avdd_saved[1]);avdd_write(0x45,avdd_saved[5]);
    avdd_changed=0;
    say("macan-touch: AVDD GPIO configuration restored\n");
}
static void power_probe(void) {
    vm_offset_t core=ml_io_map(0x0c400000,0x4000);
    vm_offset_t observer=ml_io_map(0x0c4c0000,0x40000);
    vm_offset_t mapping_base=ml_io_map(0x0c403000,0x8000);
    vm_offset_t owner=ml_io_map(0x0c42a000,0x8000);
    if(!core||!observer||!mapping_base||!owner){say("macan-touch: SPMI observer map failed\n");return;}
    uint32_t version=*(volatile uint32_t *)core;
    unsigned count=*(volatile uint32_t *)(core+4)&0x1fff;
    say("macan-touch: SPMI version=%x primary APIDs=%u\n",version,count);
    if(version<0x80000000||count>8192)return;
    for(unsigned i=0;i<count;i++) {
        uint32_t mapping=*(volatile uint32_t *)(mapping_base+4*i);
        if(!mapping||(mapping&0x1fff)!=0x68d)continue;
        unsigned ee=*(volatile uint32_t *)(owner+4*i)&7;
        if(ee!=0){say("macan-touch: AVDD APID%u ownerEE%u skipped\n",i,ee);continue;}
        vm_offset_t channel=observer+0x20*i;
        *(volatile uint32_t *)channel=(1u<<27)|(0x40u<<4)|6;
        __asm__ volatile("dsb sy");
        unsigned retry=0;uint32_t status;
        do {status=*(volatile uint32_t *)(channel+8);delay_for_interval(1,NSEC_PER_USEC);}while(!(status&1)&&++retry<1000);
        if(status!=1){say("macan-touch: AVDD APID%u status=%x\n",i,status);continue;}
        uint32_t a=*(volatile uint32_t *)(channel+0x18),b=*(volatile uint32_t *)(channel+0x1c);
        say("macan-touch: AVDD SID6 GPIO6 APID%u reg40..46=%02x %02x %02x %02x %02x %02x %02x\n",i,a&255,(a>>8)&255,(a>>16)&255,a>>24,b&255,(b>>8)&255,(b>>16)&255);
        for(unsigned j=0;j<7;j++)avdd_saved[j]=(uint8_t)((j<4?a:b)>>((j%4)*8));
        {
            vm_offset_t page=ml_io_map((0x0c900000+0x200*i)&~(vm_offset_t)PAGE_MASK,PAGE_SIZE);
            if(!page){say("macan-touch: AVDD writer channel map failed\n");break;}
            avdd_channel=page+((0x0c900000+0x200*i)&PAGE_MASK);
        }
        if(avdd_saved[2]!=4 || avdd_saved[3]!=0 || avdd_saved[6]!=0x80) {say("macan-touch: unknown AVDD configuration; unchanged\n");break;}
        if(avdd_saved[0]!=1 || avdd_saved[1]!=1 || avdd_saved[4]!=0x80 || avdd_saved[5]!=2) {
            avdd_changed=1;
            if(!avdd_write(0x41,1)||!avdd_write(0x45,2)||!avdd_write(0x40,1)||!avdd_write(0x44,0x80)){restore_avdd();break;}
            say("macan-touch: applied Android AVDD GPIO6 configuration\n");
            delay_for_interval(200,NSEC_PER_MSEC);
        }
        break;
    }
}
static vm_offset_t vddi_rsc;
static int vddi_vote;
static int vddi_send_locked(unsigned enable) {
    if(!vddi_rsc)return 0;
    unsigned major=(*(volatile uint32_t *)vddi_rsc>>16)&255;
    if(major!=2&&major!=3&&major!=4){say("macan-touch: unsupported RSC version%u\n",major);return 0;}
    unsigned control=major>=3?0x24:0x14,status=major>=3?0x28:0x18;
    unsigned wait=major>=3?0x20:0x10,cmd_enable=major>=3?0x2c:0x1c;
    unsigned msg=major>=3?0x34:0x30,stride=major>=3?24:20;
    vm_offset_t t=vddi_rsc+0xd00;
    if(!(*(volatile uint32_t *)(t+status)&1)){say("macan-touch: ACTIVE TCS0 busy; no vote\n");return 0;}
    unsigned offsets[15]={0,wait,control,cmd_enable};uint32_t saved[15];unsigned used=4;
    for(unsigned i=0;i<3;i++)for(unsigned j=0;j<3;j++)offsets[used++]=msg+i*stride+j*4;
    for(unsigned i=0;i<used;i++)saved[i]=*(volatile uint32_t *)(t+offsets[i]);
    saved[2]&=~(1u<<24);
    *(volatile uint32_t *)(t+control)=saved[2]&~(1u<<24);
    *(volatile uint32_t *)(t+control)=saved[2]&~((1u<<24)|(1u<<16));
    *(volatile uint32_t *)(t+8)=1;
    uint32_t addresses[3]={0x40700,0x40708,0x40704},data[3]={1800,7,enable};
    unsigned count=enable?3:1;
    if(!enable){addresses[0]=0x40704;data[0]=0;}
    for(unsigned i=0;i<count;i++) {
        *(volatile uint32_t *)(t+msg+i*stride)=0x10108;
        *(volatile uint32_t *)(t+msg+i*stride+4)=addresses[i];
        *(volatile uint32_t *)(t+msg+i*stride+8)=data[i];
    }
    *(volatile uint32_t *)(t+wait)=(1u<<count)-1;
    *(volatile uint32_t *)(t+cmd_enable)=(1u<<count)-1;
    *(volatile uint32_t *)t=saved[0]|1;
    *(volatile uint32_t *)(t+control)=1u<<16;
    __asm__ volatile("dsb sy");
    *(volatile uint32_t *)(t+control)=(1u<<16)|(1u<<24);
    unsigned tries=0;
    while(!(*(volatile uint32_t *)(t+4)&1)&&++tries<2000)delay_for_interval(100,NSEC_PER_USEC);
    if(tries==2000){say("macan-touch: VDDI RPMh timeout; TCS not reused\n");return 0;}
    *(volatile uint32_t *)(t+control)=1u<<16;
    *(volatile uint32_t *)(t+control)=0;
    *(volatile uint32_t *)(t+8)=1;
    for(unsigned i=used;i>0;i--)*(volatile uint32_t *)(t+offsets[i-1])=saved[i-1];
    say("macan-touch: VDDI RPMh L4B_E0 1800mV enable=%u completed\n",enable);
    return 1;
}
static int vddi_send(unsigned enable) {
    if(stopping) {
        if(!lck_mtx_try_lock(&macan_rpmh_lock)){say("macan-touch: RPMh busy during quiesce; vote retained\n");return 0;}
    } else lck_mtx_lock(&macan_rpmh_lock);
    int result=vddi_send_locked(enable);
    lck_mtx_unlock(&macan_rpmh_lock);
    return result;
}
static void vddi_restore(void) {
    if(vddi_vote){vddi_send(0);vddi_vote=0;}
}
static void vddi_enable(void) {
    vddi_rsc=ml_io_map(0x18920000,0x10000);
    if(vddi_send(1)){vddi_vote=1;delay_for_interval(100,NSEC_PER_MSEC);}
}
static void probe(void *mode,wait_result_t result) {
    (void)result;
    vddi_enable();
    power_probe();
    pins=ml_io_map(0x0f100000,0x0b0000);
    if(!pins){say("macan-touch: cannot map verified TLMM bank\n");thread_terminate(current_thread());return;}
    say("macan-touch: IRQ55 cfg=%x io=%x RESET161 cfg=%x io=%x\n",rd(55,0),rd(55,4),rd(161,0),rd(161,4));
    for(unsigned i=0;i<4;i++){unsigned p=48+i;saved_cfg[i]=rd(p,0);saved_io[i]=rd(p,4);say("macan-touch: pin%u cfg=%x io=%x\n",p,saved_cfg[i],saved_io[i]);}
    reset_cfg=rd(161,0);reset_io=rd(161,4);irq_cfg=rd(55,0);
    level(161,0);wr(161,0,(reset_cfg&~0x13ffu)|(1u<<9)|(3u<<6));
    delay_for_interval(10,NSEC_PER_MSEC);
    level(161,1);
    wr(55,0,(irq_cfg&~0x13ffu)|(3u<<6)|3u);
    delay_for_interval(100,NSEC_PER_MSEC);
    level(51,1);level(50,0);level(49,1);
    for(unsigned i=0;i<4;i++){uint32_t cfg=saved_cfg[i]&~0x13ffu;if(saved_cfg[i]&(1u<<11))cfg|=1u<<12;cfg|=1u<<6;if(i)cfg|=1u<<9;wr(48+i,0,cfg);}
    pins_owned=1;
    for(unsigned i=0;i<4;i++)say("macan-touch: AP GPIO%u cfg=%x io=%x\n",48+i,rd(48+i,0),rd(48+i,4));
    say("macan-touch: reset released cfg=%x io=%x IRQ cfg=%x io=%x\n",rd(161,0),rd(161,4),rd(55,0),rd(55,4));
    if((uintptr_t)mode==2){report_loop();restore_pins();thread_terminate(current_thread());return;}
    uint8_t h[4];
    for(unsigned attempt=0;attempt<5;attempt++) {
        transfer_spi(NULL,h,sizeof(h));
        say("macan-touch: TCM header %02x %02x %02x %02x irq=%u\n",h[0],h[1],h[2],h[3],rd(55,4)&1);
        if(h[0]==0xa5){say("macan-touch: TCMv1 marker found code=%x len=%u; report decoding pending\n",h[1],h[2]|(h[3]<<8));break;}
        delay_for_interval(20,NSEC_PER_MSEC);
    }
    restore_pins();
    say("macan-touch: SPI pin configuration restored\n");
    thread_terminate(current_thread());
}
void macan_synaptics_init(void) {
    uint32_t enabled=0;thread_t t;
    uint32_t mode=0;
    PE_parse_boot_argn("macan_touch_diag",&diagnostic,sizeof(diagnostic));
    if(PE_parse_boot_argn("macan_touch",&enabled,sizeof(enabled))&&enabled==1)mode=2;
    else if(PE_parse_boot_argn("macan_touch_probe",&enabled,sizeof(enabled))&&enabled==1)mode=1;
    if(!mode)return;
    if(kernel_thread_start(probe,(void *)(uintptr_t)mode,&t)==KERN_SUCCESS)thread_deallocate(t);
}
