/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "EventQueueTests.h"

#include "base/EventQueue.h"

#include <QTest>

#include <memory>

void EventQueueTests::initTestCase()
{
  m_arch.init();
}

void EventQueueTests::dispatchEvent_noHandler_returnsFalse()
{
  EventQueue events;

  QVERIFY(!events.dispatchEvent(Event(EventTypes::ClientDisconnected, this)));
}

void EventQueueTests::dispatchEvent_noTypeHandler_dispatchesUnknownHandler()
{
  EventQueue events;
  bool fallbackCalled = false;
  events.addHandler(EventTypes::Unknown, this, [&fallbackCalled](const Event &) { fallbackCalled = true; });

  QVERIFY(events.dispatchEvent(Event(EventTypes::ClientDisconnected, this)));
  QVERIFY(fallbackCalled);
}

void EventQueueTests::dispatchEvent_handlerRemovesItself_keepsHandlerAliveUntilReturn()
{
  EventQueue events;
  auto handlerLifetime = std::make_shared<int>(1);
  std::weak_ptr<int> handlerLifetimeObserver = handlerLifetime;
  bool handlerAliveAfterRemoval = false;

  events.addHandler(
      EventTypes::ClientDisconnected, this,
      [this, &events, &handlerLifetimeObserver, &handlerAliveAfterRemoval, handlerLifetime](const Event &) {
        events.removeHandler(EventTypes::ClientDisconnected, this);
        handlerAliveAfterRemoval = handlerLifetime != nullptr && !handlerLifetimeObserver.expired();
      }
  );
  handlerLifetime.reset();

  QVERIFY(events.dispatchEvent(Event(EventTypes::ClientDisconnected, this)));
  QVERIFY(handlerAliveAfterRemoval);
  QVERIFY(handlerLifetimeObserver.expired());
}

void EventQueueTests::removeEventsFor_queuedEvents_notDeliveredToNextTargetAtSameAddress()
{
  // this is the shape of the clipboard bug: a big clipboard is queued as one
  // event per chunk against a raw proxy pointer. if the connection drops
  // mid-transfer the backlog outlives the proxy, and the replacement proxy —
  // very often at the same address — inherits it and writes stale chunks into a
  // brand new connection.
  EventQueue events;

  int deadTarget = 0;
  void *target = &deadTarget;
  int delivered = 0;
  int otherDelivered = 0;
  int otherTarget = 0;

  // run the interesting part from inside the loop, so the events land in the
  // live event table rather than the not-yet-ready pending queue
  events.addHandler(EventTypes::ClientConnected, this, [&](const Event &) {
    events.addEvent(Event(EventTypes::ClipboardSending, target));
    events.addEvent(Event(EventTypes::ClipboardSending, target));
    events.addEvent(Event(EventTypes::ClipboardSending, &otherTarget));
    events.addEvent(Event(EventTypes::ClipboardSending, target));

    events.removeEventsFor(target);

    // the replacement object lands on the same address and registers a handler
    events.addHandler(EventTypes::ClipboardSending, target, [&delivered](const Event &) { ++delivered; });
    events.addHandler(EventTypes::ClipboardSending, &otherTarget, [&otherDelivered](const Event &) {
      ++otherDelivered;
    });

    events.addEvent(Event(EventTypes::Quit, this));
  });
  events.addEvent(Event(EventTypes::ClientConnected, this));

  events.loop();

  QCOMPARE(delivered, 0);
  // an unrelated target queued in the same breath is untouched
  QCOMPARE(otherDelivered, 1);
}

void EventQueueTests::removeEventsFor_pendingEvents_discardedBeforeQueueIsReady()
{
  // events added before loop() starts sit in the pending queue; they must be
  // purged too, or they reach the buffer the moment the queue comes up
  EventQueue events;

  int deadTarget = 0;
  int liveTarget = 0;
  int deadDelivered = 0;
  int liveDelivered = 0;

  events.addEvent(Event(EventTypes::ClipboardSending, &deadTarget));
  events.addEvent(Event(EventTypes::ClipboardSending, &liveTarget));
  events.addEvent(Event(EventTypes::ClipboardSending, &deadTarget));

  events.removeEventsFor(&deadTarget);

  events.addHandler(EventTypes::ClipboardSending, &deadTarget, [&deadDelivered](const Event &) { ++deadDelivered; });
  events.addHandler(EventTypes::ClipboardSending, &liveTarget, [&liveDelivered](const Event &) { ++liveDelivered; });
  events.addEvent(Event(EventTypes::Quit, this));

  events.loop();

  QCOMPARE(deadDelivered, 0);
  QCOMPARE(liveDelivered, 1);
}

QTEST_MAIN(EventQueueTests)
