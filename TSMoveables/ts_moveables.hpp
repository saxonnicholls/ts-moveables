//
//  ts_moveables.hpp
//  TSMoveables
//
//  Created by Saxon Nicholls on 21/7/2026.
//
//  Copyright 2026 Saxon Herschel Nicholls
//
//  Thread Safe Moveables - umbrella header
//

#ifndef ts_moveables_hpp
#define ts_moveables_hpp

#include "moveable/atomic.hpp"              // IWYU pragma: export
#include "moveable/mutex.hpp"               // IWYU pragma: export
#include "moveable/spin_lock.hpp"           // IWYU pragma: export
#include "moveable/condition_variable.hpp"  // IWYU pragma: export
#include "moveable/once_flag.hpp"           // IWYU pragma: export
#include "moveable/semaphore.hpp"           // IWYU pragma: export
#include "moveable/latch.hpp"               // IWYU pragma: export
#include "moveable/barrier.hpp"             // IWYU pragma: export
#include "concurrent/circular_buffer.hpp"              // IWYU pragma: export
#include "concurrent/mpmc_queue.hpp"                   // IWYU pragma: export
#include "concurrent/disruptor.hpp"                    // IWYU pragma: export
#include "moveable/signal.hpp"              // IWYU pragma: export
#include "logging/logger.hpp"                      // IWYU pragma: export
#include "concurrent/thread_pool.hpp"                  // IWYU pragma: export
#include "event/loop.hpp"                   // IWYU pragma: export (self-disables on Windows)
#include "event/time_master.hpp"                  // IWYU pragma: export (a scheduler on it)
#include "http/server.hpp"                  // IWYU pragma: export (follows the event loop)
#include "http/websocket.hpp"                    // IWYU pragma: export (a protocol delegate on it)
#include "concurrent/synchronized.hpp"                 // IWYU pragma: export
#include "concurrent/synchronized_heterogeneous.hpp"   // IWYU pragma: export

#endif /* ts_moveables_hpp */
