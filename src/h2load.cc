/*
 * nghttp2 - HTTP/2 C Library
 *
 * Copyright (c) 2014 Tatsuhiro Tsujikawa
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
 * LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
 * OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
 * WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */
#include "h2load.h"

#include <fcntl.h>
#include <getopt.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <sys/stat.h>

#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <future>
#include <iomanip>
#include <iostream>
#include <random>
#include <thread>

#include <openssl/err.h>

#include "url-parser/url_parser.h"

#include "h2load_http1_session.h"
#include "h2load_http2_session.h"
#include "h2load_sofarpc_session.h"
#include "http2.h"
#include "template.h"
#include "tls.h"
#include "util.h"

#include "sofarpc.h"

#include <atomic>
#include <limits>

#ifndef O_BINARY
#define O_BINARY (0)
#endif // O_BINARY

using namespace nghttp2;

namespace h2load {

namespace {
bool recorded(const std::chrono::steady_clock::time_point &t) {
    return std::chrono::steady_clock::duration::zero() != t.time_since_epoch();
}
} // namespace

Config::Config()
    : ciphers(tls::DEFAULT_CIPHER_LIST), data_length(-1), addrs(nullptr),
      nreqs(1), nclients(1), nthreads(1), max_concurrent_streams(1),
      window_bits(30), connection_window_bits(30), rate(0), rate_period(1.0),
      duration(0.0), warm_up_time(0.0), conn_active_timeout(0.),
      conn_inactivity_timeout(0.), no_tls_proto(PROTO_HTTP2),
      header_table_size(4_k), encoder_header_table_size(4_k), data_fd(-1),
      port(0), default_port(0), verbose(false),
      base_uri_unix(false), unix_addr{}, qps(0) {}

Config::~Config() {
    if (addrs) {
        if (base_uri_unix) {
        } else {
            freeaddrinfo(addrs);
        }
    }

    if (data_fd != -1) {
        close(data_fd);
    }
}

bool Config::is_qps_mode() const { return (this->qps != 0); }
bool Config::is_rate_mode() const { return (this->rate != 0); }
bool Config::is_timing_based_mode() const { return (this->duration > 0); }
bool Config::has_base_uri() const { return (!this->base_uri.empty()); }

Config config;
std::atomic_size_t total_req_left(0);
std::atomic_size_t total_req_send(0);

Stats::Stats()
    : req_started(0), req_done(0), req_success(0), req_status_success(0),
      req_failed(0), req_error(0), req_timedout(0), bytes_total(0),
      bytes_head(0), bytes_head_decomp(0), bytes_body(0), status(),
      sofarpcStatus() {}

Stream::Stream() : req_stat{}, status_success(-1) {}


namespace {
void writecb(struct ev_loop *loop, ev_io *w, int revents) {
    auto client = static_cast<Client *>(w->data);
    client->restart_timeout();
    auto rv = client->do_write();
    if (rv == Client::ERR_CONNECT_FAIL) {
        client->disconnect();
        // Try next address
        client->current_addr = nullptr;
        rv = client->connect();
        if (rv != 0) {
            client->fail();
            client->worker->free_client(client);
            return;
        }
        return;
    }
    if (rv != 0) {
        client->fail();
        client->worker->free_client(client);
    }
}
} // namespace

namespace {
void readcb(struct ev_loop *loop, ev_io *w, int revents) {
    auto client = static_cast<Client *>(w->data);
    client->restart_timeout();
    if (client->do_read() != 0) {
        if (client->try_again_or_fail() == 0) {
            return;
        }
        client->worker->free_client(client);
        return;
    }
    writecb(loop, &client->wev, revents);
    // client->disconnect() and client->fail() may be called
}
} // namespace

namespace {
// Called when the duration for infinite number of requests are over
void duration_timeout_cb(struct ev_loop *loop, ev_timer *w, int revents) {
    auto worker = static_cast<Worker *>(w->data);

    total_req_left.store(0);
    worker->current_phase = Phase::DURATION_OVER;

    ev_periodic_stop(worker->loop, &worker->qpsUpdater);

    worker->stop_all_clients();
    ev_break(loop, EVBREAK_ALL);
}
} // namespace

namespace {
// Called when the warmup duration for infinite number of requests are over
void warmup_timeout_cb(struct ev_loop *loop, ev_timer *w, int revents) {
    auto worker = static_cast<Worker *>(w->data);
    assert(worker->stats.req_started == 0);
    assert(worker->stats.req_done == 0);

    for (auto client : worker->clients) {
        if (client) {
            assert(client->req_inflight == 0);
            assert(client->req_started == 0);
            assert(client->req_done == 0);

            client->record_client_start_time();
            client->clear_connect_times();
            client->record_connect_start_time();
        }
    }

    worker->current_phase = Phase::MAIN_DURATION;

    ev_timer_start(worker->loop, &worker->duration_watcher);
    ev_periodic_start(worker->loop, &worker->qpsUpdater);
}
} // namespace

namespace {
// Called when an a connection has been inactive for a set period of time
// or a fixed amount of time after all requests have been made on a
// connection
void conn_timeout_cb(EV_P_ ev_timer *w, int revents) {
    auto client = static_cast<Client *>(w->data);

    ev_timer_stop(client->worker->loop, &client->conn_inactivity_watcher);
    ev_timer_stop(client->worker->loop, &client->conn_active_watcher);

    if (util::check_socket_connected(client->fd)) {
        client->timeout();
    }
}
} // namespace

namespace {
bool check_stop_client_request_timeout(Client *client, ev_timer *w) {
    if (total_req_left.load() <= 0) {
        // no more requests to make, stop timer
        ev_timer_stop(client->worker->loop, w);
        return true;
    }

    return false;
}
} // namespace

namespace {
void client_request_timeout_cb(struct ev_loop *loop, ev_timer *w, int revents) {
    auto client = static_cast<Client *>(w->data);

    if (client->streams.size() >= (size_t)config.max_concurrent_streams) {
        ev_timer_stop(client->worker->loop, w);
        return;
    }

    if (client->submit_request() != 0) {
        ev_timer_stop(client->worker->loop, w);
        client->process_request_failure();
        return;
    }
    client->signal_write();

    if (check_stop_client_request_timeout(client, w)) {
        return;
    }

    ev_tstamp duration =
        config.timings[client->reqidx] - config.timings[client->reqidx - 1];

    while (duration < 1e-9) {
        if (client->submit_request() != 0) {
            ev_timer_stop(client->worker->loop, w);
            client->process_request_failure();
            return;
        }
        client->signal_write();
        if (check_stop_client_request_timeout(client, w)) {
            return;
        }

        duration =
            config.timings[client->reqidx] - config.timings[client->reqidx - 1];
    }

    client->request_timeout_watcher.repeat = duration;
    ev_timer_again(client->worker->loop, &client->request_timeout_watcher);
}
} // namespace

Client::Client(uint32_t id, Worker *worker)
    : wb(&worker->mcpool), cstat{}, worker(worker), ssl(nullptr),
      next_addr(config.addrs), current_addr(nullptr), reqidx(0),
      state(CLIENT_IDLE), req_inflight(0), req_started(0), req_done(0), id(id),
      fd(-1), new_connection_requested(false), final(false) {

    ev_io_init(&wev, writecb, 0, EV_WRITE);
    ev_io_init(&rev, readcb, 0, EV_READ);

    wev.data = this;
    rev.data = this;

    ev_timer_init(&conn_inactivity_watcher, conn_timeout_cb, 0.,
                  worker->config->conn_inactivity_timeout);
    conn_inactivity_watcher.data = this;

    ev_timer_init(&conn_active_watcher, conn_timeout_cb,
                  worker->config->conn_active_timeout, 0.);
    conn_active_watcher.data = this;

    ev_timer_init(&request_timeout_watcher, client_request_timeout_cb, 0., 0.);
    request_timeout_watcher.data = this;
}

Client::~Client() {
    disconnect();

    if (ssl) {
        SSL_free(ssl);
    }

    worker->process_client_stat(&cstat);
}

int Client::do_read() { return readfn(*this); }
int Client::do_write() { return writefn(*this); }

int Client::make_socket(addrinfo *addr) {
    fd = util::create_nonblock_socket(addr->ai_family);
    if (fd == -1) {
        return -1;
    }
    if (config.scheme == "https") {
        if (!ssl) {
            ssl = SSL_new(worker->ssl_ctx);
        }

        auto config = worker->config;

        if (!util::numeric_host(config->host.c_str())) {
            SSL_set_tlsext_host_name(ssl, config->host.c_str());
        }

        SSL_set_fd(ssl, fd);
        SSL_set_connect_state(ssl);
    }

    auto rv = ::connect(fd, addr->ai_addr, addr->ai_addrlen);

    if (rv != 0 && errno != EINPROGRESS) {
        if (ssl) {
            SSL_free(ssl);
            ssl = nullptr;
        }
        close(fd);
        fd = -1;
        return -1;
    }
    return 0;
}

int Client::connect() {
    int rv;

    if (!worker->config->is_timing_based_mode() ||
        worker->current_phase == Phase::MAIN_DURATION) {
        record_client_start_time();
        clear_connect_times();
        record_connect_start_time();
    } else if (worker->current_phase == Phase::INITIAL_IDLE) {
        worker->current_phase = Phase::WARM_UP;
        ev_timer_start(worker->loop, &worker->warmup_watcher);
    }

    if (worker->config->conn_inactivity_timeout > 0.) {
        ev_timer_again(worker->loop, &conn_inactivity_watcher);
    }

    if (current_addr) {
        rv = make_socket(current_addr);
        if (rv == -1) {
            return -1;
        }
    } else {
        addrinfo *addr = nullptr;
        while (next_addr) {
            addr = next_addr;
            next_addr = next_addr->ai_next;
            rv = make_socket(addr);
            if (rv == 0) {
                break;
            }
        }

        if (fd == -1) {
            return -1;
        }

        assert(addr);

        current_addr = addr;
    }

    writefn = &Client::connected;

    ev_io_set(&rev, fd, EV_READ);
    ev_io_set(&wev, fd, EV_WRITE);

    ev_io_start(worker->loop, &wev);

    return 0;
}

void Client::timeout() {
    process_timedout_streams();

    disconnect();
}

void Client::restart_timeout() {
    if (worker->config->conn_inactivity_timeout > 0.) {
        ev_timer_again(worker->loop, &conn_inactivity_watcher);
    }
}

int Client::try_again_or_fail() {
    disconnect();

    if (new_connection_requested) {
        new_connection_requested = false;

        if (total_req_left.load() > 0) {
            if (worker->current_phase == Phase::MAIN_DURATION) {
                // At the moment, we don't have a facility to re-start request
                // already in in-flight.  Make them fail.
                worker->stats.req_failed += req_inflight;
                worker->stats.req_error += req_inflight;

                req_inflight = 0;
            }

            // Keep using current address
            if (connect() == 0) {
                return 0;
            }
            std::cerr << "client could not connect to host" << std::endl;
        }
    }

    process_abandoned_streams();

    return -1;
}

void Client::fail() {
    disconnect();
    process_abandoned_streams();
}

void Client::disconnect() {
    record_client_end_time();

    ev_timer_stop(worker->loop, &conn_inactivity_watcher);
    ev_timer_stop(worker->loop, &conn_active_watcher);
    ev_timer_stop(worker->loop, &request_timeout_watcher);
    streams.clear();
    session.reset();
    state = CLIENT_IDLE;
    ev_io_stop(worker->loop, &wev);
    ev_io_stop(worker->loop, &rev);
    if (ssl) {
        SSL_set_shutdown(ssl, SSL_get_shutdown(ssl) | SSL_RECEIVED_SHUTDOWN);
        ERR_clear_error();

        if (SSL_shutdown(ssl) != 1) {
            SSL_free(ssl);
            ssl = nullptr;
        }
    }
    if (fd != -1) {
        shutdown(fd, SHUT_WR);
        close(fd);
        fd = -1;
    }

    final = false;
}

int Client::submit_request() {
    if (config.is_qps_mode()) {
        if (worker->qpsLeft == 0) {
            worker->clientsBlockedDueToQps.push_back(this);
            return 0;
        } else {
            --worker->qpsLeft;
        }
    } else {
        if (total_req_left.load() <= 0) {
            return -1;
        }
        size_t req_left = total_req_left--;
        if (req_left <= 0) {
            return -1;
        }
    }
    total_req_send++;

    if (session && session->submit_request() != 0) {
        return -1;
    }

    if (worker->current_phase != Phase::MAIN_DURATION) {
        return 0;
    }

    ++worker->stats.req_started;
    ++req_started;
    ++req_inflight;

    if (worker->config->conn_active_timeout > 0.) {
        ev_timer_start(worker->loop, &conn_active_watcher);
    }

    return 0;
}

void Client::process_timedout_streams() {
    if (worker->current_phase != Phase::MAIN_DURATION) {
        return;
    }

    for (auto &p : streams) {
        auto &req_stat = p.second.req_stat;
        if (!req_stat.completed) {
            req_stat.stream_close_time = std::chrono::steady_clock::now();
        }
    }

    worker->stats.req_timedout += req_inflight;

    process_abandoned_streams();
}

void Client::process_abandoned_streams() {
    if (worker->current_phase != Phase::MAIN_DURATION) {
        return;
    }

    auto req_abandoned = req_inflight;

    worker->stats.req_failed += req_abandoned;
    worker->stats.req_error += req_abandoned;

    req_inflight = 0;
}

void Client::process_request_failure() {
    if (worker->current_phase != Phase::MAIN_DURATION) {
        ev_break (worker->loop, EVBREAK_ONE);
        return;
    }
}

namespace {
void print_server_tmp_key(SSL *ssl) {
// libressl does not have SSL_get_server_tmp_key
#if OPENSSL_VERSION_NUMBER >= 0x10002000L && defined(SSL_get_server_tmp_key)
    EVP_PKEY *key;

    if (!SSL_get_server_tmp_key(ssl, &key)) {
        return;
    }

    auto key_del = defer(EVP_PKEY_free, key);

    std::cout << "Server Temp Key: ";

    auto pkey_id = EVP_PKEY_id(key);
    switch (pkey_id) {
    case EVP_PKEY_RSA:
        std::cout << "RSA " << EVP_PKEY_bits(key) << " bits" << std::endl;
        break;
    case EVP_PKEY_DH:
        std::cout << "DH " << EVP_PKEY_bits(key) << " bits" << std::endl;
        break;
    case EVP_PKEY_EC: {
        auto ec = EVP_PKEY_get1_EC_KEY(key);
        auto ec_del = defer(EC_KEY_free, ec);
        auto nid = EC_GROUP_get_curve_name(EC_KEY_get0_group(ec));
        auto cname = EC_curve_nid2nist(nid);
        if (!cname) {
            cname = OBJ_nid2sn(nid);
        }

        std::cout << "ECDH " << cname << " " << EVP_PKEY_bits(key) << " bits"
                  << std::endl;
        break;
    }
    default:
        std::cout << OBJ_nid2sn(pkey_id) << " " << EVP_PKEY_bits(key) << " bits"
                  << std::endl;
        break;
    }
#endif // OPENSSL_VERSION_NUMBER >= 0x10002000L
}
} // namespace

void Client::report_tls_info() {
    if (worker->id == 0 && !worker->tls_info_report_done) {
        worker->tls_info_report_done = true;
        auto cipher = SSL_get_current_cipher(ssl);
        std::cout << "TLS Protocol: " << tls::get_tls_protocol(ssl) << "\n"
                  << "Cipher: " << SSL_CIPHER_get_name(cipher) << std::endl;
        print_server_tmp_key(ssl);
    }
}

void Client::report_app_info() {
    if (worker->id == 0 && !worker->app_info_report_done) {
        worker->app_info_report_done = true;
        std::cout << "Application protocol: " << selected_proto << std::endl;
    }
}

void Client::terminate_session() {
    session->terminate();
    // http1 session needs writecb to tear down session.
    signal_write();
}

void Client::on_request(int32_t stream_id) { streams[stream_id] = Stream(); }

void Client::on_header(int32_t stream_id, const uint8_t *name, size_t namelen,
                       const uint8_t *value, size_t valuelen) {
    auto itr = streams.find(stream_id);
    if (itr == std::end(streams)) {
        return;
    }
    auto &stream = (*itr).second;

    if (worker->current_phase != Phase::MAIN_DURATION) {
        // If the stream is for warm-up phase, then mark as a success
        // But we do not update the count for 2xx, 3xx, etc status codes
        // Same has been done in on_status_code function
        stream.status_success = 1;
        return;
    }

    if (stream.status_success == -1 && namelen == 7 &&
        util::streq_l(":status", name, namelen)) {
        int status = 0;
        for (size_t i = 0; i < valuelen; ++i) {
            if ('0' <= value[i] && value[i] <= '9') {
                status *= 10;
                status += value[i] - '0';
                if (status > 999) {
                    stream.status_success = 0;
                    return;
                }
            } else {
                break;
            }
        }

        stream.req_stat.status = status;
        if (status >= 200 && status < 300) {
            ++worker->stats.status[2];
            stream.status_success = 1;
        } else if (status < 400) {
            ++worker->stats.status[3];
            stream.status_success = 1;
        } else if (status < 600) {
            ++worker->stats.status[status / 100];
            stream.status_success = 0;
        } else {
            stream.status_success = 0;
        }
    }
}

void Client::on_status_code(int32_t stream_id, uint16_t status) {
    auto itr = streams.find(stream_id);
    if (itr == std::end(streams)) {
        return;
    }
    auto &stream = (*itr).second;

    if (worker->current_phase != Phase::MAIN_DURATION) {
        stream.status_success = 1;
        return;
    }

    stream.req_stat.status = status;
    if (status >= 200 && status < 300) {
        ++worker->stats.status[2];
        stream.status_success = 1;
    } else if (status < 400) {
        ++worker->stats.status[3];
        stream.status_success = 1;
    } else if (status < 600) {
        ++worker->stats.status[status / 100];
        stream.status_success = 0;
    } else {
        stream.status_success = 0;
    }
}

void Client::on_sofarpc_status(int32_t stream_id, uint16_t status) {
    auto itr = streams.find(stream_id);
    if (itr == std::end(streams)) {
        return;
    }
    auto &stream = (*itr).second;

    if (worker->current_phase != Phase::MAIN_DURATION) {
        stream.status_success = 1;
        return;
    }

    stream.req_stat.status = status;
    stream.status_success = (status == RESPONSE_STATUS_SUCCESS);

    ++worker->stats.sofarpcStatus[status];
}

void Client::on_stream_close(int32_t stream_id, bool success, bool final) {
    if (worker->current_phase == Phase::MAIN_DURATION) {
        if (req_inflight > 0) {
            --req_inflight;
        }
        auto req_stat = get_req_stat(stream_id);
        if (!req_stat) {
            return;
        }

        req_stat->stream_close_time = std::chrono::steady_clock::now();
        if (success) {
            req_stat->completed = true;
            ++worker->stats.req_success;
            ++cstat.req_success;

            if (streams[stream_id].status_success == 1) {
                ++worker->stats.req_status_success;
            } else {
                ++worker->stats.req_failed;
            }

            worker->process_req_stat(req_stat);
        } else {
            ++worker->stats.req_failed;
            ++worker->stats.req_error;
        }
        ++worker->stats.req_done;
        ++req_done;

        uint64_t rtt =
            std::chrono::duration_cast<std::chrono::duration<double>>(
                req_stat->stream_close_time - req_stat->request_time)
                .count() *
            1000000;
        worker->record_rtt(rtt);
    }

    streams.erase(stream_id);

    if (total_req_left.load() <= 0) {
        terminate_session();
        return;
    }

    if (!final) {
        if (submit_request() != 0) {
            process_request_failure();
        }
    }
}

RequestStat *Client::get_req_stat(int32_t stream_id) {
    auto it = streams.find(stream_id);
    if (it == std::end(streams)) {
        return nullptr;
    }

    return &(*it).second.req_stat;
}

int Client::connection_made() {
    if (ssl) {
        report_tls_info();

        const unsigned char *next_proto = nullptr;
        unsigned int next_proto_len;

#ifndef OPENSSL_NO_NEXTPROTONEG
        SSL_get0_next_proto_negotiated(ssl, &next_proto, &next_proto_len);
#endif // !OPENSSL_NO_NEXTPROTONEG
#if OPENSSL_VERSION_NUMBER >= 0x10002000L
        if (next_proto == nullptr) {
            SSL_get0_alpn_selected(ssl, &next_proto, &next_proto_len);
        }
#endif // OPENSSL_VERSION_NUMBER >= 0x10002000L

        if (next_proto) {
            auto proto = StringRef{next_proto, next_proto_len};
            if (util::check_h2_is_selected(proto)) {
                session = std::make_unique<Http2Session>(this);
            } else if (util::streq(NGHTTP2_H1_1, proto)) {
                session = std::make_unique<Http1Session>(this);
            } else if (util::streq(SOFARPC, proto)) {
                session = std::make_unique<SofaRpcSession>(this);
            }

            // Just assign next_proto to selected_proto anyway to show the
            // negotiation result.
            selected_proto = proto.str();
        } else {
            std::cout
                << "No protocol negotiated. Fallback behaviour may be activated"
                << std::endl;

            for (const auto &proto : config.npn_list) {
                if (util::streq(NGHTTP2_H1_1_ALPN, StringRef{proto})) {
                    std::cout << "Server does not support NPN/ALPN. Falling "
                                 "back to HTTP/1.1."
                              << std::endl;
                    session = std::make_unique<Http1Session>(this);
                    selected_proto = NGHTTP2_H1_1.str();
                    break;
                }
            }
        }

        if (!selected_proto.empty()) {
            report_app_info();
        }

        if (!session) {
            std::cout << "No supported protocol was negotiated. Supported "
                         "protocols were:"
                      << std::endl;
            for (const auto &proto : config.npn_list) {
                std::cout << proto.substr(1) << std::endl;
            }
            disconnect();
            return -1;
        }
    } else {
        switch (config.no_tls_proto) {
        case Config::PROTO_HTTP2:
            session = std::make_unique<Http2Session>(this);
            selected_proto = NGHTTP2_CLEARTEXT_PROTO_VERSION_ID;
            break;
        case Config::PROTO_HTTP1_1:
            session = std::make_unique<Http1Session>(this);
            selected_proto = NGHTTP2_H1_1.str();
            break;
        case Config::PROTO_SOFARPC:
            session = std::make_unique<SofaRpcSession>(this);
            selected_proto = SOFARPC.str();
            break;
        default:
            // unreachable
            assert(0);
        }

        report_app_info();
    }

    state = CLIENT_CONNECTED;

    session->on_connect();

    record_connect_time();

    auto nreq = session->max_concurrent_streams();
    for (; nreq > 0; --nreq) {
        if (submit_request() != 0) {
            process_request_failure();
            break;
        }
    }

    signal_write();

    return 0;
}

int Client::on_read(const uint8_t *data, size_t len) {
    auto rv = session->on_read(data, len);
    if (rv != 0) {
        return -1;
    }
    if (worker->current_phase == Phase::MAIN_DURATION) {
        worker->stats.bytes_total += len;
    }
    signal_write();
    return 0;
}

int Client::on_write() {
    if (wb.rleft() >= BACKOFF_WRITE_BUFFER_THRES) {
        return 0;
    }

    if (session && session->on_write() != 0) {
        return -1;
    }
    return 0;
}

int Client::read_clear() {
    uint8_t buf[8_k];

    for (;;) {
        ssize_t nread;
        while ((nread = read(fd, buf, sizeof(buf))) == -1 && errno == EINTR)
            ;
        if (nread == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return 0;
            }
            return -1;
        }

        if (nread == 0) {
            return -1;
        }

        if (on_read(buf, nread) != 0) {
            return -1;
        }
    }

    return 0;
}

int Client::write_clear() {
    std::array<struct iovec, 2> iov;

    for (;;) {
        if (on_write() != 0) {
            return -1;
        }

        auto iovcnt = wb.riovec(iov.data(), iov.size());

        if (iovcnt == 0) {
            break;
        }

        ssize_t nwrite;
        while ((nwrite = writev(fd, iov.data(), iovcnt)) == -1 &&
               errno == EINTR)
            ;

        if (nwrite == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                ev_io_start(worker->loop, &wev);
                return 0;
            }
            return -1;
        }

        wb.drain(nwrite);
    }

    ev_io_stop(worker->loop, &wev);

    return 0;
}

int Client::connected() {
    if (!util::check_socket_connected(fd)) {
        return ERR_CONNECT_FAIL;
    }
    ev_io_start(worker->loop, &rev);
    ev_io_stop(worker->loop, &wev);

    if (ssl) {
        readfn = &Client::tls_handshake;
        writefn = &Client::tls_handshake;

        return do_write();
    }

    readfn = &Client::read_clear;
    writefn = &Client::write_clear;

    if (connection_made() != 0) {
        return -1;
    }

    return 0;
}

int Client::tls_handshake() {
    ERR_clear_error();

    auto rv = SSL_do_handshake(ssl);

    if (rv <= 0) {
        auto err = SSL_get_error(ssl, rv);
        switch (err) {
        case SSL_ERROR_WANT_READ:
            ev_io_stop(worker->loop, &wev);
            return 0;
        case SSL_ERROR_WANT_WRITE:
            ev_io_start(worker->loop, &wev);
            return 0;
        default:
            return -1;
        }
    }

    ev_io_stop(worker->loop, &wev);

    readfn = &Client::read_tls;
    writefn = &Client::write_tls;

    if (connection_made() != 0) {
        return -1;
    }

    return 0;
}

int Client::read_tls() {
    uint8_t buf[8_k];

    ERR_clear_error();

    for (;;) {
        auto rv = SSL_read(ssl, buf, sizeof(buf));

        if (rv <= 0) {
            auto err = SSL_get_error(ssl, rv);
            switch (err) {
            case SSL_ERROR_WANT_READ:
                return 0;
            case SSL_ERROR_WANT_WRITE:
                // renegotiation started
                return -1;
            default:
                return -1;
            }
        }

        if (on_read(buf, rv) != 0) {
            return -1;
        }
    }
}

int Client::write_tls() {
    ERR_clear_error();

    struct iovec iov;

    for (;;) {
        if (on_write() != 0) {
            return -1;
        }

        auto iovcnt = wb.riovec(&iov, 1);

        if (iovcnt == 0) {
            break;
        }

        auto rv = SSL_write(ssl, iov.iov_base, iov.iov_len);

        if (rv <= 0) {
            auto err = SSL_get_error(ssl, rv);
            switch (err) {
            case SSL_ERROR_WANT_READ:
                // renegotiation started
                return -1;
            case SSL_ERROR_WANT_WRITE:
                ev_io_start(worker->loop, &wev);
                return 0;
            default:
                return -1;
            }
        }

        wb.drain(rv);
    }

    ev_io_stop(worker->loop, &wev);

    return 0;
}

void Client::record_request_time(RequestStat *req_stat) {
    req_stat->request_time = std::chrono::steady_clock::now();
    req_stat->request_wall_time = std::chrono::system_clock::now();
}

void Client::record_connect_start_time() {
    cstat.connect_start_time = std::chrono::steady_clock::now();
}

void Client::record_connect_time() {
    cstat.connect_time = std::chrono::steady_clock::now();
}

void Client::record_ttfb() {
    if (recorded(cstat.ttfb)) {
        return;
    }

    cstat.ttfb = std::chrono::steady_clock::now();
}

void Client::clear_connect_times() {
    cstat.connect_start_time = std::chrono::steady_clock::time_point();
    cstat.connect_time = std::chrono::steady_clock::time_point();
    cstat.ttfb = std::chrono::steady_clock::time_point();
}

void Client::record_client_start_time() {
    // Record start time only once at the very first connection is going
    // to be made.
    if (recorded(cstat.client_start_time)) {
        return;
    }

    cstat.client_start_time = std::chrono::steady_clock::now();
}

void Client::record_client_end_time() {
    // Unlike client_start_time, we overwrite client_end_time.  This
    // handles multiple connect/disconnect for HTTP/1.1 benchmark.
    cstat.client_end_time = std::chrono::steady_clock::now();
}

void Client::signal_write() { ev_io_start(worker->loop, &wev); }

void Client::try_new_connection() { new_connection_requested = true; }

namespace {
int get_ev_loop_flags() {
    if (ev_supported_backends() & ~ev_recommended_backends() &
        EVBACKEND_KQUEUE) {
        return ev_recommended_backends() | EVBACKEND_KQUEUE;
    }

    return 0;
}
} // namespace

namespace {
void update_worker_qpsLeft(struct ev_loop *loop, ev_periodic *w, int revents) {
    auto worker = static_cast<Worker *>(w->data);
    if (!worker->qps_counts_.empty()) {
        worker->qpsLeft += worker->qps_counts_[worker->qps_count_index_];
        worker->qps_count_index_ = (worker->qps_count_index_ + 1) % worker->qps_counts_.size();
    } else {
        worker->qpsLeft = std::numeric_limits<int>::max();
    }
    while (worker->qpsLeft && !worker->clientsBlockedDueToQps.empty()) {
        Client *c = worker->clientsBlockedDueToQps.back();
        worker->clientsBlockedDueToQps.pop_back();
        if (c->submit_request() != 0) {
            c->process_request_failure();
        }
        c->signal_write();
    }
}
} // namespace

namespace {
constexpr size_t qps_update_period_ms = 5;
constexpr size_t qps_update_per_second = 1000 / qps_update_period_ms;
} // namespace

Worker::Worker(uint32_t id, SSL_CTX *ssl_ctx, size_t nclients, size_t rate,
               Config *config)
    : stats(), loop(ev_loop_new(get_ev_loop_flags())), ssl_ctx(ssl_ctx),
      config(config), id(id), tls_info_report_done(false),
      app_info_report_done(false), nconns_made(0), nclients(nclients),
      rate(rate), next_client_id(0), rtts(),
      rtt_min(std::numeric_limits<uint64_t>::max()),
      rtt_max(std::numeric_limits<uint64_t>::min()), qpsLeft(0),
      qps_count_index_(0) {

    ev_timer_init(&duration_watcher, duration_timeout_cb, config->duration, 0.);
    duration_watcher.data = this;

    ev_timer_init(&warmup_watcher, warmup_timeout_cb, config->warm_up_time, 0.);
    warmup_watcher.data = this;

    ev_periodic_init(&qpsUpdater, update_worker_qpsLeft, 0.,
                     (double)qps_update_period_ms / 1000.0, 0);
    qpsUpdater.data = this;

    if (config->is_timing_based_mode()) {
        current_phase = Phase::INITIAL_IDLE;
    } else {
        current_phase = Phase::MAIN_DURATION;
    }
}

Worker::~Worker() {
    ev_loop_destroy(loop);
}

void Worker::stop_all_clients() {
    for (auto client : clients) {
        if (!client)
            continue;
        client->record_client_end_time();
        if (client->session) {
            client->terminate_session();
            client->disconnect();
        }
        process_client_stat(&client->cstat);
    }
}

void Worker::free_client(Client *deleted_client) {
}

void Worker::run() {
    for (size_t i = 0; i < nclients; ++i) {

        auto client = new Client(next_client_id++, this);

        ++nconns_made;

        if (client->connect() != 0) {
            std::cerr << "client could not connect to host" << std::endl;
            client->fail();
        } else {
            clients.push_back(client);
        }
    }
    ev_run(loop, 0);
}

void Worker::process_req_stat(RequestStat *req_stat) {
    stats.req_stats.push_back(*req_stat);
}

void Worker::process_client_stat(ClientStat *cstat) {
    stats.client_stats.push_back(*cstat);
}

void Worker::record_rtt(uint64_t rtt_in_us) {
    rtts.push_back(rtt_in_us);
    rtt_min = std::min(rtt_min, rtt_in_us);
    rtt_max = std::max(rtt_max, rtt_in_us);
}

void Worker::set_qps_counts(std::vector<size_t> qps_count) {
    qps_counts_ = qps_count;
}

namespace {
// Returns percentage of number of samples within mean +/- sd.
double within_sd(const std::vector<double> &samples, double mean, double sd) {
    if (samples.size() == 0) {
        return 0.0;
    }
    auto lower = mean - sd;
    auto upper = mean + sd;
    auto m = std::count_if(
        std::begin(samples), std::end(samples),
        [&lower, &upper](double t) { return lower <= t && t <= upper; });
    return (m / static_cast<double>(samples.size())) * 100;
}
} // namespace

namespace {
// Computes statistics using |samples|. The min, max, mean, sd, and
// percentage of number of samples within mean +/- sd are computed.
// If |sampling| is true, this computes sample variance.  Otherwise,
// population variance.
SDStat compute_time_stat(const std::vector<double> &samples,
                         bool sampling = false) {
    if (samples.empty()) {
        return {0.0, 0.0, 0.0, 0.0, 0.0};
    }
    // standard deviation calculated using Rapid calculation method:
    // https://en.wikipedia.org/wiki/Standard_deviation#Rapid_calculation_methods
    double a = 0, q = 0;
    size_t n = 0;
    double sum = 0;
    auto res = SDStat{std::numeric_limits<double>::max(),
                      std::numeric_limits<double>::min()};
    for (const auto &t : samples) {
        ++n;
        res.min = std::min(res.min, t);
        res.max = std::max(res.max, t);
        sum += t;

        auto na = a + (t - a) / n;
        q += (t - a) * (t - na);
        a = na;
    }

    assert(n > 0);
    res.mean = sum / n;
    res.sd = sqrt(q / (sampling && n > 1 ? n - 1 : n));
    res.within_sd = within_sd(samples, res.mean, res.sd);

    return res;
}
} // namespace

namespace {
SDStats
process_time_stats(const std::vector<Worker*> &workers) {

    std::vector<double> request_times, connect_times, ttfb_times, rps_values;

    for (const auto &w : workers) {
        for (const auto &req_stat : w->stats.req_stats) {
            if (!req_stat.completed) {
                continue;
            }
            request_times.push_back(
                std::chrono::duration_cast<std::chrono::duration<double>>(
                    req_stat.stream_close_time - req_stat.request_time)
                    .count());
        }

        const auto &stat = w->stats;

        for (const auto &cstat : stat.client_stats) {
            if (recorded(cstat.client_start_time) &&
                recorded(cstat.client_end_time)) {
                auto t =
                    std::chrono::duration_cast<std::chrono::duration<double>>(
                        cstat.client_end_time - cstat.client_start_time)
                        .count();
                if (t > 1e-9) {
                    rps_values.push_back(cstat.req_success / t);
                }
            }

            // We will get connect event before FFTB.
            if (!recorded(cstat.connect_start_time) ||
                !recorded(cstat.connect_time)) {
                continue;
            }

            connect_times.push_back(
                std::chrono::duration_cast<std::chrono::duration<double>>(
                    cstat.connect_time - cstat.connect_start_time)
                    .count());

            if (!recorded(cstat.ttfb)) {
                continue;
            }

            ttfb_times.push_back(
                std::chrono::duration_cast<std::chrono::duration<double>>(
                    cstat.ttfb - cstat.connect_start_time)
                    .count());
        }
    }

    return {compute_time_stat(request_times), compute_time_stat(connect_times),
            compute_time_stat(ttfb_times), compute_time_stat(rps_values)};
}
} // namespace

namespace {
void resolve_host() {
    if (config.base_uri_unix) {
        auto res = std::make_unique<addrinfo>();
        res->ai_family = config.unix_addr.sun_family;
        res->ai_socktype = SOCK_STREAM;
        res->ai_addrlen = sizeof(config.unix_addr);
        res->ai_addr = static_cast<struct sockaddr *>(
            static_cast<void *>(&config.unix_addr));

        config.addrs = res.release();
        return;
    };

    int rv;
    addrinfo hints{}, *res;

    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = 0;
    hints.ai_flags = AI_ADDRCONFIG;

    rv = getaddrinfo(config.host.c_str(), util::utos(config.port).c_str(),
                     &hints, &res);
    if (rv != 0) {
        std::cerr << "getaddrinfo() failed: " << gai_strerror(rv) << std::endl;
        exit(EXIT_FAILURE);
    }
    if (res == nullptr) {
        std::cerr << "No address returned" << std::endl;
        exit(EXIT_FAILURE);
    }
    config.addrs = res;
}
} // namespace

namespace {
std::string get_reqline(const char *uri, const http_parser_url &u) {
    std::string reqline;

    if (util::has_uri_field(u, UF_PATH)) {
        reqline = util::get_uri_field(uri, u, UF_PATH).str();
    } else {
        reqline = "/";
    }

    if (util::has_uri_field(u, UF_QUERY)) {
        reqline += '?';
        reqline += util::get_uri_field(uri, u, UF_QUERY);
    }

    return reqline;
}
} // namespace

#ifndef OPENSSL_NO_NEXTPROTONEG
namespace {
int client_select_next_proto_cb(SSL *ssl, unsigned char **out,
                                unsigned char *outlen, const unsigned char *in,
                                unsigned int inlen, void *arg) {
    if (util::select_protocol(const_cast<const unsigned char **>(out), outlen,
                              in, inlen, config.npn_list)) {
        return SSL_TLSEXT_ERR_OK;
    }

    // OpenSSL will terminate handshake with fatal alert if we return
    // NOACK.  So there is no way to fallback.
    return SSL_TLSEXT_ERR_NOACK;
}
} // namespace
#endif // !OPENSSL_NO_NEXTPROTONEG

namespace {
constexpr char UNIX_PATH_PREFIX[] = "unix:";
} // namespace

namespace {
bool parse_base_uri(const StringRef &base_uri) {
    http_parser_url u{};
    if (http_parser_parse_url(base_uri.c_str(), base_uri.size(), 0, &u) != 0 ||
        !util::has_uri_field(u, UF_SCHEMA) ||
        !util::has_uri_field(u, UF_HOST)) {
        return false;
    }

    config.scheme = util::get_uri_field(base_uri.c_str(), u, UF_SCHEMA).str();
    config.host = util::get_uri_field(base_uri.c_str(), u, UF_HOST).str();
    config.default_port = util::get_default_port(base_uri.c_str(), u);
    if (util::has_uri_field(u, UF_PORT)) {
        config.port = u.port;
    } else {
        config.port = config.default_port;
    }

    return true;
}
} // namespace
namespace {
// Use std::vector<std::string>::iterator explicitly, without that,
// http_parser_url u{} fails with clang-3.4.
std::vector<std::string> parse_uris(std::vector<std::string>::iterator first,
                                    std::vector<std::string>::iterator last) {
    std::vector<std::string> reqlines;

    if (first == last) {
        std::cerr << "no URI available" << std::endl;
        exit(EXIT_FAILURE);
    }

    if (!config.has_base_uri()) {

        if (!parse_base_uri(StringRef{*first})) {
            std::cerr << "invalid URI: " << *first << std::endl;
            exit(EXIT_FAILURE);
        }

        config.base_uri = *first;
    }

    for (; first != last; ++first) {
        http_parser_url u{};

        auto uri = (*first).c_str();

        if (http_parser_parse_url(uri, (*first).size(), 0, &u) != 0) {
            std::cerr << "invalid URI: " << uri << std::endl;
            exit(EXIT_FAILURE);
        }

        reqlines.push_back(get_reqline(uri, u));
    }

    return reqlines;
}
} // namespace

namespace {
std::vector<std::string> read_uri_from_file(std::istream &infile) {
    std::vector<std::string> uris;
    std::string line_uri;
    while (std::getline(infile, line_uri)) {
        uris.push_back(line_uri);
    }

    return uris;
}
} // namespace

namespace {
Worker * create_worker(uint32_t id, SSL_CTX *ssl_ctx,
                                      size_t nclients, size_t rate) {
    if (config.is_rate_mode()) {
        return new Worker(id, ssl_ctx, nclients, rate, &config);
    } else {
        return new Worker(id, ssl_ctx, nclients, nclients, &config);
    }
}
} // namespace

namespace {
int parse_header_table_size(uint32_t &dst, const char *opt,
                            const char *optarg) {
    auto n = util::parse_uint_with_unit(optarg);
    if (n == -1) {
        std::cerr << "--" << opt << ": Bad option value: " << optarg
                  << std::endl;
        return -1;
    }
    if (n > std::numeric_limits<uint32_t>::max()) {
        std::cerr << "--" << opt
                  << ": Value too large.  It should be less than or equal to "
                  << std::numeric_limits<uint32_t>::max() << std::endl;
        return -1;
    }

    dst = n;

    return 0;
}
} // namespace

namespace {
void print_version(std::ostream &out) {
    out << "h2load nghttp2/" NGHTTP2_VERSION << std::endl;
}
} // namespace

namespace {
void print_usage(std::ostream &out) {
    out << R"(Usage: h2load [OPTIONS]... [URI]...
benchmarking tool for HTTP/2 server)"
        << std::endl;
}
} // namespace

namespace {
constexpr char DEFAULT_NPN_LIST[] = "h2,h2-16,h2-14,http/1.1";
} // namespace

namespace {
void print_help(std::ostream &out) {
    print_usage(out);

    auto config = Config();

    out << R"(
  <URI>       Specify URI to access.   Multiple URIs can be specified.
			  URIs are used  in this order for each  client.  All URIs
			  are used, then  first URI is used and then  2nd URI, and
			  so  on.  The  scheme, host  and port  in the  subsequent
			  URIs, if present,  are ignored.  Those in  the first URI
			  are used solely.  Definition of a base URI overrides all
			  scheme, host or port values.
Options:
  -n, --requests=<N>
			  Number of  requests across all  clients.  If it  is used
			  with --timing-script-file option,  this option specifies
			  the number of requests  each client performs rather than
			  the number of requests  across all clients.  This option
			  is ignored if timing-based  benchmarking is enabled (see
			  --duration option).
			  Default: )"
        << config.nreqs << R"(
  -c, --clients=<N>
			  Number  of concurrent  clients.   With  -r option,  this
			  specifies the maximum number of connections to be made.
			  Default: )"
        << config.nclients << R"(
  -t, --threads=<N>
			  Number of native threads.
			  Default: )"
        << config.nthreads << R"(
  -m, --max-concurrent-streams=<N>
			  Max  concurrent  streams  to issue  per  session.   When
			  http/1.1  is used,  this  specifies the  number of  HTTP
			  pipelining requests in-flight.
			  Default: 1
  -H, --header=<HEADER>
			  Add/Override a header to the requests.
  -p, --no-tls-proto=<PROTOID>
			  Specify ALPN identifier of the  protocol to be used when
			  accessing http URI without SSL/TLS.
			  Available protocols: )"
        << NGHTTP2_CLEARTEXT_PROTO_VERSION_ID << R"( and )" << NGHTTP2_H1_1
        << R"( and )" << SOFARPC << R"(
			  Default: )"
        << NGHTTP2_CLEARTEXT_PROTO_VERSION_ID << R"(
  -d, --data=<PATH>
			  Post FILE to  server.  The request method  is changed to
			  POST.   For  http/1.1 connection,  if  -d  is used,  the
			  maximum number of in-flight pipelined requests is set to
			  1.
  -r, --rate=<N>
			  Specifies  the  fixed  rate  at  which  connections  are
			  created.   The   rate  must   be  a   positive  integer,
			  representing the  number of  connections to be  made per
			  rate period.   The maximum  number of connections  to be
			  made  is  given  in  -c   option.   This  rate  will  be
			  distributed among  threads as  evenly as  possible.  For
			  example,  with   -t2  and   -r4,  each  thread   gets  2
			  connections per period.  When the rate is 0, the program
			  will run  as it  normally does, creating  connections at
			  whatever variable rate it  wants.  The default value for
			  this option is 0.  -r and -D are mutually exclusive.
  --rate-period=<DURATION>
			  Specifies the time  period between creating connections.
			  The period  must be a positive  number, representing the
			  length of the period in time.  This option is ignored if
			  the rate option is not used.  The default value for this
			  option is 1s.
  -D, --duration=<N>
			  Specifies the main duration for the measurements in case
			  of timing-based  benchmarking.  -D  and -r  are mutually
			  exclusive.
  --warm-up-time=<DURATION>
			  Specifies the  time  period  before  starting the actual
			  measurements, in  case  of  timing-based benchmarking.
			  Needs to provided along with -D option.
  -T, --connection-active-timeout=<DURATION>
			  Specifies  the maximum  time that  h2load is  willing to
			  keep a  connection open,  regardless of the  activity on
			  said connection.  <DURATION> must be a positive integer,
			  specifying the amount of time  to wait.  When no timeout
			  value is  set (either  active or inactive),  h2load will
			  keep  a  connection  open indefinitely,  waiting  for  a
			  response.
  -N, --connection-inactivity-timeout=<DURATION>
			  Specifies the amount  of time that h2load  is willing to
			  wait to see activity  on a given connection.  <DURATION>
			  must  be a  positive integer,  specifying the  amount of
			  time  to wait.   When no  timeout value  is set  (either
			  active or inactive), h2load  will keep a connection open
			  indefinitely, waiting for a response.
  --h1        Short        hand         for        --npn-list=http/1.1
			  --no-tls-proto=http/1.1,    which   effectively    force
			  http/1.1 for both http and https URI.
  --header-table-size=<SIZE>
			  Specify decoder header table size.
			  Default: )"
        << util::utos_unit(config.header_table_size) << R"(
  --encoder-header-table-size=<SIZE>
			  Specify encoder header table size.  The decoder (server)
			  specifies  the maximum  dynamic table  size it  accepts.
			  Then the negotiated dynamic table size is the minimum of
			  this option value and the value which server specified.
			  Default: )"
        << util::utos_unit(config.encoder_header_table_size) << R"(
  -v, --verbose
			  Output debug information.
  --version   Display version information and exit.
  -h, --help  Display this help and exit.

--

  The <SIZE> argument is an integer and an optional unit (e.g., 10K is
  10 * 1024).  Units are K, M and G (powers of 1024).

  The <DURATION> argument is an integer and an optional unit (e.g., 1s
  is 1 second and 500ms is 500 milliseconds).  Units are h, m, s or ms
  (hours, minutes, seconds and milliseconds, respectively).  If a unit
  is omitted, a second is used as unit.)"
        << std::endl;
}
} // namespace

int main(int argc, char **argv) {

    tls::libssl_init();

    tls::LibsslGlobalLock lock;

    std::string datafile;

    std::string sofaRpcClassname;
    std::string sofaRpcHeaderArg;
    std::string sofaRpcContent;
    size_t sofaRpcTimeout;

    while (1) {
        static int flag = 0;
        constexpr static option long_options[] = {
            {"sofaRpcClassName", required_argument, nullptr, 'e'},
            {"sofaRpcHeader", required_argument, nullptr, 'a'},
            {"sofaRpcContent", required_argument, nullptr, 'o'},
            {"sofaRpcTimeout", required_argument, nullptr, 'k'},
            {"requests", required_argument, nullptr, 'n'},
            {"clients", required_argument, nullptr, 'c'},
            {"data", required_argument, nullptr, 'd'},
            {"threads", required_argument, nullptr, 't'},
            {"max-concurrent-streams", required_argument, nullptr, 'm'},
            {"header", required_argument, nullptr, 'H'},
            {"no-tls-proto", required_argument, nullptr, 'p'},
            {"verbose", no_argument, nullptr, 'v'},
            {"help", no_argument, nullptr, 'h'},
            {"version", no_argument, &flag, 1},
            {"rate", required_argument, nullptr, 'r'},
            {"connection-active-timeout", required_argument, nullptr, 'T'},
            {"connection-inactivity-timeout", required_argument, nullptr, 'N'},
            {"duration", required_argument, nullptr, 'D'},
            {"rate-period", required_argument, &flag, 5},
            {"h1", no_argument, &flag, 6},
            {"header-table-size", required_argument, &flag, 7},
            {"encoder-header-table-size", required_argument, &flag, 8},
            {"warm-up-time", required_argument, &flag, 9},
            {"qps", required_argument, &flag, 11},
            {nullptr, 0, nullptr, 0}};
        int option_index = 0;
        auto c =
            getopt_long(argc, argv, "hv:c:d:m:n:p:t:H:r:T:N:D:e:a:o:k:",
                        long_options, &option_index);
        if (c == -1) {
            break;
        }
        switch (c) {
        case 'n':
            config.nreqs = strtoul(optarg, nullptr, 10);
            break;
        case 'c':
            config.nclients = strtoul(optarg, nullptr, 10);
            break;
        case 'd':
            datafile = optarg;
            break;
        case 't':
            config.nthreads = strtoul(optarg, nullptr, 10);
            break;
        case 'm':
            config.max_concurrent_streams = strtoul(optarg, nullptr, 10);
            break;
        case 'H': {
            char *header = optarg;
            // Skip first possible ':' in the header name
            char *value = strchr(optarg + 1, ':');
            if (!value || (header[0] == ':' && header + 1 == value)) {
                std::cerr << "-H: invalid header: " << optarg << std::endl;
                exit(EXIT_FAILURE);
            }
            *value = 0;
            value++;
            while (isspace(*value)) {
                value++;
            }
            if (*value == 0) {
                // This could also be a valid case for suppressing a header
                // similar to curl
                std::cerr << "-H: invalid header - value missing: " << optarg
                          << std::endl;
                exit(EXIT_FAILURE);
            }
            // Note that there is no processing currently to handle multiple
            // message-header fields with the same field name
            config.custom_headers.emplace_back(header, value);
            util::inp_strlower(config.custom_headers.back().name);
            break;
        }
        case 'e':
            sofaRpcClassname = optarg;
            break;
        case 'a':
            sofaRpcHeaderArg = optarg;
            break;
        case 'o':
            sofaRpcContent = optarg;
            break;
        case 'k':
            sofaRpcTimeout = strtoul(optarg, nullptr, 10);
            break;
        case 'p': {
            auto proto = StringRef{optarg};
            if (util::strieq(
                    StringRef::from_lit(NGHTTP2_CLEARTEXT_PROTO_VERSION_ID),
                    proto)) {
                config.no_tls_proto = Config::PROTO_HTTP2;
            } else if (util::strieq(NGHTTP2_H1_1, proto)) {
                config.no_tls_proto = Config::PROTO_HTTP1_1;
            } else if (util::strieq(SOFARPC, proto)) {
                config.no_tls_proto = Config::PROTO_SOFARPC;
            } else {
                std::cerr << "-p: unsupported protocol " << proto << std::endl;
                exit(EXIT_FAILURE);
            }
            break;
        }
        case 'r':
            config.rate = strtoul(optarg, nullptr, 10);
            if (config.rate == 0) {
                std::cerr << "-r: the rate at which connections are made "
                          << "must be positive." << std::endl;
                exit(EXIT_FAILURE);
            }
            break;
        case 'T':
            config.conn_active_timeout = util::parse_duration_with_unit(optarg);
            if (!std::isfinite(config.conn_active_timeout)) {
                std::cerr
                    << "-T: bad value for the conn_active_timeout wait time: "
                    << optarg << std::endl;
                exit(EXIT_FAILURE);
            }
            break;
        case 'N':
            config.conn_inactivity_timeout =
                util::parse_duration_with_unit(optarg);
            if (!std::isfinite(config.conn_inactivity_timeout)) {
                std::cerr << "-N: bad value for the conn_inactivity_timeout "
                             "wait time: "
                          << optarg << std::endl;
                exit(EXIT_FAILURE);
            }
            break;
        case 'D':
            config.duration = strtoul(optarg, nullptr, 10);
            if (config.duration == 0) {
                std::cerr
                    << "-D: the main duration for timing-based benchmarking "
                    << "must be positive." << std::endl;
                exit(EXIT_FAILURE);
            }
            break;
        case 'v':
            config.verbose = true;
            break;
        case 'h':
            print_help(std::cout);
            exit(EXIT_SUCCESS);
        case '?':
            util::show_candidates(argv[optind - 1], long_options);
            exit(EXIT_FAILURE);
        case 0:
            switch (flag) {
            case 1:
                // version option
                print_version(std::cout);
                exit(EXIT_SUCCESS);
            case 5:
                // rate-period
                config.rate_period = util::parse_duration_with_unit(optarg);
                if (!std::isfinite(config.rate_period)) {
                    std::cerr << "--rate-period: value error " << optarg
                              << std::endl;
                    exit(EXIT_FAILURE);
                }
                break;
            case 6:
                // --h1
                config.npn_list = util::parse_config_str_list(
                    StringRef::from_lit("http/1.1"));
                config.no_tls_proto = Config::PROTO_HTTP1_1;
                break;
            case 7:
                // --header-table-size
                if (parse_header_table_size(config.header_table_size,
                                            "header-table-size", optarg) != 0) {
                    exit(EXIT_FAILURE);
                }
                break;
            case 8:
                // --encoder-header-table-size
                if (parse_header_table_size(config.encoder_header_table_size,
                                            "encoder-header-table-size",
                                            optarg) != 0) {
                    exit(EXIT_FAILURE);
                }
                break;
            case 9:
                // --warm-up-time
                config.warm_up_time = util::parse_duration_with_unit(optarg);
                if (!std::isfinite(config.warm_up_time)) {
                    std::cerr << "--warm-up-time: value error " << optarg
                              << std::endl;
                    exit(EXIT_FAILURE);
                }
                break;
            case 11:
                // --qps
                config.qps = strtoul(optarg, nullptr, 10);
                break;
            }
            break;
        default:
            break;
        }
    }

    if (argc == optind) {
        if (config.ifile.empty()) {
            std::cerr << "no URI or input file given" << std::endl;
            exit(EXIT_FAILURE);
        }
    }

    if (config.nclients == 0) {
        std::cerr
            << "-c: the number of clients must be strictly greater than 0."
            << std::endl;
        exit(EXIT_FAILURE);
    }

    if (config.npn_list.empty()) {
        config.npn_list = util::parse_config_str_list(StringRef::from_lit(DEFAULT_NPN_LIST));
    }

    // serialize the APLN tokens
    for (auto &proto : config.npn_list) {
        proto.insert(proto.begin(), static_cast<unsigned char>(proto.size()));
    }

    std::vector<std::string> reqlines;

    if (config.ifile.empty()) {
        std::vector<std::string> uris;
        std::copy(&argv[optind], &argv[argc], std::back_inserter(uris));
        reqlines = parse_uris(std::begin(uris), std::end(uris));
    } else {
        std::vector<std::string> uris;
        if (config.ifile == "-") {
            uris = read_uri_from_file(std::cin);
        } else {
            std::ifstream infile(config.ifile);
            if (!infile) {
                std::cerr << "cannot read input file: " << config.ifile
                          << std::endl;
                exit(EXIT_FAILURE);
            }

            uris = read_uri_from_file(infile);
        }
        reqlines = parse_uris(std::begin(uris), std::end(uris));
    }

    if (reqlines.empty()) {
        std::cerr << "No URI given" << std::endl;
        exit(EXIT_FAILURE);
    }

    if (config.is_qps_mode() && config.is_rate_mode()) {
        std::cerr << "-r, --qps: they are mutually exclusive." << std::endl;
        exit(EXIT_FAILURE);
    }

    if (config.is_qps_mode() && config.duration == 0) {
        std::cerr << "duration(-D) must be positive in --qps mode" << std::endl;
        exit(EXIT_FAILURE);
    }

    if (config.is_timing_based_mode() && config.is_rate_mode()) {
        std::cerr << "-r, -D: they are mutually exclusive." << std::endl;
        exit(EXIT_FAILURE);
    }

    if (config.nreqs == 0 && !config.is_timing_based_mode()) {
        std::cerr
            << "-n: the number of requests must be strictly greater than 0 "
               "if timing-based test is not being run."
            << std::endl;
        exit(EXIT_FAILURE);
    }

    if (config.max_concurrent_streams == 0) {
        std::cerr << "-m: the max concurrent streams must be strictly greater "
                  << "than 0." << std::endl;
        exit(EXIT_FAILURE);
    }

    if (config.nthreads == 0) {
        std::cerr
            << "-t: the number of threads must be strictly greater than 0."
            << std::endl;
        exit(EXIT_FAILURE);
    }

    if (config.nthreads > std::thread::hardware_concurrency()) {
        std::cerr
            << "-t: warning: the number of threads is greater than hardware "
            << "cores." << std::endl;
    }

    if (config.nclients < config.nthreads && !config.is_qps_mode()) {
        std::cerr
            << "-c, -t: the number of clients must be greater than or equal "
            << "to the number of threads." << std::endl;
        exit(EXIT_FAILURE);
    }

    if (config.is_timing_based_mode()) {
        config.nreqs = 0;
    }

    if (config.is_rate_mode()) {
        if (config.rate < config.nthreads) {
            std::cerr
                << "-r, -t: the connection rate must be greater than or equal "
                << "to the number of threads." << std::endl;
            exit(EXIT_FAILURE);
        }

        if (config.rate > config.nclients) {
            std::cerr
                << "-r, -c: the connection rate must be smaller than or equal "
                   "to the number of clients."
                << std::endl;
            exit(EXIT_FAILURE);
        }
    }

    if (!datafile.empty()) {
        config.data_fd = open(datafile.c_str(), O_RDONLY | O_BINARY);
        if (config.data_fd == -1) {
            std::cerr << "-d: Could not open file " << datafile << std::endl;
            exit(EXIT_FAILURE);
        }
        struct stat data_stat;
        if (fstat(config.data_fd, &data_stat) == -1) {
            std::cerr << "-d: Could not stat file " << datafile << std::endl;
            exit(EXIT_FAILURE);
        }
        config.data_length = data_stat.st_size;
    }

    if (config.nreqs == 0 && !config.is_timing_based_mode()) {
        std::cerr
            << "-n, -D: Must have one"
            << std::endl;
        exit(EXIT_FAILURE);
    }

    if (config.is_timing_based_mode()) {
        if (config.is_qps_mode()) {
            config.nreqs = config.duration * config.qps;
        } else {
            config.nreqs = std::numeric_limits<std::size_t>::max();
        }
    }
    total_req_left.store(config.nreqs);

    struct sigaction act {};
    act.sa_handler = SIG_IGN;
    sigaction(SIGPIPE, &act, nullptr);

    auto ssl_ctx = SSL_CTX_new(SSLv23_client_method());
    if (!ssl_ctx) {
        std::cerr << "Failed to create SSL_CTX: "
                  << ERR_error_string(ERR_get_error(), nullptr) << std::endl;
        exit(EXIT_FAILURE);
    }

    auto ssl_opts = (SSL_OP_ALL & ~SSL_OP_DONT_INSERT_EMPTY_FRAGMENTS) |
                    SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3 | SSL_OP_NO_COMPRESSION |
                    SSL_OP_NO_SESSION_RESUMPTION_ON_RENEGOTIATION;

    SSL_CTX_set_options(ssl_ctx, ssl_opts);
    SSL_CTX_set_mode(ssl_ctx, SSL_MODE_AUTO_RETRY);
    SSL_CTX_set_mode(ssl_ctx, SSL_MODE_RELEASE_BUFFERS);

    if (nghttp2::tls::ssl_ctx_set_proto_versions(
            ssl_ctx, nghttp2::tls::NGHTTP2_TLS_MIN_VERSION,
            nghttp2::tls::NGHTTP2_TLS_MAX_VERSION) != 0) {
        std::cerr << "Could not set TLS versions" << std::endl;
        exit(EXIT_FAILURE);
    }

    if (SSL_CTX_set_cipher_list(ssl_ctx, config.ciphers.c_str()) == 0) {
        std::cerr << "SSL_CTX_set_cipher_list with " << config.ciphers
                  << " failed: " << ERR_error_string(ERR_get_error(), nullptr)
                  << std::endl;
        exit(EXIT_FAILURE);
    }

#ifndef OPENSSL_NO_NEXTPROTONEG
    SSL_CTX_set_next_proto_select_cb(ssl_ctx, client_select_next_proto_cb,
                                     nullptr);
#endif // !OPENSSL_NO_NEXTPROTONEG

#if OPENSSL_VERSION_NUMBER >= 0x10002000L
    std::vector<unsigned char> proto_list;
    for (const auto &proto : config.npn_list) {
        std::copy_n(proto.c_str(), proto.size(),
                    std::back_inserter(proto_list));
    }

    SSL_CTX_set_alpn_protos(ssl_ctx, proto_list.data(), proto_list.size());
#endif // OPENSSL_VERSION_NUMBER >= 0x10002000L

    std::string user_agent = "h2load nghttp2/" NGHTTP2_VERSION;
    Headers shared_nva;
    shared_nva.emplace_back(":scheme", config.scheme);
    if (config.port != config.default_port) {
        shared_nva.emplace_back(":authority",
                                config.host + ":" + util::utos(config.port));
    } else {
        shared_nva.emplace_back(":authority", config.host);
    }
    shared_nva.emplace_back(":method", config.data_fd == -1 ? "GET" : "POST");
    shared_nva.emplace_back("user-agent", user_agent);

    // list overridalbe headers
    auto override_hdrs = make_array<std::string>(
        ":authority", ":host", ":method", ":scheme", "user-agent");

    for (auto &kv : config.custom_headers) {
        if (std::find(std::begin(override_hdrs), std::end(override_hdrs),
                      kv.name) != std::end(override_hdrs)) {
            // override header
            for (auto &nv : shared_nva) {
                if ((nv.name == ":authority" && kv.name == ":host") ||
                    (nv.name == kv.name)) {
                    nv.value = kv.value;
                }
            }
        } else {
            // add additional headers
            shared_nva.push_back(kv);
        }
    }

    std::string content_length_str;
    if (config.data_fd != -1) {
        content_length_str = util::utos(config.data_length);
    }

    auto method_it =
        std::find_if(std::begin(shared_nva), std::end(shared_nva),
                     [](const Header &nv) { return nv.name == ":method"; });
    assert(method_it != std::end(shared_nva));

    config.h1reqs.reserve(reqlines.size());
    config.nva.reserve(reqlines.size());
    config.sofarpcreqs.reserve(reqlines.size());

    for (auto &req : reqlines) {
        // For HTTP/1.1
        auto h1req = (*method_it).value;
        h1req += ' ';
        h1req += req;
        h1req += " HTTP/1.1\r\n";
        for (auto &nv : shared_nva) {
            if (nv.name == ":authority") {
                h1req += "Host: ";
                h1req += nv.value;
                h1req += "\r\n";
                continue;
            }
            if (nv.name[0] == ':') {
                continue;
            }
            h1req += nv.name;
            h1req += ": ";
            h1req += nv.value;
            h1req += "\r\n";
        }

        if (!content_length_str.empty()) {
            h1req += "Content-Length: ";
            h1req += content_length_str;
            h1req += "\r\n";
        }
        h1req += "\r\n";

        config.h1reqs.push_back(std::move(h1req));

        // For nghttp2
        std::vector<nghttp2_nv> nva;
        // 2 for :path, and possible content-length
        nva.reserve(2 + shared_nva.size());

        nva.push_back(http2::make_nv_ls(":path", req));

        for (auto &nv : shared_nva) {
            nva.push_back(http2::make_nv(nv.name, nv.value, false));
        }

        if (!content_length_str.empty()) {
            nva.push_back(http2::make_nv(StringRef::from_lit("content-length"),
                                         StringRef{content_length_str}));
        }

        config.nva.push_back(std::move(nva));

        // For sofarpc   hardcode :)

        sofaRpcClassname = "com.alipay.sofa.rpc.core.request.SofaRequest";
        sofaRpcHeaderArg = "service:com.alipay.test.TestService:1.0";
        sofaRpcTimeout = 5000;

        std::string sofaRpcHeader = util::convertMap(sofaRpcHeaderArg);
        char bytes[22];
        bytes[0] = PROTOCOL_CODE_V1;                                // proto
        bytes[1] = REQUEST;                                         // type
        util::putBigEndianI16(&bytes[2], RPC_REQUEST);              // cmdcode
        bytes[4] = 1;                                               // version
        bytes[9] = 1;                                               // codec
        util::putBigEndianI32(&bytes[10], sofaRpcTimeout);          // timeout
        util::putBigEndianI16(&bytes[14], sofaRpcClassname.size()); // classLen
        util::putBigEndianI16(&bytes[16], sofaRpcHeader.size());    // headerLen
        util::putBigEndianI32(&bytes[18], 1314); // contentLen
        std::string sofaReq(22, 0);
        std::memcpy(&sofaReq[0], bytes, 22);
        unsigned char contentbytes[] = {
            0x4f, 0xbc, 0x63, 0x6f, 0x6d, 0x2e, 0x61, 0x6c, 0x69, 0x70, 0x61,
            0x79, 0x2e, 0x73, 0x6f, 0x66, 0x61, 0x2e, 0x72, 0x70, 0x63, 0x2e,
            0x63, 0x6f, 0x72, 0x65, 0x2e, 0x72, 0x65, 0x71, 0x75, 0x65, 0x73,
            0x74, 0x2e, 0x53, 0x6f, 0x66, 0x61, 0x52, 0x65, 0x71, 0x75, 0x65,
            0x73, 0x74, 0x95, 0x0d, 0x74, 0x61, 0x72, 0x67, 0x65, 0x74, 0x41,
            0x70, 0x70, 0x4e, 0x61, 0x6d, 0x65, 0x0a, 0x6d, 0x65, 0x74, 0x68,
            0x6f, 0x64, 0x4e, 0x61, 0x6d, 0x65, 0x17, 0x74, 0x61, 0x72, 0x67,
            0x65, 0x74, 0x53, 0x65, 0x72, 0x76, 0x69, 0x63, 0x65, 0x55, 0x6e,
            0x69, 0x71, 0x75, 0x65, 0x4e, 0x61, 0x6d, 0x65, 0x0c, 0x72, 0x65,
            0x71, 0x75, 0x65, 0x73, 0x74, 0x50, 0x72, 0x6f, 0x70, 0x73, 0x0d,
            0x6d, 0x65, 0x74, 0x68, 0x6f, 0x64, 0x41, 0x72, 0x67, 0x53, 0x69,
            0x67, 0x73, 0x6f, 0x90, 0x4e, 0x07, 0x65, 0x63, 0x68, 0x6f, 0x53,
            0x74, 0x72, 0x1f, 0x63, 0x6f, 0x6d, 0x2e, 0x61, 0x6c, 0x69, 0x70,
            0x61, 0x79, 0x2e, 0x74, 0x65, 0x73, 0x74, 0x2e, 0x54, 0x65, 0x73,
            0x74, 0x53, 0x65, 0x72, 0x76, 0x69, 0x63, 0x65, 0x3a, 0x31, 0x2e,
            0x30, 0x4d, 0x08, 0x70, 0x72, 0x6f, 0x74, 0x6f, 0x63, 0x6f, 0x6c,
            0x04, 0x62, 0x6f, 0x6c, 0x74, 0x7a, 0x56, 0x74, 0x00, 0x07, 0x5b,
            0x73, 0x74, 0x72, 0x69, 0x6e, 0x67, 0x6e, 0x01, 0x10, 0x6a, 0x61,
            0x76, 0x61, 0x2e, 0x6c, 0x61, 0x6e, 0x67, 0x2e, 0x53, 0x74, 0x72,
            0x69, 0x6e, 0x67, 0x7a, 0x53, 0x04, 0x4a, 0x31, 0x32, 0x33, 0x34,
            0x35, 0x36, 0x37, 0x38, 0x39, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35,
            0x36, 0x37, 0x38, 0x39, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36,
            0x37, 0x38, 0x39, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37,
            0x38, 0x39, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38,
            0x39, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39,
            0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x30,
            0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x30, 0x31,
            0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x30, 0x31, 0x32,
            0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x30, 0x31, 0x32, 0x33,
            0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x30, 0x31, 0x32, 0x33, 0x34,
            0x35, 0x36, 0x37, 0x38, 0x39, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35,
            0x36, 0x37, 0x38, 0x39, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36,
            0x37, 0x38, 0x39, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37,
            0x38, 0x39, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38,
            0x39, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39,
            0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x30,
            0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x30, 0x31,
            0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x30, 0x31, 0x32,
            0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x30, 0x31, 0x32, 0x33,
            0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x30, 0x31, 0x32, 0x33, 0x34,
            0x35, 0x36, 0x37, 0x38, 0x39, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35,
            0x36, 0x37, 0x38, 0x39, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36,
            0x37, 0x38, 0x39, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37,
            0x38, 0x39, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38,
            0x39, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39,
            0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x30,
            0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x30, 0x31,
            0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x30, 0x31, 0x32,
            0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x30, 0x31, 0x32, 0x33,
            0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x30, 0x31, 0x32, 0x33, 0x34,
            0x35, 0x36, 0x37, 0x38, 0x39, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35,
            0x36, 0x37, 0x38, 0x39, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36,
            0x37, 0x38, 0x39, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37,
            0x38, 0x39, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38,
            0x39, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39,
            0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x30,
            0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x30, 0x31,
            0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x30, 0x31, 0x32,
            0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x30, 0x31, 0x32, 0x33,
            0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x30, 0x31, 0x32, 0x33, 0x34,
            0x35, 0x36, 0x37, 0x38, 0x39, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35,
            0x36, 0x37, 0x38, 0x39, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36,
            0x37, 0x38, 0x39, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37,
            0x38, 0x39, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38,
            0x39, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39,
            0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x30,
            0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x30, 0x31,
            0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x30, 0x31, 0x32,
            0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x30, 0x31, 0x32, 0x33,
            0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x30, 0x31, 0x32, 0x33, 0x34,
            0x35, 0x36, 0x37, 0x38, 0x39, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35,
            0x36, 0x37, 0x38, 0x39, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36,
            0x37, 0x38, 0x39, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37,
            0x38, 0x39, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38,
            0x39, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39,
            0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x30,
            0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x30, 0x31,
            0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x30, 0x31, 0x32,
            0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x30, 0x31, 0x32, 0x33,
            0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x30, 0x31, 0x32, 0x33, 0x34,
            0x35, 0x36, 0x37, 0x38, 0x39, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35,
            0x36, 0x37, 0x38, 0x39, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36,
            0x37, 0x38, 0x39, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37,
            0x38, 0x39, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38,
            0x39, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39,
            0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x30,
            0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x30, 0x31,
            0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x30, 0x31, 0x32,
            0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x30, 0x31, 0x32, 0x33,
            0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x30, 0x31, 0x32, 0x33, 0x34,
            0x35, 0x36, 0x37, 0x38, 0x39, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35,
            0x36, 0x37, 0x38, 0x39, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36,
            0x37, 0x38, 0x39, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37,
            0x38, 0x39, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38,
            0x39, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39,
            0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x30,
            0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x30, 0x31,
            0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x30, 0x31, 0x32,
            0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x30, 0x31, 0x32, 0x33,
            0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x30, 0x31, 0x32, 0x33, 0x34,
            0x35, 0x36, 0x37, 0x38, 0x39, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35,
            0x36, 0x37, 0x38, 0x39, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36,
            0x37, 0x38, 0x39, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37,
            0x38, 0x39, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38,
            0x39, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39,
            0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x30,
            0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x30, 0x31,
            0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x30, 0x31, 0x32,
            0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x30, 0x31, 0x32, 0x33,
            0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x30, 0x31, 0x32, 0x33, 0x34,
            0x35, 0x36, 0x37, 0x38, 0x39, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35,
            0x36, 0x37, 0x38, 0x39, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36,
            0x37, 0x38, 0x39, 0x30, 0x31, 0x32, 0x33, 0x34, 0x31, 0x32, 0x33,
            0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x30, 0x31, 0x32, 0x33, 0x34,
            0x35, 0x36, 0x37, 0x38, 0x39, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35,
            0x36, 0x37, 0x38, 0x39, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36,
            0x37, 0x38, 0x39, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37,
            0x38, 0x39, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38,
            0x39, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39,
            0x30, 0x31, 0x32, 0x33, 0x34};
        std::string contentStr(1314, 0);
        std::memcpy(&contentStr[0], contentbytes, 1314);
        sofaReq += sofaRpcClassname + sofaRpcHeader + contentStr;
        config.sofarpcreqs.push_back(sofaReq);
    }

    // Don't DOS our server!
    if (config.host == "nghttp2.org") {
        std::cerr << "Using h2load against public server " << config.host
                  << " should be prohibited." << std::endl;
        exit(EXIT_FAILURE);
    }

    resolve_host();

    std::cout << "starting benchmark..." << std::endl;

    std::vector<Worker *> workers;
    workers.reserve(config.nthreads);

    size_t nclients_per_thread = config.nclients / config.nthreads;
    ssize_t nclients_rem = config.nclients % config.nthreads;

    size_t rate_per_thread = config.rate / config.nthreads;
    ssize_t rate_per_thread_rem = config.rate % config.nthreads;

    std::mutex mu;
    std::condition_variable cv;
    auto ready = false;

    std::vector<std::future<void>> futures;
    for (size_t i = 0; i < config.nthreads; ++i) {
        auto rate = rate_per_thread;
        if (rate_per_thread_rem > 0) {
            --rate_per_thread_rem;
            ++rate;
        }
        auto nclients = nclients_per_thread;
        if (nclients_rem > 0) {
            --nclients_rem;
            ++nclients;
        }

        workers.push_back(create_worker(i, ssl_ctx, nclients, rate));
        auto &worker = workers.back();
        if (config.is_qps_mode()) {
            size_t nqps = config.qps / config.nthreads;
            if (i < config.qps % config.nthreads)
                ++nqps;
            std::vector<size_t> qps_counts(qps_update_per_second, 0);
            for (size_t q = 0; q < nqps; q++) {
                qps_counts[std::rand() % qps_update_per_second]++;
            }
            worker->set_qps_counts(qps_counts);
        }
        futures.push_back(
            std::async(std::launch::async, [&worker, &mu, &cv, &ready]() {
                {
                    std::unique_lock<std::mutex> ulk(mu);
                    cv.wait(ulk, [&ready] { return ready; });
                }
                worker->run();
            }));
    }

    {
        std::lock_guard<std::mutex> lg(mu);
        ready = true;
        cv.notify_all();
    }

    auto start = std::chrono::steady_clock::now();

    for (auto &fut : futures) {
        fut.get();
    }

    auto end = std::chrono::steady_clock::now();
    auto duration =
        std::chrono::duration_cast<std::chrono::microseconds>(end - start);

    Stats stats;
    for (const auto &w : workers) {
        const auto &s = w->stats;

        stats.req_started += s.req_started;
        stats.req_done += s.req_done;
        stats.req_timedout += s.req_timedout;
        stats.req_success += s.req_success;
        stats.req_status_success += s.req_status_success;
        stats.req_failed += s.req_failed;
        stats.req_error += s.req_error;
        stats.bytes_total += s.bytes_total;
        stats.bytes_head += s.bytes_head;
        stats.bytes_head_decomp += s.bytes_head_decomp;
        stats.bytes_body += s.bytes_body;

        for (size_t i = 0; i < stats.status.size(); ++i) {
            stats.status[i] += s.status[i];
        }
        for (size_t i = 0; i < stats.sofarpcStatus.size(); ++i) {
            stats.sofarpcStatus[i] += s.sofarpcStatus[i];
        }
    }

    auto ts = process_time_stats(workers);

    // Requests which have not been issued due to connection errors, are
    // counted towards req_failed and req_error.
    auto req_not_issued = (config.nreqs - stats.req_status_success - stats.req_failed);
    if (config.is_timing_based_mode() || config.is_qps_mode()) {
        req_not_issued = 0;
    }
    stats.req_failed += req_not_issued;
    stats.req_error += req_not_issued;

    // UI is heavily inspired by weighttp[1] and wrk[2]
    //
    // [1] https://github.com/lighttpd/weighttp
    // [2] https://github.com/wg/wrk
    double rps = 0;
    int64_t bps = 0;
    if (duration.count() > 0) {
        if (config.is_timing_based_mode()) {
            // we only want to consider the main duration if warm-up is given
            rps = stats.req_success / config.duration;
            bps = stats.bytes_total / config.duration;
        } else {
            auto secd = std::chrono::duration_cast<
                std::chrono::duration<double, std::chrono::seconds::period>>(
                duration);
            rps = stats.req_success / secd.count();
            bps = stats.bytes_total / secd.count();
        }
    }

    double header_space_savings = 0.;
    if (stats.bytes_head_decomp > 0) {
        header_space_savings = 1. - static_cast<double>(stats.bytes_head) /
                                        stats.bytes_head_decomp;
    }

    auto totalReq = config.nreqs;
    if (config.is_timing_based_mode() && !config.is_qps_mode()) {
        totalReq = total_req_send.load();
    }

    std::cout << std::fixed << std::setprecision(2) << R"(
finished in )" << util::format_duration(duration)
              << ", " << rps << " req/s, " << util::utos_funit(bps) << R"(B/s
requests: )" << totalReq
              << " total, " << stats.req_started << " started, "
              << stats.req_done << " done, " << stats.req_status_success
              << " succeeded, " << stats.req_failed << " failed, "
              << stats.req_error << " errored, " << stats.req_timedout
              << " timeout";

    if (config.no_tls_proto == Config::PROTO_SOFARPC) {
        std::cout
            << std::fixed << std::setprecision(2) << R"(
sofaRPC status codes: )"
            << "\n\t" << stats.sofarpcStatus[RESPONSE_STATUS_SUCCESS]
            << " success, " << stats.sofarpcStatus[RESPONSE_STATUS_ERROR]
            << " error, "
            << stats.sofarpcStatus[RESPONSE_STATUS_SERVER_EXCEPTION]
            << " server exception, "
            << stats.sofarpcStatus[RESPONSE_STATUS_UNKNOWN] << " unknown\n\t"
            << stats.sofarpcStatus[RESPONSE_STATUS_SERVER_THREADPOOL_BUSY]
            << " server threadpool busy, "
            << stats.sofarpcStatus[RESPONSE_STATUS_ERROR_COMM]
            << " error comm, "
            << stats.sofarpcStatus[RESPONSE_STATUS_NO_PROCESSOR]
            << " no processor, " << stats.sofarpcStatus[RESPONSE_STATUS_TIMEOUT]
            << " timeout\n\t"
            << stats.sofarpcStatus[RESPONSE_STATUS_CLIENT_SEND_ERROR]
            << " client send error, "
            << stats.sofarpcStatus[RESPONSE_STATUS_CODEC_EXCEPTION]
            << " codec exception, "
            << stats.sofarpcStatus[RESPONSE_STATUS_CONNECTION_CLOSED]
            << " connection closed, "
            << stats.sofarpcStatus[RESPONSE_STATUS_SERVER_SERIAL_EXCEPTION]
            << " server serial exception\n\t"
            << stats.sofarpcStatus[RESPONSE_STATUS_SERVER_DESERIAL_EXCEPTION]
            << " server deserial exception";
    } else {
        std::cout << std::fixed << std::setprecision(2) << R"(
status codes: )" << stats.status[2]
                  << " 2xx, " << stats.status[3] << " 3xx, " << stats.status[4]
                  << " 4xx, " << stats.status[5] << " 5xx";
    }
    std::cout << std::fixed << std::setprecision(2) << R"(
traffic: )" << util::utos_funit(stats.bytes_total)
              << "B (" << stats.bytes_total << ") total, "
              << util::utos_funit(stats.bytes_head) << "B (" << stats.bytes_head
              << ") headers (space savings " << header_space_savings * 100
              << "%), " << util::utos_funit(stats.bytes_body) << "B ("
              << stats.bytes_body << R"() data
                     min         max         mean         sd        +/- sd
time for request: )"
              << std::setw(10) << util::format_duration(ts.request.min) << "  "
              << std::setw(10) << util::format_duration(ts.request.max) << "  "
              << std::setw(10) << util::format_duration(ts.request.mean) << "  "
              << std::setw(10) << util::format_duration(ts.request.sd)
              << std::setw(9) << util::dtos(ts.request.within_sd) << "%"
              << "\ntime for connect: " << std::setw(10)
              << util::format_duration(ts.connect.min) << "  " << std::setw(10)
              << util::format_duration(ts.connect.max) << "  " << std::setw(10)
              << util::format_duration(ts.connect.mean) << "  " << std::setw(10)
              << util::format_duration(ts.connect.sd) << std::setw(9)
              << util::dtos(ts.connect.within_sd) << "%"
              << "\nreq/s           : " << std::setw(10) << ts.rps.min << "  "
              << std::setw(10) << ts.rps.max << "  " << std::setw(10)
              << ts.rps.mean << "  " << std::setw(10) << ts.rps.sd
              << std::setw(9) << util::dtos(ts.rps.within_sd) << "%"
              << std::endl;

    SSL_CTX_free(ssl_ctx);

    uint64_t rtt_min = std::numeric_limits<uint64_t>::max();
    uint64_t rtt_max = std::numeric_limits<uint64_t>::min();
    for (const auto &worker : workers) {
        rtt_min = std::min(rtt_min, worker->rtt_min);
        rtt_max = std::max(rtt_max, worker->rtt_max);
    }
    bool invalid = false;
    if (rtt_min > rtt_max) {
        rtt_min = rtt_max = 0;
        invalid = true;
    }
    std::vector<uint64_t> rtts(rtt_max - rtt_min + 1, 0);
    uint64_t rtt_counts = 0;
    for (const auto &worker : workers) {
        rtt_counts += worker->rtts.size();
        for (auto i : worker->rtts)
            ++rtts[i - rtt_min];
    }
    std::vector<double> percentiles{50.0, 75.0, 90.0, 95.0, 99.0};
    std::cout << "\n  Latency  Distribution" << std::endl;
    for (int i = 0; i < percentiles.size(); i++) {
        double percentile = percentiles[i];
        uint64_t rank = std::round((percentile / 100.0) * rtt_counts + 0.5);
        uint64_t total = 0;
        uint64_t rtt = 0;
        for (rtt = rtt_min; rtt <= rtt_max; ++rtt) {
            total += rtts[rtt - rtt_min];
            if (total >= rank) {
                break;
            }
        }
        std::cout << std::setw(5) << std::setprecision(0) << percentile << "%"
                  << std::setw(13)
                  << (invalid ? "0us"
                              : util::format_duration(double(rtt) / 1000000.0))
                  << std::endl;
    }

    return 0;
}

} // namespace h2load

int main(int argc, char **argv) { return h2load::main(argc, argv); }
