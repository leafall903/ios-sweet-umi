
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>
typedef uint32_t GLuint; typedef uint32_t GLenum; typedef int32_t GLsizei; typedef void *IOSurfaceRef;
struct MockSurface { unsigned refs; };
static unsigned retains, releases, deletes; static bool fail_alloc;
static const void *CFRetain(void *p) { ((struct MockSurface*)p)->refs++;retains++;return p; }
static void CFRelease(void *p) { assert(p);struct MockSurface *s=p;assert(s->refs>1);s->refs--;releases++; }
static void glDeleteTextures(int n,const GLuint *p) { assert(n==1&&*p);deletes++; }
static void say(const char *p,...) { (void)p; }
static void *allocate_table(size_t n,size_t s) { if(fail_alloc){fail_alloc=false;return NULL;}return calloc(n,s); }
#define UMI_EAGL_IMAGE_CAPACITY 2048
struct image {
	GLuint texture;
	IOSurfaceRef surface;
	bool target;                    /* drawn into (attachImage) rather than shown (texImageIOSurface) */
	bool invert, drawn;
	bool shared;                    /* the texture is the surface's memory: nothing to copy */
	GLuint mirror;                  /* a texture that is the surface's memory, which the GPU copies the frame into (blit_targets) */
	bool wrapped;                   /* the surface is memory of somebody's with a surface made around it */
	bool alpha_first;               /* its pixels are opacity, blue, green, red in memory: see my_attachImage() */
	bool painted;                   /* something has been drawn into it (it is a target in fact, not only in name) */
	unsigned looked;                /* ... the frame in which that memory was last looked at, */
	uint64_t sum;                   /* ... and what it added up to */
	GLenum to, internal, format, type;
	uint32_t plane, seed;
	GLsizei width, height;
};

struct eagl {struct image *images;unsigned image_count;void *groups;};
#define calloc allocate_table
static struct image *
image_of(struct eagl *e, GLuint texture)
{
	for (unsigned i = 0; i < e->image_count; i++) {
		if (e->images[i].texture == texture) {
			return &e->images[i];
		}
	}
	return NULL;
}

static struct image *
image_new(struct eagl *e, GLuint texture, IOSurfaceRef surface)
{
	struct image *image = image_of(e, texture);

	if (image == NULL) {
        if (e->images == NULL) {
            e->images = calloc(UMI_EAGL_IMAGE_CAPACITY, sizeof(*e->images));
            if (e->images == NULL) {
                say("UMI GL v6: image table allocation failed\n");
                return NULL;
            }
            say("UMI GL v6: image table capacity=2048, stable heap storage\n");
        }
		if (e->image_count == UMI_EAGL_IMAGE_CAPACITY) {
			say("hakios-gl: more textures with IOSurfaces than there is room for\n");
			return NULL;
		}
		image = &e->images[e->image_count++];
	} else {
		if (image->mirror != 0) {
			glDeleteTextures(1, &image->mirror);
		}
		CFRelease(image->surface);
	}
	memset(image, 0, sizeof(*image));
	image->texture = texture;
	image->surface = (IOSurfaceRef)CFRetain(surface);
	return image;
}

static void
image_forget(struct eagl *e, GLuint texture)
{
	struct image *image = image_of(e, texture);

	if (image != NULL) {
		if (image->mirror != 0) {
			glDeleteTextures(1, &image->mirror);
		}
		CFRelease(image->surface);
		*image = e->images[--e->image_count];
	}
}

/* The GL context is destroyed first; release all remaining CF ownership. */
static void
image_dispose(struct eagl *e)
{
    for (unsigned i = 0; i < e->image_count; i++) CFRelease(e->images[i].surface);
    free(e->images);
    e->images = NULL;
    e->image_count = 0;
    free(e->groups);
    e->groups = NULL;
}


#undef calloc
int main(void) {
 struct MockSurface surfaces[1025];for(unsigned i=0;i<1025;i++)surfaces[i].refs=1;
 struct eagl e={0};fail_alloc=true;
 assert(!image_new(&e,1,&surfaces[0])&&!e.images&&!e.image_count&&surfaces[0].refs==1);
 for(unsigned i=0;i<1024;i++)assert(image_new(&e,i+1,&surfaces[i]));
 assert(e.image_count==1024&&retains==1024);
 struct image *stable=e.images;
 image_of(&e,5)->mirror=99;assert(image_new(&e,5,&surfaces[1024]));
 assert(e.image_count==1024&&surfaces[4].refs==1&&surfaces[1024].refs==2&&deletes==1);
 image_of(&e,10)->mirror=100;image_forget(&e,10);
 assert(!image_of(&e,10)&&e.image_count==1023&&surfaces[9].refs==1&&deletes==2);
 for(unsigned i=0;e.image_count<UMI_EAGL_IMAGE_CAPACITY;i++)assert(image_new(&e,2000+i,&surfaces[1024]));
 assert(e.images==stable&&e.image_count==UMI_EAGL_IMAGE_CAPACITY);
 unsigned r=retains;assert(!image_new(&e,99999,&surfaces[0])&&retains==r);
 image_forget(&e,99999);assert(e.image_count==UMI_EAGL_IMAGE_CAPACITY);
 e.groups=calloc(8192,84);assert(e.groups);image_dispose(&e);
 assert(!e.images&&!e.groups&&!e.image_count&&retains==releases);
 for(unsigned i=0;i<1025;i++)assert(surfaces[i].refs==1);
 image_dispose(&e);
 printf("PASS real image_of/new/forget/dispose:1024+ live entries,2048limit, allocationfailure, stableaddresses, replacement, deletion and dealloc; retains=%u releases=%u\n",retains,releases);
}
