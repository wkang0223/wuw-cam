#pragma once
/* Which way up a stored photograph goes, without touching its pixels.
 *
 * The sensor delivers frames in its own orientation. When the camera is held so that the
 * picture is turned a quarter, the phone page and the TFT show it turned back (the VIEW
 * ROTATE setting). A saved photo needs the same turn or it opens sideways in the gallery.
 * Re-encoding a 5 MP JPEG on this chip is slow and loses quality, so the turn is recorded
 * the way every phone records it: as an EXIF Orientation tag, which Photos, Google Photos,
 * every browser and every desktop viewer apply on their own.
 *
 * Pure C++ with no Arduino dependency so it can be tested on a computer
 * (tools/jpeg_orient_test.cpp). */
#include <stddef.h>
#include <stdint.h>

#define JPEG_EXIF_BYTES 36   // marker 2 + length 2 + "Exif\0\0" 6 + TIFF header 8 + IFD 18

// Clockwise quarter turns (0..3) the picture needs when shown -> EXIF Orientation (1, 6, 3, 8).
inline uint8_t jpegOrientationForTurns(uint8_t quarterTurnsClockwise) {
  static const uint8_t table[4] = { 1, 6, 3, 8 };
  return table[quarterTurnsClockwise & 3];
}

// Writes the APP1 segment for `orientation` (1..8) into out and returns JPEG_EXIF_BYTES.
inline size_t jpegExifSegment(uint8_t orientation, uint8_t out[JPEG_EXIF_BYTES]) {
  static const uint8_t head[] = {
    0xFF, 0xE1, 0x00, 0x22,                    // APP1, length 34 (this field and everything after it)
    'E', 'x', 'i', 'f', 0, 0,
    'M', 'M', 0x00, 0x2A, 0x00, 0x00, 0x00, 0x08,   // big-endian TIFF, first IFD at offset 8
    0x00, 0x01,                                // one entry
    0x01, 0x12, 0x00, 0x03, 0x00, 0x00, 0x00, 0x01, // tag 0x0112 Orientation, SHORT, count 1
  };
  size_t n = 0;
  for (; n < sizeof(head); ++n) out[n] = head[n];
  out[n++] = 0x00; out[n++] = orientation;     // the value, left-justified in its 4 bytes
  out[n++] = 0x00; out[n++] = 0x00;
  out[n++] = 0x00; out[n++] = 0x00; out[n++] = 0x00; out[n++] = 0x00;   // no next IFD
  return n;                                    // == JPEG_EXIF_BYTES
}

// True when the stream is a JPEG that already carries an EXIF block before its image data.
inline bool jpegHasExif(const uint8_t* b, size_t n) {
  if (n < 4 || b[0] != 0xFF || b[1] != 0xD8) return false;
  size_t i = 2;
  while (i + 4 <= n && b[i] == 0xFF) {
    uint8_t marker = b[i + 1];
    if (marker == 0xDA || marker == 0xD9) return false;           // image data reached
    size_t len = ((size_t)b[i + 2] << 8) | b[i + 3];
    if (len < 2) return false;
    if (marker == 0xE1 && i + 10 <= n && b[i + 4] == 'E' && b[i + 5] == 'x' &&
        b[i + 6] == 'i' && b[i + 7] == 'f') return true;
    i += 2 + len;
  }
  return false;
}

/* Whether a photo needs the tag: the picture is turned, it is a real JPEG, and it does not
   already say how it is oriented (a stream with its own EXIF is left exactly as it is). */
inline bool jpegNeedsOrientation(const uint8_t* b, size_t n, uint8_t quarterTurnsClockwise) {
  return (quarterTurnsClockwise & 3) != 0 && n > 4 && b[0] == 0xFF && b[1] == 0xD8 &&
         !jpegHasExif(b, n);
}

/* Streams a photo through `put(ctx, bytes, count)` with the orientation tag spliced in right
   after the start-of-image marker. Returns the number of bytes handed to `put`, or 0 if a
   write failed. Works for an SD file, an HTTP chunk or a test buffer alike. */
typedef bool (*JpegPutFn)(void* ctx, const uint8_t* data, size_t count);
inline size_t jpegWriteOriented(JpegPutFn put, void* ctx, const uint8_t* b, size_t n,
                                uint8_t quarterTurnsClockwise) {
  if (!jpegNeedsOrientation(b, n, quarterTurnsClockwise)) {
    return put(ctx, b, n) ? n : 0;
  }
  uint8_t seg[JPEG_EXIF_BYTES];
  size_t segN = jpegExifSegment(jpegOrientationForTurns(quarterTurnsClockwise), seg);
  if (!put(ctx, b, 2) || !put(ctx, seg, segN) || !put(ctx, b + 2, n - 2)) return 0;
  return n + segN;
}
