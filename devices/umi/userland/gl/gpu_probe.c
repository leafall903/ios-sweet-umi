#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "hakios.gl.h"
extern const unsigned char *glGetString(unsigned);
extern void glClearColor(float,float,float,float);
extern void glClear(unsigned);
extern void glFinish(void);
extern unsigned glGetError(void);
extern void glReadPixels(int,int,int,int,unsigned,unsigned,void *);
int umi_v20_probe_gpu(char *description, size_t capacity) {
    struct hakios_gl *gl = hakios_gl_create(3);
    if (!gl) { snprintf(description,capacity,"context creation failed"); return 0; }
    if (!hakios_gl_make_current(gl)) { hakios_gl_destroy(gl); snprintf(description,capacity,"make-current failed"); return 0; }
    const char *renderer=(const char *)glGetString(0x1f01);
    int hardware=renderer && strstr(renderer,"FD650") && !strstr(renderer,"softpipe") && !strstr(renderer,"llvmpipe");
    unsigned char pixel[4]={0}; unsigned error=0;
    if (hardware) {
        unsigned attempts = 16;
        do { error = glGetError(); } while (error != 0 && --attempts != 0);
        if (error != 0) {
            snprintf(description,capacity,"renderer=%s persistent pre-draw GL error=%u after 16 checks",renderer,error);
            hakios_gl_make_current(NULL); hakios_gl_destroy(gl); return 0;
        }
        glClearColor(0.25f,0.5f,0.75f,1.f); glClear(0x4000); glFinish();
        glReadPixels(0,0,1,1,0x1908,0x1401,pixel); error=glGetError();
    }
    snprintf(description,capacity,"renderer=%s rgba=%u,%u,%u,%u error=%u",renderer?renderer:"NULL",pixel[0],pixel[1],pixel[2],pixel[3],error);
    int okay=hardware && error==0 && pixel[0]>=63 && pixel[0]<=65 && pixel[1]>=127 && pixel[1]<=129 && pixel[2]>=190 && pixel[2]<=192 && pixel[3]==255;
    hakios_gl_make_current(NULL); hakios_gl_destroy(gl); return okay;
}
