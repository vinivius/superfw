// Headless libretro frontend, to run SuperFW in gpsp without a window.
//   fe CORE.so ROM.gba FRAMES:KEYMASK:OUT.ppm [...]
// Runs FRAMES frames with KEYMASK (hex, RETRO_DEVICE_ID_JOYPAD_* bits) held,
// then saves the frame to OUT.ppm; repeats for each argument. With
// FE_REALTIME set it runs at 60fps (needed to talk to it over the UART pty).
// The core opens sdcard.img in the current directory.
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <time.h>
#include "libretro.h"
static const void *last; static unsigned W,H; static size_t P;
static int fmt = RETRO_PIXEL_FORMAT_0RGB1555;
static unsigned keys; // bitmask of RETRO_DEVICE_ID_JOYPAD_*
static bool env(unsigned cmd, void *d){
  switch(cmd){
  case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT: fmt=*(int*)d; return true;
  case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY: case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY: *(const char**)d="."; return true;
  case RETRO_ENVIRONMENT_GET_CAN_DUPE: *(bool*)d=true; return true;
  default: return false; }
}
static void vid(const void*d,unsigned w,unsigned h,size_t p){ if(d){last=d;W=w;H=h;P=p;} }
static void aud(int16_t l,int16_t r){} static size_t audb(const int16_t*d,size_t f){return f;}
static void poll(void){} static int16_t inp(unsigned port,unsigned dev,unsigned idx,unsigned id){ return port==0 && dev==RETRO_DEVICE_JOYPAD && (keys>>id&1); }
static void shot(const char*fn){ FILE*f=fopen(fn,"wb"); fprintf(f,"P6 %u %u 255\n",W,H);
  for(unsigned y=0;y<H;y++)for(unsigned x=0;x<W;x++){ unsigned char c[3];
    if(fmt==RETRO_PIXEL_FORMAT_XRGB8888){uint32_t v=((uint32_t*)((char*)last+y*P))[x];c[0]=v>>16;c[1]=v>>8;c[2]=v;}
    else {uint16_t v=((uint16_t*)((char*)last+y*P))[x];
      if(fmt==RETRO_PIXEL_FORMAT_RGB565){c[0]=(v>>11)<<3;c[1]=((v>>5)&63)<<2;c[2]=(v&31)<<3;}
      else {c[0]=((v>>10)&31)<<3;c[1]=((v>>5)&31)<<3;c[2]=(v&31)<<3;}}
    fwrite(c,1,3,f);} fclose(f); }
int main(int argc,char**argv){
  void*h=dlopen(argv[1],RTLD_NOW); if(!h){puts(dlerror());return 1;}
#define S(n) typeof(n)*p_##n=dlsym(h,#n)
  S(retro_set_environment);S(retro_set_video_refresh);S(retro_set_audio_sample);S(retro_set_audio_sample_batch);
  S(retro_set_input_poll);S(retro_set_input_state);S(retro_init);S(retro_load_game);S(retro_run);
  p_retro_set_environment(env);p_retro_init();p_retro_set_video_refresh(vid);p_retro_set_audio_sample(aud);
  p_retro_set_audio_sample_batch(audb);p_retro_set_input_poll(poll);p_retro_set_input_state(inp);
  struct retro_game_info gi={argv[2],NULL,0,NULL}; if(!p_retro_load_game(&gi)){puts("load failed");return 1;}
  // script: argv[3..] = "frames:keymask:outfile"
  for(int a=3;a<argc;a++){ int n; unsigned k; char fn[256]; sscanf(argv[a],"%d:%x:%255s",&n,&k,fn);
    keys=k; for(int i=0;i<n;i++){ p_retro_run();
      if(getenv("FE_REALTIME")){ static struct timespec next; struct timespec now; clock_gettime(CLOCK_MONOTONIC,&now);
        if(!next.tv_sec) next=now; next.tv_nsec+=16742706; if(next.tv_nsec>=1000000000){next.tv_nsec-=1000000000;next.tv_sec++;}
        clock_nanosleep(CLOCK_MONOTONIC,TIMER_ABSTIME,&next,NULL);} }
    shot(fn); printf("%s %ux%u\n",fn,W,H); fflush(stdout); }
  return 0; }
