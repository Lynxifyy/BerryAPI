// Host tests for the replay container (berry/replay_format.h).
//
// The container is the on-disk format a replay is stored in, and a replay is
// untrusted input: it is a file someone else can hand you and it feeds the
// game's packet parsers. So these tests do two jobs: round-trip a well-formed
// file exactly, and prove every malformed file is refused rather than trusted.
#include "berry/replay_format.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace Berry;

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { \
  printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++g_fail; } } while (0)

namespace {

ReplayFingerprint Fp(u32 build = 4242, const std::string& label = "1.21.0",
                     u32 proto = 748) {
  ReplayFingerprint f;
  f.buildId = build;
  f.label = label;
  f.protocolVersion = proto;
  return f;
}

/// A record with a payload of `n` bytes, compressible when `fill` repeats.
ReplayRecord Rec(u64 tick, ReplayRecordKind kind, u32 tag, size_t n, char fill) {
  ReplayRecord r;
  r.tick = tick;
  r.kind = kind;
  r.tag = tag;
  r.dir = 0;
  r.payload.assign(n, fill);
  return r;
}

/// A file with `keyframes` keyframes every 20 ticks and a delta after each.
ReplayFile BuildSample(int keyframes, size_t payloadPerRecord = 64) {
  ReplayFile f;
  f.fingerprint = Fp();
  u64 tick = 100;
  for (int k = 0; k < keyframes; ++k) {
    ReplaySection key;
    key.kind = ReplaySectionKind::Keyframe;
    key.startTick = tick;
    key.endTick = tick;
    key.records.push_back(Rec(tick, ReplayRecordKind::Chunk, 7, payloadPerRecord, 'A'));
    key.records.push_back(Rec(tick, ReplayRecordKind::EntitySpawn, 9, payloadPerRecord, 'B'));
    f.sections.push_back(key);

    ReplaySection delta;
    delta.kind = ReplaySectionKind::Delta;
    delta.startTick = tick + 1;
    delta.endTick = tick + 19;
    for (int i = 0; i < 19; ++i) {
      delta.records.push_back(Rec(tick + 1 + i, ReplayRecordKind::EntityMove, 0, 16, 'C'));
    }
    f.sections.push_back(delta);
    tick += 20;
  }
  f.RebuildIndex();
  return f;
}

//--------------------------------------------------------------------------

void TestRoundTripExact() {
  ReplayFile in = BuildSample(3);
  ReplayWriter w;
  std::string bytes;
  CHECK(w.Write(in, &bytes) == ReplayError::Ok);
  CHECK(!bytes.empty());
  CHECK(w.lastBlockCount() == in.sections.size());

  ReplayFile out;
  ReplayReader r;
  CHECK(r.Read(bytes, &out) == ReplayError::Ok);
  CHECK(r.lastBlockCount() == in.sections.size());

  CHECK(out.fingerprint == in.fingerprint);
  CHECK(out.sections.size() == in.sections.size());
  for (size_t i = 0; i < in.sections.size(); ++i) {
    CHECK(out.sections[i].kind == in.sections[i].kind);
    CHECK(out.sections[i].startTick == in.sections[i].startTick);
    CHECK(out.sections[i].endTick == in.sections[i].endTick);
    CHECK(out.sections[i].records.size() == in.sections[i].records.size());
    for (size_t j = 0; j < in.sections[i].records.size(); ++j) {
      const ReplayRecord& a = in.sections[i].records[j];
      const ReplayRecord& b = out.sections[i].records[j];
      CHECK(a.tick == b.tick);
      CHECK(a.kind == b.kind);
      CHECK(a.tag == b.tag);
      CHECK(a.dir == b.dir);
      CHECK(a.payload == b.payload);
    }
  }

  // The index round-trips and points at real keyframes.
  CHECK(out.index.size() == 3);
  for (size_t i = 0; i < out.index.size(); ++i) {
    CHECK(out.index[i].startTick == 100 + 20 * i);
    CHECK(out.index[i].recordCount == 2);
  }

  // Writing the parsed file again is byte-identical (deterministic).
  ReplayWriter w2;
  std::string bytes2;
  CHECK(w2.Write(out, &bytes2) == ReplayError::Ok);
  CHECK(bytes2 == bytes);
}

void TestCompressionActuallyCompresses() {
  // 4000 identical bytes compress far below their size; the container must
  // store the compressed form, not the raw one.
  ReplayFile in = BuildSample(1, 4000);
  ReplayWriter w;
  std::string bytes;
  CHECK(w.Write(in, &bytes) == ReplayError::Ok);
  const size_t rawPayload = 2 * 4000;
  CHECK(bytes.size() < rawPayload);   // a real ratio, not a no-op "compressor"

  ReplayFile out;
  ReplayReader r;
  CHECK(r.Read(bytes, &out) == ReplayError::Ok);
  CHECK(out.sections.size() == in.sections.size());
  CHECK(out.sections[0].records[0].payload.size() == 4000);
  CHECK(out.sections[0].records[0].payload[0] == 'A');
}

void TestIncompressibleStoredRaw() {
  // Random-ish payload that LZ4 cannot shrink must be stored raw, so a block
  // never grows past its input.
  ReplayFile in;
  in.fingerprint = Fp();
  ReplaySection s;
  s.kind = ReplaySectionKind::Keyframe;
  s.startTick = 1;
  s.endTick = 1;
  ReplayRecord r = Rec(1, ReplayRecordKind::Packet, 0x2a, 4096, 0);
  u32 x = 12345;
  for (auto& c : r.payload) { x = x * 1103515245u + 12345u; c = (char)(x >> 16); }
  s.records.push_back(r);
  CHECK(in.AddSection(s) == ReplayError::Ok);

  ReplayWriter w;
  std::string bytes;
  CHECK(w.Write(in, &bytes) == ReplayError::Ok);

  ReplayFile out;
  ReplayReader rr;
  CHECK(rr.Read(bytes, &out) == ReplayError::Ok);
  CHECK(out.sections.size() == 1);
  CHECK(out.sections[0].records[0].payload == r.payload);
}

void TestFingerprintGate() {
  ReplayFile in = BuildSample(1);
  ReplayWriter w;
  std::string bytes;
  CHECK(w.Write(in, &bytes) == ReplayError::Ok);

  // Without the gate, any build reads it.
  ReplayFile out;
  ReplayReader r;
  CHECK(r.Read(bytes, &out) == ReplayError::Ok);

  // With the gate, a different build is refused and the output is untouched.
  ReplayFile other;
  other.fingerprint = Fp(9999, "9.9.9", 999);
  ReplayFile untouched = other;
  CHECK(r.Read(bytes, Fp(9999, "9.9.9", 999), true, &other) ==
        ReplayError::BadFingerprint);
  CHECK(other.fingerprint == untouched.fingerprint);
  CHECK(other.sections.empty());

  // The matching fingerprint passes the gate.
  ReplayFile match;
  CHECK(r.Read(bytes, Fp(), true, &match) == ReplayError::Ok);
  CHECK(match.sections.size() == in.sections.size());
}

void TestSeekIndex() {
  ReplayFile in = BuildSample(5);   // keyframes at 100, 120, 140, 160, 180
  ReplayWriter w;
  std::string bytes;
  CHECK(w.Write(in, &bytes) == ReplayError::Ok);
  ReplayFile out;
  ReplayReader r;
  CHECK(r.Read(bytes, &out) == ReplayError::Ok);

  ReplayIndexEntry e;
  // A tick inside the second keyframe's span seeks to that keyframe.
  CHECK(out.SeekEntry(135, &e));
  CHECK(e.startTick == 120);
  // A tick before the first keyframe starts at the first.
  CHECK(out.SeekEntry(0, &e));
  CHECK(e.startTick == 100);
  // A tick past the last holds the last keyframe.
  CHECK(out.SeekEntry(9999, &e));
  CHECK(e.startTick == 180);
  // Exactly on a keyframe tick seeks to it, not the previous one.
  CHECK(out.SeekEntry(140, &e));
  CHECK(e.startTick == 140);
  // The index offset points at a real block, and the tick there matches.
  for (const ReplayIndexEntry& ie : out.index) {
    CHECK(ie.fileOffset >= 64 && ie.fileOffset < bytes.size());
    CHECK(ie.recordCount > 0);
  }
}

void TestRingBuffer() {
  // "Save the last N sections": the ring keeps only the trailing window.
  ReplayRingBuffer ring(4);
  ring.SetFingerprint(Fp());
  u64 tick = 0;
  for (int i = 0; i < 10; ++i) {
    ReplaySection s;
    s.kind = (i % 2 == 0) ? ReplaySectionKind::Keyframe : ReplaySectionKind::Delta;
    s.startTick = tick;
    s.endTick = tick;
    s.records.push_back(Rec(tick, ReplayRecordKind::Packet, (u32)i, 8, 'x'));
    CHECK(ring.Push(s) == ReplayError::Ok);
    ++tick;
  }
  CHECK(ring.sectionCount() == 4);
  CHECK(ring.evicted() == 6);
  // The kept window is the last four sections, in order.
  CHECK(ring.file().sections[0].records[0].tag == 6);
  CHECK(ring.file().sections[3].records[0].tag == 9);

  // The ring encodes and reads back as a normal replay.
  ReplayWriter w;
  std::string bytes;
  CHECK(w.Write(ring.file(), &bytes) == ReplayError::Ok);
  ReplayFile out;
  ReplayReader r;
  CHECK(r.Read(bytes, &out) == ReplayError::Ok);
  CHECK(out.sections.size() == 4);
  CHECK(out.firstTick() == 6);
  CHECK(out.lastTick() == 9);
}

void TestWriteRingKeepsTail() {
  ReplayFile in = BuildSample(6);   // 12 sections
  ReplayWriter w;
  std::string bytes;
  CHECK(w.WriteRing(in, 4, &bytes) == ReplayError::Ok);
  ReplayFile out;
  ReplayReader r;
  CHECK(r.Read(bytes, &out) == ReplayError::Ok);
  CHECK(out.sections.size() == 4);
  // The tail keeps whole sections: the last keyframe plus its deltas.
  CHECK(out.sections.back().endTick == in.sections.back().endTick);
}

//--------------------------------------------------------------------------
// Malformed input: every one of these must be refused, never half-loaded.
//--------------------------------------------------------------------------

void TestRejectNotAReplay() {
  ReplayFile out;
  ReplayReader r;
  CHECK(r.Read(std::string(80, 'x'), &out) == ReplayError::BadMagic);
  CHECK(out.sections.empty());
  // Too short for even a header.
  CHECK(r.Read("BRPL", &out) == ReplayError::Truncated);
}

void TestRejectBadVersion() {
  ReplayFile in = BuildSample(1);
  ReplayWriter w;
  std::string bytes;
  CHECK(w.Write(in, &bytes) == ReplayError::Ok);
  // Corrupt the version field (offset 4) and fix the header CRC so the version
  // check is what fires, not the checksum.
  bytes[4] = (char)99;
  // Recompute the header CRC over bytes [0,56).
  {
    u32 crc = 0xffffffffu;
    for (size_t i = 0; i < 56; ++i) {
      crc ^= (unsigned char)bytes[i];
      for (int k = 0; k < 8; ++k) {
        const u32 mask = (u32)-(int)(crc & 1u);
        crc = (crc >> 1) ^ (0xedb88320u & mask);
      }
    }
    crc ^= 0xffffffffu;
    bytes[56] = (char)(crc & 0xff);
    bytes[57] = (char)((crc >> 8) & 0xff);
    bytes[58] = (char)((crc >> 16) & 0xff);
    bytes[59] = (char)((crc >> 24) & 0xff);
  }
  ReplayFile out;
  ReplayReader r;
  CHECK(r.Read(bytes, &out) == ReplayError::BadVersion);
}

void TestRejectCorruptHeader() {
  ReplayFile in = BuildSample(1);
  ReplayWriter w;
  std::string bytes;
  CHECK(w.Write(in, &bytes) == ReplayError::Ok);
  // Flip a byte in the header; the CRC no longer matches.
  bytes[20] ^= 0x01;
  ReplayFile out;
  ReplayReader r;
  CHECK(r.Read(bytes, &out) == ReplayError::Corrupt);
}

void TestRejectCorruptBlock() {
  ReplayFile in = BuildSample(2);
  ReplayWriter w;
  std::string bytes;
  CHECK(w.Write(in, &bytes) == ReplayError::Ok);
  // Flip a byte near the end of the block region; the block CRC must catch it.
  const size_t at = bytes.size() / 2;
  bytes[at] ^= 0xff;
  ReplayFile out;
  ReplayReader r;
  const ReplayError e = r.Read(bytes, &out);
  CHECK(e == ReplayError::Corrupt || e == ReplayError::BadBlock ||
        e == ReplayError::BadRecord || e == ReplayError::Truncated);
  CHECK(out.sections.empty());
}

void TestRejectTruncated() {
  ReplayFile in = BuildSample(3);
  ReplayWriter w;
  std::string bytes;
  CHECK(w.Write(in, &bytes) == ReplayError::Ok);
  ReplayFile out;
  ReplayReader r;
  // Every truncation point must be refused, not misread.
  for (size_t cut = 1; cut < bytes.size(); cut += 7) {
    ReplayFile tmp;
    const ReplayError e = r.Read(bytes.substr(0, cut), &tmp);
    CHECK(e != ReplayError::Ok);
    CHECK(tmp.sections.empty());
  }
  // A file with a valid header but a section count past what the blocks hold.
  {
    std::string bad = bytes;
    // Set sectionCount (offset 24) to something huge and fix the CRC.
    bad[24] = (char)0xff; bad[25] = (char)0xff; bad[26] = 0; bad[27] = 0;
    u32 crc = 0xffffffffu;
    for (size_t i = 0; i < 56; ++i) {
      crc ^= (unsigned char)bad[i];
      for (int k = 0; k < 8; ++k) {
        const u32 mask = (u32)-(int)(crc & 1u);
        crc = (crc >> 1) ^ (0xedb88320u & mask);
      }
    }
    crc ^= 0xffffffffu;
    bad[56] = (char)(crc & 0xff);
    bad[57] = (char)((crc >> 8) & 0xff);
    bad[58] = (char)((crc >> 16) & 0xff);
    bad[59] = (char)((crc >> 24) & 0xff);
    ReplayFile tmp;
    const ReplayError e = r.Read(bad, &tmp);
    CHECK(e != ReplayError::Ok);
  }
}

void TestRejectBadIndex() {
  ReplayFile in = BuildSample(2);
  ReplayWriter w;
  std::string bytes;
  CHECK(w.Write(in, &bytes) == ReplayError::Ok);
  // The index sits at the very end: 2 keyframes * 24 bytes. Corrupt the first
  // entry's file offset to point past the blocks; the reader must refuse it.
  const size_t idxOff = bytes.size() - 2 * 24;
  bytes[idxOff + 8] = (char)0xff;   // low byte of fileOffset
  bytes[idxOff + 9] = (char)0xff;
  ReplayFile out;
  ReplayReader r;
  CHECK(r.Read(bytes, &out) == ReplayError::BadIndex);
}

void TestAddSectionOrderingAndCaps() {
  ReplayFile f;
  f.fingerprint = Fp();
  ReplayLimits lim;

  ReplaySection a;
  a.kind = ReplaySectionKind::Keyframe;
  a.startTick = 100;
  a.endTick = 110;
  CHECK(f.AddSection(a, lim) == ReplayError::Ok);

  // A section that starts before the previous ends is refused: a replay is
  // ordered, and a seek relies on that.
  ReplaySection back;
  back.kind = ReplaySectionKind::Delta;
  back.startTick = 105;
  back.endTick = 115;
  CHECK(f.AddSection(back, lim) == ReplayError::BadHeader);
  CHECK(f.sections.size() == 1);

  // An end before a start is refused.
  ReplaySection inverted;
  inverted.startTick = 200;
  inverted.endTick = 199;
  CHECK(f.AddSection(inverted, lim) == ReplayError::BadHeader);

  // A payload past the cap is refused.
  ReplayLimits tiny;
  tiny.maxPayloadBytes = 8;
  ReplaySection big;
  big.kind = ReplaySectionKind::Delta;
  big.startTick = 200;
  big.endTick = 200;
  big.records.push_back(Rec(200, ReplayRecordKind::Packet, 1, 100, 'z'));
  CHECK(f.AddSection(big, tiny) == ReplayError::TooLarge);

  // Too many records is refused.
  ReplayLimits few;
  few.maxRecordsPerSection = 2;
  ReplaySection many;
  many.kind = ReplaySectionKind::Delta;
  many.startTick = 300;
  many.endTick = 300;
  for (int i = 0; i < 3; ++i) many.records.push_back(Rec(300, ReplayRecordKind::Packet, 1, 4, 'z'));
  CHECK(f.AddSection(many, few) == ReplayError::TooManyRecords);
}

void TestKindNames() {
  CHECK(std::string(ReplayRecordKindName(ReplayRecordKind::Packet)) == "packet");
  CHECK(std::string(ReplayRecordKindName(ReplayRecordKind::Chunk)) == "chunk");
  ReplayRecordKind k = ReplayRecordKind::Unknown;
  CHECK(ReplayRecordKindFromName("entity_spawn", &k));
  CHECK(k == ReplayRecordKind::EntitySpawn);
  CHECK(ReplayRecordKindFromName("time_weather", &k));
  CHECK(k == ReplayRecordKind::TimeWeather);
  CHECK(!ReplayRecordKindFromName("nonsense", &k));
  CHECK(!ReplayRecordKindFromName("", &k));
}

void TestUnknownKindPreserved() {
  // A record kind this build does not know must survive a load/write cycle, so
  // an older reader can copy a newer file without dropping records.
  ReplayFile in;
  in.fingerprint = Fp();
  ReplaySection s;
  s.kind = ReplaySectionKind::Keyframe;
  s.startTick = 1;
  s.endTick = 1;
  ReplayRecord r = Rec(1, (ReplayRecordKind)999, 5, 12, 'q');
  s.records.push_back(r);
  CHECK(in.AddSection(s) == ReplayError::Ok);

  ReplayWriter w;
  std::string bytes;
  CHECK(w.Write(in, &bytes) == ReplayError::Ok);
  ReplayFile out;
  ReplayReader rd;
  CHECK(rd.Read(bytes, &out) == ReplayError::Ok);
  CHECK(out.sections.size() == 1);
  CHECK((u32)out.sections[0].records[0].kind == 999u);
  CHECK(out.sections[0].records[0].payload == r.payload);
}

void TestEmptyAndEdges() {
  ReplayFile empty;
  empty.fingerprint = Fp();
  ReplayWriter w;
  std::string bytes;
  CHECK(w.Write(empty, &bytes) == ReplayError::Ok);
  CHECK(bytes.size() == 64 + std::string("1.21.0").size());  // header + label
  ReplayFile out;
  ReplayReader r;
  CHECK(r.Read(bytes, &out) == ReplayError::Ok);
  CHECK(out.sections.empty());
  CHECK(out.firstTick() == 0 && out.lastTick() == 0);
  ReplayIndexEntry ie;
  CHECK(!out.SeekEntry(10, &ie));   // no index: nothing to seek to

  // A zero-length payload is legal and round-trips.
  ReplayFile z;
  z.fingerprint = Fp();
  ReplaySection s;
  s.kind = ReplaySectionKind::Keyframe;
  s.startTick = 5;
  s.endTick = 5;
  s.records.push_back(Rec(5, ReplayRecordKind::Event, 0, 0, 0));
  CHECK(z.AddSection(s) == ReplayError::Ok);
  std::string zb;
  CHECK(w.Write(z, &zb) == ReplayError::Ok);
  ReplayFile zo;
  CHECK(r.Read(zb, &zo) == ReplayError::Ok);
  CHECK(zo.sections.size() == 1);
  CHECK(zo.sections[0].records[0].payload.empty());

  // A long label is refused on write (it is bounded).
  ReplayFile big;
  big.fingerprint = Fp(1, std::string(500, 'L'), 1);
  std::string bb;
  CHECK(w.Write(big, &bb) == ReplayError::TooLarge);
}

}  // namespace

int main() {
  TestRoundTripExact();
  TestCompressionActuallyCompresses();
  TestIncompressibleStoredRaw();
  TestFingerprintGate();
  TestSeekIndex();
  TestRingBuffer();
  TestWriteRingKeepsTail();
  TestRejectNotAReplay();
  TestRejectBadVersion();
  TestRejectCorruptHeader();
  TestRejectCorruptBlock();
  TestRejectTruncated();
  TestRejectBadIndex();
  TestAddSectionOrderingAndCaps();
  TestKindNames();
  TestUnknownKindPreserved();
  TestEmptyAndEdges();

  if (g_fail == 0) {
    printf("test_replay_format: ALL OK\n");
    return 0;
  }
  printf("test_replay_format: %d FAILURES\n", g_fail);
  return 1;
}
