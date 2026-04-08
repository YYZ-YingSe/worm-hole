// Defines DAG-specific graph-run sender and operation wrappers.
#pragma once

#include <memory>

#include "wh/compose/graph/graph.hpp"
#include "wh/compose/graph/detail/runtime/dag_run_state.hpp"
#include "wh/compose/graph/detail/dag.hpp"
#include "wh/core/stdexec/detail/shared_operation_state.hpp"

namespace wh::compose::detail::invoke_runtime {

class dag_run_sender {
public:
  using sender_concept = stdexec::sender_t;
  using completion_signatures = stdexec::completion_signatures<
      stdexec::set_value_t(wh::core::result<graph_value>)>;

  explicit dag_run_sender(dag_run_state state) {
    state_.emplace(std::move(state));
    state_->rebind_moved_runtime_storage();
  }

  template <typename self_t,
            stdexec::receiver_of<completion_signatures> receiver_t>
    requires std::same_as<std::remove_cvref_t<self_t>, dag_run_sender>
  STDEXEC_EXPLICIT_THIS_BEGIN(auto connect)(this self_t &&self,
                                            receiver_t receiver) {
    using stored_receiver_t = std::remove_cvref_t<receiver_t>;
    using graph_scheduler_t = wh::core::detail::any_resume_scheduler_t;
    using dag_run_t = dag_run<stored_receiver_t, graph_scheduler_t>;
    auto graph_scheduler =
        graph_scheduler_t{*self.state_->invoke_state().graph_scheduler};
    return wh::core::detail::shared_operation_state<dag_run_t>{
        std::make_shared<dag_run_t>(std::move(*self.state_),
                                    std::move(receiver),
                                    std::move(graph_scheduler))};
  }
  STDEXEC_EXPLICIT_THIS_END(connect)

private:
  mutable std::optional<dag_run_state> state_{};
};

} // namespace wh::compose::detail::invoke_runtime
