#include <stdio.h>
#include <stdlib.h>
#include "../mu/Platforms/OnePlus/macanPkg/Applications/DarwinMenu/HackIOSUi.h"
int main(int argc,char **argv) {
 int mode=argc>1?atoi(argv[1]):0;
 int selected=argc>2?atoi(argv[2]):0;
 int width=argc>3?1272:636,height=width*1400/636;
 HxCanvas c={calloc((size_t)width*height,4),width,height,width};if(!c.pixels)return 1;
 hx_render(&c,selected,mode);
 printf("P6\n%d %d\n255\n",width,height);
 for(int i=0;i<width*height;i++){unsigned int p=c.pixels[i];putchar(p>>16);putchar(p>>8);putchar(p);}
 free(c.pixels);return 0;
}
