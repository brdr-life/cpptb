// SPDX-License-Identifier: Apache-2.0
`ifndef CPPTB_LOG_SVH
`define CPPTB_LOG_SVH

`define cpptb_log(Level, Message, Scope="")                                \
  begin                                                                      \
    if (cpptb_log_pkg::enabled(Level)) begin                                 \
      cpptb_log_pkg::emit(                                                  \
          Level, Message, Scope, `__FILE__, `__LINE__, $sformatf("%m"),    \
          longint'($realtime / 1s * 1.0e15));                               \
    end                                                                      \
  end

`define cpptb_trace(Message, Scope="")                                    \
  `cpptb_log(cpptb_log_pkg::TRACE, Message, Scope)
`define cpptb_debug(Message, Scope="")                                    \
  `cpptb_log(cpptb_log_pkg::DEBUG, Message, Scope)
`define cpptb_info(Message, Scope="")                                     \
  `cpptb_log(cpptb_log_pkg::INFO, Message, Scope)
`define cpptb_warning(Message, Scope="")                                  \
  `cpptb_log(cpptb_log_pkg::WARNING, Message, Scope)
`define cpptb_warn(Message, Scope="")                                     \
  `cpptb_warning(Message, Scope)
`define cpptb_error(Message, Scope="")                                    \
  `cpptb_log(cpptb_log_pkg::ERROR, Message, Scope)

`endif
