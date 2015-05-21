#pragma once

#include <functional>
#include <string>
#include <system_error>

#include <boost/circular_buffer.hpp>
#include <boost/variant/variant.hpp>

#include <asio/io_service.hpp>
#include <asio/deadline_timer.hpp>
#include <asio/posix/stream_descriptor.hpp>

#include "cocaine/api/isolate.hpp"
#include "cocaine/logging.hpp"
#include "cocaine/idl/rpc.hpp"
#include "cocaine/idl/node.hpp"

#include "cocaine/detail/unique_id.hpp"
#include "cocaine/detail/service/node/manifest.hpp"
#include "cocaine/detail/service/node/profile.hpp"

#include "cocaine/detail/service/node_v2/splitter.hpp"

#include "slave/error.hpp"

// TODO: Temporary.
#include "cocaine/detail/service/node_v2/slot.hpp"

namespace cocaine {
// TODO: Temporary.
class streaming_dispatch_t;

class active_t;
class broken_t;
class handshaking_t;
class spawning_t;
class state_t;
class terminating_t;

class control_t;
class fetcher_t;

typedef std::shared_ptr<
    const dispatch<io::event_traits<io::worker::rpc::invoke>::dispatch_type>
> inject_dispatch_ptr_t;

typedef std::function<void()> close_callback;

namespace slave {

struct channel_t {
    /// Event name to be processed.
    std::string event;
    std::shared_ptr<streaming_dispatch_t> dispatch;
    io::streaming_slot<io::app::enqueue>::upstream_type upstream;
};

}

struct slave_context {
    context_t&  context;
    manifest_t  manifest;
    profile_t   profile;
    std::string id;

    slave_context(context_t& context, manifest_t manifest, profile_t profile) :
        context(context),
        manifest(manifest),
        profile(profile),
        id(unique_id_t().string())
    {}
};

/// Actual slave implementation.
class state_machine_t:
    public std::enable_shared_from_this<state_machine_t>
{
    friend class active_t;
    friend class broken_t;
    friend class handshaking_t;
    friend class spawning_t;
    friend class terminating_t;

    friend class control_t;
    friend class fetcher_t;

    class channel_watcher_t;

    class lock_t {};

public:
    typedef std::function<void(std::uint64_t)> channel_handler;
    typedef std::function<void(const std::error_code&)> cleanup_handler;

private:
    const std::unique_ptr<logging::log_t> log;

    const slave_context context;
    // TODO: In current implementation this can be invalid, when engine is stopped.
    asio::io_service& loop;

    /// The flag means that the overseer has been destroyed and we shouldn't call the callback.
    std::atomic<bool> closed;
    cleanup_handler cleanup;

    splitter_t splitter;
    std::shared_ptr<fetcher_t> fetcher;
    boost::circular_buffer<std::string> lines;

    std::atomic<bool> shutdowned;

    synchronized<std::shared_ptr<state_t>> state;

    std::atomic<std::uint64_t> counter;
    enum side_t { none = 0x00, tx = 0x01, rx = 0x02, both = tx | rx };
    struct load_ctx {
        int side;
        channel_handler handler;
    };
    typedef std::unordered_map<std::uint64_t, load_ctx> load_map_t;

public:
    synchronized<load_map_t> load_;

public:
    /// Creates the state machine instance and immediately starts it.
    static
    std::shared_ptr<state_machine_t>
    create(slave_context context, asio::io_service& loop, cleanup_handler cleanup);

    state_machine_t(lock_t, slave_context context, asio::io_service& loop, cleanup_handler cleanup);

    ~state_machine_t();

    /// Returns true is the slave is in active state.
    bool
    active() const noexcept;

    std::uint64_t
    load() const;

    std::shared_ptr<control_t>
    activate(std::shared_ptr<session_t> session, upstream<io::worker::control_tag> stream);

    std::uint64_t
    inject(slave::channel_t& channel, channel_handler handler);

    /// Terminates the slave by sending terminate message to the worker instance.
    ///
    /// The cleanup callback won't be called after this call.
    void
    terminate(std::error_code ec);

private:
    /// Spawns a slave.
    ///
    /// \pre state == nullptr.
    /// \post state != nullptr.
    void
    start();

    void
    output(const char* data, size_t size);

    void
    migrate(std::shared_ptr<state_t> desired);

    /// Internal termination.
    ///
    /// Can be called multiple times, but only the first one takes an effect.
    void
    shutdown(std::error_code ec);

    void
    on_tx_channel_close(std::uint64_t id);

    void
    on_rx_channel_close(std::uint64_t id);

    void
    on_channel_close(std::uint64_t id, side_t side);
};

// TODO: Rename to `comrade`, because in Soviet Russia slave owns you!
class slave_t {
public:
    typedef state_machine_t::cleanup_handler cleanup_handler;

private:
    /// Termination reason.
    std::error_code ec;

    struct {
        const std::string id;
        const std::chrono::high_resolution_clock::time_point birthstamp;
    } data;

    /// The slave state machine implementation.
    std::shared_ptr<state_machine_t> machine;

public:
    slave_t(slave_context context, asio::io_service& loop, cleanup_handler fn);
    slave_t(const slave_t& other) = delete;
    slave_t(slave_t&&) = default;

    ~slave_t();

    slave_t& operator=(const slave_t& other) = delete;
    slave_t& operator=(slave_t&&) = default;

    const std::string&
    id() const noexcept;

    long long
    uptime() const;

    std::uint64_t
    load() const;

    struct channel_stats_t {
        std::uint64_t tx;
        std::uint64_t rx;
        std::uint64_t load;
    };

    channel_stats_t
    stats() const;

    bool
    active() const noexcept;

    std::shared_ptr<control_t>
    activate(std::shared_ptr<session_t> session, upstream<io::worker::control_tag> stream);

    std::uint64_t
    inject(slave::channel_t& channel, state_machine_t::channel_handler handler);

    /// Marks the slave for termination using the given error code.
    ///
    /// It will be terminated later in destructor.
    void
    terminate(std::error_code ec);
};

}
