/*
  Copyright (c) DataStax, Inc.

  Licensed under the Apache License, Version 2.0 (the "License");
  you may not use this file except in compliance with the License.
  You may obtain a copy of the License at

  http://www.apache.org/licenses/LICENSE-2.0

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.
*/

#ifndef __CASS_REQUEST_QUEUE_HPP_INCLUDED__
#define __CASS_REQUEST_QUEUE_HPP_INCLUDED__

#include "async.hpp"
#include "dense_hash_map.hpp"
#include "dense_hash_set.hpp"
#include "mpmc_queue.hpp"
#include "scoped_ptr.hpp"
#include "timer.hpp"

namespace cass {

class EventLoop;
class EventLoopGroup;
class RequestCallback;
class PooledConnection;

/**
 * A request queue that coalesces flushes to reduce the number system calls
 * required to process requests.
 */
class RequestQueue {
public:
  RequestQueue();
  /**
   * Initialize the request queue's libuv handles.
   *
   * @return Returns 0 if successful, otherwise an error occurred.
   */
  int init(EventLoop* event_loop, size_t queue_size);

  /**
   * Close the request queue's libuv handles (thread-safe).
   */
  void close_handles();

  /**
   * Queue a request to be written on a connection (thread-safe).
   *
   * @param connection The connection where the request is to be written.
   * @param callback The callback that handles the request.
   * @return Returns true if the request is queue, otherwise the queue is full.
   */
  bool write(PooledConnection* connection, RequestCallback* callback);

private:
  static void on_flush(Async* async);
  static void on_flush_timer(Timer *timer);
  void handle_flush();

private:
  typedef DenseHashSet<PooledConnection*> Set;

  struct Item {
    PooledConnection* connection;
    RequestCallback* callback;
  };

private:
  Atomic<bool> is_flushing_;
  Atomic<bool> is_closing_;
  int flushes_without_writes_;
  Set connections_;
  ScopedPtr<MPMCQueue<Item> > queue_;
  Async async_;
  Timer timer_;
};

/**
 * A manager that handles mapping request queues to event loop threads.
 */
class RequestQueueManager {
public:
  /**
   * Constructor
   *
   * @param event_loop_group The event loop group that will process the request
   * queues.
   */
  RequestQueueManager(EventLoopGroup* event_loop_group);

  /**
   * Initialize the request queues.
   *
   * @return Returns 0 if successful, otherwise an error occurred.
   */
  int init(size_t queue_size);


  /**
   * Close all libuv handles for all request queues (thread-safe).
   */
  void close_handles();

  /**
   * Get the request queue for a given event loop (thread-safe).
   *
   * @param event_loop The current event loop.
   * @return Returns the request queue that's handling requests for the given
   * event loop.
   */
  RequestQueue* get(EventLoop* event_loop) const;

public:
  EventLoopGroup* event_loop_group() const { return event_loop_group_; }

private:
  typedef DenseHashMap<EventLoop*, RequestQueue*> Map;

private:
  Map request_queues_;
  DynamicArray<RequestQueue> storage_;
  EventLoopGroup* const event_loop_group_;
};

} // namespace cass

#endif
