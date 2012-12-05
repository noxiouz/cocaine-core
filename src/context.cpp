/*
    Copyright (c) 2011-2012 Andrey Sibiryov <me@kobology.ru>
    Copyright (c) 2011-2012 Other contributors as noted in the AUTHORS file.

    This file is part of Cocaine.

    Cocaine is free software; you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published by
    the Free Software Foundation; either version 3 of the License, or
    (at your option) any later version.

    Cocaine is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
    GNU Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with this program. If not, see <http://www.gnu.org/licenses/>. 
*/

#include "cocaine/context.hpp"

#include "cocaine/io.hpp"
#include "cocaine/logging.hpp"

#include "cocaine/isolates/process.hpp"

#include "cocaine/loggers/files.hpp"
#include "cocaine/loggers/stdout.hpp"
#include "cocaine/loggers/syslog.hpp"

#include "cocaine/storages/files.hpp"

#include <boost/filesystem/fstream.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/iterator/counting_iterator.hpp>

#include <netdb.h>

using namespace cocaine;

namespace fs = boost::filesystem;

const char defaults::slave[] = "/usr/bin/cocaine-worker-generic";

const float defaults::heartbeat_timeout = 30.0f;
const float defaults::idle_timeout = 600.0f;
const float defaults::startup_timeout = 10.0f;
const float defaults::termination_timeout = 5.0f;
const unsigned long defaults::pool_limit = 10L;
const unsigned long defaults::queue_limit = 100L;
const unsigned long defaults::concurrency = 10L;

const long defaults::control_timeout = 500L;
const unsigned long defaults::io_bulk_size = 100L;

const char defaults::plugins_path[] = "/usr/lib/cocaine";
const char defaults::runtime_path[] = "/var/run/cocaine";
const char defaults::spool_path[] = "/var/spool/cocaine";

// Config

namespace {
    void
    validate_path(const fs::path& path) {
        if(!fs::exists(path)) {
            throw configuration_error_t("the '%s' path does not exist", path.string());
        } else if(fs::exists(path) && !fs::is_directory(path)) {
            throw configuration_error_t("the '%s' path is not a directory", path.string());
        }
    }
}

config_t::config_t(const std::string& config_path) {
    if(!fs::exists(config_path)) {
        throw configuration_error_t("the configuration path doesn't exist");
    }

    if(!fs::is_regular(config_path)) {
        throw configuration_error_t("the configuration path doesn't point to a file");
    }

    path.config = config_path;

    fs::ifstream stream(path.config);

    if(!stream) {
        throw configuration_error_t("unable to open the configuration file");
    }

    Json::Reader reader(Json::Features::strictMode());
    Json::Value root;

    if(!reader.parse(stream, root)) {
        throw configuration_error_t("the configuration file is corrupted");
    }

    // Validation

    if(root.get("version", 0).asUInt() != 2) {
        throw configuration_error_t("the configuration version is invalid");
    }

    path.plugins = root["paths"].get("plugins", defaults::plugins_path).asString();
    path.runtime = root["paths"].get("runtime", defaults::runtime_path).asString();
    path.spool = root["paths"].get("spool", defaults::spool_path).asString();

    validate_path(path.plugins);
    validate_path(path.runtime);
    validate_path(path.spool);
    
    // IO configuration

    char hostname[256];

    if(gethostname(hostname, 256) == 0) {
        addrinfo hints,
                 * result;
        
        std::memset(&hints, 0, sizeof(addrinfo));

        hints.ai_flags = AI_CANONNAME;

        int rv = getaddrinfo(hostname, NULL, &hints, &result);
        
        if(rv != 0) {
            throw configuration_error_t("unable to determine the hostname - %s", gai_strerror(rv));
        }

        if(result == NULL) {
            throw configuration_error_t("unable to determine the hostname");
        }
        
        network.hostname = result->ai_canonname;

        freeaddrinfo(result);
    } else {
        throw system_error_t("unable to determine the hostname");
    }

    Json::Value range(root["port-mapper"]["range"]);

    network.ports = {
        range[0].asUInt(),
        range[1].asUInt()
    };

    network.threads = 1;

    // Component configuration

    services = parse(root["services"]);
    storages = parse(root["storages"]);
    loggers = parse(root["loggers"]);
}

config_t::component_map_t
config_t::parse(const Json::Value& config) {
    component_map_t components;

    if(config.empty()) {
        return components;
    }

    Json::Value::Members names(config.getMemberNames());

    for(Json::Value::Members::const_iterator it = names.begin();
        it != names.end();
        ++it)
    {
        component_t info = {
            config[*it].get("type", "unspecified").asString(),
            config[*it]["args"]
        };

        components.emplace(*it, info);
    }

    return components;
}

// Port mapper

port_mapper_t::port_mapper_t(const std::pair<uint16_t, uint16_t>& limits):
    m_ports(
        boost::make_counting_iterator(limits.first),
        boost::make_counting_iterator(limits.second)
    )
{ }

uint16_t
port_mapper_t::get() {
    boost::unique_lock<boost::mutex> lock(m_mutex);

    if(m_ports.empty()) {
        throw cocaine::error_t("no available ports left");
    }

    uint16_t port = m_ports.top();
    m_ports.pop();

    return port;
}

void
port_mapper_t::retain(uint16_t port) {
    boost::unique_lock<boost::mutex> lock(m_mutex);
    m_ports.push(port);
}

// Context

context_t::context_t(config_t config_,
                     const std::string& logger):
    config(config_)
{
    initialize();

    // Get the default logger for this context.
    m_logger = api::logger(*this, logger);
}

context_t::context_t(config_t config_,
                     std::unique_ptr<api::logger_t>&& logger):
    config(config_)
{
    initialize();

    // NOTE: The context takes the ownership of the passed logger, so it will
    // become invalid at the calling site after this call.
    m_logger = std::move(logger);
}

context_t::~context_t() {
    // Empty.
}

void
context_t::initialize() {
    // Initialize the ZeroMQ context.
    m_io.reset(new zmq::context_t(config.network.threads));

    // Initialize the ZeroMQ port mapper.
    m_port_mapper.reset(new port_mapper_t(config.network.ports));

    // Initialize the repository, without any components yet.
    m_repository.reset(new api::repository_t());

    // Register the builtin isolates.
    m_repository->insert<isolate::process_t>("process");

    // Register the builtin loggers
    m_repository->insert<logger::files_t>("files");
    m_repository->insert<logger::stdout_t>("stdout");
    m_repository->insert<logger::syslog_t>("syslog");
    
    // Register the builtin storages.
    m_repository->insert<storage::files_t>("files");

    // Register the plugins.
    m_repository->load(config.path.plugins);
}
