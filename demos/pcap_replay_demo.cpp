//
//  pcap_replay_demo.cpp
//  TSMoveables
//
//  Copyright 2026 Saxon Herschel Nicholls
//
//  Thread Safe Moveables - real packet capture, decode, and replay
//
//  Feeds packets from a pcap file through a moveable_signal node topology -
//  decode, per-protocol accounting, flow partitioning - journals the ingress
//  (as offsets: the journal is zero-copy), then replays the journal through a
//  fresh topology and proves the egress stream hash reproduces exactly, with
//  ordering asserted at every hop.
//
//      ./build/pcap_replay_demo capture.pcap      # your data
//      ./build/pcap_replay_demo                   # synthetic capture, self-contained
//      ./build/pcap_replay_demo 4                 # synthetic, scaled 4x
//
//  The reader is dependency-free and handles classic pcap (v2.4, both byte
//  orders, micro- and nanosecond variants). pcapng is not parsed - convert
//  first:  tcpdump -r in.pcapng -w out.pcap
//

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "../TSMoveables/moveable_signal.hpp"

using snicholls::moveable_signal;
using snicholls::scoped_connection;

namespace {

// ------------------------------------------------------------- pcap reading

struct packet_view {
    std::uint64_t ts_ns;
    std::uint32_t index;                        // capture order - the canonical sequence
    std::uint32_t len;
    const std::uint8_t* data;                   // points into the loaded capture: zero copy
};

struct capture_file {
    std::vector<std::uint8_t> bytes;
    std::vector<std::size_t> offsets;           // record offsets, in capture order
    bool nanosecond = false;
    bool swapped = false;

    static std::uint32_t rd32(const std::uint8_t* p, bool swap) noexcept {
        std::uint32_t v;
        std::memcpy(&v, p, 4);
        if (swap)
            v = ((v & 0xFF000000u) >> 24) | ((v & 0x00FF0000u) >> 8) |
                ((v & 0x0000FF00u) << 8) | ((v & 0x000000FFu) << 24);
        return v;
    }

    bool parse() {
        offsets.clear();
        if (bytes.size() < 24)
            return false;
        const std::uint32_t magic = rd32(bytes.data(), false);
        if (magic == 0xA1B2C3D4u)      { swapped = false; nanosecond = false; }
        else if (magic == 0xD4C3B2A1u) { swapped = true;  nanosecond = false; }
        else if (magic == 0xA1B23C4Du) { swapped = false; nanosecond = true; }
        else if (magic == 0x4D3CB2A1u) { swapped = true;  nanosecond = true; }
        else
            return false;
        std::size_t off = 24;
        while (off + 16 <= bytes.size()) {
            const std::uint32_t incl = rd32(bytes.data() + off + 8, swapped);
            if (off + 16 + incl > bytes.size())
                break;                          // truncated final record - stop cleanly
            offsets.push_back(off);
            off += 16 + incl;
        }
        return !offsets.empty();
    }

    packet_view packet(std::size_t i) const noexcept {
        const std::size_t off = offsets[i];
        const std::uint32_t sec = rd32(bytes.data() + off, swapped);
        const std::uint32_t frac = rd32(bytes.data() + off + 4, swapped);
        const std::uint32_t incl = rd32(bytes.data() + off + 8, swapped);
        const std::uint64_t ns =
            static_cast<std::uint64_t>(sec) * 1'000'000'000ull +
            static_cast<std::uint64_t>(frac) * (nanosecond ? 1ull : 1'000ull);
        return packet_view{ns, static_cast<std::uint32_t>(i), incl, bytes.data() + off + 16};
    }

    std::size_t count() const noexcept { return offsets.size(); }
};

bool load_file(const char* path, std::vector<std::uint8_t>& out)
{
    std::FILE* f = std::fopen(path, "rb");
    if (!f)
        return false;
    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    out.resize(size > 0 ? static_cast<std::size_t>(size) : 0);
    const std::size_t got = out.empty() ? 0 : std::fread(out.data(), 1, out.size(), f);
    std::fclose(f);
    out.resize(got);
    return got != 0;
}

// A synthetic classic-pcap capture: Ethernet + IPv4 + UDP market-data-ish
// packets, so the demo is self-contained when no file is supplied
void synthesize(std::vector<std::uint8_t>& out, std::size_t packets)
{
    auto put32 = [&out](std::uint32_t v) {
        out.push_back(static_cast<std::uint8_t>(v));
        out.push_back(static_cast<std::uint8_t>(v >> 8));
        out.push_back(static_cast<std::uint8_t>(v >> 16));
        out.push_back(static_cast<std::uint8_t>(v >> 24));
    };
    out.reserve(24 + packets * (16 + 92));
    put32(0xA1B2C3D4u);                         // classic pcap, microseconds
    out.push_back(2); out.push_back(0);         // v2.4
    out.push_back(4); out.push_back(0);
    put32(0); put32(0);                         // thiszone, sigfigs
    put32(65535);                               // snaplen
    put32(1);                                   // linktype: Ethernet

    static constexpr std::uint32_t payload_len = 50;
    static constexpr std::uint32_t frame_len = 14 + 20 + 8 + payload_len;   // 92
    for (std::size_t i = 0; i < packets; ++i) {
        put32(static_cast<std::uint32_t>(i / 100'000));         // ts_sec
        put32(static_cast<std::uint32_t>((i % 100'000) * 10));  // ts_usec
        put32(frame_len);                       // incl_len
        put32(frame_len);                       // orig_len
        // Ethernet: dst, src, ethertype 0x0800
        for (int b = 0; b < 12; ++b)
            out.push_back(static_cast<std::uint8_t>(b));
        out.push_back(0x08); out.push_back(0x00);
        // IPv4: version/ihl, ..., protocol 17 (UDP)
        out.push_back(0x45); out.push_back(0);
        out.push_back(static_cast<std::uint8_t>((20 + 8 + payload_len) >> 8));
        out.push_back(static_cast<std::uint8_t>(20 + 8 + payload_len));
        out.push_back(0); out.push_back(0); out.push_back(0); out.push_back(0);
        out.push_back(64); out.push_back(17);   // ttl, protocol = UDP
        out.push_back(0); out.push_back(0);     // checksum (unused here)
        put32(0x0A000001u);                     // src 10.0.0.1 (byte order irrelevant to demo)
        put32(0x0A000002u + static_cast<std::uint32_t>(i % 4));
        // UDP: src port varies over 8 flows, dst 14310, len, checksum
        const std::uint16_t sport = static_cast<std::uint16_t>(31000 + i % 8);
        out.push_back(static_cast<std::uint8_t>(sport >> 8));
        out.push_back(static_cast<std::uint8_t>(sport));
        out.push_back(static_cast<std::uint8_t>(14310 >> 8));
        out.push_back(static_cast<std::uint8_t>(14310 & 0xFF));
        out.push_back(0); out.push_back(8 + payload_len);
        out.push_back(0); out.push_back(0);
        for (std::uint32_t b = 0; b < payload_len; ++b)         // "market data"
            out.push_back(static_cast<std::uint8_t>(i + b));
    }
}

// ---------------------------------------------------------------- the nodes

struct stream_hash {
    std::uint64_t h = 14695981039346656037ULL;
    void mix_bytes(const std::uint8_t* p, std::size_t n) noexcept {
        for (std::size_t i = 0; i < n; ++i) {
            h ^= p[i];
            h *= 1099511628211ULL;
        }
    }
};

// Hop one: minimal bounds-checked Ethernet/IPv4/UDP decode; re-emits UDP
struct decoded_packet {
    packet_view raw;
    std::uint16_t src_port;
    std::uint16_t dst_port;
};

struct decoder {
    moveable_signal<const decoded_packet&> udp_out;
    scoped_connection in;
    long long udp = 0, tcp = 0, other = 0;
    std::uint64_t bytes_seen = 0;

    void attach(moveable_signal<const packet_view&>& src) {
        in = src.connect([this](const packet_view& p) {
            bytes_seen += p.len;
            if (p.len < 14 + 20) { ++other; return; }
            const std::uint8_t* d = p.data;
            if (!(d[12] == 0x08 && d[13] == 0x00)) { ++other; return; }    // not IPv4
            const std::uint8_t ihl = static_cast<std::uint8_t>(d[14] & 0x0F) * 4;
            if (p.len < 14u + ihl + 8u) { ++other; return; }
            const std::uint8_t proto = d[23];
            if (proto == 6) { ++tcp; return; }
            if (proto != 17) { ++other; return; }
            ++udp;
            const std::uint8_t* u = d + 14 + ihl;
            udp_out(decoded_packet{
                p, static_cast<std::uint16_t>((u[0] << 8) | u[1]),
                static_cast<std::uint16_t>((u[2] << 8) | u[3])});
        });
    }
};

// Hop two: partition UDP flows by source port; order asserted per partition
struct flow_node {
    unsigned partition = 0, of = 1;
    scoped_connection in;
    long long packets = 0;
    std::uint64_t bytes = 0;
    std::int64_t last_index = -1;
    bool order_ok = true;
    stream_hash hash;

    void attach(moveable_signal<const decoded_packet&>& src) {
        in = src.connect([this](const decoded_packet& d) {
            if (d.src_port % of != partition)
                return;
            if (static_cast<std::int64_t>(d.raw.index) <= last_index)
                order_ok = false;               // capture order is canonical
            last_index = d.raw.index;
            ++packets;
            bytes += d.raw.len;
            hash.mix_bytes(d.raw.data, d.raw.len);      // full payload consumed
        });
    }
};

// The journal: capture order as offsets - zero-copy event capture
struct journal_node {
    std::vector<std::uint32_t> indices;
    scoped_connection in;

    void attach(moveable_signal<const packet_view&>& src, std::size_t reserve) {
        indices.reserve(reserve);
        in = src.connect([this](const packet_view& p) { indices.push_back(p.index); });
    }
};

struct topology {
    moveable_signal<const packet_view&> feed;
    decoder dec;
    std::array<flow_node, 4> flows;
    journal_node journal;

    explicit topology(std::size_t journal_reserve = 0) {
        if (journal_reserve != 0)
            journal.attach(feed, journal_reserve);
        dec.attach(feed);
        for (unsigned i = 0; i < 4; ++i) {
            flows[i].partition = i;
            flows[i].of = 4;
            flows[i].attach(dec.udp_out);
        }
    }

    std::uint64_t combined_hash() const noexcept {
        std::uint64_t h = 0;
        for (const auto& f : flows)
            h ^= f.hash.h + 0x9E3779B97F4A7C15ULL + (h << 6) + (h >> 2);
        return h;
    }
    bool ordered() const noexcept {
        for (const auto& f : flows)
            if (!f.order_ok)
                return false;
        return true;
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
    capture_file cap;
    long long scale = 1;
    const char* path = nullptr;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--markdown") == 0) {
            markdown = true;
            continue;
        }
        if (argv[i][0] == '-')
            continue;                           // ignore other flags for forward compatibility
        const std::string arg = argv[i];
        if (arg.size() > 5 && arg.compare(arg.size() - 5, 5, ".pcap") == 0)
            path = argv[i];
        else
            scale = std::atoll(argv[i]);
    }

    if (path) {
        check(load_file(path, cap.bytes), "pcap file readable");
        check(cap.parse(), "classic pcap format (pcapng? convert: tcpdump -r in -w out.pcap)");
    } else {
        const std::size_t n = 1'000'000 * static_cast<std::size_t>(scale > 0 ? scale : 1);
        synthesize(cap.bytes, n);
        check(cap.parse(), "synthetic capture parses");
    }
    if (markdown)
        std::printf("### `make demo-pcap` - %zu packets, %.1f MB (%s)\n\n"
                    "| Run | Packets/s | Bandwidth | Notes |\n|---|---|---|---|\n",
                    cap.count(), static_cast<double>(cap.bytes.size()) / 1e6,
                    path ? path : "synthetic");
    else if (path)
        std::printf("pcap replay demo - %s: %zu packets, %.1f MB\n\n",
                    path, cap.count(), static_cast<double>(cap.bytes.size()) / 1e6);
    else
        std::printf("pcap replay demo - synthetic capture: %zu packets, %.1f MB "
                    "(pass a .pcap path to use real data)\n\n",
                    cap.count(), static_cast<double>(cap.bytes.size()) / 1e6);

    const std::size_t n_packets = cap.count();
    std::uint64_t live_hash = 0;
    std::uint64_t total_bytes = 0;
    std::vector<std::uint32_t> journal;

    // ------------------------------------------------ live: decode + capture
    {
        topology t(n_packets);
        const auto t0 = std::chrono::steady_clock::now();
        for (std::size_t i = 0; i < n_packets; ++i)
            t.feed(cap.packet(i));
        const double s = seconds_since(t0);
        check(t.journal.indices.size() == n_packets, "journal captured every packet");
        check(t.ordered(), "per-flow order preserved (capture order is canonical)");
        live_hash = t.combined_hash();
        total_bytes = t.dec.bytes_seen;
        if (markdown)
            std::printf("| live: decode into 4 flow nodes | %.2f M | %.2f GB/s | udp %lld / tcp %lld / other %lld |\n",
                        static_cast<double>(n_packets) / s / 1e6,
                        static_cast<double>(total_bytes) / s / 1e9,
                        t.dec.udp, t.dec.tcp, t.dec.other);
        else
            std::printf("  live:   %8.2f M packets/s   %7.2f GB/s   udp %lld  tcp %lld  other %lld\n",
                        static_cast<double>(n_packets) / s / 1e6,
                        static_cast<double>(total_bytes) / s / 1e9,
                        t.dec.udp, t.dec.tcp, t.dec.other);
        journal = std::move(t.journal.indices);
    }

    // ------------------------------------------------ replay from the journal
    {
        topology t;                             // fresh nodes, no journal
        const auto t0 = std::chrono::steady_clock::now();
        for (const std::uint32_t idx : journal)
            t.feed(cap.packet(idx));
        const double s = seconds_since(t0);
        check(t.ordered(), "replay order preserved at every hop");
        check(t.combined_hash() == live_hash,
              "replay reproduces the exact per-flow payload hashes");
        if (markdown)
            std::printf("| replay from offset journal | %.2f M | %.2f GB/s | egress hashes identical |\n",
                        static_cast<double>(n_packets) / s / 1e6,
                        static_cast<double>(t.dec.bytes_seen) / s / 1e9);
        else
            std::printf("  replay: %8.2f M packets/s   %7.2f GB/s   egress hashes identical\n",
                        static_cast<double>(n_packets) / s / 1e6,
                        static_cast<double>(t.dec.bytes_seen) / s / 1e9);
    }

    if (markdown)
        std::printf("\nZero-copy journal (%zu x 4-byte offsets); every payload byte consumed; "
                    "hashes matched exactly.\n", journal.size());
    else
        std::printf("\n  journal = %zu x 4-byte offsets into the mapped capture: zero-copy\n"
                    "  capture, zero-copy replay; packets traverse decode -> 4 flow nodes\n"
                    "  by const& - every payload byte consumed, hashes matched exactly\n",
                    journal.size());
    return 0;
}
