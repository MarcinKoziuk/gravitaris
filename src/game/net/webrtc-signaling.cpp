#include <gravitaris/game/net/webrtc-signaling.hpp>

namespace Gravitaris {

namespace {

std::string Encode(const char* kind, const std::string& a, const std::string& b)
{
    std::string out(kind);
    out += '\n';
    out += a;
    out += '\n';
    out += b;
    return out;
}

} // namespace

std::string EncodeDescriptionFrame(const std::string& sdp, const std::string& type)
{
    return Encode("desc", type, sdp);
}

std::string EncodeCandidateFrame(const std::string& candidate, const std::string& mid)
{
    return Encode("cand", mid, candidate);
}

std::optional<SignalingFrame> DecodeSignalingFrame(const std::string& frame)
{
    const std::size_t firstNewline = frame.find('\n');
    if (firstNewline == std::string::npos) return std::nullopt;
    const std::size_t secondNewline = frame.find('\n', firstNewline + 1);
    if (secondNewline == std::string::npos) return std::nullopt;

    const std::string kind = frame.substr(0, firstNewline);
    SignalingFrame result;
    result.a = frame.substr(firstNewline + 1, secondNewline - firstNewline - 1);
    result.b = frame.substr(secondNewline + 1);

    if (kind == "desc") {
        result.kind = SignalingFrame::Kind::Description;
    }
    else if (kind == "cand") {
        result.kind = SignalingFrame::Kind::Candidate;
    }
    else {
        return std::nullopt;
    }
    return result;
}

} // namespace Gravitaris
