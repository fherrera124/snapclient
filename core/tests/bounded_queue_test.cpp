#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <thread>

#include "snapclient/BoundedQueue.h"

namespace {

int gFailures = 0;

void checkImpl(bool cond, const char* expr, const char* file, int line) {
  if (!cond) {
    std::fprintf(stderr, "FAIL %s:%d: %s\n", file, line, expr);
    gFailures++;
  }
}

}  // namespace

#define CHECK(cond) checkImpl((cond), #cond, __FILE__, __LINE__)

namespace snapclient {
namespace {

using std::chrono::milliseconds;
using std::chrono::steady_clock;

int64_t msSince(steady_clock::time_point start) {
  return std::chrono::duration_cast<milliseconds>(steady_clock::now() - start)
      .count();
}

void testZeroTimeoutNeverWaits() {
  BoundedQueue<int> queue(4);
  // Late push only so a regression returns instead of hanging.
  std::thread late([&] {
    std::this_thread::sleep_for(milliseconds(500));
    queue.tryPush(1);
  });

  int out = 0;
  const auto start = steady_clock::now();
  CHECK(!queue.tryPop(out, 0));
  CHECK(msSince(start) < 250);
  late.join();
}

void testEveryWaiterGetsAnItem() {
  BoundedQueue<int> queue(4);
  std::atomic<int> popped{0};
  auto waiter = [&] {
    int out = 0;
    if (queue.tryPop(out, 1000)) {
      ++popped;
    }
  };
  std::thread first(waiter);
  std::thread second(waiter);
  std::this_thread::sleep_for(milliseconds(100));

  CHECK(queue.tryPush(1));
  CHECK(queue.tryPush(2));
  first.join();
  second.join();
  CHECK(popped == 2);
  CHECK(queue.size() == 0);
}

void testEmptyQueueWaitsTheFullTimeout() {
  BoundedQueue<int> queue(8);
  int out = 0;
  for (int i = 0; i < 5; ++i) {
    CHECK(queue.tryPush(i));
  }
  for (int i = 0; i < 5; ++i) {
    CHECK(queue.tryPop(out, 0));
    CHECK(out == i);
  }

  for (int i = 0; i < 3; ++i) {
    const auto start = steady_clock::now();
    CHECK(!queue.tryPop(out, 100));
    CHECK(msSince(start) >= 90);
  }
}

void testTryPopIfStopsAtTheFirstItemThatFails() {
  BoundedQueue<int> queue(8);
  for (int value : {1, 2, 3, 10, 4}) {
    CHECK(queue.tryPush(value));
  }
  auto small = [](int value) { return value < 5; };
  int out = 0;
  int popped = 0;
  while (queue.tryPopIf(out, small)) {
    ++popped;
  }
  CHECK(popped == 3);
  CHECK(queue.size() == 2);
  CHECK(queue.tryPop(out, 0));
  CHECK(out == 10);
}

void testTryPopIfNeverWaits() {
  BoundedQueue<int> queue(4);
  int out = 0;
  const auto start = steady_clock::now();
  CHECK(!queue.tryPopIf(out, [](int) { return true; }));
  CHECK(msSince(start) < 50);
}

void testTryPopIfReleasesAWaitingPush() {
  BoundedQueue<int> queue(1);
  queue.push(1);
  std::thread producer([&] { queue.push(2); });
  std::this_thread::sleep_for(milliseconds(50));

  int out = 0;
  CHECK(queue.tryPopIf(out, [](int) { return true; }));
  CHECK(out == 1);
  producer.join();
  CHECK(queue.size() == 1);
}

void testTryPushRefusesWhenFull() {
  BoundedQueue<int> queue(2);
  CHECK(queue.tryPush(1));
  CHECK(queue.tryPush(2));
  CHECK(!queue.tryPush(3));
  CHECK(queue.size() == 2);
}

void testPushWaitsForRoom() {
  BoundedQueue<int> queue(1);
  queue.push(1);
  std::atomic<bool> pushed{false};
  std::thread producer([&] {
    queue.push(2);
    pushed = true;
  });
  std::this_thread::sleep_for(milliseconds(50));
  CHECK(!pushed);

  int out = 0;
  CHECK(queue.tryPop(out, 0));
  CHECK(out == 1);
  producer.join();
  CHECK(queue.tryPop(out, 0));
  CHECK(out == 2);
}

void testRaisingCapacityReleasesAWaitingPush() {
  BoundedQueue<int> queue(1);
  queue.push(1);
  std::atomic<bool> pushed{false};
  std::thread producer([&] {
    queue.push(2);
    pushed = true;
  });
  std::this_thread::sleep_for(milliseconds(50));
  CHECK(!pushed);

  queue.setCapacity(2);
  producer.join();
  CHECK(queue.size() == 2);
}

void testClearReleasesAWaitingPush() {
  BoundedQueue<int> queue(1);
  queue.push(1);
  std::thread producer([&] { queue.push(2); });
  std::this_thread::sleep_for(milliseconds(50));

  queue.clear();
  producer.join();
  int out = 0;
  CHECK(queue.tryPop(out, 0));
  CHECK(out == 2);
}

}  // namespace
}  // namespace snapclient

int main() {
  snapclient::testZeroTimeoutNeverWaits();
  snapclient::testEveryWaiterGetsAnItem();
  snapclient::testEmptyQueueWaitsTheFullTimeout();
  snapclient::testTryPopIfStopsAtTheFirstItemThatFails();
  snapclient::testTryPopIfNeverWaits();
  snapclient::testTryPopIfReleasesAWaitingPush();
  snapclient::testTryPushRefusesWhenFull();
  snapclient::testPushWaitsForRoom();
  snapclient::testRaisingCapacityReleasesAWaitingPush();
  snapclient::testClearReleasesAWaitingPush();

  if (gFailures != 0) {
    std::fprintf(stderr, "%d check(s) failed\n", gFailures);
    return 1;
  }
  std::printf("all tests passed\n");
  return 0;
}
