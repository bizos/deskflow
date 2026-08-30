/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2025 Chris Rizzitello <sithlord48@gmail.com>
 * SPDX-FileCopyrightText: (C) 2015 - 2016 Synergy App Ltd
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "ClipboardChunksTests.h"

#include "deskflow/ClipboardChunk.h"
#include "deskflow/ProtocolTypes.h"
#include "deskflow/ProtocolUtil.h"
#include "deskflow/StreamChunker.h"
#include "base/IEventQueue.h"
#include "io/IStream.h"

#include <algorithm>
#include <cstring>
#include <deque>

namespace {

class MemoryStream : public deskflow::IStream
{
public:
  void push(const std::string &bytes)
  {
    m_queue.push_back(bytes);
  }

  void close() override
  {
    m_queue.clear();
    m_inputShutdown = true;
  }

  uint32_t read(void *buffer, uint32_t n) override
  {
    if (m_inputShutdown || m_queue.empty() || n == 0) {
      return 0;
    }

    auto &front = m_queue.front();
    const size_t take = std::min(static_cast<size_t>(n), front.size());
    if (buffer != nullptr) {
      std::memcpy(buffer, front.data(), take);
    }

    front.erase(0, take);
    if (front.empty()) {
      m_queue.pop_front();
    }

    return static_cast<uint32_t>(take);
  }

  void write(const void *, uint32_t) override
  {
  }

  void flush() override
  {
  }

  void shutdownInput() override
  {
    close();
  }

  void shutdownOutput() override
  {
  }

  void *getEventTarget() const override
  {
    return const_cast<MemoryStream *>(this);
  }

  bool isReady() const override
  {
    return !m_inputShutdown && !m_queue.empty();
  }

  uint32_t getSize() const override
  {
    size_t total = 0;
    for (const auto &chunk : m_queue) {
      total += chunk.size();
    }
    return static_cast<uint32_t>(std::min<size_t>(total, UINT32_MAX));
  }

private:
  std::deque<std::string> m_queue;
  bool m_inputShutdown = false;
};

class BufferWriteStream : public deskflow::IStream
{
public:
  const std::string &str() const
  {
    return m_buffer;
  }

  void close() override
  {
    m_outputShutdown = true;
  }

  uint32_t read(void *, uint32_t) override
  {
    return 0;
  }

  void write(const void *buffer, uint32_t n) override
  {
    if (!m_outputShutdown && n != 0) {
      m_buffer.append(static_cast<const char *>(buffer), n);
    }
  }

  void flush() override
  {
  }

  void shutdownInput() override
  {
  }

  void shutdownOutput() override
  {
    m_outputShutdown = true;
  }

  void *getEventTarget() const override
  {
    return const_cast<BufferWriteStream *>(this);
  }

  bool isReady() const override
  {
    return false;
  }

  uint32_t getSize() const override
  {
    return 0;
  }

private:
  std::string m_buffer;
  bool m_outputShutdown = false;
};

std::string encodeClipboardMsg(ClipboardID id, uint32_t seq, uint8_t mark, const std::string &data)
{
  BufferWriteStream stream;
  auto payload = data;
  ProtocolUtil::writef(&stream, kMsgDClipboard + 4, id, seq, mark, &payload);
  return stream.str();
}

//
// full-scale round trip harness
//
// drives the real sender (StreamChunker, which queues one event per 512 KiB
// chunk) and the real receiver (ClipboardChunk::assemble) against each other, so
// the payload sizes and chunk counts below are the ones that actually go over
// the wire.
//

//! An event queue that writes each queued clipboard chunk straight to the wire
/*!
Stands in for the ClipboardSending handler in ServerProxy / ClientProxy1_6,
which does exactly this: ClipboardChunk::send(stream, e.getDataObject()).
*/
class ImmediateSendQueue : public IEventQueue
{
public:
  explicit ImmediateSendQueue(MemoryStream *wire) : m_wire(wire)
  {
  }

  void addEvent(Event &&event) override
  {
    // ClipboardChunk::send writes the whole message including the 4 byte code
    BufferWriteStream out;
    ClipboardChunk::send(&out, event.getDataObject());
    Event::deleteData(event);

    // hand the message to the wire the way handleData() sees it: the code is
    // consumed by the dispatcher, so store code and body separately
    const std::string &msg = out.str();
    QVERIFY2(msg.size() >= 4, "clipboard message shorter than its code");
    m_wire->push(msg);
    ++m_queued;
  }

  size_t queued() const
  {
    return m_queued;
  }

  // the rest is unused by StreamChunker
  int loop() override
  {
    return 0;
  }
  void adoptBuffer(IEventQueueBuffer *) override
  {
  }
  bool getEvent(Event &, double) override
  {
    return false;
  }
  bool dispatchEvent(const Event &) override
  {
    return false;
  }
  EventQueueTimer *newTimer(double, void *) override
  {
    return nullptr;
  }
  EventQueueTimer *newOneShotTimer(double, void *) override
  {
    return nullptr;
  }
  void deleteTimer(EventQueueTimer *) override
  {
  }
  void addHandler(EventTypes, void *, const EventHandler &) override
  {
  }
  void removeHandler(EventTypes, void *) override
  {
  }
  void removeHandlers(void *) override
  {
  }
  void removeEventsFor(void *) override
  {
  }
  void waitForReady() const override
  {
  }
  void *getSystemTarget() override
  {
    return this;
  }

private:
  MemoryStream *m_wire;
  size_t m_queued = 0;
};

//! Outcome of draining a wire through a ServerProxy-shaped dispatcher
struct DrainResult
{
  std::string assembled;
  size_t finished = 0;
  size_t rejected = 0;
  size_t errors = 0;
  size_t chunksSeen = 0;
  size_t mouseMovesSeen = 0;
  size_t entersSeen = 0;
  bool unknownCode = false;
};

//! Read a 4 byte message code, mirroring ServerProxy::handleData
bool readCode(deskflow::IStream *stream, char code[4])
{
  uint32_t got = 0;
  while (got < 4) {
    const uint32_t n = stream->read(code + got, 4 - got);
    if (n == 0) {
      return false;
    }
    got += n;
  }
  return true;
}

/*!
Drains \p wire the way ServerProxy::handleData does: read a code, dispatch on it.
Clipboard chunks and input messages are interleaved on one stream, so this also
proves a clipboard transfer cannot desync the input protocol.
*/
DrainResult drain(MemoryStream *wire, size_t limitBytes, std::string &dataCached, ClipboardChunkAssemblyState &state)
{
  DrainResult result;
  char code[4] = {};

  while (readCode(wire, code)) {
    if (std::memcmp(code, "DCLP", 4) == 0) {
      ClipboardID id = kClipboardEnd;
      uint32_t seq = 0;
      const auto r = ClipboardChunk::assemble(wire, dataCached, id, seq, state, limitBytes);
      ++result.chunksSeen;
      switch (r) {
        using enum TransferState;
      case Finished:
        ++result.finished;
        result.assembled = dataCached;
        dataCached.clear();
        break;
      case Rejected:
        ++result.rejected;
        break;
      case Error:
        ++result.errors;
        break;
      default:
        break;
      }
    } else if (std::memcmp(code, "DMMV", 4) == 0) {
      int32_t x = 0;
      int32_t y = 0;
      if (!ProtocolUtil::readf(wire, kMsgDMouseMove + 4, &x, &y)) {
        result.unknownCode = true;
        break;
      }
      ++result.mouseMovesSeen;
    } else if (std::memcmp(code, "CINN", 4) == 0) {
      int32_t x = 0;
      int32_t y = 0;
      uint32_t seq = 0;
      int32_t mask = 0;
      if (!ProtocolUtil::readf(wire, kMsgCEnter + 4, &x, &y, &seq, &mask)) {
        result.unknownCode = true;
        break;
      }
      ++result.entersSeen;
    } else {
      // an unrecognised code is what a desynced stream looks like
      result.unknownCode = true;
      break;
    }
  }

  return result;
}

std::string encodeMouseMove(int32_t x, int32_t y)
{
  BufferWriteStream out;
  ProtocolUtil::writef(&out, kMsgDMouseMove, x, y);
  return out.str();
}

std::string encodeEnter(int32_t x, int32_t y, uint32_t seq, int32_t mask)
{
  BufferWriteStream out;
  ProtocolUtil::writef(&out, kMsgCEnter, x, y, seq, mask);
  return out.str();
}

//! A marshalled clipboard the size Deskflow really produces for an image
/*!
The Bitmap format travels as a DIB with no file header: a 40 byte INFOHEADER
followed by uncompressed 32bpp pixels. The byte counts here are therefore the
real ones, which is what decides the chunk count.
*/
std::string makeDib(int32_t width, int32_t height)
{
  const size_t pixels = static_cast<size_t>(width) * static_cast<size_t>(height) * 4;
  std::string dib(40 + pixels, '\0');

  std::memcpy(&dib[0], "\x28\x00\x00\x00", 4); // biSize = 40
  std::memcpy(&dib[4], &width, 4);
  std::memcpy(&dib[8], &height, 4);
  const uint16_t bpp = 32;
  std::memcpy(&dib[14], &bpp, 2);

  // vary the body so a truncated or duplicated chunk cannot pass unnoticed
  for (size_t i = 40; i < dib.size(); i += 4093) {
    dib[i] = static_cast<char>(i & 0xff);
  }
  return dib;
}

constexpr size_t kChunkSize = 512 * 1024;
constexpr size_t k32MiB = 32u * 1024 * 1024;

size_t expectedChunkCount(size_t payload)
{
  // one start, one end, and ceil(payload / 512 KiB) data chunks
  return 2 + (payload + kChunkSize - 1) / kChunkSize;
}

} // namespace

void ClipboardChunksTests::initTestCase()
{
  m_log.setFilter(LogLevel::Level::Debug);
}

void ClipboardChunksTests::startFormatData()
{
  ClipboardID id = 0;
  uint32_t sequence = 0;
  std::string mockDataSize("10");
  ClipboardChunk *chunk = ClipboardChunk::start(id, sequence, mockDataSize);
  uint32_t temp_m_chunk;
  memcpy(&temp_m_chunk, &(chunk->m_chunk[1]), 4);

  QCOMPARE(chunk->m_chunk[0], id);
  QCOMPARE(temp_m_chunk, sequence);
  QCOMPARE(chunk->m_chunk[5], ChunkType::DataStart);
  QCOMPARE(chunk->m_chunk[6], '1');
  QCOMPARE(chunk->m_chunk[7], '0');
  QCOMPARE(chunk->m_chunk[8], '\0');
  delete chunk;
}

void ClipboardChunksTests::formatDataChunk()
{
  ClipboardID id = 0;
  uint32_t sequence = 1;
  uint32_t temp_m_chunk;
  std::string mockData("mock data");
  ClipboardChunk *chunk = ClipboardChunk::data(id, sequence, mockData);
  memcpy(&temp_m_chunk, &chunk->m_chunk[1], 4);

  QCOMPARE(chunk->m_chunk[0], id);
  QCOMPARE(temp_m_chunk, sequence);
  QCOMPARE(chunk->m_chunk[5], ChunkType::DataChunk);
  QCOMPARE(chunk->m_chunk[6], 'm');
  QCOMPARE(chunk->m_chunk[7], 'o');
  QCOMPARE(chunk->m_chunk[8], 'c');
  QCOMPARE(chunk->m_chunk[9], 'k');
  QCOMPARE(chunk->m_chunk[10], ' ');
  QCOMPARE(chunk->m_chunk[11], 'd');
  QCOMPARE(chunk->m_chunk[12], 'a');
  QCOMPARE(chunk->m_chunk[13], 't');
  QCOMPARE(chunk->m_chunk[14], 'a');
  QCOMPARE(chunk->m_chunk[15], '\0');

  delete chunk;
}

void ClipboardChunksTests::endFormatData()
{
  ClipboardID id = 1;
  uint32_t sequence = 1;
  uint32_t temp_m_chunk;
  ClipboardChunk *chunk = ClipboardChunk::end(id, sequence);
  memcpy(&temp_m_chunk, &chunk->m_chunk[1], 4);

  QCOMPARE(chunk->m_chunk[0], id);
  QCOMPARE(temp_m_chunk, sequence);
  QCOMPARE(chunk->m_chunk[5], ChunkType::DataEnd);
  QCOMPARE(chunk->m_chunk[6], '\0');

  delete chunk;
}

void ClipboardChunksTests::assembleAllowsDataAtExpectedSizeAndLimit()
{
  MemoryStream stream;
  stream.push(encodeClipboardMsg(0, 7, ChunkType::DataStart, "4"));
  stream.push(encodeClipboardMsg(0, 7, ChunkType::DataChunk, "AB"));
  stream.push(encodeClipboardMsg(0, 7, ChunkType::DataChunk, "CD"));
  stream.push(encodeClipboardMsg(0, 7, ChunkType::DataEnd, ""));

  std::string cached;
  ClipboardID id = kClipboardEnd;
  uint32_t seq = 0;
  ClipboardChunkAssemblyState state;

  QCOMPARE(ClipboardChunk::assemble(&stream, cached, id, seq, state, 4), TransferState::Started);
  QCOMPARE(ClipboardChunk::assemble(&stream, cached, id, seq, state, 4), TransferState::InProgress);
  QCOMPARE(ClipboardChunk::assemble(&stream, cached, id, seq, state, 4), TransferState::InProgress);
  QCOMPARE(ClipboardChunk::assemble(&stream, cached, id, seq, state, 4), TransferState::Finished);

  QCOMPARE(cached, std::string("ABCD"));
  QCOMPARE(id, static_cast<ClipboardID>(0));
  QCOMPARE(seq, static_cast<uint32_t>(7));
  QCOMPARE(ClipboardChunk::getExpectedSize(state), static_cast<size_t>(4));
  QVERIFY(!state.active);
}

void ClipboardChunksTests::assembleRejectsDataBeyondExpectedSize()
{
  MemoryStream stream;
  stream.push(encodeClipboardMsg(0, 7, ChunkType::DataStart, "1"));
  stream.push(encodeClipboardMsg(0, 7, ChunkType::DataChunk, "AA"));

  std::string cached;
  ClipboardID id = kClipboardEnd;
  uint32_t seq = 0;
  ClipboardChunkAssemblyState state;

  QCOMPARE(ClipboardChunk::assemble(&stream, cached, id, seq, state, 1024), TransferState::Started);
  QCOMPARE(ClipboardChunk::assemble(&stream, cached, id, seq, state, 1024), TransferState::Rejected);
  QVERIFY(cached.empty());
  QCOMPARE(ClipboardChunk::getExpectedSize(state), static_cast<size_t>(0));
  QVERIFY(!state.active);
}

void ClipboardChunksTests::assembleRejectsExpectedSizeBeyondLimit()
{
  MemoryStream stream;
  stream.push(encodeClipboardMsg(0, 7, ChunkType::DataStart, "8"));

  std::string cached;
  ClipboardID id = kClipboardEnd;
  uint32_t seq = 0;
  ClipboardChunkAssemblyState state;

  QCOMPARE(ClipboardChunk::assemble(&stream, cached, id, seq, state, 4), TransferState::Rejected);
  QVERIFY(cached.empty());
}

void ClipboardChunksTests::assembleSwallowsRestOfRefusedTransfer()
{
  // the peer queues every chunk before we can refuse, so they all still arrive.
  // eating them must leave the stream in sync and never report a hard Error,
  // otherwise the receiver drops a perfectly good connection over a payload.
  MemoryStream stream;
  stream.push(encodeClipboardMsg(0, 7, ChunkType::DataStart, "8"));
  stream.push(encodeClipboardMsg(0, 7, ChunkType::DataChunk, "ABCD"));
  stream.push(encodeClipboardMsg(0, 7, ChunkType::DataChunk, "EFGH"));
  stream.push(encodeClipboardMsg(0, 7, ChunkType::DataEnd, ""));
  // a second, small transfer must still be understood afterwards
  stream.push(encodeClipboardMsg(0, 8, ChunkType::DataStart, "2"));
  stream.push(encodeClipboardMsg(0, 8, ChunkType::DataChunk, "hi"));
  stream.push(encodeClipboardMsg(0, 8, ChunkType::DataEnd, ""));

  std::string cached;
  ClipboardID id = kClipboardEnd;
  uint32_t seq = 0;
  ClipboardChunkAssemblyState state;

  QCOMPARE(ClipboardChunk::assemble(&stream, cached, id, seq, state, 4), TransferState::Rejected);
  QCOMPARE(ClipboardChunk::assemble(&stream, cached, id, seq, state, 4), TransferState::Rejected);
  QCOMPARE(ClipboardChunk::assemble(&stream, cached, id, seq, state, 4), TransferState::Rejected);
  QCOMPARE(ClipboardChunk::assemble(&stream, cached, id, seq, state, 4), TransferState::Rejected);
  QVERIFY(cached.empty());
  QVERIFY(!state.active);

  QCOMPARE(ClipboardChunk::assemble(&stream, cached, id, seq, state, 4), TransferState::Started);
  QCOMPARE(ClipboardChunk::assemble(&stream, cached, id, seq, state, 4), TransferState::InProgress);
  QCOMPARE(ClipboardChunk::assemble(&stream, cached, id, seq, state, 4), TransferState::Finished);
  QCOMPARE(cached, std::string("hi"));
  QCOMPARE(seq, static_cast<uint32_t>(8));
}

void ClipboardChunksTests::assembleRejectsOrphanChunkWithoutDesync()
{
  // a leftover chunk from an interrupted transfer arriving with no DataStart:
  // dropped, but the stream must stay usable for the next transfer
  MemoryStream stream;
  stream.push(encodeClipboardMsg(0, 7, ChunkType::DataChunk, "stale"));
  stream.push(encodeClipboardMsg(0, 7, ChunkType::DataEnd, ""));
  stream.push(encodeClipboardMsg(0, 9, ChunkType::DataStart, "2"));
  stream.push(encodeClipboardMsg(0, 9, ChunkType::DataChunk, "ok"));
  stream.push(encodeClipboardMsg(0, 9, ChunkType::DataEnd, ""));

  std::string cached;
  ClipboardID id = kClipboardEnd;
  uint32_t seq = 0;
  ClipboardChunkAssemblyState state;

  QCOMPARE(ClipboardChunk::assemble(&stream, cached, id, seq, state, 1024), TransferState::Rejected);
  QCOMPARE(ClipboardChunk::assemble(&stream, cached, id, seq, state, 1024), TransferState::Rejected);

  QCOMPARE(ClipboardChunk::assemble(&stream, cached, id, seq, state, 1024), TransferState::Started);
  QCOMPARE(ClipboardChunk::assemble(&stream, cached, id, seq, state, 1024), TransferState::InProgress);
  QCOMPARE(ClipboardChunk::assemble(&stream, cached, id, seq, state, 1024), TransferState::Finished);
  QCOMPARE(cached, std::string("ok"));
}

void ClipboardChunksTests::roundTripSizes_data()
{
  QTest::addColumn<std::string>("payload");
  QTest::addColumn<QString>("label");

  QTest::newRow("text") << std::string("abc") << QStringLiteral("plain text");
  QTest::newRow("300x300") << makeDib(300, 300) << QStringLiteral("300x300");
  QTest::newRow("1000x800") << makeDib(1000, 800) << QStringLiteral("1000x800");
  QTest::newRow("2000x1200") << makeDib(2000, 1200) << QStringLiteral("2000x1200");
  QTest::newRow("3360x1728") << makeDib(3360, 1728) << QStringLiteral("3360x1728 full screen");
}

void ClipboardChunksTests::roundTripSizes()
{
  QFETCH(std::string, payload);
  QFETCH(QString, label);

  MemoryStream wire;
  ImmediateSendQueue queue(&wire);
  StreamChunker::sendClipboard(payload, payload.size(), 0, 1, &queue, &queue);

  QCOMPARE(queue.queued(), expectedChunkCount(payload.size()));

  std::string cached;
  ClipboardChunkAssemblyState state;
  const auto result = drain(&wire, k32MiB, cached, state);

  QVERIFY2(!result.unknownCode, qPrintable(label + ": stream desynced"));
  QCOMPARE(result.errors, static_cast<size_t>(0));
  QCOMPARE(result.rejected, static_cast<size_t>(0));
  QCOMPARE(result.finished, static_cast<size_t>(1));
  QCOMPARE(result.assembled.size(), payload.size());
  QVERIFY2(result.assembled == payload, qPrintable(label + ": payload came back altered"));
  QVERIFY(!state.active);
}

void ClipboardChunksTests::roundTripTenConsecutiveFullScreenCopies()
{
  // ten large copies in a row over one connection: the assembly state must come
  // back clean each time, or transfer N+1 inherits N's leftovers
  const auto payload = makeDib(3360, 1728);

  std::string cached;
  ClipboardChunkAssemblyState state;

  for (uint32_t seq = 1; seq <= 10; ++seq) {
    MemoryStream wire;
    ImmediateSendQueue queue(&wire);
    StreamChunker::sendClipboard(payload, payload.size(), 0, seq, &queue, &queue);

    const auto result = drain(&wire, k32MiB, cached, state);

    QVERIFY2(!result.unknownCode, qPrintable(QStringLiteral("copy %1 desynced the stream").arg(seq)));
    QCOMPARE(result.errors, static_cast<size_t>(0));
    QCOMPARE(result.rejected, static_cast<size_t>(0));
    QCOMPARE(result.finished, static_cast<size_t>(1));
    QVERIFY2(result.assembled == payload, qPrintable(QStringLiteral("copy %1 came back altered").arg(seq)));
    QVERIFY(!state.active);
    QVERIFY(cached.empty());
  }
}

void ClipboardChunksTests::roundTripFullScreenImageThenText()
{
  const auto image = makeDib(3360, 1728);
  const std::string text = "abc";

  std::string cached;
  ClipboardChunkAssemblyState state;

  MemoryStream wire;
  ImmediateSendQueue queue(&wire);
  StreamChunker::sendClipboard(image, image.size(), 0, 1, &queue, &queue);
  StreamChunker::sendClipboard(text, text.size(), 0, 2, &queue, &queue);

  const auto result = drain(&wire, k32MiB, cached, state);

  QVERIFY(!result.unknownCode);
  QCOMPARE(result.errors, static_cast<size_t>(0));
  QCOMPARE(result.rejected, static_cast<size_t>(0));
  QCOMPARE(result.finished, static_cast<size_t>(2));
  // the text is the last transfer to finish
  QCOMPARE(result.assembled, text);
  QVERIFY(!state.active);
}

void ClipboardChunksTests::roundTripInputMessagesInterleavedWithChunks()
{
  // the cursor moves and crosses screens while a 22 MiB clipboard is in flight.
  // clipboard chunks and input messages share one stream, so a clipboard
  // transfer must leave every input message that follows it parseable.
  const auto payload = makeDib(3360, 1728);

  MemoryStream wire;
  ImmediateSendQueue queue(&wire);

  // enter first, exactly as Server::switchScreen sends it: enter, then clipboard
  wire.push(encodeEnter(100, 200, 7, 0));
  StreamChunker::sendClipboard(payload, payload.size(), 0, 1, &queue, &queue);
  // and input keeps flowing while the transfer drains
  for (int i = 0; i < 20; ++i) {
    wire.push(encodeMouseMove(i, i * 2));
  }

  std::string cached;
  ClipboardChunkAssemblyState state;
  const auto result = drain(&wire, k32MiB, cached, state);

  QVERIFY2(!result.unknownCode, "input protocol desynced by the clipboard transfer");
  QCOMPARE(result.errors, static_cast<size_t>(0));
  QCOMPARE(result.finished, static_cast<size_t>(1));
  QVERIFY(result.assembled == payload);
  QCOMPARE(result.entersSeen, static_cast<size_t>(1));
  QCOMPARE(result.mouseMovesSeen, static_cast<size_t>(20));
}

void ClipboardChunksTests::roundTripOverLimitKeepsInputAndLaterTransfersAlive()
{
  // the regression for the reported bug, at full scale: a 22 MiB clipboard
  // arriving at a receiver that only allows 3 MiB. every chunk is refused, but
  // the stream must stay in sync so the enter and mouse messages still arrive
  // and the next clipboard still works. before the fix this returned Error,
  // which made ServerProxy tear the connection down and stopped the cursor from
  // crossing at all.
  const auto payload = makeDib(3360, 1728);
  constexpr size_t k3MiB = 3u * 1024 * 1024;

  MemoryStream wire;
  ImmediateSendQueue queue(&wire);

  wire.push(encodeEnter(100, 200, 7, 0));
  StreamChunker::sendClipboard(payload, payload.size(), 0, 1, &queue, &queue);
  for (int i = 0; i < 5; ++i) {
    wire.push(encodeMouseMove(i, i));
  }
  // a small clipboard afterwards must still get through
  const std::string text = "abc";
  StreamChunker::sendClipboard(text, text.size(), 0, 2, &queue, &queue);

  std::string cached;
  ClipboardChunkAssemblyState state;
  const auto result = drain(&wire, k3MiB, cached, state);

  QVERIFY2(!result.unknownCode, "refusing an oversize clipboard desynced the stream");
  QCOMPARE(result.errors, static_cast<size_t>(0));
  // start, 45 data chunks and end were all read and thrown away
  QCOMPARE(result.rejected, expectedChunkCount(payload.size()));
  QCOMPARE(result.finished, static_cast<size_t>(1));
  QCOMPARE(result.assembled, text);
  QCOMPARE(result.entersSeen, static_cast<size_t>(1));
  QCOMPARE(result.mouseMovesSeen, static_cast<size_t>(5));
  QVERIFY(!state.active);
}

QTEST_MAIN(ClipboardChunksTests)
