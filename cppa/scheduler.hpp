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


#ifndef CPPA_SCHEDULER_HPP
#define CPPA_SCHEDULER_HPP

#include <chrono>
#include <memory>
#include <cstdint>
#include <functional>

#include "cppa/atom.hpp"
#include "cppa/actor.hpp"
#include "cppa/channel.hpp"
#include "cppa/cow_tuple.hpp"
#include "cppa/attachable.hpp"
#include "cppa/local_actor.hpp"
#include "cppa/scheduling_hint.hpp"

#include "cppa/util/duration.hpp"

namespace cppa {

class self_type;
class scheduled_actor;
class scheduler_helper;

typedef std::function<void()> void_function;
typedef std::function<void(local_actor*)> init_callback;

namespace detail {
// forwards self_type as actor_ptr, otherwise equal to std::forward
template<typename T>
struct spawn_fwd_ {
    static inline T&& _(T&& arg) { return std::move(arg); }
    static inline T& _(T& arg) { return arg; }
    static inline const T& _(const T& arg) { return arg; }
};
template<>
struct spawn_fwd_<self_type> {
    static inline actor_ptr _(const self_type& s) { return s.get(); }
};
class singleton_manager;
} // namespace detail

/**
 * @brief
 */
class scheduler {

    scheduler_helper* m_helper;

    channel* delayed_send_helper();

    friend class detail::singleton_manager;

 protected:

    scheduler();

    virtual ~scheduler();


    /**
     * @warning Always call scheduler::initialize on overriding.
     */
    virtual void initialize();

    /**
     * @warning Always call scheduler::destroy on overriding.
     */
    virtual void destroy();

 private:

    static scheduler* create_singleton();

    inline void dispose() { delete this; }

 public:

    virtual void enqueue(scheduled_actor*) = 0;

    /**
     * @brief Informs the scheduler about a converted context
     *        (a thread that acts as actor).
     * @note Calls <tt>what->attach(...)</tt>.
     */
    virtual void register_converted_context(actor* what);

    /**
     * @brief Informs the scheduler about a hidden (non-actor)
     *        context that should be counted by await_others_done().
     * @returns An {@link attachable} that the hidden context has to destroy
     *         if his lifetime ends.
     */
    virtual attachable* register_hidden_context();

    template<typename Duration, typename... Data>
    void delayed_send(const channel_ptr& to,
                      const Duration& rel_time,
                      any_tuple data           ) {
        auto tup = make_any_tuple(atom("SEND"),
                                  util::duration{rel_time},
                                  to,
                                  std::move(data));
        delayed_send_helper()->enqueue(self, std::move(tup));
    }

    template<typename Duration, typename... Data>
    void delayed_reply(const actor_ptr& to,
                       const Duration& rel_time,
                       message_id_t id,
                       any_tuple data           ) {
        CPPA_REQUIRE(!id.valid() || id.is_response());
        if (id.valid()) {
            auto tup = make_any_tuple(atom("REPLY"),
                                      util::duration{rel_time},
                                      to,
                                      id,
                                      std::move(data));
            delayed_send_helper()->enqueue(self, std::move(tup));
        }
        else {
            this->delayed_send(to, rel_time, std::move(data));
        }
    }

    /**
     * @brief Spawns a new actor that executes <code>fun()</code>
     *        with the scheduling policy @p hint if possible.
     */
    virtual actor_ptr spawn(void_function fun, scheduling_hint hint) = 0;

    /**
     * @brief Spawns a new actor that executes <code>behavior()</code>
     *        with the scheduling policy @p hint if possible and calls
     *        <code>init_cb</code> after the actor is initialized but before
     *        it starts execution.
     */
    virtual actor_ptr spawn(void_function fun,
                            init_callback init_cb,
                            scheduling_hint hint) = 0;

    /**
     * @brief Spawns a new event-based actor.
     */
    virtual actor_ptr spawn(scheduled_actor* what,
                            scheduling_hint hint = scheduled) = 0;

    /**
     * @brief Spawns a new event-based actor and calls
     *        <code>init_cb</code> after the actor is initialized but before
     *        it starts execution.
     */
    virtual actor_ptr spawn(scheduled_actor* what,
                            init_callback init_cb,
                            scheduling_hint hint = scheduled) = 0;

    // hide implementation details for documentation
#   ifndef CPPA_DOCUMENTATION

    template<typename Fun, typename Arg0, typename... Args>
    actor_ptr spawn_impl(scheduling_hint hint, Fun&& fun, Arg0&& arg0, Args&&... args) {
        return this->spawn(
                    std::bind(
                        std::forward<Fun>(fun),
                        detail::spawn_fwd_<typename util::rm_ref<Arg0>::type>::_(arg0),
                        detail::spawn_fwd_<typename util::rm_ref<Args>::type>::_(args)...),
                    hint);
    }

    template<typename Fun>
    actor_ptr spawn_impl(scheduling_hint hint, Fun&& fun) {
        return this->spawn(std::forward<Fun>(fun), hint);
    }

    template<typename InitCallback, typename Fun, typename Arg0, typename... Args>
    actor_ptr spawn_cb_impl(scheduling_hint hint,
                            InitCallback&& init_cb,
                            Fun&& fun, Arg0&& arg0, Args&&... args) {
        return this->spawn(
                    std::bind(
                        std::forward<Fun>(fun),
                        detail::spawn_fwd_<typename util::rm_ref<Arg0>::type>::_(arg0),
                        detail::spawn_fwd_<typename util::rm_ref<Args>::type>::_(args)...),
                    std::forward<InitCallback>(init_cb),
                    hint);
    }

    template<typename InitCallback, typename Fun>
    actor_ptr spawn_cb_impl(scheduling_hint hint, InitCallback&& init_cb, Fun&& fun) {
        return this->spawn(std::forward<Fun>(fun),
                           std::forward<InitCallback>(init_cb),
                           hint);
    }

#   endif // CPPA_DOCUMENTATION

};

/**
 * @brief Sets the scheduler to @p sched.
 * @param sched A user-defined scheduler implementation.
 * @pre <tt>sched != nullptr</tt>.
 * @throws std::runtime_error if there's already a scheduler defined.
 */
void set_scheduler(scheduler* sched);

/**
 * @brief Sets a thread pool scheduler with @p num_threads worker threads.
 * @throws std::runtime_error if there's already a scheduler defined.
 */
void set_default_scheduler(size_t num_threads);

/**
 * @brief Returns the currently running scheduler.
 */
scheduler* get_scheduler();

} // namespace cppa::detail

#endif // CPPA_SCHEDULER_HPP
