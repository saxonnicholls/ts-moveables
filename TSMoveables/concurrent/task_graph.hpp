//
//  task_graph.hpp
//  TSMoveables
//
//  Copyright 2010-2026 Saxon Herschel Nicholls
//
//  Thread Safe Moveables - a dependency graph, executed level by level
//
//      snicholls::task_graph g(pool);
//      const auto fetch = g.add([&] { data = fetch_it(); });
//      const auto parse = g.add([&] { tree = parse(data); });
//      const auto emit  = g.add([&] { write(tree); });
//      g.precede(fetch, parse);
//      g.precede(parse, emit);
//      g.run();                       // fetch, then parse, then emit
//
//  `precede` is spelled the way stl-topological-sorting spells it, because it
//  is the same relation and there is no reason for the same author's two
//  libraries to disagree about the verb.
//
//  ------------------------------------------- what this is, and what it is not
//
//  A sequential topological sort answers "in what ORDER may these run?" and
//  hands back one valid linearisation. This answers a different question -
//  "which of these may run AT THE SAME TIME?" - and hands back levels.
//
//  A level is an antichain: every node in it has all of its dependencies
//  satisfied by earlier levels and none by its neighbours, so the whole level
//  can run at once. That set is the useful product. The linear order is a
//  by-product you get by concatenating the levels, and if a linear order is
//  all you want then a sequential sort is the better tool and this is the wrong
//  file.
//
//  So this is deliberately NOT a graph library. There is no traversal, no
//  shortest path, no components - just enough structure to say what can run
//  together, and a runner that does it.
//
//  ------------------------------------------------------- the ceiling, up front
//
//  Level-synchronous execution has two hard limits, and they are properties of
//  the GRAPH rather than of the machine:
//
//    width   the largest level. This is the most parallelism that exists. Eight
//            cores do not help a graph whose widest level is three.
//    depth   the number of levels, i.e. the critical path. This is a serial
//            floor: a 10,000-node chain has depth 10,000 and width 1, and runs
//            at exactly serial speed on any number of cores.
//
//  Every level also pays a join, so a deep, narrow graph can finish SLOWER than
//  running the nodes in order on one thread. depth() and width() are public for
//  exactly this reason - they are the two numbers that explain a disappointing
//  speedup, and a benchmark that reports the speedup without them is not
//  reporting anything.
//
//  The other parallel Kahn - one shared frontier queue, in-degrees decremented
//  atomically as each node finishes - is deliberately not what this does. It
//  removes the per-level join, and pays for it with every worker contending on
//  the one hot counter that every finishing node must touch. On a multicore box
//  the level-synchronous version is simpler and usually faster; the frontier
//  version earns its keep on graphs far more irregular than the ones a task
//  scheduler sees.
//
//  Computing the levels is O(V+E) and SEQUENTIAL, on purpose. It is bookkeeping
//  over the graph, not over the work, and in any graph worth running in
//  parallel the nodes cost enormously more than the edges. Parallelising it
//  would add contention to something that is already noise.
//

#ifndef task_graph_hpp
#define task_graph_hpp

#include <cstddef>
#include <functional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "../interfaces/task_pool.hpp"
#include "parallel_for.hpp"

namespace snicholls
{
    // Thrown by levels() and run() when the graph has a cycle. Loud, and with
    // the count, because "your graph has a cycle" without saying how much of it
    // is stuck sends you reading the whole thing.
    class task_graph_cycle : public std::runtime_error
    {
    public:
        explicit task_graph_cycle(std::size_t stuck)
            : std::runtime_error("task_graph: cycle - " + std::to_string(stuck) +
                                 " node(s) never reach in-degree zero"),
              stuck_(stuck) {}

        std::size_t stuck() const noexcept { return stuck_; }

    private:
        std::size_t stuck_;
    };

    class task_graph
    {
    public:
        using node_id = std::size_t;

        explicit task_graph(task_pool& pool) : pool_(&pool) {}

        // Moveable, like everything else here: a graph is a value you build in
        // one place and run in another. Nothing outside holds a node's address,
        // so there is no contract to break by moving one - and run() blocks, so
        // a graph cannot be moved while it is executing.
        task_graph(task_graph&&) noexcept = default;
        task_graph& operator=(task_graph&&) noexcept = default;
        task_graph(const task_graph&) = delete;
        task_graph& operator=(const task_graph&) = delete;

        // A node with work, or without: an empty node is a join point, and a
        // graph of empty nodes is a pure dependency analysis with nothing to
        // run. levels() works either way.
        node_id add(std::function<void()> work = {})
        {
            work_.push_back(std::move(work));
            succ_.emplace_back();
            indeg_.push_back(0);
            dirty_ = true;
            return work_.size() - 1;
        }

        // `before` must finish before `after` starts. Self-edges are refused
        // rather than silently producing an unsatisfiable graph that only
        // reports itself much later as a cycle.
        void precede(node_id before, node_id after)
        {
            check(before);
            check(after);
            if (before == after)
                throw std::invalid_argument("task_graph: a node cannot precede itself");
            succ_[before].push_back(after);
            ++indeg_[after];
            dirty_ = true;
        }

        std::size_t size() const noexcept { return work_.size(); }

        // The antichains, in execution order. Throws task_graph_cycle if the
        // graph is not a DAG.
        const std::vector<std::vector<node_id>>& levels() const
        {
            if (dirty_)
                build_levels();
            return levels_;
        }

        // The critical path: no schedule can be shorter than this many steps.
        std::size_t depth() const { return levels().size(); }

        // The widest level: the most parallelism the graph actually contains.
        std::size_t width() const
        {
            std::size_t w = 0;
            for (const auto& lv : levels())
                w = (lv.size() > w) ? lv.size() : w;
            return w;
        }

        // Run every node, one level at a time, each level in parallel.
        //
        // Exceptions follow parallel_for: the first one is rethrown here once
        // the rest of its level has settled, and no LATER level runs at all -
        // which is the only sane answer, since those nodes were waiting on the
        // one that failed.
        void run()
        {
            const auto& lv = levels();
            for (const auto& level : lv) {
                parallel_for(*pool_, std::size_t{0}, level.size(),
                             [this, &level](std::size_t i) {
                                 const auto& fn = work_[level[i]];
                                 if (fn)
                                     fn();
                             },
                             1);                // one node per chunk
            }
        }

    private:
        void check(node_id n) const
        {
            if (n >= work_.size())
                throw std::out_of_range("task_graph: no such node");
        }

        // Kahn by waves. The cycle check is the same count Kahn always gives
        // you: anything never reaching in-degree zero is in one, or downstream
        // of one.
        void build_levels() const
        {
            levels_.clear();
            std::vector<std::size_t> remaining = indeg_;
            std::vector<node_id> current;

            for (node_id n = 0; n < work_.size(); ++n)
                if (remaining[n] == 0)
                    current.push_back(n);

            std::size_t placed = 0;
            while (!current.empty()) {
                placed += current.size();
                std::vector<node_id> next;
                for (const node_id n : current)
                    for (const node_id s : succ_[n])
                        if (--remaining[s] == 0)
                            next.push_back(s);
                levels_.push_back(std::move(current));
                current = std::move(next);
            }

            if (placed != work_.size()) {
                levels_.clear();
                throw task_graph_cycle(work_.size() - placed);
            }
            dirty_ = false;
        }

        task_pool*                            pool_;
        std::vector<std::function<void()>>    work_;
        std::vector<std::vector<node_id>>     succ_;
        std::vector<std::size_t>              indeg_;

        // levels() is logically const - it is a query - but caches. The cache
        // is rebuilt on the next query after any edit rather than eagerly, so
        // building a graph stays O(1) per edge.
        mutable std::vector<std::vector<node_id>> levels_;
        mutable bool                              dirty_ = true;
    };
} // namespace snicholls

#endif /* task_graph_hpp */
