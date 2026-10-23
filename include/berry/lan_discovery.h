#pragma once
// mDNS discovery for the LAN cosmetic transport.
//
// The transport reaches peers by sending to a fixed multicast group, but a
// client still needs to learn who is on the subnet and on what port. This is a
// small mDNS (`_berry._udp.local`) responder and browser: it publishes the
// local client as a service (PTR/SRV/TXT/A) and browses for peers, caching what
// it hears with a TTL. It is deliberately minimal: the records this service
// needs, unicast-free (mDNS multicast only), and no compression on send (the
// decoder still follows pointers, because other responders compress).
//
// This is discovery only. It carries no cosmetic data and no trust: a peer
// found here is still verified by the sync channel before anything it says is
// applied.

#include "berry/export.h"
#include "berry/types.h"
#include "berry/net.h"

#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace Berry {

struct BERRY_API LanDiscoveryConfig {
  /// The service type browsed and published. `_berry._udp.local` per the spec.
  std::string serviceType = "_berry._udp.local";
  /// The mDNS multicast group and port (the standard ones).
  std::string group = "224.0.0.251";
  u16 port = 5353;
  /// The interface to join and send on. The default uses the system's multicast
  /// route; a host test sets "127.0.0.1" to stay on loopback.
  std::string iface = "0.0.0.0";
  /// TTL, in seconds, put on the records this client publishes.
  u32 ttlSeconds = 120;
  /// How long a discovered service stays listed after its last record.
  u64 cacheTtlMs = 120000;
};

/// A peer discovered over mDNS.
struct BERRY_API LanService {
  std::string instance;   // the full instance name, e.g. "alice._berry._udp.local"
  std::string host;       // the SRV target host
  std::string address;    // the IPv4 address from the host's A record ("" if unseen)
  u16 port = 0;           // the SRV port
  std::string txt;        // the concatenated TXT key=value pairs
  u64 lastSeenMs = 0;
};

class BERRY_API LanDiscovery {
 public:
  LanDiscovery();
  ~LanDiscovery();

  /// Binds the mDNS port and joins the group. False (fail closed) when the
  /// socket cannot bind; a failed group join leaves a unicast-only socket and
  /// is reported by `multicastJoined`.
  bool Open(const LanDiscoveryConfig& cfg);
  void Close();
  bool open() const { return open_; }
  bool multicastJoined() const { return joined_; }
  u16 localPort() const;

  /// Publishes `instance` as a service on `port`, with an optional TXT string.
  /// Sends one unsolicited mDNS response (PTR/SRV/TXT/A). `instance` is the
  /// short name; the full instance name is built from it and the service type.
  bool Publish(const std::string& instance, u16 port, const std::string& txt = "");

  /// Sends a PTR query for the service type, so responders announce themselves.
  bool Query();

  /// Reads datagrams for up to `budgetMs`, updating the cache. Expires entries
  /// past `cacheTtlMs` even when nothing arrives.
  void Poll(int budgetMs = 20);

  /// Live services, most recently seen first.
  std::vector<LanService> services() const;

  /// The full instance name for a short name (also the cache key).
  std::string FullInstanceName(const std::string& instance) const;

  struct Stats {
    u64 packetsSent = 0;
    u64 packetsReceived = 0;
    u64 recordsParsed = 0;
    u64 packetsDropped = 0;   // malformed or from another service type
    u64 servicesExpired = 0;
  };
  Stats stats() const;

  /// Injects a monotonic millisecond clock for the TTLs (a test pins them).
  void SetClock(std::function<u64()> nowMs);

 private:
  struct Entry {
    std::string host;
    std::string txt;
    u16 port = 0;
    bool haveSrv = false;
    u64 lastSeenMs = 0;
  };

  /// Parses one mDNS packet and folds its records into the cache. Sets
  /// `*wantsReply` when the packet is a PTR query for our service type. Caller
  /// holds the lock.
  void HandlePacketLocked(const u8* data, size_t len, u64 now, bool* wantsReply);

  /// Drops services and address records past their TTL. Caller holds the lock.
  void ExpireLocked(u64 now);

  /// Builds the four records (PTR/SRV/TXT/A) this client publishes. Caller
  /// holds the lock.
  std::string BuildResponse() const;

  UdpSocket sock_;
  LanDiscoveryConfig cfg_{};
  bool open_ = false;
  bool joined_ = false;
  std::string localAddress_;

  // Published service, set by Publish and used to answer a query.
  bool published_ = false;
  std::string pubInstance_;
  u16 pubPort_ = 0;
  std::string pubTxt_;

  mutable std::mutex mutex_;
  std::unordered_map<std::string, Entry> services_;   // key: full instance name
  std::unordered_map<std::string, std::pair<std::string, u64>> addresses_;  // host -> {addr, expiryMs}
  Stats stats_{};
  std::function<u64()> clock_;
};

}  // namespace Berry
