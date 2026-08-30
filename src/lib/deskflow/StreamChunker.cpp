/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2013 - 2016 Synergy App Ltd
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "deskflow/StreamChunker.h"

#include "base/Event.h"
#include "base/IEventQueue.h"
#include "base/Log.h"
#include "deskflow/ClipboardChunk.h"

static const size_t g_chunkSize = 512 * 1024; // 512kb

void StreamChunker::sendClipboard(
    const std::string_view &data, size_t size, ClipboardID id, uint32_t sequence, IEventQueue *events, void *eventTarget
)
{
  // send first message (data size)
  std::string dataSize = QString::number(size).toStdString();
  ClipboardChunk *sizeMessage = ClipboardChunk::start(id, sequence, dataSize);

  // [clipboard-debug] temporary instrumentation
  const size_t chunkCount = (size + g_chunkSize - 1) / g_chunkSize;
  LOG_DEBUG(
      "[clipboard-debug] send begin id=%d seq=%u total=%zu chunkSize=%zu chunkCount=%zu declaredHeader=%s", id,
      sequence, size, g_chunkSize, chunkCount, dataSize.c_str()
  );

  events->addEvent(Event(EventTypes::ClipboardSending, eventTarget, sizeMessage));

  // send clipboard chunk with a fixed size
  size_t sentLength = 0;
  size_t chunkSize = g_chunkSize;
  size_t chunkIndex = 0;

  while (true) {
    // make sure we don't read too much from the mock data.
    if (sentLength + chunkSize > size) {
      chunkSize = size - sentLength;
    }

    std::string chunk(data.substr(sentLength, chunkSize).data(), chunkSize);
    ClipboardChunk *dataChunk = ClipboardChunk::data(id, sequence, chunk);

    // [clipboard-debug] temporary instrumentation
    LOG_DEBUG(
        "[clipboard-debug] send chunk %zu/%zu offset=%zu len=%zu", ++chunkIndex, chunkCount, sentLength, chunkSize
    );

    events->addEvent(Event(EventTypes::ClipboardSending, eventTarget, dataChunk));

    sentLength += chunkSize;
    if (sentLength == size) {
      break;
    }
  }

  // send last message
  ClipboardChunk *end = ClipboardChunk::end(id, sequence);

  events->addEvent(Event(EventTypes::ClipboardSending, eventTarget, end));

  // [clipboard-debug] note this only means the chunks were QUEUED as events,
  // not that they reached the wire. the queue drains later, one chunk per
  // event loop iteration.
  LOG_DEBUG(
      "[clipboard-debug] send queued id=%d seq=%u queuedBytes=%zu chunks=%zu (queued, not yet on the wire)", id,
      sequence, sentLength, chunkIndex
  );
  LOG_DEBUG("sent clipboard size=%d", sentLength);
}
