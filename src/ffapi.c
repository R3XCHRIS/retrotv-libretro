#include "ffapi.h"

#include <dlfcn.h>
#include <stdio.h>
#include <string.h>

#define RETROTV_FF_DEFINE(lib, fn) __typeof__(&fn) p_##fn;
RETROTV_FF_FUNCS(RETROTV_FF_DEFINE)
#undef RETROTV_FF_DEFINE

/* Sonames must match the headers we compiled against (FFmpeg 7.1). */
static const struct { const char *name; const char *soname; } libs[] = {
  { "avutil",     "libavutil.so.59" },
  { "swresample", "libswresample.so.5" },
  { "swscale",    "libswscale.so.8" },
  { "avcodec",    "libavcodec.so.61" },
  { "avformat",   "libavformat.so.61" },
};
#define NLIBS (sizeof(libs) / sizeof(libs[0]))
static void *handles[NLIBS];

static void *handle_for(const char *name)
{
  for (size_t i = 0; i < NLIBS; i++)
    if (!strcmp(libs[i].name, name)) return handles[i];
  return NULL;
}

int ffapi_load(char *err, size_t errlen)
{
  for (size_t i = 0; i < NLIBS; i++) {
    if (handles[i]) continue;
    handles[i] = dlopen(libs[i].soname, RTLD_NOW | RTLD_LOCAL);
    if (!handles[i]) {
      snprintf(err, errlen, "cannot load %s: %s", libs[i].soname, dlerror());
      ffapi_unload();
      return -1;
    }
  }
#define RETROTV_FF_RESOLVE(lib, fn)                                         \
  p_##fn = (__typeof__(&fn))dlsym(handle_for(#lib), #fn);                   \
  if (!p_##fn) {                                                            \
    snprintf(err, errlen, "missing symbol %s in lib%s", #fn, #lib);         \
    ffapi_unload();                                                         \
    return -1;                                                              \
  }
  RETROTV_FF_FUNCS(RETROTV_FF_RESOLVE)
#undef RETROTV_FF_RESOLVE
  return 0;
}

void ffapi_unload(void)
{
  for (size_t i = NLIBS; i-- > 0;) {
    if (handles[i]) dlclose(handles[i]);
    handles[i] = NULL;
  }
}
