// UDP forwarder for the cosmetic-sync channel. A thin, cosmetic-typed
// front-end over the shared RoomRelay engine (core/net/room_relay.h); see
// include/berry/cosmetic_relay.h for the design and the threat model.
#include "berry/cosmetic_relay.h"

#include "relay_wire.h"
#include "room_relay.h"

namespace Berry {

// The public header declares the wire sizes; the shared envelope defines them.
static_assert(CosmeticRelay::kVersion == relaywire::kVersion, "relay version");
static_assert(CosmeticRelay::kKindRegister == relaywire::kKindRegister, "reg");
static_assert(CosmeticRelay::kKindData == relaywire::kKindData, "data kind");
static_assert(CosmeticRelay::kTagBytes == relaywire::kTagBytes, "tag size");
static_assert(CosmeticRelay::kAuthBytes == relaywire::kAuthBytes, "auth size");
static_assert(CosmeticRelay::kEnvelopeBytes == relaywire::kEnvelopeBytes,
              "envelope size");
static_assert(CosmeticRelay::kMinSecretBytes == relaywire::kMinSecretBytes,
              "min secret");

const char* CosmeticRelay::Magic() {
  static char m[5] = {relaywire::kMagic[0], relaywire::kMagic[1],
                      relaywire::kMagic[2], relaywire::kMagic[3], '\0'};
  return m;
}

std::string CosmeticRelay::RoomTagHex(const std::string& name,
                                      const std::string& secret) {
  return RoomRelay::RoomTagHex(name, secret);
}

std::string CosmeticRelay::RegisterAuthHex(const std::string& tagHex,
                                           const std::string& secret) {
  return RoomRelay::RegisterAuthHex(tagHex, secret);
}

CosmeticRelay::CosmeticRelay() : relay_(std::make_unique<RoomRelay>()) {}
CosmeticRelay::~CosmeticRelay() = default;

bool CosmeticRelay::AddRoom(const std::string& name, const std::string& secret) {
  return relay_->AddRoom(name, secret);
}

size_t CosmeticRelay::roomCount() const { return relay_->roomCount(); }

bool CosmeticRelay::Open(const CosmeticRelayConfig& cfg) {
  RoomRelayConfig rc;
  rc.port = cfg.port;
  rc.bindAddr = cfg.bindAddr;
  rc.mtu = cfg.mtu;
  rc.peerTtlMs = cfg.peerTtlMs;
  rc.maxPeersPerRoom = cfg.maxPeersPerRoom;
  rc.maxRooms = cfg.maxRooms;
  return relay_->Open(rc);
}

void CosmeticRelay::Close() { relay_->Close(); }
bool CosmeticRelay::open() const { return relay_->open(); }
u16 CosmeticRelay::localPort() const { return relay_->localPort(); }
size_t CosmeticRelay::Pump(int budgetMs) { return relay_->Pump(budgetMs); }
bool CosmeticRelay::Start() { return relay_->Start(); }
void CosmeticRelay::Stop() { relay_->Stop(); }

CosmeticRelay::Stats CosmeticRelay::stats() const {
  const RoomRelay::Stats s = relay_->stats();
  Stats out;
  out.datagramsReceived = s.datagramsReceived;
  out.datagramsForwarded = s.datagramsForwarded;
  out.registrations = s.registrations;
  out.datagramsDropped = s.datagramsDropped;
  out.peersEvicted = s.peersEvicted;
  return out;
}

size_t CosmeticRelay::peerCount() const { return relay_->peerCount(); }

}  // namespace Berry
