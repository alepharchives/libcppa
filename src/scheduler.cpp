/******************************************************************************\
 *           ___        __                                                    *
 *          /\_ \    __/\ \                                                   *
 *          \//\ \  /\_\ \ \____    ___   _____   _____      __               *
 *            \ \ \ \/\ \ \ '__`\  /'___\/\ '__`\/\ '__`\  /'__`\             *
 *             \_\ \_\ \ \ \ \L\ \/\ \__/\ \ \L\ \ \ \L\ \/\ \L\.\_           *
 *             /\____\\ \_\ \_,__/\ \____\\ \ ,__/\ \ ,__/\ \__/.\_\          *
 *             \/____/ \/_/\/___/  \/____/ \ \ \/  \ \ \/  \/__/\/_/          *
 *                                          \ \_\   \ \_\                     *
 *                                           \/_/    \/_/                     *
 *                                                                            *
 * Copyright (C) 2011, 2012                                                   *
 * Dominik Charousset <dominik.charousset@haw-hamburg.de>                     *
 *                                                                            *
 * This file is part of libcppa.                                              *
 * libcppa is free software: you can redistribute it and/or modify it under   *
 * the terms of the GNU Lesser General Public License as published by the     *
 * Free Software Foundation, either version 3 of the License                  *
 * or (at your option) any later version.                                     *
 *                                                                            *
 * libcppa is distributed in the hope that it will be useful,                 *
 * but WITHOUT ANY WARRANTY; without even the implied warranty of             *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.                       *
 * See the GNU Lesser General Public License for more details.                *
 *                                                                            *
 * You should have received a copy of the GNU Lesser General Public License   *
 * along with libcppa. If not, see <http://www.gnu.org/licenses/>.            *
\******************************************************************************/


#include <thread>
#include <atomic>
#include <chrono>
#include <iostream>

#include "cppa/on.hpp"
#include "cppa/self.hpp"
#include "cppa/anything.hpp"
#include "cppa/to_string.hpp"
#include "cppa/scheduler.hpp"
#include "cppa/local_actor.hpp"
#include "cppa/thread_mapped_actor.hpp"

#include "cppa/detail/actor_count.hpp"
#include "cppa/detail/singleton_manager.hpp"
#include "cppa/detail/thread_pool_scheduler.hpp"

using std::move;

namespace cppa { namespace {

typedef std::uint32_t ui32;

struct exit_observer : cppa::attachable {
    ~exit_observer() {
        cppa::detail::dec_actor_count();
    }
    void actor_exited(std::uint32_t) {
    }
    bool matches(const token&) {
        return false;
    }
};

inline decltype(std::chrono::high_resolution_clock::now()) now() {
    return std::chrono::high_resolution_clock::now();
}

void async_send_impl(channel* to, actor* from, any_tuple&& msg) {
    to->enqueue(from, move(msg));
}

void sync_reply_impl(actor* to, actor* from, message_id_t id, any_tuple&& msg) {
    to->sync_enqueue(from, id, move(msg));
}

typedef void (*async_send_fun)(channel*, actor*, any_tuple&&);
typedef void (*sync_reply_fun)(actor*, actor*, message_id_t, any_tuple&&);

class delayed_msg {

 public:

    delayed_msg(async_send_fun     arg0,
                const channel_ptr& arg1,
                const actor_ptr&   arg2,
                message_id_t,
                any_tuple&&        arg4)
    : fun(arg0), ptr_a(arg1), from(arg2), msg(move(arg4)) { }

    delayed_msg(sync_reply_fun     arg0,
                const actor_ptr&   arg1,
                const actor_ptr&   arg2,
                message_id_t       arg3,
                any_tuple&&        arg4)
    : fun(arg0), ptr_b(arg1), from(arg2), id(arg3), msg(move(arg4)) { }

    delayed_msg(delayed_msg&&) = default;
    delayed_msg(const delayed_msg&) = default;
    delayed_msg& operator=(delayed_msg&&) = default;
    delayed_msg& operator=(const delayed_msg&) = default;

    inline void eval() {
        if (fun.is_left()) (fun.left())(ptr_a.get(), from.get(), move(msg));
        else (fun.right())(ptr_b.get(), from.get(), id, move(msg));
    }

 private:

    either<async_send_fun, sync_reply_fun> fun;

    channel_ptr   ptr_a;
    actor_ptr     ptr_b;
    actor_ptr     from;
    message_id_t  id;
    any_tuple     msg;

};

} // namespace <anonymous>

class scheduler_helper {

 public:

    typedef intrusive_ptr<thread_mapped_actor> ptr_type;

    scheduler_helper() : m_worker(new thread_mapped_actor) { }

    void start() {
        m_thread = std::thread(&scheduler_helper::time_emitter, m_worker);
    }

    void stop() {
        m_worker->enqueue(nullptr, make_cow_tuple(atom("DIE")));
        m_thread.join();
    }

    ptr_type m_worker;
    std::thread m_thread;

 private:

    static void time_emitter(ptr_type m_self);

};

template<class Map, typename Fun, class T>
void insert_dmsg(Map& storage,
                 const util::duration& d,
                 Fun fun_ptr,
                 const T& to,
                 const actor_ptr& sender,
                 message_id_t id,
                 any_tuple&& tup    ) {
    auto tout = now();
    tout += d;
    delayed_msg dmsg{fun_ptr, to, sender, id, move(tup)};
    storage.insert(std::make_pair(std::move(tout), std::move(dmsg)));
}

void scheduler_helper::time_emitter(scheduler_helper::ptr_type m_self) {
    typedef detail::abstract_actor<local_actor> impl_type;
    typedef std::unique_ptr<detail::recursive_queue_node,detail::disposer> queue_node_ptr;
    // setup & local variables
    self.set(m_self.get());
    auto& queue = m_self->mailbox();
    std::multimap<decltype(now()), delayed_msg> messages;
    queue_node_ptr msg_ptr;
    //decltype(queue.pop()) msg_ptr = nullptr;
    decltype(now()) tout;
    bool done = false;
    // message handling rules
    auto mfun = (
        on(atom("SEND"), arg_match) >> [&](const util::duration& d,
                                           const channel_ptr& ptr,
                                           any_tuple& tup) {
            insert_dmsg(messages, d, async_send_impl,
                        ptr, msg_ptr->sender, message_id_t(), move(tup));
        },
        on(atom("REPLY"), arg_match) >> [&](const util::duration& d,
                                            const actor_ptr& ptr,
                                            message_id_t id,
                                            any_tuple& tup) {
            insert_dmsg(messages, d, sync_reply_impl,
                        ptr, msg_ptr->sender, id, move(tup));
        },
        on<atom("DIE")>() >> [&]() {
            done = true;
        },
        others() >> [&]() {
#           ifdef CPPA_DEBUG
                std::cerr << "scheduler_helper::time_emitter: UNKNOWN MESSAGE: "
                          << to_string(msg_ptr->msg)
                          << std::endl;
#           endif
        }
    );
    // loop
    while (!done) {
        while (!msg_ptr) {
            if (messages.empty()) {
                msg_ptr.reset(queue.pop());
            }
            else {
                tout = now();
                // handle timeouts (send messages)
                auto it = messages.begin();
                while (it != messages.end() && (it->first) <= tout) {
                    it->second.eval();
                    messages.erase(it);
                    it = messages.begin();
                }
                // wait for next message or next timeout
                if (it != messages.end()) {
                    msg_ptr.reset(queue.try_pop(it->first));
                }
            }
        }
        mfun(msg_ptr->msg);
        msg_ptr.reset();
    }
}

scheduler::scheduler() : m_helper(new scheduler_helper) {
}

void scheduler::initialize() {
    m_helper->start();
}

void scheduler::destroy() {
    m_helper->stop();
    delete this;
}

scheduler::~scheduler() {
    delete m_helper;
}

channel* scheduler::delayed_send_helper() {
    return m_helper->m_worker.get();
}

void scheduler::register_converted_context(actor* what) {
    if (what) {
        detail::inc_actor_count();
        what->attach(new exit_observer);
    }
}

attachable* scheduler::register_hidden_context() {
    detail::inc_actor_count();
    return new exit_observer;
}

void set_scheduler(scheduler* sched) {
    if (detail::singleton_manager::set_scheduler(sched) == false) {
        throw std::runtime_error("scheduler already set");
    }
}

void set_default_scheduler(size_t num_threads) {
    set_scheduler(new detail::thread_pool_scheduler(num_threads));
}

scheduler* get_scheduler() {
    return detail::singleton_manager::get_scheduler();
}

scheduler* scheduler::create_singleton() {
    return new detail::thread_pool_scheduler;
}

} // namespace cppa
