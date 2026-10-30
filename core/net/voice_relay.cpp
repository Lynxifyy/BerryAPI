// UDP forwarder for the proximity voice channel. A thin, voice-typed front-end
// over the shared RoomRelay engine (core/net/room_relay.h); see
// include/berry/voice_relay.h for the envelope and the threat model.
#include "berry/voice_relay.h"

#include "relay_wire.h"
#include "room_relay.h"

namespace Berry {

// The public header declares the wire sizes; the shared envelope defines them.
// Assert they agree, so a change to one without the other fails to build rather
// than putting two incompatible envelopes on the wire.
static_assert(VoiceRelay::kVersion == relaywire::kVersion, "relay version");
static_assert(VoiceRelay::kKindRegister == relaywire::kKindRegister, "reg kind");
static_assert(VoiceRelay::kKindData == relaywire::kKindData, "data kind");
static_assert(VoiceRelay::kTagBytes == relaywire::kTagBytes, "tag size");
static_assert(VoiceRelay::kAuthBytes == relaywire::kAuthBytes, "auth size");
static_assert(VoiceRelay::kEnvelopeBytes == relaywire::kEnvelopeBytes,
              "envelope size");
static_assert(VoiceRelay::kMinSecretBytes == relaywire::kMinSecretBytes,
              "min secret");

const char* VoiceRelay::Magic() {
  static char m[5] = {relaywire::kMagic[0], relaywire::kMagic[1],
                      relaywire::kMagic[2], relaywire::kMagic[3], '\0'};
  return m;
}

std::string VoiceRelay::RoomTagHex(const std::string& name,
                                   const std::string& secret) {
  return RoomRelay::RoomTagHex(name, secret);
}

std::string VoiceRelay::RegisterAuthHex(const std::string& tagHex,
                                        const std::string& secret) {
  return RoomRelay::RegisterAuthHex(tagHex, secret);
}

VoiceRelay::VoiceRelay() : relay_(std::make_unique<RoomRelay>()) {}
VoiceRelay::~VoiceRelay() = default;

bool VoiceRelay::AddRoom(const std::string& name, const std::string& secret) {
  return relay_->AddRoom(name, secret);
}

size_t VoiceRelay::roomCount() const { return relay_->roomCount(); }

bool VoiceRelay::Open(const VoiceRelayConfig& cfg) {
  RoomRelayConfig rc;
  rc.port = cfg.port;
  rc.bindAddr = cfg.bindAddr;
  rc.mtu = cfg.mtu;
  rc.peerTtlMs = cfg.peerTtlMs;
  rc.maxPeersPerRoom = cfg.maxPeersPerRoom;
  rc.maxRooms = cfg.maxRooms;
  return relay_->Open(rc);
}

void VoiceRelay::Close() { relay_->Close(); }
bool VoiceRelay::open() const { return relay_->open(); }
u16 VoiceRelay::localPort() const { return relay_->localPort(); }
size_t VoiceRelay::Pump(int budgetMs) { return relay_->Pump(budgetMs); }
bool VoiceRelay::Start() { return relay_->Start(); }
void VoiceRelay::Stop() { relay_->Stop(); }

VoiceRelay::Stats VoiceRelay::stats() const {
  const RoomRelay::Stats s = relay_->stats();
  Stats out;
  out.datagramsReceived = s.datagramsReceived;
  out.datagramsForwarded = s.datagramsForwarded;
  out.registrations = s.registrations;
  out.datagramsDropped = s.datagramsDropped;
  out.peersEvicted = s.peersEvicted;
  return out;
}

size_t VoiceRelay::peerCount() const { return relay_->peerCount(); }

}  // namespace Berry
