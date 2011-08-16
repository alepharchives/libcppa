#ifndef POST_OFFICE_HPP
#define POST_OFFICE_HPP

#include <memory>

#include "cppa/actor_proxy.hpp"
#include "cppa/detail/native_socket.hpp"

namespace cppa { namespace detail {

void post_office_add_peer(native_socket_t peer_socket,
                          const process_information_ptr& peer_ptr,
                          const actor_proxy_ptr& peer_actor_ptr,
                          std::unique_ptr<attachable>&& peer_observer);

void post_office_publish(native_socket_t server_socket,
                         const actor_ptr& published_actor);

void post_office_unpublish(std::uint32_t actor_id);

void post_office_close_socket(native_socket_t sfd);

//void post_office_unpublish(const actor_ptr& published_actor);

//void post_office_proxy_exited(const actor_proxy_ptr& proxy_ptr);

} } // namespace cppa::detail

#endif // POST_OFFICE_HPP