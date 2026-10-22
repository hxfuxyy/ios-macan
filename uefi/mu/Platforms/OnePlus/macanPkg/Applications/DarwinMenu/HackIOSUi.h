// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef HACKIOS_UI_H
#define HACKIOS_UI_H 
typedef struct { const unsigned char *data, *width; const unsigned int *offset; int height; } HxFont;
#include "HackIOSAssets.h"
typedef struct { unsigned int *pixels; int width, height, stride; } HxCanvas;
static int hx_scale(HxCanvas *c, int n) { return n*c->width/636; }
static void hx_rect(HxCanvas *c,int x,int y,int w,int h,unsigned int rgb) {
 int x0=hx_scale(c,x),x1=hx_scale(c,x+w),y0=hx_scale(c,y),y1=hx_scale(c,y+h);
 if(x0<0)x0=0;
 if(y0<0)y0=0;
 if(x1>c->width)x1=c->width;
 if(y1>c->height)y1=c->height;
 for(int j=y0;j<y1;j++)for(int i=x0;i<x1;i++)c->pixels[j*c->stride+i]=rgb;
}
static unsigned int hx_mix(unsigned int bg,unsigned int fg,unsigned int a) {
 unsigned int r=(((bg>>16)&255)*(255-a)+((fg>>16)&255)*a)/255;
 unsigned int g=(((bg>>8)&255)*(255-a)+((fg>>8)&255)*a)/255;
 unsigned int b=((bg&255)*(255-a)+(fg&255)*a)/255;
 return (r<<16)|(g<<8)|b;
}
static void hx_round(HxCanvas *c,int x,int y,int w,int h,int r,unsigned int color) {
 for(int j=0;j<h;j++) {
  int inset=0;
  int dy=j<r?r-1-j:(j>=h-r?j-(h-r):0);
  if(j<r||j>=h-r)while(inset<r&&(r-inset)*(r-inset)+dy*dy>r*r)inset++;
  hx_rect(c,x+inset,y+j,w-2*inset,1,color);
 }
}
static int hx_text_width(const HxFont *f,const char *s) {
 int w=0;for(;*s;s++){unsigned char ch=(unsigned char)*s;if(ch>=32&&ch<=126)w+=f->width[ch-32];}return w;
}
static void hx_text(HxCanvas *c,const HxFont *f,int x,int y,const char *s,unsigned int fg,unsigned int bg) {
 for(;*s;s++) {
  unsigned char ch=(unsigned char)*s;if(ch<32||ch>126)continue;
  int k=ch-32,w=f->width[k];const unsigned char *data=f->data+f->offset[k];
  for(int j=0;j<f->height;j++)for(int i=0;i<w;i++)if(data[j*w+i])hx_rect(c,x+i,y+j,1,1,hx_mix(bg,fg,data[j*w+i]));
  x+=w;
 }
}
static void hx_center(HxCanvas *c,const HxFont *f,int y,const char *s,unsigned int fg,unsigned int bg) {
 hx_text(c,f,(636-hx_text_width(f,s))/2,y,s,fg,bg);
}
static void hx_icon(HxCanvas *c,int x,int y,int kind,unsigned int color) {
 if(kind==0) {
  for(int j=0;j<25;j++){int span=j<=12?j:24-j;hx_rect(c,x+9,y+j,span+1,1,color);}
 } else if(kind==1) {
  for(int j=0;j<3;j++){int yy=y+4+j*9;hx_rect(c,x+2,yy,26,2,color);hx_round(c,x+6+(j==1?10:0),yy-3,5,8,2,color);}
 } else if(kind==2) {
  hx_rect(c,x+13,y,2,23,color);hx_rect(c,x+5,y+12,10,2,color);hx_rect(c,x+5,y+6,2,8,color);
  hx_rect(c,x+14,y+17,10,2,color);hx_rect(c,x+22,y+7,2,12,color);
  hx_round(c,x+10,y+22,8,8,4,color);hx_rect(c,x+20,y+3,6,6,color);
  for(int j=0;j<5;j++)hx_rect(c,x+14-j,y+j,1+2*j,1,color);
 } else {
  for(int j=0;j<25;j++)for(int i=0;i<25;i++) {
   int dx=i-12,dy=j-12,d=dx*dx+dy*dy;
   if(d>=92&&d<=144&&(j>7||i<8||i>16))hx_rect(c,x+i,y+j,1,1,color);
  }
  hx_rect(c,x+11,y-1,3,14,color);
 }
}
static void hx_render(HxCanvas *c,int selected,int mode) {
 const unsigned int bg=0x08090b,white=0xf5f5f7,muted=0x818690,blue=0x409cff;
 for(int y=0;y<c->height;y++)for(int x=0;x<c->width;x++)c->pixels[y*c->stride+x]=bg;
 int logical_height=c->height*636/c->width;
 int offset=(logical_height-1400)/2;
 for(int y=0;y<160;y++)for(int x=0;x<160;x++){
  unsigned int v=hx_logo[y*160+x];if(v)hx_rect(c,238+x,200+offset+y,1,1,(v<<16)|(v<<8)|v);
 }
 hx_center(c,&hx_title,385+offset,"HackIOS UEFI",white,bg);
 hx_center(c,&hx_small,438+offset,"A new beginning for your device.",muted,bg);
 if(mode==0) {
  const char *names[]={"Boot iOS","Diagnostics","USB Storage","Restart Android"};
  const char *detail[]={"Open the Darwin USB loader","Hardware and boot information","Connect storage over USB","Return to your phone"};
  for(int i=0;i<4;i++) {
   int y=526+offset+i*106;unsigned int card=i==selected?0x102238:0x14161b;
   hx_round(c,58,y,520,94,20,card);
   if(i==selected)hx_round(c,58,y+30,3,34,1,blue);
   hx_icon(c,82,y+31,i,i==selected?blue:0xb0b5bf);
   hx_text(c,&hx_body,132,y+14,names[i],white,card);
   hx_text(c,&hx_caption,132,y+48,detail[i],i==selected?0x9ab4d3:muted,card);
   hx_text(c,&hx_body,540,y+27,">",i==selected?blue:0x535965,card);
  }
  hx_center(c,&hx_small,1013+offset,"Volume to select  /  Power to open",muted,bg);
 } else if(mode==1) {
  hx_round(c,108,568+offset,420,124,22,0x14161b);
  hx_round(c,140,605+offset,8,8,4,0x30d18a);
  hx_text(c,&hx_body,166,586+offset,"Ready to boot",white,0x14161b);
  hx_text(c,&hx_small,166,624+offset,"Waiting for the USB payload",muted,0x14161b);
  hx_center(c,&hx_small,745+offset,"Keep your device connected.",muted,bg);
 } else {
  hx_round(c,286,585+offset,64,3,1,0x30343d);
 }
 hx_center(c,&hx_caption,logical_height-133,"ONEPLUS ACE 6T",0x666d78,bg);
 hx_center(c,&hx_caption,logical_height-106,"@hxfuxyyXD  #oosexgang",0x818690,bg);
 hx_round(c,257,logical_height-39,122,4,2,0x4a4f58);
}
#endif
