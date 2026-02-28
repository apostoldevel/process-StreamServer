#pragma once

// ─── StreamServer ────────────────────────────────────────────────────────────
//
// Universal UDP module. Binds a non-blocking UDP socket in on_start() and
// registers it with EventLoop. Each incoming datagram triggers the virtual
// on_datagram() callback.
//
// PgStreamServer extends this with PG stream.parse() dispatch — mirrors v1
// CStreamServer behavior (S228 protocol and similar).
//
// Unlike v1, HTTP and UDP work simultaneously: the module creates its own
// UDP socket alongside the HTTP listener.

#include "apostol/module.hpp"
#include "apostol/event_loop.hpp"
#include "apostol/logger.hpp"
#include "apostol/udp.hpp"

#ifdef WITH_POSTGRESQL
#include "apostol/pg.hpp"
#endif

#include <memory>
#include <string>
#include <string_view>

namespace apostol
{

class Application;

class StreamServer : public Module
{
public:
    explicit StreamServer(Application& app);

    std::string_view name() const override { return "StreamServer"; }
    bool enabled() const override { return enabled_; }
    bool execute(const HttpRequest&, HttpResponse&) override { return false; }

    void on_start() override;
    void on_stop() override;

    UdpSocket* udp_socket() { return udp_.get(); }

protected:
    /// Called for each incoming UDP datagram.
    /// Override for custom processing. Default: debug log only.
    virtual void on_datagram(const UdpDatagram& dgram);

    /// Debug hex dump (mirrors v1 CStreamServer::Debug).
    void debug_log(const UdpDatagram& dgram);

    EventLoop& loop_;
    Logger&    logger_;

private:
    uint16_t port_;
    bool     enabled_;
    std::unique_ptr<UdpSocket> udp_;
};

#ifdef WITH_POSTGRESQL

/// StreamServer + PG stream.parse() — mirrors v1 CStreamServer.
class PgStreamServer : public StreamServer
{
public:
    explicit PgStreamServer(Application& app);

protected:
    void on_datagram(const UdpDatagram& dgram) override;

private:
    PgPool&     pool_;
    std::string protocol_;
};

#endif // WITH_POSTGRESQL

} // namespace apostol
