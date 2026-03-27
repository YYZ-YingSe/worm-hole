// Defines the public stdexec utility facade.
#pragma once

#include "wh/core/stdexec/component_async_entry.hpp"
#include "wh/core/stdexec/concurrent_sender_vector.hpp"
#include "wh/core/stdexec/defer_sender.hpp"
#include "wh/core/stdexec/inspect_result_sender.hpp"
#include "wh/core/stdexec/manual_lifetime_box.hpp"
#include "wh/core/stdexec/op_buffer.hpp"
#include "wh/core/stdexec/map_result_sender.hpp"
#include "wh/core/stdexec/ready_result_sender.hpp"
#include "wh/core/stdexec/request_result_sender.hpp"
#include "wh/core/stdexec/result_sender.hpp"
#include "wh/core/stdexec/resume_policy.hpp"
#include "wh/core/stdexec/resume_scheduler.hpp"
#include "wh/core/stdexec/scheduler_handoff.hpp"
#include "wh/core/stdexec/try_schedule.hpp"
#include "wh/core/stdexec/variant_sender.hpp"
