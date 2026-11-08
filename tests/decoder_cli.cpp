#include "frame_decoder.h"
#include <iostream>
#include <iterator>
#include <vector>
static FrameDecoder decoder;
int main() {
  std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(std::cin)), std::istreambuf_iterator<char>());
  if (!decoder.decode(bytes.data(), bytes.size())) { std::cout << 422; return 0; }
  std::cout << "200 " << decoder.pixels()[0] << " " << decoder.pixels()[4095];
}
