#include "receiver_protocol.h"
#include <iostream>
#include <iterator>
#ifdef _WIN32
#include <cstdio>
#include <fcntl.h>
#include <io.h>
#endif
int main(int argc, char **argv) {
#ifdef _WIN32
  _setmode(_fileno(stdin), _O_BINARY);  // Windows CRT text stdin otherwise strips CR/LF and treats 0x1A as EOF.
#endif
  const std::string input((std::istreambuf_iterator<char>(std::cin)), std::istreambuf_iterator<char>());
  if (argc == 2 && std::string(argv[1]) == "png") {
    std::cout << (receiver::validPngEnvelope(reinterpret_cast<const uint8_t *>(input.data()), input.size()) ? 200 : 422);
  } else {
    receiver::Request request;
    std::cout << receiver::parseHeaders(input, request);
  }
}
