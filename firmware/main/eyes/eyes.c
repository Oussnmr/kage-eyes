/* Geometry-only eyes: no bitmap assets and no blocking delays. */
#include "eyes.h"
#include <stdbool.h>
#include <string.h>
#include "esp_random.h"
typedef struct { float x, y, w, h, r, tilt; } eye_shape_t;
static eye_shape_t s_now[2], s_from[2], s_to[2];
static eye_expression_t s_expression;
static int64_t s_transition_start, s_next_blink, s_blink_start;
static bool s_blinking;
#define CYAN 0x05FF
#define BLACK 0x0000
#define TRANSITION_US 220000LL
#define BLINK_US 150000LL
static float ease(float t) { return t * t * (3.0f - 2.0f * t); }
static float lerp(float a, float b, float t) { return a + (b - a) * t; }
static eye_shape_t shape(float x, float y, float w, float h, float r, float tilt) { return (eye_shape_t){x,y,w,h,r,tilt}; }
static void targets(eye_expression_t e, eye_shape_t out[2]) {
    float x=96,y=99,w=120,h=168,r=38,shift=0,tilt=0;
    switch(e) { case EYES_HAPPY:y=155;h=82;r=40;break; case EYES_ANGRY:y=106;h=150;r=30;tilt=26;break; case EYES_SURPRISED:x=116;y=78;w=80;h=210;r=40;break; case EYES_SLEEPY:y=184;h=38;r=19;break; case EYES_LOOK_LEFT:shift=-28;break; case EYES_LOOK_RIGHT:shift=28;break; default:break; }
    out[0]=shape(x+shift,y,w,h,r,tilt); out[1]=shape(448-x-w+shift,y,w,h,r,-tilt);
}
void eyes_init(void) { targets(EYES_NORMAL,s_now); memcpy(s_from,s_now,sizeof(s_now)); memcpy(s_to,s_now,sizeof(s_now)); s_expression=EYES_NORMAL; s_next_blink=2500000LL+(esp_random()%3500000); }
void eyes_set_expression(eye_expression_t e) { if(e>=EYES_EXPRESSION_COUNT)e=EYES_NORMAL; memcpy(s_from,s_now,sizeof(s_now)); targets(e,s_to); s_expression=e;s_transition_start=0;s_blinking=false; }
void eyes_next_expression(void) { eyes_set_expression((s_expression+1)%EYES_EXPRESSION_COUNT); }
eye_expression_t eyes_expression(void) { return s_expression; }
void eyes_update(int64_t now) {
    if(!s_transition_start)s_transition_start=now; float t=(float)(now-s_transition_start)/TRANSITION_US;if(t>1)t=1;t=ease(t);
    for(int i=0;i<2;i++){s_now[i].x=lerp(s_from[i].x,s_to[i].x,t);s_now[i].y=lerp(s_from[i].y,s_to[i].y,t);s_now[i].w=lerp(s_from[i].w,s_to[i].w,t);s_now[i].h=lerp(s_from[i].h,s_to[i].h,t);s_now[i].r=lerp(s_from[i].r,s_to[i].r,t);s_now[i].tilt=lerp(s_from[i].tilt,s_to[i].tilt,t);}
    if(s_expression!=EYES_NORMAL)return; if(!s_blinking&&now>=s_next_blink){s_blinking=true;s_blink_start=now;} if(s_blinking&&now-s_blink_start>BLINK_US){s_blinking=false;s_next_blink=now+2500000LL+(esp_random()%3500000);}
}
static void pixel(uint16_t *f,int w,int h,int x,int y,uint16_t c){if((unsigned)x<(unsigned)w&&(unsigned)y<(unsigned)h)f[y*w+x]=c;}
static void rounded(uint16_t *f,int fw,int fh,eye_shape_t e,int grow,uint16_t c){int x0=(int)(e.x-grow),y0=(int)(e.y-grow),x1=(int)(e.x+e.w+grow),y1=(int)(e.y+e.h+grow);float r=e.r+grow;for(int y=y0;y<y1;y++)for(int x=x0;x<x1;x++){float dx=x<e.x+r?e.x+r-x:x>e.x+e.w-r?x-(e.x+e.w-r):0;float dy=y<e.y+r?e.y+r-y:y>(e.y+e.h-r)?y-(e.y+e.h-r):0;if(dx*dx+dy*dy<=r*r)pixel(f,fw,fh,x,y,c);}}
void eyes_render(uint16_t *frame,int width,int height){memset(frame,0,width*height*sizeof(*frame));for(int i=0;i<2;i++){eye_shape_t e=s_now[i];if(s_blinking)e.h=8;rounded(frame,width,height,e,12,0x0130);rounded(frame,width,height,e,6,0x03BF);rounded(frame,width,height,e,0,CYAN);if(e.tilt!=0){int inward=i==0?1:-1;for(int y=0;y<35;y++)for(int x=0;x<(int)e.w;x++)if(x*inward>(int)e.w/2-(int)e.tilt+y)pixel(frame,width,height,(int)e.x+x,(int)e.y+y,BLACK);}}}
