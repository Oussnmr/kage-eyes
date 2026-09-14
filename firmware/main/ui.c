#include "ui.h"
#include <string.h>
#include "display_port.h"
#include "wifi_port.h"

static bool s_open;
static int s_selected;
static const char *items[] = {"LUMINOSITE", "WIFI CONFIG", "MISE A JOUR", "FERMER"};

/* Compact 5x7 ASCII renderer, intentionally kept local so the home screen
 * remains asset-free and the setting UI cannot consume flash with fonts. */
static const uint8_t glyphs[][5] = {
 [ 'A'-'A']={14,17,17,31,17}, ['B'-'A']={30,17,30,17,30}, ['C'-'A']={15,16,16,16,15}, ['D'-'A']={30,17,17,17,30}, ['E'-'A']={31,16,30,16,31}, ['F'-'A']={31,16,30,16,16}, ['G'-'A']={15,16,23,17,15}, ['H'-'A']={17,17,31,17,17}, ['I'-'A']={31,4,4,4,31}, ['J'-'A']={7,2,2,18,12}, ['K'-'A']={17,18,28,18,17}, ['L'-'A']={16,16,16,16,31}, ['M'-'A']={17,27,21,17,17}, ['N'-'A']={17,25,21,19,17}, ['O'-'A']={14,17,17,17,14}, ['P'-'A']={30,17,30,16,16}, ['Q'-'A']={14,17,17,19,15}, ['R'-'A']={30,17,30,18,17}, ['S'-'A']={15,16,14,1,30}, ['T'-'A']={31,4,4,4,4}, ['U'-'A']={17,17,17,17,14}, ['V'-'A']={17,17,17,10,4}, ['W'-'A']={17,17,21,27,17}, ['X'-'A']={17,10,4,10,17}, ['Y'-'A']={17,10,4,4,4}, ['Z'-'A']={31,2,4,8,31}
};
static void px(uint16_t *f,int w,int h,int x,int y,uint16_t c){if((unsigned)x<(unsigned)w&&(unsigned)y<(unsigned)h)f[y*w+x]=c;}
static void box(uint16_t *f,int w,int h,int x,int y,int bw,int bh,uint16_t c){for(int yy=y;yy<y+bh;yy++)for(int xx=x;xx<x+bw;xx++)px(f,w,h,xx,yy,c);}
static void text(uint16_t *f,int w,int h,int x,int y,const char *s,int scale,uint16_t c){for(;*s;s++,x+=6*scale){if(*s==' '){continue;}if(*s<'A'||*s>'Z')continue;const uint8_t *g=glyphs[*s-'A'];for(int col=0;col<5;col++)for(int row=0;row<5;row++)if(g[col]&(1<<(4-row)))box(f,w,h,x+col*scale,y+row*scale,scale,scale,c);}}
void ui_toggle_settings(void){s_open=!s_open;s_selected=0;}
bool ui_settings_visible(void){return s_open;}
void ui_tap(void){if(s_open)s_selected=(s_selected+1)%4;}
void ui_hold(void){if(!s_open){ui_toggle_settings();return;}if(s_selected==0){uint8_t level=display_port_brightness();display_port_set_brightness(level>220?60:level+35);}else if(s_selected==1)wifi_port_begin_setup();else if(s_selected==2)wifi_port_start_update();else s_open=false;}
void ui_render(uint16_t *f,int w,int h){if(!s_open)return;box(f,w,h,24,36,w-48,h-72,0x0841);box(f,w,h,27,39,w-54,42,0x0200);text(f,w,h,46,52,"REGLAGES",3,0x05FF);for(int i=0;i<4;i++){int y=108+i*52;box(f,w,h,46,y,w-92,39,i==s_selected?0x05FF:0x1082);text(f,w,h,62,y+13,items[i],2,i==s_selected?0x0000:0xBFFF);}text(f,w,h,56,h-58,"TAP SUIVANT",2,0x05FF);text(f,w,h,56,h-34,"APPUI LONG OK",2,0x05FF);}
