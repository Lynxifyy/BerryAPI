// Host tests for the read-only packet log (Phase F).
#include "berry/packet.h"

#include <cstdio>
#include <string>

using namespace Berry;

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { \
  printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++g_fail; } } while (0)

static void TestRecordAndTotals() {
  PacketLog log;
  CHECK(log.size() == 0);
  CHECK(log.totals().total() == 0);

  CHECK(log.Record(PacketDir::Inbound, 0x2A, 64, 1000, "PlayerAuthInput"));
  CHECK(log.Record(PacketDir::Outbound, 0x2B, 32, 1001, "MovePlayer"));
  CHECK(log.Record(PacketDir::Inbound, 0x2A, 8, 1002, "PlayerAuthInput"));

  const PacketTotals t = log.totals();
  CHECK(t.inbound == 2);
  CHECK(t.outbound == 1);
  CHECK(t.inboundBytes == 72);
  CHECK(t.outboundBytes == 32);
  CHECK(t.total() == 3);
  CHECK(t.dropped == 0);
  CHECK(log.size() == 3);

  // Sequence numbers are monotonic and unique.
  const auto rec = log.Recent(0);
  CHECK(rec.size() == 3);
  CHECK(rec[0].seq > rec[1].seq && rec[1].seq > rec[2].seq);   // newest first
  CHECK(rec[0].id == 0x2A);           // newest record
  CHECK(rec[0].dir == PacketDir::Inbound);
  CHECK(rec[0].payloadLen == 8);

  PacketRecord last;
  CHECK(log.Last(&last));
  CHECK(last.seq == rec[0].seq);

  CHECK(log.CountOf(0x2A) == 2);
  CHECK(log.CountOf(0x2B) == 1);
  CHECK(log.CountOf(0x99) == 0);
}

static void TestRecentLimit() {
  PacketLog log;
  for (int i = 0; i < 10; ++i) {
    log.Record(PacketDir::Inbound, (u32)i, (u32)(i + 1), (u64)i);
  }
  CHECK(log.Recent(3).size() == 3);
  // Newest first: id 9, 8, 7.
  const auto r = log.Recent(3);
  CHECK(r[0].id == 9 && r[1].id == 8 && r[2].id == 7);
  // A count larger than the log returns everything; 0 means everything too.
  CHECK(log.Recent(100).size() == 10);
  CHECK(log.Recent(0).size() == 10);
}

static void TestSummary() {
  PacketLog log;
  log.Record(PacketDir::Inbound, 1, 10, 0);
  log.Record(PacketDir::Inbound, 1, 10, 1);
  log.Record(PacketDir::Inbound, 1, 10, 2);
  log.Record(PacketDir::Outbound, 1, 5, 3);   // same id, other direction
  log.Record(PacketDir::Inbound, 2, 100, 4, "Rare");

  const auto both = log.Summary(PacketDir::Count);
  CHECK(both.size() == 3);
  CHECK(both[0].id == 1 && both[0].dir == PacketDir::Inbound && both[0].count == 3);
  CHECK(both[0].bytes == 30);
  // Highest count first.
  CHECK(both[1].count == 1);
  CHECK(both[2].count == 1);

  const auto in = log.Summary(PacketDir::Inbound);
  CHECK(in.size() == 2);
  for (const auto& s : in) CHECK(s.dir == PacketDir::Inbound);

  const auto out = log.Summary(PacketDir::Outbound);
  CHECK(out.size() == 1);
  CHECK(out[0].id == 1 && out[0].count == 1);

  // The name is carried through when present.
  bool found = false;
  for (const auto& s : log.Summary(PacketDir::Count)) {
    if (s.id == 2) { found = true; CHECK(s.name == "Rare"); }
  }
  CHECK(found);
}

static void TestSummaryIsCapped() {
  PacketLog log;
  // More distinct ids than the summary cap, so the table cannot grow unbounded.
  for (u32 i = 0; i < PacketLog::kMaxSummaryIds + 20; ++i) {
    log.Record(PacketDir::Inbound, i, 1, (u64)i);
  }
  CHECK(log.Summary(PacketDir::Count).size() == PacketLog::kMaxSummaryIds);
}

static void TestRingIsBounded() {
  PacketLog log;
  const size_t n = PacketLog::kMaxRecords + 50;
  for (size_t i = 0; i < n; ++i) {
    CHECK(log.Record(PacketDir::Inbound, 7, 4, (u64)i));
  }
  // The ring holds only the cap, but the totals count every record and the
  // overflow is reported rather than lost silently.
  CHECK(log.size() == PacketLog::kMaxRecords);
  CHECK(log.totals().inbound == n);
  CHECK(log.totals().dropped == 50);
  CHECK(log.CountOf(7) == PacketLog::kMaxRecords);
}

static void TestFailsClosedOnBadInput() {
  PacketLog log;
  // A payload larger than the sanity cap is refused, so a bogus length cannot
  // inflate the totals.
  CHECK(!log.Record(PacketDir::Inbound, 1, PacketLog::kMaxPayloadLen + 1, 0));
  CHECK(log.totals().total() == 0);
  // The cap itself is accepted.
  CHECK(log.Record(PacketDir::Inbound, 1, PacketLog::kMaxPayloadLen, 0));
  CHECK(log.totals().inbound == 1);

  // An out-of-range direction is refused.
  CHECK(!log.Record(PacketDir::Count, 1, 1, 0));

  // Last() on an empty log reports false and does not write through.
  PacketLog empty;
  PacketRecord r;
  CHECK(!empty.Last(&r));
  CHECK(!empty.Last(nullptr));
}

static void TestClear() {
  PacketLog log;
  log.Record(PacketDir::Inbound, 1, 10, 0);
  log.Record(PacketDir::Outbound, 2, 20, 1);
  log.Clear();
  CHECK(log.size() == 0);
  CHECK(log.totals().total() == 0);
  CHECK(log.Recent(0).empty());
  CHECK(!log.Last(nullptr));
  // A cleared log restarts its sequence.
  log.Record(PacketDir::Inbound, 1, 1, 0);
  PacketRecord r;
  CHECK(log.Last(&r));
  CHECK(r.seq == 1);
}

int main() {
  TestRecordAndTotals();
  TestRecentLimit();
  TestSummary();
  TestSummaryIsCapped();
  TestRingIsBounded();
  TestFailsClosedOnBadInput();
  TestClear();
  if (g_fail == 0) printf("test_packet: ALL OK\n");
  return g_fail;
}
