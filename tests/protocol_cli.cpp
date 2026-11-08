#include "receiver_protocol.h"
#include <iostream>
#include <iterator>
int main(int argc, char **argv) {
  const std::string input((std::istreambuf_iterator<char>(std::cin)), std::istreambuf_iterator<char>());
  if (argc == 2 && std::string(argv[1]) == "png") {
    std::cout << (receiver::validPngEnvelope(reinterpret_cast<const uint8_t *>(input.data()), input.size()) ? 200 : 422);
  } else {
    receiver::Request request;
    std::cout << receiver::parseHeaders(input, request);
  }
}
