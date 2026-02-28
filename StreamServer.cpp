#include "StreamServer.hpp"
#include "apostol/application.hpp"

#include <sys/epoll.h>

#include <fmt/format.h>

#ifdef WITH_POSTGRESQL
#include "apostol/base64.hpp"
#include "apostol/pg_utils.hpp"
#endif

namespace apostol
{

// ─── StreamServer ────────────────────────────────────────────────────────────

StreamServer::StreamServer(Application& app)
    : loop_(app.worker_loop())
    , logger_(app.stream_logger())
    , port_(0)
    , enabled_(app.module_enabled("StreamServer"))
{
    if (auto* cfg = app.module_config("StreamServer")) {
        if (cfg->contains("port"))
            port_ = (*cfg)["port"].get<uint16_t>();
    }
}

void StreamServer::on_start()
{
    if (!enabled_ || port_ == 0)
        return;

    udp_ = std::make_unique<UdpSocket>(port_);
    logger_.notice("[StreamServer] UDP listening on port {}", udp_->local_port());

    loop_.add_io(udp_->fd(), EPOLLIN, [this](uint32_t) {
        while (auto dgram = udp_->recv()) {
            on_datagram(*dgram);
        }
    });
}

void StreamServer::on_stop()
{
    if (udp_) {
        loop_.remove_io(udp_->fd());
        udp_.reset();
        logger_.notice("[StreamServer] UDP stopped");
    }
}

void StreamServer::on_datagram(const UdpDatagram& dgram)
{
    debug_log(dgram);
}

void StreamServer::debug_log(const UdpDatagram& dgram)
{
    // Hex dump: "AA BB CC DD ..." — mirrors v1 ByteToHexStr with space delimiter
    std::string hex;
    hex.reserve(dgram.data.size() * 3);

    for (std::size_t i = 0; i < dgram.data.size(); ++i) {
        if (i > 0)
            hex += ' ';
        fmt::format_to(std::back_inserter(hex), "{:02X}",
                       static_cast<unsigned char>(dgram.data[i]));
    }

    logger_.debug("[StreamServer] {}:{} ({} bytes): {}",
                  dgram.peer_ip(), dgram.peer_port(), dgram.data.size(), hex);
}

// ─── PgStreamServer ─────────────────────────────────────────────────────────

#ifdef WITH_POSTGRESQL

PgStreamServer::PgStreamServer(Application& app)
    : StreamServer(app)
    , pool_(app.db_pool())
{
    if (auto* cfg = app.module_config("StreamServer")) {
        if (cfg->contains("protocol"))
            protocol_ = (*cfg)["protocol"].get<std::string>();
    }
    if (protocol_.empty())
        protocol_ = "S228";
}

void PgStreamServer::on_datagram(const UdpDatagram& dgram)
{
    debug_log(dgram);

    auto b64 = base64_encode(dgram.data);
    auto sql = fmt::format(
        "SELECT * FROM stream.parse({}, '{}:{}', {});",
        pq_quote_literal(protocol_),
        dgram.peer_ip(), dgram.peer_port(),
        pq_quote_literal(b64));

    auto peer_addr = dgram.peer_addr;
    auto peer_len  = dgram.peer_len;

    pool_.execute(std::move(sql),
        [this, peer_addr, peer_len](std::vector<PgResult> results) {
            for (auto& res : results) {
                if (res.ok() && res.rows() > 0 && !res.is_null(0, 0)) {
                    auto reply = base64_decode(res.value(0, 0));
                    udp_socket()->send(reply.data(), reply.size(), peer_addr, peer_len);
                }
            }
        },
        [this](std::string_view err) {
            logger_.error("[StreamServer] PG error: {}", err);
        });
}

#endif // WITH_POSTGRESQL

} // namespace apostol
