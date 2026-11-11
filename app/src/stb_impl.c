/*
 * stb_impl.c - the single-file libraries' implementations (stb, public
 * domain or MIT; see third_party/stb/LICENSE).
 */
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

#ifdef HOST_BUILD
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#endif
