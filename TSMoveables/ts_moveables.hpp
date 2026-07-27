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

#include "moveable_atomic.hpp"              // IWYU pragma: export
#include "moveable_mutex.hpp"               // IWYU pragma: export
#include "moveable_spin_lock.hpp"           // IWYU pragma: export
#include "moveable_condition_variable.hpp"  // IWYU pragma: export
#include "moveable_once_flag.hpp"           // IWYU pragma: export
#include "moveable_semaphore.hpp"           // IWYU pragma: export
#include "moveable_latch.hpp"               // IWYU pragma: export
#include "moveable_barrier.hpp"             // IWYU pragma: export
#include "circular_buffer.hpp"              // IWYU pragma: export
#include "mpmc_queue.hpp"                   // IWYU pragma: export
#include "disruptor.hpp"                    // IWYU pragma: export
#include "moveable_signal.hpp"              // IWYU pragma: export
#include "logging.hpp"                      // IWYU pragma: export
#include "thread_pool.hpp"                  // IWYU pragma: export
#include "event_loop.hpp"                   // IWYU pragma: export (self-disables on Windows)
#include "time_master.hpp"                  // IWYU pragma: export (a scheduler on it)
#include "http_server.hpp"                  // IWYU pragma: export (follows the event loop)
#include "websocket.hpp"                    // IWYU pragma: export (a protocol delegate on it)
#include "synchronized.hpp"                 // IWYU pragma: export
#include "synchronized_heterogeneous.hpp"   // IWYU pragma: export

#endif /* ts_moveables_hpp */
