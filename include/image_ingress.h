#pragma once
#include <cstdint>
#include "idle_clock.h"
#include "serial_protocol.h"
#include "session_owner.h"

// Shared image-ingress decisions (T2): the exact admission, control and
// commit sequence the HTTP path performs in pollHttp, factored so the serial
// ingress and future callers reuse the same Owner and title-liveness policy
// instead of re-implementing it. Pure functions over the existing state
// objects; nothing here mutates anything except through those objects.
namespace image_ingress {

// Admission for an image frame, mirroring pollHttp: the whole payload has
// already been received and checksum-validated when this runs; a conflict
// answers Busy and changes no state.
inline serial::Code admitImage(const serial::Frame &frame, session::Owner &owner,
                               uint32_t now) {
  return owner.pngAccess(frame.hasSession, frame.sessionId, now) == 200
             ? serial::Code::Ok
             : serial::Code::Busy;
}

// Lifecycle controls call the identical session::Owner methods as HTTP and
// refresh the session-title liveness window exactly on success. The name
// field is never consulted here, matching the desktop lifecycle endpoints.
inline serial::Code admitControl(const serial::Frame &frame, session::Owner &owner,
                                 idle_clock::State &clock, uint32_t now) {
  int control;
  switch (frame.op) {
    case serial::Op::Claim: control = owner.claim(frame.sessionId, now); break;
    case serial::Op::Heartbeat: control = owner.heartbeat(frame.sessionId, now); break;
    default: control = owner.release(frame.sessionId, now); break;
  }
  if (control == 200) {
    clock.touchTitle(now);
    return serial::Code::Ok;
  }
  return control == 409 ? serial::Code::Busy : serial::Code::Session;
}

// Called only after a successful presentation (PNG decode + panel update):
// lease renewal and title commit, identical to the HTTP image path. An image
// without a name clears the stored title.
inline void commitImage(session::Owner &owner, idle_clock::State &clock,
                        const serial::Frame &frame, uint32_t now) {
  owner.renewPng(now);
  clock.commitTitle(now, frame.hasName, frame.projectName);
}
} // namespace image_ingress
