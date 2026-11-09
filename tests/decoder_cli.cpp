#include "frame_decoder.h"
#include <iostream>
#include <iterator>
#include <vector>
#ifdef _WIN32
#include <cstdio>
#include <fcntl.h>
#include <io.h>
#endif
static FrameDecoder decoder;
int main() {
#ifdef _WIN32
  _setmode(_fileno(stdin), _O_BINARY);  // Windows CRT text stdin otherwise strips CR/LF and treats 0x1A as EOF.
#endif
  std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(std::cin)), std::istreambuf_iterator<char>());
  if (!decoder.decode(bytes.data(), bytes.size())) { std::cout << 422; return 0; }
  std::cout << "200 " << decoder.pixels()[0] << " " << decoder.pixels()[4095];
}
