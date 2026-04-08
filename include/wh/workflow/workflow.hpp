// Defines the public fixed-shape workflow authoring entry by directly reusing
// compose::workflow instead of inventing a second workflow runtime.
#pragma once

#include "wh/compose/authored/workflow.hpp"
#include "wh/compose/field/mapping.hpp"

namespace wh::workflow {

using workflow = wh::compose::workflow;
using workflow_step_ref = wh::compose::workflow::step_ref;
using field_mapping_rule = wh::compose::field_mapping_rule;
using compiled_field_mapping_rule = wh::compose::compiled_field_mapping_rule;
using workflow_dependency_kind = wh::compose::workflow_dependency_kind;
using workflow_dependency = wh::compose::workflow_dependency;

} // namespace wh::workflow
