//
//  taskflow_style_demo.cpp
//  TSMoveables
//
//  Copyright 2026 Saxon Herschel Nicholls
//
//  Thread Safe Moveables - Taskflow-style dependency graphs on signals
//
//  Taskflow (taskflow.github.io, Dr. Tsung-Wei Huang et al.) executes task
//  dependency graphs on a worker-pool scheduler. This demo replicates its
//  core static patterns - dependencies, fan-out/fan-in joins, graph reuse,
//  pipelines - on moveable_signal, in about forty lines of node code, with a
//  property worth noticing: there is no scheduler. A task runs inline on the
//  thread that completes its last dependency, so no thread ever parks waiting
//  to be assigned work - thread starvation is designed out rather than
//  managed. Computation itself travels through the graph too: jobs go in as
//  events, results come back as events.
//
//  Build and run:   make demo-taskflow          (compiled -O3 -DNDEBUG)
//

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "../TSMoveables/moveable_signal.hpp"

using snicholls::moveable_signal;
using snicholls::scoped_connection;

namespace {

// ------------------------------------------------------- the task-node layer
//
// A task fires when ALL of its dependencies have fired - a fan-in join
// counter on the done signals. The last-arriving dependency's thread runs the
// task inline and emits done, which may run successors inline in turn.
// One wave at a time per graph instance (as with taskflow's run()).

struct task_node {
    std::function<void()> work;
    moveable_signal<> done;
    std::vector<scoped_connection> deps;
    std::atomic<int> pending{0};
    int fan_in = 0;

    explicit task_node(std::function<void()> w = {}) : work(std::move(w)) {}

    void depends_on(task_node& d) {
        ++fan_in;
        deps.emplace_back(d.done.connect([this] {
            if (pending.fetch_add(1, std::memory_order_acq_rel) + 1 == fan_in) {
                pending.store(0, std::memory_order_relaxed);    // ready for the next wave
                fire();
            }
        }));
    }

    void fire() {                               // sources are fired directly
        if (work)
            work();
        done();
    }
};

bool markdown = false;                      // --markdown: emit a GitHub-flavoured table

void check(bool ok, const char* what)
{
    if (!ok) {
        std::fprintf(stderr, "FAILED: %s\n", what);
        std::exit(1);
    }
}

double seconds_since(std::chrono::steady_clock::time_point t0)
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

} // namespace

int main(int argc, char** argv)
{
    for (int i = 1; i < argc; ++i)
        if (std::strcmp(argv[i], "--markdown") == 0)
            markdown = true;

    if (markdown)
        std::printf("### `make demo-taskflow` - dependency graphs on signals\n\n"
                    "| Scenario | Result |\n|---|---|\n");
    else
        std::printf("taskflow-style demo - dependency graphs on moveable_signal\n\n");

    // ------------------------------------------- 1. the classic diamond, reused
    // A -> (B, C) -> D, run three times on the same graph - taskflow's
    // executor.run(3) equivalent, except the graph runs itself
    {
        std::vector<std::string> order;
        task_node A([&] { order.push_back("A"); });
        task_node B([&] { order.push_back("B"); });
        task_node C([&] { order.push_back("C"); });
        task_node D([&] { order.push_back("D"); });
        B.depends_on(A);
        C.depends_on(A);
        D.depends_on(B);
        D.depends_on(C);

        for (int run = 0; run < 3; ++run)
            A.fire();

        check(order.size() == 12, "diamond ran three full waves");
        for (int run = 0; run < 3; ++run) {
            const auto* w = &order[static_cast<std::size_t>(run * 4)];
            check(w[0] == "A" && w[3] == "D" &&
                  ((w[1] == "B" && w[2] == "C") || (w[1] == "C" && w[2] == "B")),
                  "diamond dependency order respected (A first, D after both)");
        }
        if (markdown)
            std::printf("| diamond A->(B,C)->D, 3 reused waves | order respected on every wave |\n");
        else
            std::printf("  diamond A->(B,C)->D, 3 reused waves: order respected on every wave\n");
    }

    // ------------------------------------------- 2. wide fan-out / fan-in DAG
    // source -> 64 parallel tasks -> sink, driven for many waves: the join
    // counter is the whole "scheduler"
    {
        static constexpr int width = 64;
        static constexpr int waves = 100'000;

        long long executed = 0;
        long long sink_runs = 0;
        task_node source;
        std::array<task_node, width> middle;
        task_node sink([&] { ++sink_runs; });
        for (auto& m : middle) {
            m.work = [&] { ++executed; };
            m.depends_on(source);
            sink.depends_on(m);
        }

        const auto t0 = std::chrono::steady_clock::now();
        for (int w = 0; w < waves; ++w)
            source.fire();
        const double s = seconds_since(t0);

        check(executed == static_cast<long long>(width) * waves, "every middle task ran every wave");
        check(sink_runs == waves, "sink joined all 64 dependencies every wave");
        if (markdown)
            std::printf("| fan-out/fan-in 1->64->1 | %.2f M task executions/s (%.0f waves/s) |\n",
                        static_cast<double>(executed + 2 * waves) / s / 1e6,
                        static_cast<double>(waves) / s);
        else
            std::printf("  fan-out/fan-in 1->64->1: %.2f M task executions/s (%.0f waves/s)\n",
                        static_cast<double>(executed + 2 * waves) / s / 1e6,
                        static_cast<double>(waves) / s);
    }

    // ------------------------------------------- 3. independent graphs in parallel
    // Four diamond+chain graphs on four threads: shared-nothing, so the
    // inline execution model scales without a scheduler to contend on
    {
        static constexpr int n_graphs = 4;
        static constexpr int waves = 200'000;

        struct graph {
            task_node a, b, c, d;
            long long runs = 0;
            graph() {
                d.work = [this] { ++runs; };
                b.depends_on(a);
                c.depends_on(a);
                d.depends_on(b);
                d.depends_on(c);
            }
        };
        std::array<graph, n_graphs> graphs;

        const auto t0 = std::chrono::steady_clock::now();
        std::vector<std::thread> threads;
        for (int g = 0; g < n_graphs; ++g)
            threads.emplace_back([&graphs, g] {
                for (int w = 0; w < waves; ++w)
                    graphs[static_cast<std::size_t>(g)].a.fire();
            });
        for (auto& t : threads)
            t.join();
        const double s = seconds_since(t0);

        for (const auto& g : graphs)
            check(g.runs == waves, "each graph completed every wave");
        if (markdown)
            std::printf("| 4 independent diamonds on 4 threads | %.2f M waves/s total |\n",
                        static_cast<double>(n_graphs) * waves / s / 1e6);
        else
            std::printf("  4 independent diamonds on 4 threads: %.2f M waves/s total - no scheduler,\n"
                        "  no parked threads, nothing shared to contend on\n",
                        static_cast<double>(n_graphs) * waves / s / 1e6);
    }

    // ------------------------------------------- 4. pipeline: tokens through stages
    // Taskflow's pipeline pattern - a token stream through 4 stages, fed by 4
    // producer threads concurrently. Each token rides its producing thread
    // through every stage; per-producer order is asserted at the end.
    {
        static constexpr int n_stages = 4;
        static constexpr int n_producers = 4;
        static constexpr long long tokens_per_producer = 500'000;

        struct token { int producer; std::int64_t seq; int hops; };
        struct stage {
            moveable_signal<const token&> out;
            scoped_connection in;
            std::atomic<long long> processed{0};
            void attach(moveable_signal<const token&>& src) {
                in = src.connect([this](const token& t) {
                    processed.fetch_add(1, std::memory_order_relaxed);
                    token u = t;
                    ++u.hops;
                    out(u);
                });
            }
        };

        moveable_signal<const token&> ingress;
        std::array<stage, n_stages> stages;
        stages[0].attach(ingress);
        for (int i = 1; i < n_stages; ++i)
            stages[static_cast<std::size_t>(i)].attach(stages[static_cast<std::size_t>(i - 1)].out);

        std::array<std::int64_t, n_producers> last{};   // each entry owned by one thread
        last.fill(-1);
        std::atomic<bool> order_ok{true};
        std::atomic<long long> completed{0};
        scoped_connection collect = stages[n_stages - 1].out.connect([&](const token& t) {
            if (t.hops != n_stages || t.seq <= last[static_cast<std::size_t>(t.producer)])
                order_ok.store(false, std::memory_order_relaxed);
            last[static_cast<std::size_t>(t.producer)] = t.seq;
            completed.fetch_add(1, std::memory_order_relaxed);
        });

        const auto t0 = std::chrono::steady_clock::now();
        std::vector<std::thread> threads;
        for (int p = 0; p < n_producers; ++p)
            threads.emplace_back([&ingress, p] {
                for (std::int64_t i = 0; i < tokens_per_producer; ++i)
                    ingress(token{p, i, 0});
            });
        for (auto& t : threads)
            t.join();
        const double s = seconds_since(t0);

        check(completed.load() == n_producers * tokens_per_producer, "every token completed the pipeline");
        check(order_ok.load(), "per-producer order preserved through all stages");
        for (const auto& st : stages)
            check(st.processed.load() == n_producers * tokens_per_producer, "every stage saw every token");
        if (markdown)
            std::printf("| 4-stage pipeline, 4 concurrent producers | %.2f M tokens/s, per-producer FIFO verified |\n",
                        static_cast<double>(completed.load()) / s / 1e6);
        else
            std::printf("  4-stage pipeline, 4 concurrent producers: %.2f M tokens/s end to end,\n"
                        "  per-producer FIFO order verified at the final stage\n",
                        static_cast<double>(completed.load()) / s / 1e6);
    }

    // ------------------------------------------- 5. computation as the event
    // Jobs (a value plus the code to run on it) flow in on one signal; results
    // flow out on another - work moves to the node, answers move back
    {
        struct job { int id; int x; std::function<int(int)> fn; };
        struct answer { int id; int y; };

        moveable_signal<const job&> submit;
        moveable_signal<const answer&> results;
        scoped_connection compute = submit.connect([&results](const job& j) {
            results(answer{j.id, j.fn(j.x)});
        });

        long long sum = 0, received = 0;
        scoped_connection collect = results.connect([&](const answer& a) {
            sum += a.y;
            ++received;
        });

        static constexpr int n_jobs = 1'000'000;
        const auto t0 = std::chrono::steady_clock::now();
        long long expected = 0;
        for (int i = 0; i < n_jobs; ++i) {
            const int x = i % 1000;             // keep x*x inside int range
            if (i % 2 == 0) {
                expected += x * x;
                submit(job{i, x, [](int v) { return v * v; }});
            } else {
                expected += x + 3;
                submit(job{i, x, [](int v) { return v + 3; }});
            }
        }
        const double s = seconds_since(t0);

        check(received == n_jobs, "every job produced an answer");
        check(sum == expected, "every computation ran correctly");
        if (markdown)
            std::printf("| computation-as-event round trip | %.2f M jobs/s |\n",
                        static_cast<double>(n_jobs) / s / 1e6);
        else
            std::printf("  computation-as-event: %.2f M jobs/s round-tripped (submit -> compute -> result)\n",
                        static_cast<double>(n_jobs) / s / 1e6);
    }

    if (markdown)
        std::printf("\nNo scheduler: a task runs on the thread that completes its last dependency.\n");
    else
        std::printf("\n  dependencies, joins, graph reuse, pipelines and work-passing - the\n"
                    "  static-graph heart of the taskflow model - with no scheduler at all:\n"
                    "  a task runs on the thread that completes its last dependency\n");
    return 0;
}
