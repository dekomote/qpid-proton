/*
 *
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 *
 */
#include "proton/container.hpp"
#include "proton/connection_options.hpp"
#include "proton/event.hpp"
#include "messaging_event.hpp"
#include "proton/connection.hpp"
#include "proton/session.hpp"
#include "proton/messaging_adapter.hpp"
#include "proton/acceptor.hpp"
#include "proton/error.hpp"
#include "proton/url.hpp"
#include "proton/sender.hpp"
#include "proton/receiver.hpp"
#include "proton/task.hpp"
#include "proton/ssl.hpp"
#include "proton/sasl.hpp"
#include "proton/transport.hpp"

#include "msg.hpp"
#include "container_impl.hpp"
#include "connector.hpp"
#include "contexts.hpp"
#include "uuid.hpp"

#include "proton/connection.h"
#include "proton/session.h"
#include "proton/handlers.h"
#include "proton/reactor.h"

namespace proton {

namespace {

struct handler_context {
    static handler_context& get(pn_handler_t* h) {
        return *reinterpret_cast<handler_context*>(pn_handler_mem(h));
    }
    static void cleanup(pn_handler_t*) {}

    /*
     * NOTE: this call, at the transition from C to C++ is possibly
     * the biggest performance bottleneck.  "Average" clients ignore
     * 90% of these events.  Current strategy is to create the
     * messaging_event on the stack.  For success, the messaging_event
     * should be small and free of indirect malloc/free/new/delete.
     */

    static void dispatch(pn_handler_t *c_handler, pn_event_t *c_event, pn_event_type_t type)
    {
        handler_context& hc(handler_context::get(c_handler));
        messaging_event mevent(c_event, type, hc.container_);
        mevent.dispatch(*hc.handler_);
        return;
    }

    container *container_;
    handler *handler_;
};


// Used to sniff for connector events before the reactor's global handler sees them.
class override_handler : public handler
{
  public:
    counted_ptr<pn_handler_t> base_handler;
    container_impl &container_impl_;

    override_handler(pn_handler_t *h, container_impl &c) : base_handler(h), container_impl_(c) {}

    virtual void on_unhandled(event &e) {
        proton_event *pne = dynamic_cast<proton_event *>(&e);
        // If not a Proton reactor event, nothing to override, nothing to pass along.
        if (!pne) return;
        int type = pne->type();
        if (!type) return;  // Also not from the reactor

        pn_event_t *cevent = pne->pn_event();
        pn_connection_t *conn = pn_event_connection(cevent);
        if (conn) {
            handler *override = connection_context::get(conn).handler.get();
            if (override && type != PN_CONNECTION_INIT) {
                // Send event to connector
                e.dispatch(*override);
            }
            else if (!override && type == PN_CONNECTION_INIT) {
                // Newly accepted connection from lister socket
                connection c(conn);
                container_impl_.configure_server_connection(c);
            }
        }
        pn_handler_dispatch(base_handler.get(), cevent, (pn_event_type_t) type);
    }
};

} // namespace

counted_ptr<pn_handler_t> container_impl::cpp_handler(handler *h)
{
    if (h->pn_handler_)
        return h->pn_handler_;
    counted_ptr<pn_handler_t> handler(
        pn_handler_new(&handler_context::dispatch, sizeof(struct handler_context),
                       &handler_context::cleanup),
        false);
    handler_context &hc = handler_context::get(handler.get());
    hc.container_ = &container_;
    hc.handler_ = h;
    h->pn_handler_ = handler;
    return handler;
}

container_impl::container_impl(container& c, handler *h, const std::string& id) :
    container_(c), reactor_(reactor::create()), handler_(h), id_(id),
    link_id_(0)
{
    if (id_.empty()) id_ = uuid().str();
    reactor_.container_context(container_);

    // Set our own global handler that "subclasses" the existing one
    pn_handler_t *global_handler = reactor_.pn_global_handler();
    override_handler_.reset(new override_handler(global_handler, *this));
    counted_ptr<pn_handler_t> cpp_global_handler(cpp_handler(override_handler_.get()));
    reactor_.pn_global_handler(cpp_global_handler.get());
    if (handler_) {
        counted_ptr<pn_handler_t> pn_handler(cpp_handler(handler_));
        reactor_.pn_handler(pn_handler.get());
    }


    // Note: we have just set up the following handlers that see events in this order:
    // messaging_handler (Proton C events), pn_flowcontroller (optional), messaging_adapter,
    // messaging_handler (Messaging events from the messaging_adapter, i.e. the delegate),
    // connector override, the reactor's default globalhandler (pn_iohandler)
}

container_impl::~container_impl() {}

connection container_impl::connect(const proton::url &url, const connection_options &user_opts) {
    connection_options opts = client_connection_options(); // Defaults
    opts.override(user_opts);
    handler *h = opts.handler();

    counted_ptr<pn_handler_t> chandler = h ? cpp_handler(h) : counted_ptr<pn_handler_t>();
    connection conn(reactor_.connection(chandler.get()));
    pn_unique_ptr<connector> ctor(new connector(conn, opts));
    ctor->address(url);  // TODO: url vector
    connection_context& cc(conn.context());
    cc.container_impl = this;
    cc.handler.reset(ctor.release());
    conn.open();
    return conn;
}

sender container_impl::open_sender(const proton::url &url) {
    connection conn = connect(url, connection_options());
    std::string path = url.path();
    sender snd = conn.default_session().open_sender(id_ + '-' + path);
    snd.target().address(path);
    snd.open();
    return snd;
}

receiver container_impl::open_receiver(const proton::url &url) {
    connection conn = connect(url, connection_options());
    std::string path = url.path();
    receiver rcv = conn.default_session().open_receiver(id_ + '-' + path);
    rcv.source().address(path);
    rcv.open();
    return rcv;
}

acceptor container_impl::listen(const proton::url& url) {
    connection_options opts = server_connection_options(); // Defaults
#ifdef PN_COMING_SOON
    opts.override(user_opts);
#endif
    handler *h = opts.handler();
    counted_ptr<pn_handler_t> chandler = h ? cpp_handler(h) : counted_ptr<pn_handler_t>();
    pn_acceptor_t *acptr = pn_reactor_acceptor(reactor_.pn_object(), url.host().c_str(), url.port().c_str(), chandler.get());
    if (!acptr)
        throw error(MSG("accept fail: " <<
                        pn_error_text(pn_io_error(reactor_.pn_io())))
                        << "(" << url << ")");
#ifdef PROTON_1054_FIXED
    // Do not use pn_acceptor_set_ssl_domain().  Manage the incoming connections ourselves for
    // more flexibility (i.e. ability to change the server cert for a long running listener).
    listener_context& lc(listener_context::get(acptr));
    lc.connection_options = opts;
    lc.ssl = url.scheme() == url::AMQPS;
#else
    if (url.scheme() == url::AMQPS)
        pn_acceptor_set_ssl_domain(acptr, server_connection_options_.server_domain().pn_domain());
#endif
    return acceptor(acptr);
}

std::string container_impl::next_link_name() {
    std::ostringstream s;
    // TODO aconway 2015-09-01: atomic operation
    s << std::hex << ++link_id_ << "@" << id_;
    return s.str();
}

task container_impl::schedule(int delay, handler *h) {
    counted_ptr<pn_handler_t> task_handler;
    if (h)
        task_handler = cpp_handler(h);
    return reactor_.schedule(delay, task_handler.get());
}

void container_impl::client_connection_options(const connection_options &opts) {
    client_connection_options_ = opts;
}

void container_impl::server_connection_options(const connection_options &opts) {
    server_connection_options_ = opts;
}

void container_impl::configure_server_connection(connection &c) {
#ifdef PN_1054_FIXED
    pn_acceptor_t *pnp = pn_connection_acceptor(pn_cast(&c));
    listener_context &lc(listener_context::get(pnp));
    class connection_options &opts(lc.connection_options);
#else
    // Can't distinguish between multiple listeners yet.  See PROTON-1054
    class connection_options &opts(server_connection_options_);
#endif
    opts.apply(c);
}

}