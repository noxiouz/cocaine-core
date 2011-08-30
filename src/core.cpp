#include <iomanip>
#include <sstream>

#include <boost/bind.hpp>
#include <boost/tuple/tuple.hpp>
#include <boost/lexical_cast.hpp>
#include <msgpack.hpp>

#include "core.hpp"
#include "future.hpp"

using namespace yappi::core;
using namespace yappi::engine;
using namespace yappi::persistance;
using namespace yappi::plugin;

core_t::core_t(helpers::auto_uuid_t uuid,
               const std::vector<std::string>& listeners,
               const std::vector<std::string>& publishers,
               uint64_t hwm, bool purge):
    m_registry("/usr/lib/yappi") /* [CONFIG] */,
    m_signer(uuid),
    m_storage(uuid),
    m_context(1),
    s_events(m_context, ZMQ_PULL),
    s_publisher(m_context, ZMQ_PUB),
    s_requests(m_context, ZMQ_ROUTER),
    s_futures(m_context, ZMQ_PULL),
    s_reaper(m_context, ZMQ_PULL)
{
    // Version dump
    int minor, major, patch;
    zmq_version(&major, &minor, &patch);

    syslog(LOG_INFO, "core: using libzmq version %d.%d.%d", major, minor, patch);
    syslog(LOG_INFO, "core: using libev version %d.%d", ev_version_major(), ev_version_minor());
    syslog(LOG_INFO, "core: using libmsgpack version %s", msgpack_version());
    syslog(LOG_INFO, "core: instance uuid - %s", uuid.get().c_str());

    // Internal event sink socket
    s_events.bind("inproc://events");
    e_events.set<core_t, &core_t::event>(this);
    e_events.start(s_events.fd(), EV_READ);

    // Internal future sink socket
    s_futures.bind("inproc://futures");
    e_futures.set<core_t, &core_t::future>(this);
    e_futures.start(s_futures.fd(), EV_READ);

    // Internal engine reaping requests sink
    s_reaper.bind("inproc://reaper");
    e_reaper.set<core_t, &core_t::reap>(this);
    e_reaper.start(s_reaper.fd(), EV_READ);

    // Listening socket
    for(std::vector<std::string>::const_iterator it = listeners.begin(); it != listeners.end(); ++it) {
        s_requests.bind(*it);
        syslog(LOG_INFO, "core: listening for requests on %s", it->c_str());
    }

    e_requests.set<core_t, &core_t::request>(this);
    e_requests.start(s_requests.fd(), EV_READ);

    // Publishing socket
    s_publisher.setsockopt(ZMQ_HWM, &hwm, sizeof(hwm));

    for(std::vector<std::string>::const_iterator it = publishers.begin(); it != publishers.end(); ++it) {
        s_publisher.bind(*it);
        syslog(LOG_INFO, "core: publishing events on %s", it->c_str());
    }

    // Initialize signal watchers
    e_sigint.set<core_t, &core_t::terminate>(this);
    e_sigint.start(SIGINT);

    e_sigterm.set<core_t, &core_t::terminate>(this);
    e_sigterm.start(SIGTERM);

    e_sigquit.set<core_t, &core_t::terminate>(this);
    e_sigquit.start(SIGQUIT);

    e_sighup.set<core_t, &core_t::reload>(this);
    e_sighup.start(SIGHUP);

    if(purge) {
        m_storage.purge();
    } else {
        recover();
    }
}

core_t::~core_t() {
    syslog(LOG_INFO, "core: shutting down the engines");

    // Clearing up all the pending futures
    m_futures.clear();
    
    // Stopping the engines
    m_engines.clear();
}

void core_t::run() {
    m_loop.loop();
}

void core_t::terminate(ev::sig& sig, int revents) {
    m_loop.unloop();
}

void core_t::reload(ev::sig& sig, int revents) {
    syslog(LOG_NOTICE, "core: reloading tasks");

    m_futures.clear();
    m_engines.clear();

    recover();
}

void core_t::request(ev::io& io, int revents) {
    zmq::message_t message, signature;
    std::vector<std::string> route;
    std::string request;
    
    Json::Reader reader(Json::Features::strictMode());
    Json::Value root;

    while(s_requests.pending()) {
        route.clear();
        
        while(true) {
            s_requests.recv(&message);

            if(message.size() == 0) {
                // Break if we got a delimiter
                break;
            }

            route.push_back(std::string(
                static_cast<const char*>(message.data()),
                message.size()));
        }

        // Receive the request
        s_requests.recv(&message);

        request.assign(static_cast<const char*>(message.data()),
            message.size());

        // Receive the signature, if it's there
        signature.rebuild();

        if(s_requests.has_more()) {
            s_requests.recv(&signature);
        }

        // Construct the future
        future_t* future = new future_t(this, route);
        m_futures.insert(future->id(), future);
        
        // Parse the request
        root.clear();

        if(reader.parse(request, root)) {
            try {
                if(!root.isObject()) {
                    throw std::runtime_error("object expected");
                }

                unsigned int version = root.get("version", 1).asUInt();
                std::string token = root.get("token", "").asString();
                
                if(version >= 2) {
                    future->set("protocol", boost::lexical_cast<std::string>(version));
                } else {
                    throw std::runtime_error("outdated protocol version");
                }
      
                if(!token.empty()) {
                    future->set("token", token);

                    if(version > 2) {
                        m_signer.verify(request,
                            static_cast<const unsigned char*>(signature.data()),
                            signature.size(), token);
                    }
                } else {
                    throw std::runtime_error("security token expected");
                }

                dispatch(future, root); 
            } catch(const std::exception& e) {
                syslog(LOG_ERR, "core: invalid request - %s", e.what());
                future->fulfill("error", e.what());
            }
        } else {
            syslog(LOG_ERR, "core: invalid json - %s",
                reader.getFormatedErrorMessages().c_str());
            future->fulfill("error", reader.getFormatedErrorMessages());
        }
    }
}

void core_t::dispatch(future_t* future, const Json::Value& root) {
    std::string action = root.get("action", "push").asString();
    
    if(action == "push" || action == "drop") {
        Json::Value targets = root["targets"];

        if(!targets.isObject() || !targets.size()) {
            throw std::runtime_error("no targets specified");
        }

        // Iterate over all the targets
        Json::Value::Members names = targets.getMemberNames();
        future->await(names.size());

        for(Json::Value::Members::const_iterator it = names.begin(); it != names.end(); ++it) {
            std::string target = *it;

            // Get the target args
            Json::Value args = targets[target];
            Json::Value response;

            // And check if it's an object
            if(!args.isObject()) {
                syslog(LOG_ERR, "core: invalid request - target arguments expected");
                response["error"] = "target arguments expected";
                future->fulfill(target, response);
                continue;
            }

            if(action == "push") {
                push(future, target, args);
            } else {
                drop(future, target, args);
            }
        }
    } else if(action == "stats") {
        stat(future);
    } else {
        throw std::runtime_error("unsupported action");
    }
}

// Built-in commands:
// --------------
// * Push - launches a thread which fetches data from the
//   specified source and publishes it via the PUB socket.
//
// * Drop - shuts down the specified collector.
//   Remaining messages will stay orphaned in the queue,
//   so it's a good idea to drain it after the unsubscription:
//
// * Stats - fetches the current running stats

void core_t::push(future_t* future, const std::string& target, const Json::Value& args) {
    Json::Value response;
   
    // Check if we have an engine running for the given uri
    engine_map_t::iterator it = m_engines.find(target); 
    engine_t* engine = NULL;

    if(it == m_engines.end()) {
        try {
            // If the engine wasn't found, try to start a new one
            engine = new engine_t(m_context, m_registry, m_storage, target);
            m_engines.insert(target, engine);
        } catch(const std::exception& e) {
            syslog(LOG_ERR, "core: exception in push() - %s", e.what());
            response["error"] = e.what();
            future->fulfill(target, response);
            return;
        } catch(...) {
            syslog(LOG_CRIT, "core: unexpected exception in push()");
            abort();
        }
    } else {
        engine = it->second;
    }

    // Dispatch!
    engine->push(future, args);
}

void core_t::drop(future_t* future, const std::string& target, const Json::Value& args) {
    Json::Value response;

    engine_map_t::iterator it = m_engines.find(target);
    
    if(it == m_engines.end()) {
        syslog(LOG_ERR, "core: engine %s not found", target.c_str());
        response["error"] = "engine not found";
        future->fulfill(target, response);
        return;
    }

    // Dispatch!
    engine_t* engine = it->second;
    engine->drop(future, args);
}

void core_t::stat(future_t* future) {
    Json::Value engines, threads, requests;

    future->await(3);

    for(engine_map_t::const_iterator it = m_engines.begin(); it != m_engines.end(); ++it) {
        engines["list"].append(it->first);
    }
    
    engines["total"] = engine::engine_t::objects_created;
    engines["alive"] = engine::engine_t::objects_alive;
    future->fulfill("engines", engines);
    
    threads["total"] = engine::detail::thread_t::objects_created;
    threads["alive"] = engine::detail::thread_t::objects_alive;
    future->fulfill("threads", threads);

    requests["total"] = future_t::objects_created;
    requests["pending"] = future_t::objects_alive;
    future->fulfill("requests", requests);
}

void core_t::seal(const std::string& future_id) {
    future_map_t::iterator it = m_futures.find(future_id);

    if(it == m_futures.end()) {
        syslog(LOG_ERR, "core: found an orphan - future %s", future_id.c_str());
        return;
    }
        
    future_t* future = it->second;
    std::vector<std::string> route = future->route();

    // Send it if it's not an internal future
    if(!route.empty()) {
        zmq::message_t message;
        
        syslog(LOG_DEBUG, "core: sending response to '%s' - future %s", 
            future->get("token").c_str(), future->id().c_str());

        // Send the identity
        for(std::vector<std::string>::const_iterator id = route.begin(); id != route.end(); ++id) {
            message.rebuild(id->length());
            memcpy(message.data(), id->data(), id->length());
            s_requests.send(message, ZMQ_SNDMORE);
        }
        
        // Send the delimiter
        message.rebuild(0);
        s_requests.send(message, ZMQ_SNDMORE);

        // Send the JSON
        s_requests.send_json(future->root());
    }

    // Release the future
    m_futures.erase(it);
}

// Publishing format (not JSON, as it will render subscription mechanics pointless):
// ------------------
//   multipart: [key field timestamp] [blob]

void core_t::event(ev::io& io, int revents) {
    zmq::message_t message;
    std::string scheduler_id;
    dict_t dict;
    
    while(s_events.pending()) {
        // Receive the scheduler id
        s_events.recv(&message);
        scheduler_id.assign(
            static_cast<const char*>(message.data()),
            message.size());
    
        // Receive the data
        s_events.recv(&message);
        msgpack::unpacked unpacked;
        msgpack::unpack(&unpacked,
            static_cast<const char*>(message.data()),
            message.size());
        msgpack::object object = unpacked.get();
        object.convert(&dict);

        // Disassemble and send in the envelopes
        for(dict_t::const_iterator it = dict.begin(); it != dict.end(); ++it) {
            std::ostringstream envelope;
            envelope << scheduler_id << " " << it->first << " "
                     << std::fixed << std::setprecision(3) << m_loop.now();

            message.rebuild(envelope.str().length());
            memcpy(message.data(), envelope.str().data(), envelope.str().length());
            s_publisher.send(message, ZMQ_SNDMORE);

            message.rebuild(it->second.length());
            memcpy(message.data(), it->second.data(), it->second.length());
            s_publisher.send(message);
        }

        dict.clear();
    }
}

void core_t::future(ev::io& io, int revents) {
    while(s_futures.pending()) {
        Json::Value message;
        s_futures.recv_json(message);

        future_map_t::iterator it = m_futures.find(message["future"].asString());
        
        if(it == m_futures.end()) {
            syslog(LOG_ERR, "core: found an orphan - slice for future %s", 
                message["future"].asCString());
            continue;
        }

        future_t* future = it->second;
        future->fulfill(message["engine"].asString(), message["result"]);
    }
}

void core_t::reap(ev::io& io, int revents) {
    while(s_reaper.pending()) {
        Json::Value message;
        s_reaper.recv_json(message);

        engine_map_t::iterator it = m_engines.find(message["engine"].asString());

        if(it == m_engines.end()) {
            syslog(LOG_ERR, "core: found an orphan - engine %s", message["engine"].asCString());
            continue;
        }
        
        syslog(LOG_DEBUG, "core: suicide requested for thread %s in engine %s",
            message["thread"].asCString(), message["engine"].asCString());

        engine_t* engine = it->second;
        engine->reap(message["thread"].asString());
    }
}

void core_t::recover() {
    Json::Value root = m_storage.all();

    if(root.size()) {
        syslog(LOG_NOTICE, "core: loaded %d task(s)", root.size());
        
        future_t* future = new future_t(this, std::vector<std::string>());
        m_futures.insert(future->id(), future);
        future->await(root.size());
                
        Json::Value::Members ids = root.getMemberNames();
        
        for(Json::Value::Members::const_iterator it = ids.begin(); it != ids.end(); ++it) {
            Json::Value object = root[*it];
            future->set("token", object["token"].asString());
            push(future, object["url"].asString(), object["args"]);
        }
    }
}

