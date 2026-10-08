// Host test for the EXIF orientation splice.
//   g++ -std=c++17 -Wall -Wextra -I. tools/jpeg_orient_test.cpp -o /tmp/jpeg_orient_test && /tmp/jpeg_orient_test <some.jpg> <out_prefix>
// With a real JPEG it writes <out_prefix>0..3.jpg so the result can be inspected with any viewer or ffprobe.
#include <cstdio>
#include <cstring>
#include <vector>

#include "jpeg_orient.h"

static int failures = 0;
#define CHECK(cond, what) do { bool ok_ = (cond); std::printf("%s  %s\n", ok_ ? "PASS" : "FAIL", what); if (!ok_) ++failures; } while (0)

static bool sink(void* ctx, const uint8_t* d, size_t n) {
  auto* v = static_cast<std::vector<uint8_t>*>(ctx);
  v->insert(v->end(), d, d + n);
  return true;
}
static bool failing(void*, const uint8_t*, size_t) { return false; }

int main(int argc, char** argv) {
  CHECK(jpegOrientationForTurns(0) == 1 && jpegOrientationForTurns(1) == 6 &&
        jpegOrientationForTurns(2) == 3 && jpegOrientationForTurns(3) == 8, "turns -> orientation 1,6,3,8");
  uint8_t seg[JPEG_EXIF_BYTES];
  CHECK(jpegExifSegment(6, seg) == JPEG_EXIF_BYTES, "segment is 36 bytes");
  CHECK(seg[0] == 0xFF && seg[1] == 0xE1 && seg[2] == 0 && seg[3] == 34, "APP1 marker and length 34 (covers the segment minus its marker)");
  CHECK(std::memcmp(seg + 4, "Exif\0\0", 6) == 0, "Exif header");
  CHECK(seg[28] == 0x00 && seg[29] == 6, "orientation value sits in the IFD entry");

  const uint8_t tiny[] = { 0xFF, 0xD8, 0xFF, 0xDB, 0x00, 0x04, 0x00, 0x00, 0xFF, 0xDA, 0x00, 0x02, 0x11, 0xFF, 0xD9 };
  std::vector<uint8_t> out;
  size_t wrote = jpegWriteOriented(sink, &out, tiny, sizeof(tiny), 1);
  CHECK(wrote == sizeof(tiny) + JPEG_EXIF_BYTES && out.size() == wrote, "a turned photo grows by exactly the segment");
  CHECK(out[0] == 0xFF && out[1] == 0xD8 && out[2] == 0xFF && out[3] == 0xE1, "SOI then APP1");
  CHECK(std::memcmp(out.data() + 2 + JPEG_EXIF_BYTES, tiny + 2, sizeof(tiny) - 2) == 0, "the rest of the file is untouched");
  CHECK(jpegHasExif(out.data(), out.size()), "result is recognised as having EXIF");

  out.clear();
  wrote = jpegWriteOriented(sink, &out, tiny, sizeof(tiny), 0);
  CHECK(wrote == sizeof(tiny) && out.size() == sizeof(tiny) && std::memcmp(out.data(), tiny, sizeof(tiny)) == 0, "no turn: bytes pass through unchanged");

  std::vector<uint8_t> twice;
  jpegWriteOriented(sink, &twice, tiny, sizeof(tiny), 3);
  std::vector<uint8_t> again;
  wrote = jpegWriteOriented(sink, &again, twice.data(), twice.size(), 1);
  CHECK(wrote == twice.size() && again == twice, "a photo that already has EXIF is never tagged twice");

  const uint8_t notJpeg[] = { 'P', 'N', 'G', 0, 0, 0 };
  out.clear();
  wrote = jpegWriteOriented(sink, &out, notJpeg, sizeof(notJpeg), 1);
  CHECK(wrote == sizeof(notJpeg) && out.size() == sizeof(notJpeg), "something that is not a JPEG passes through");
  CHECK(jpegWriteOriented(failing, nullptr, tiny, sizeof(tiny), 1) == 0, "a failed write reports 0");

  if (argc >= 3) {
    FILE* f = std::fopen(argv[1], "rb");
    if (f) {
      std::vector<uint8_t> in;
      uint8_t buf[4096]; size_t n;
      while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) in.insert(in.end(), buf, buf + n);
      std::fclose(f);
      for (uint8_t q = 0; q < 4; ++q) {
        std::vector<uint8_t> o;
        jpegWriteOriented(sink, &o, in.data(), in.size(), q);
        char name[256]; std::snprintf(name, sizeof(name), "%s%u.jpg", argv[2], q);
        FILE* g = std::fopen(name, "wb"); std::fwrite(o.data(), 1, o.size(), g); std::fclose(g);
        std::printf("wrote %s (%zu bytes)\n", name, o.size());
      }
    }
  }
  std::printf("%d failure(s)\n", failures);
  return failures ? 1 : 0;
}
