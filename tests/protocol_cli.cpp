#include "receiver_protocol.h"
#include "idle_clock.h"
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
  } else if (argc == 2 && std::string(argv[1]) == "state") {
    receiver::Request request;
    const int status = receiver::parseHeaders(input, request);
    std::cout << status << (request.clockState ? " clock" : " avatar");
  } else if (argc == 2 && std::string(argv[1]) == "session") {
    receiver::Request request;
    const int status = receiver::parseHeaders(input, request);
    std::cout << status << ' ' << receiver::kindName(request) << ' '
              << (request.hasSession ? 1 : 0) << ' '
              << (request.hasSession ? request.sessionId : "-") << '\n';
  } else if (argc == 2 && std::string(argv[1]) == "store") {
    // Sequential session-title commit simulation: requests separated by a
    // \n----\n line share one idle_clock::State in this process. Each line is
    // "<status> <title or ->" plus " invalid-name" when the request carried
    // the distinct project-name rejection. Only a 200 image commits or clears
    // the stored title; only a 200 control refreshes its liveness.
    idle_clock::State state;
    const std::string separator = "\n----\n";
    size_t start = 0;
    while (start < input.size()) {
      const size_t next = input.find(separator, start);
      const std::string part = input.substr(
          start, next == std::string::npos ? std::string::npos : next - start);
      receiver::Request request;
      const int status = receiver::parseHeaders(part, request);
      if (status == 200) {
        if (request.kind == receiver::Request::Kind::Image)
          state.commitTitle(1000, request.hasName, request.projectName);
        else
          state.touchTitle(1000);
      }
      const char *title = state.sessionTitle(1000);
      std::cout << status << ' ' << (*title ? title : "-")
                << (request.hasName && !request.nameValid ? " invalid-name" : "")
                << '\n';
      if (next == std::string::npos) break;
      start = next + separator.size();
    }
  } else {
    receiver::Request request;
    std::cout << receiver::parseHeaders(input, request);
  }
}
