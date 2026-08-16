#pragma once

/**
 * HeliosView.Core -- detail: minimal sender/operation-state machinery.
 *
 * Custom senders all follow one pattern: a sender whose operation state, once
 * started, completes its receiver through a small piece of "completion
 * logic". This header removes the boilerplate (sender_concept,
 * completion_signatures, connect(), the nested operation_state struct) and
 * keeps only the completion functor as the customization point:
 *
 *   struct Complete {
 *       // invoked exactly once from start(); must complete the receiver
 *       template <std::execution::receiver Recv>
 *       void operator()(Recv&& recv) const noexcept {
 *           std::execution::set_value(std::move(recv));            // inline
 *           // or boost::asio::post(*ctx, [recv = std::move(recv)]() mutable {
 *           //     std::execution::set_value(std::move(recv));     // deferred
 *           // });
 *       }
 *   };
 *   auto snd = helios::detail::BasicSender{Complete{}};
 *
 * The functor may capture any payload (an executor, a timer, a socket, ...);
 * it is stored in both the sender and the operation state. Value completions
 * are declared through the Values... template parameter:
 *   BasicSender<Complete, int>{...}   -> set_value_t(int)
 *
 * Internal header: not part of the public API surface; the namespace is
 * helios::detail by design.
 */

#include <HeliosViewCore/Execution.h>

#include <exception>
#include <type_traits>
#include <utility>

namespace helios::detail {

// BasicOperationState<Complete, Recv>:
//   stores the completion functor and the receiver; start() hands the
//   receiver to the functor, which must complete it exactly once (and
//   noexcept).
template <class Complete, class Recv>
struct BasicOperationState {
    using operation_state_concept = std::execution::operation_state_t;

    Complete complete;
    Recv recv;

    void start() & noexcept { complete(std::move(recv)); }
};

// BasicSender<Complete, Values...>:
//   a sender completing with set_value_t(Values...), set_error_t(
//   std::exception_ptr), set_stopped_t(). `Complete` is a functor with a
//   template operator():
//       template <std::execution::receiver Recv>
//       void operator()(Recv&& recv) const noexcept;
//   that completes the receiver.
template <class Complete, class... Values>
class BasicSender {
    Complete complete_;

public:
    using sender_concept = std::execution::sender_t;
    using completion_signatures = std::execution::completion_signatures<
        std::execution::set_value_t(Values...),
        std::execution::set_error_t(std::exception_ptr),
        std::execution::set_stopped_t()>;

    explicit BasicSender(Complete complete)
        : complete_(std::move(complete))
    {
    }

    template <std::execution::receiver Recv>
    auto connect(Recv recv) const
    {
        using recv_t = std::decay_t<Recv>;
        static_assert(std::is_nothrow_invocable_v<Complete&, recv_t>,
                      "helios::detail::BasicSender: the completion functor must be "
                      "noexcept-invocable with the receiver (template operator()(Recv&&) "
                      "const noexcept)");
        return BasicOperationState<Complete, recv_t>{complete_, std::move(recv)};
    }
};

// CTAD: BasicSender{functor} -> BasicSender<functor> (no value completions).
template <class Complete>
BasicSender(Complete) -> BasicSender<std::decay_t<Complete>>;

} // namespace helios::detail
