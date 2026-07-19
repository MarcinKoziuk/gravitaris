#pragma once

#include <optional>
#include <string>

namespace Gravitaris {

// Wire format for the signaling channel WebRtcTransport/WebRtcServerTransport
// bootstrap a data channel over (docs/networking-plan.md 3.5.1): a WebSocket
// carries only these small text frames (SDP offer/answer, ICE candidates),
// never game traffic. Shared between the client and server sides so both
// encode/decode the same way.
//
// "desc\n<type>\n<sdp>"      -- sdp may itself contain newlines; it's
//                               everything after the second '\n'.
// "cand\n<mid>\n<candidate>"
struct SignalingFrame {
    enum class Kind { Description, Candidate };
    Kind kind;
    std::string a; // type (Description) or mid (Candidate)
    std::string b; // sdp (Description) or candidate string (Candidate)
};

std::string EncodeDescriptionFrame(const std::string& sdp, const std::string& type);
std::string EncodeCandidateFrame(const std::string& candidate, const std::string& mid);
std::optional<SignalingFrame> DecodeSignalingFrame(const std::string& frame);

} // namespace Gravitaris
