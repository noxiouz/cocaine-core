#pragma once

#include "cocaine/context.hpp"
#include "cocaine/detail/service/node.v2/slot.hpp"
#include "cocaine/idl/node.hpp"

namespace cocaine {

struct manifest_t;
struct profile_t;

} // namespace cocaine

namespace cocaine {

class unix_actor_t;
class overseer_t;
class slave_t;
class control_t;
class streaming_dispatch_t;

}

namespace cocaine { namespace service { namespace v2 {

class balancer_t {
public:
    // TODO: Use signals?
    virtual void attach(std::shared_ptr<overseer_t>) = 0;
    virtual std::shared_ptr<streaming_dispatch_t>
    queue_changed(io::streaming_slot<io::app::enqueue>::upstream_type& upstream, std::string event) = 0;
    virtual void pool_changed() = 0;

    virtual void channel_started(/*uuid*/) {}
    virtual void channel_finished(/*uuid*/) {}
};

class app_t {
    context_t& context;

    const std::unique_ptr<logging::log_t> log;

    // Configuration.
    std::unique_ptr<const manifest_t> manifest;
    std::unique_ptr<const profile_t>  profile;

    std::shared_ptr<asio::io_service> loop;
    std::unique_ptr<unix_actor_t> engine;
    std::shared_ptr<overseer_t> overseer;
    std::shared_ptr<balancer_t> balancer;

public:
    app_t(context_t& context, const std::string& manifest, const std::string& profile);
    app_t(const app_t& other) = delete;

    ~app_t();

    app_t& operator=(const app_t& other) = delete;

private:
    void start();
};

}}} // namespace cocaine::service::v2
