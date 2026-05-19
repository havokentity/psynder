// SPDX-License-Identifier: MIT
// Single-TU vendor implementation for stb_image / stb_image_write. Lane 24 /
// tools — only used by lm_cook for PNG codec. We disable the implementations
// we don't need (HDR, GIF, PIC, PNM, PSD, TGA, JPEG decoder is kept because
// it's tiny and useful for the cooker to also accept JPEG textures down the
// road; trim if size becomes an issue).

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_HDR
#define STBI_NO_GIF
#define STBI_NO_PIC
#define STBI_NO_PNM
#define STBI_NO_PSD
#define STBI_NO_TGA
#define STBI_NO_THREAD_LOCALS
#include "stb_image.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include "stb_image_write.h"
