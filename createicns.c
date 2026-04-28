// -*- Mode: c; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*-
//
// Copyright (c) 2017, Arjan van Leeuwen
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// * Redistributions of source code must retain the above copyright notice, this
//   list of conditions and the following disclaimer.
//
// * Redistributions in binary form must reproduce the above copyright notice,
//   this list of conditions and the following disclaimer in the documentation
//   and/or other materials provided with the distribution.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.

// This is a generator for the Apple Icon Image Format (.icns) which reads PNGs
// without changing them.
//
// It's run like this: 'createicons x.iconset' and outputs a file 'x.icns'.
//
// The input is a .iconset directory with files conforming to the naming scheme
// for .iconset directories. It reads a 'complete' set of PNG icons as described
// here:
// https://developer.apple.com/library/content/documentation/GraphicsAnimation/Conceptual/HighResolutionOSX/Optimizing/Optimizing.html
//
// To generate a .iconset directory from an existing x.icns file, use
//   'iconutil -c iconset x.icns'
//
// This tool is similar to running 'iconutil -c icns x.iconset', except it
// doesn't change the PNG images in any way.

#include <arpa/inet.h>
#include <dirent.h>
#include <libgen.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include "stb_image.h"

// Magic values for headers were found at
// https://en.wikipedia.org/wiki/Apple_Icon_Image_format.

enum kBufferSize { kBufferSize = 1024 };
static const char kIconsetExtension[] = ".iconset";
static const char kIcnsExtension[] = ".icns";
static const char kUnknownFormatFilename[] = "icon_data_";
static const uint32_t kMagicHeader = 'icns';
static const uint32_t kArgbMagic = 'ARGB';

// 16x16 and 32x32 are stored as ic04/ic05 (ARGB with per-channel PackBits RLE),
// not as icp4/icp5 PNGs. macOS's small-icon non-retina rendering path expects
// the ARGB form; PNGs in icp4/icp5 render garbled. Apple's iconutil emits the
// same way, and Preview/iconutil happen to read either form so the breakage
// only shows up in Finder/Dock at 1x backing scale.
struct {
  const char* icon_filename;
  uint32_t icon_type;
} static const kIconTypes[] = {
    {"icon_16x16.png", 'ic04'},
    {"icon_16x16@2x.png", 'ic11'},
    {"icon_32x32.png", 'ic05'},
    {"icon_32x32@2x.png", 'ic12'},
    {"icon_64x64.png", 'icp6'},
    {"icon_128x128.png", 'ic07'},
    {"icon_128x128@2x.png", 'ic13'},
    {"icon_256x256.png", 'ic08'},
    {"icon_256x256@2x.png", 'ic14'},
    {"icon_512x512.png", 'ic09'},
    {"icon_512x512@2x.png", 'ic10'}
};

static bool IsArgbType(uint32_t icon_type) {
  return icon_type == 'ic04' || icon_type == 'ic05';
}

static int ArgbSideForType(uint32_t icon_type) {
  return icon_type == 'ic04' ? 16 : 32;
}

void PrintError(const char* error) {
  fprintf(stderr, "Error: %s\n", error);
}

void PrintSystemError() {
  perror("Error");
}

void PrintUsage(const char* own_path) {
  fprintf(stderr, "Usage: %s [iconset] [outfile]\n", own_path);
}

char* Basename(const char* path, char* basename) {
  if (!path || *path == '\0') {
    strlcpy(basename, ".", sizeof("."));
    return basename;
  }

  size_t length = strlen(path);
  const char* end;
  for (end = path + length - 1; end > path && *end == '/'; end--) {}
  const char* begin;
  for (begin = end; begin > path && *(begin - 1) != '/'; begin--) {}

  size_t base_length = (end - begin) + 1;
  if (base_length >= MAXPATHLEN)
    return NULL;

  strlcpy(basename, begin, base_length + 1);
  return basename;
}

bool WriteUint32(uint32_t to_write, FILE* file) {
  uint32_t msb_first = htonl(to_write);
  return fwrite(&msb_first, sizeof(msb_first), 1, file) == 1;
}

FILE* OpenIcnsFileForIconset(const char* iconset_path, const char* outfile_path) {
  char path[MAXPATHLEN];
  FILE* file;

  if (!outfile_path) {
    if (!Basename(iconset_path, path)) {
      PrintError("Can't determine basename for iconset");
      return NULL;
    }

    const size_t path_length = strlen(path);
    const size_t extension_length = sizeof(kIconsetExtension) - 1;
    const size_t base_path_length = path_length - extension_length;

    if (path_length <= extension_length ||
        strncmp(path + base_path_length, kIconsetExtension, extension_length) !=
            0) {
      PrintError("Need .iconset directory as input.");
      return NULL;
    }
    memcpy(path + base_path_length, kIcnsExtension, sizeof(kIcnsExtension));
    file = fopen(path, "w");
  } else {
    file = fopen(outfile_path, "w");
  }

  if (!file) {
    PrintSystemError();
    return NULL;
  }

  // Every .icns file starts with a magic header (4 bytes) and the total size
  // including the header (4 bytes). Since we don't know the size yet, we'll
  // overwrite this in WriteIcnsFileMetadata() with the real value.
  if (!WriteUint32(kMagicHeader, file) ||
      !WriteUint32(0, file)) {
    PrintSystemError();
    fclose(file);
    return NULL;
  }

  return file;
}

uint32_t FindIconType(const char* icon_filename) {
  if (strncmp(icon_filename, kUnknownFormatFilename,
              sizeof(kUnknownFormatFilename) - 1) == 0) {
    const char* format = icon_filename + sizeof(kUnknownFormatFilename) - 1;
    if (strlen(format) != 4)
      return 0;

    return (format[0] << 24) | (format[1] << 16) | (format[2] << 8) | format[3];
  }

  for (size_t i = 0; i < (sizeof(kIconTypes) / sizeof(*kIconTypes)); i++) {
    if (strcmp(kIconTypes[i].icon_filename, icon_filename) == 0)
      return kIconTypes[i].icon_type;
  }

  return 0;
}

// PackBits-style RLE used by the icns ARGB and is32/il32 chunks. Control bytes
// in 0x00..0x7F mean "the next n+1 bytes are literals" (1..128). Control bytes
// in 0x80..0xFF mean "the next byte repeats n-125 times" (3..130). Returns the
// number of bytes written to `out`, or 0 on overflow.
size_t RleEncodeChannel(const uint8_t* in, size_t n, uint8_t* out, size_t cap) {
  size_t i = 0, j = 0;
  while (i < n) {
    size_t run = 1;
    while (run < 130 && i + run < n && in[i + run] == in[i])
      run++;

    if (run >= 3) {
      if (j + 2 > cap)
        return 0;
      out[j++] = (uint8_t)(run + 125);
      out[j++] = in[i];
      i += run;
    } else {
      size_t lit_end = i + 1;
      while (lit_end - i < 128 && lit_end < n) {
        if (lit_end + 2 < n && in[lit_end] == in[lit_end + 1] &&
            in[lit_end + 1] == in[lit_end + 2])
          break;
        lit_end++;
      }
      size_t lit = lit_end - i;
      if (j + 1 + lit > cap)
        return 0;
      out[j++] = (uint8_t)(lit - 1);
      memcpy(out + j, in + i, lit);
      j += lit;
      i = lit_end;
    }
  }
  return j;
}

bool WriteArgbIcon(const char* iconset_path, const char* icon_filename,
                   uint32_t icon_type, FILE* outfile) {
  const size_t iconset_path_length = strlen(iconset_path);
  const size_t icon_filename_length = strlen(icon_filename);
  char* icon_path = malloc(iconset_path_length + icon_filename_length + 2);
  if (!icon_path) {
    PrintError("Out of memory");
    return false;
  }
  memcpy(icon_path, iconset_path, iconset_path_length);
  icon_path[iconset_path_length] = '/';
  memcpy(icon_path + iconset_path_length + 1, icon_filename,
         icon_filename_length + 1);

  int width = 0, height = 0, src_channels = 0;
  uint8_t* pixels = stbi_load(icon_path, &width, &height, &src_channels, 4);
  free(icon_path);
  if (!pixels) {
    fprintf(stderr, "Error: Can't decode %s: %s\n", icon_filename,
            stbi_failure_reason());
    return false;
  }

  const int expected = ArgbSideForType(icon_type);
  if (width != expected || height != expected) {
    fprintf(stderr, "Error: %s is %dx%d, expected %dx%d\n", icon_filename,
            width, height, expected, expected);
    stbi_image_free(pixels);
    return false;
  }

  const size_t pixel_count = (size_t)width * height;
  uint8_t* channels = malloc(pixel_count * 4);
  if (!channels) {
    PrintError("Out of memory");
    stbi_image_free(pixels);
    return false;
  }
  uint8_t* a = channels;
  uint8_t* r = channels + pixel_count;
  uint8_t* g = channels + pixel_count * 2;
  uint8_t* b = channels + pixel_count * 3;
  for (size_t k = 0; k < pixel_count; k++) {
    r[k] = pixels[k * 4 + 0];
    g[k] = pixels[k * 4 + 1];
    b[k] = pixels[k * 4 + 2];
    a[k] = pixels[k * 4 + 3];
  }
  stbi_image_free(pixels);

  // RLE worst case is roughly n + ceil(n/128); 2n + 16 is comfortably above.
  const size_t cap = pixel_count * 2 + 16;
  uint8_t* enc = malloc(cap * 4);
  if (!enc) {
    PrintError("Out of memory");
    free(channels);
    return false;
  }
  size_t lengths[4] = {0};
  uint8_t* slots[4] = {a, r, g, b};
  size_t total = 0;
  for (int c = 0; c < 4; c++) {
    lengths[c] = RleEncodeChannel(slots[c], pixel_count, enc + total, cap);
    if (lengths[c] == 0) {
      PrintError("RLE encode overflow");
      free(channels);
      free(enc);
      return false;
    }
    total += lengths[c];
  }
  free(channels);

  // Chunk: type (4) + total_size (4) + ARGB magic (4) + four RLE channels.
  const uint32_t chunk_size = (uint32_t)(8 + 4 + total);
  bool ok = WriteUint32(icon_type, outfile) &&
            WriteUint32(chunk_size, outfile) &&
            WriteUint32(kArgbMagic, outfile) &&
            fwrite(enc, 1, total, outfile) == total;
  if (!ok)
    PrintSystemError();

  free(enc);
  return ok;
}

bool WriteIconToFile(const char *iconset_path, const char *icon_filename,
                     uint32_t icon_type, FILE *outfile) {
  const size_t iconset_path_length = strlen(iconset_path);
  const size_t icon_filename_length = strlen(icon_filename);
  char* icon_path = malloc(iconset_path_length + icon_filename_length + 2);

  memcpy(icon_path, iconset_path, iconset_path_length);
  icon_path[iconset_path_length] = '/';
  memcpy(icon_path + iconset_path_length + 1, icon_filename,
         icon_filename_length + 1);

  FILE* infile = fopen(icon_path, "r");
  free(icon_path);
  if (!infile) {
    PrintSystemError();
    return false;
  }

  // For every icon, we put a magic header (4 bytes) and the total size of the
  // icon following including the header (4 bytes), followed by the icon
  // itself.
  long size = 0;
  if (fseek(infile, 0L, SEEK_END) < 0 ||
      (size = ftell(infile)) < 0 ||
      fseek(infile, 0L, SEEK_SET) < 0) {
    PrintSystemError();
    fclose(infile);
    return false;
  }

  if (!WriteUint32(icon_type, outfile) ||
      !WriteUint32(size + 8, outfile)) {
    PrintSystemError();
    fclose(infile);
    return false;
  }

  uint8_t buffer[kBufferSize];
  size_t read;
  do {
    read = fread(buffer, 1, kBufferSize, infile);
    if ((read < kBufferSize && ferror(infile)) ||
        fwrite(buffer, 1, read, outfile) < read) {
      PrintSystemError();
      fclose(infile);
      return false;
    }
  } while (read == kBufferSize);

  fclose(infile);
  return true;
}

bool WriteIcnsFileMetadata(FILE* file) {
  long size = ftell(file);
  return size >= 0 && fseek(file, 4L, SEEK_SET) == 0 && WriteUint32(size, file);
}

bool CreateIcnsFromIconset(const char* iconset_path, const char* outfile_path) {
  DIR* iconset = opendir(iconset_path);
  if (!iconset) {
    PrintSystemError();
    return false;
  }

  FILE* icns = OpenIcnsFileForIconset(iconset_path, outfile_path);
  if (!icns)
    return false;

  for (struct dirent *entry = readdir(iconset); entry;
       entry = readdir(iconset)) {
    if (entry->d_name[0] == '.')
      continue;

    uint32_t icon_type = FindIconType(entry->d_name);
    if (!icon_type) {
      fprintf(stderr, "Warning: Don't know icon type for %s, skipping\n",
              entry->d_name);
      continue;
    }

    // icon_data_xxxx entries are raw chunk payloads — pass them through
    // verbatim even when the type is one we'd normally synthesize (e.g.
    // icon_data_ic04 from a readicns roundtrip is already ARGB-RLE).
    const bool is_raw = strncmp(entry->d_name, kUnknownFormatFilename,
                                sizeof(kUnknownFormatFilename) - 1) == 0;
    bool wrote = (!is_raw && IsArgbType(icon_type))
        ? WriteArgbIcon(iconset_path, entry->d_name, icon_type, icns)
        : WriteIconToFile(iconset_path, entry->d_name, icon_type, icns);
    if (!wrote) {
      fclose(icns);
      return false;
    }
  }

  if (!WriteIcnsFileMetadata(icns)) {
    fclose(icns);
    return false;
  }

  fclose(icns);
  return true;
}

int main(int argc, char* argv[]) {
  const char *iconset_path = NULL;
  const char *outfile_path = NULL;

  if (argc < 2) {
    PrintError("No path given to iconset directory.");
    PrintUsage(argv[0]);
    return -1;
  }
  if (argc > 3) {
    PrintError("Too many arguments.");
    PrintUsage(argv[0]);
    return -1;
  }
  if (argc == 2) {
    iconset_path = argv[1];
    outfile_path = NULL;
  } else if (argc == 3) {
    iconset_path = argv[1];
    outfile_path = argv[2];
  }

  if (!CreateIcnsFromIconset(iconset_path, outfile_path)) {
    fprintf(stderr, "Failed to create .icns file from %s\n", iconset_path);
    return -1;
  }

  return 0;
}
