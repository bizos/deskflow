/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2025 Chris Rizzitello <sithlord48@gmail.com>
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "base/Log.h"

#include <QTest>

class ClipboardChunksTests : public QObject
{
  Q_OBJECT
private Q_SLOTS:
  // Test are run in order top to bottom
  void initTestCase();
  void startFormatData();
  void formatDataChunk();
  void endFormatData();
  void assembleAllowsDataAtExpectedSizeAndLimit();
  void assembleRejectsDataBeyondExpectedSize();
  void assembleRejectsExpectedSizeBeyondLimit();
  void assembleSwallowsRestOfRefusedTransfer();
  void assembleRejectsOrphanChunkWithoutDesync();

  // full scale send/receive round trips
  void roundTripSizes_data();
  void roundTripSizes();
  void roundTripTenConsecutiveFullScreenCopies();
  void roundTripFullScreenImageThenText();
  void roundTripInputMessagesInterleavedWithChunks();
  void roundTripOverLimitKeepsInputAndLaterTransfersAlive();

private:
  Log m_log;
};
