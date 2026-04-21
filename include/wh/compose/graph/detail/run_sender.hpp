// Defines shared graph-run sender wrappers for concrete DAG/Pregel runtimes.
#pragma once

#include "wh/compose/graph/detail/dag.hpp"
#include "wh/compose/graph/detail/pregel.hpp"
#include "wh/compose/graph/graph.hpp"

namespace wh::compose::detail::invoke_runtime {

template <typename runtime_t, template <typename, typename> class run_t> class basic_run_sender {
public:
  using sender_concept = stdexec::sender_t;
  using completion_signatures =
      stdexec::completion_signatures<stdexec::set_value_t(wh::core::result<graph_value>)>;

  explicit basic_run_sender(runtime_t runtime) {
    runtime_.emplace(std::move(runtime));
    runtime_->rebind_moved_runtime_storage();
  }

  template <typename self_t, stdexec::receiver_of<completion_signatures> receiver_t>
    requires std::same_as<std::remove_cvref_t<self_t>, basic_run_sender> &&
             (!std::is_const_v<std::remove_reference_t<self_t>>)
  STDEXEC_EXPLICIT_THIS_BEGIN(auto connect)(this self_t &&self, receiver_t receiver) {
    using stored_receiver_t = std::remove_cvref_t<receiver_t>;
    using graph_scheduler_t = wh::core::detail::any_resume_scheduler_t;
    using run_operation_t = run_t<stored_receiver_t, graph_scheduler_t>;
    auto graph_scheduler =
        graph_scheduler_t{*self.runtime_->session().invoke_state().control_scheduler};
    return run_operation_t{std::move(*self.runtime_), std::move(receiver),
                           std::move(graph_scheduler)};
  }
  STDEXEC_EXPLICIT_THIS_END(connect)

private:
  mutable std::optional<runtime_t> runtime_{};
};

using dag_run_sender = basic_run_sender<dag_runtime, dag_run>;
using pregel_run_sender = basic_run_sender<pregel_runtime, pregel_run>;

} // namespace wh::compose::detail::invoke_runtime
