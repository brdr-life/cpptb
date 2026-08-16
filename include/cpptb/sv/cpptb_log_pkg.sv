// SPDX-License-Identifier: Apache-2.0
package cpptb_log_pkg;
  localparam int unsigned TRACE = 0;
  localparam int unsigned DEBUG = 1;
  localparam int unsigned INFO = 2;
  localparam int unsigned WARNING = 3;
  localparam int unsigned ERROR = 4;
  localparam int unsigned OFF = 5;

  import "DPI-C" function int unsigned cpptb_sv_log_minimum_level();
  import "DPI-C" function void cpptb_sv_log(
      input int unsigned level,
      input string message,
      input string scope,
      input string source_file,
      input int unsigned source_line,
      input string hierarchy,
      input longint unsigned simulation_time_fs
  );

  int unsigned minimum_level = TRACE;

  function automatic void configure();
    minimum_level = cpptb_sv_log_minimum_level();
  endfunction

  function automatic bit enabled(input int unsigned level);
    return minimum_level != OFF && level != OFF && level >= minimum_level;
  endfunction

  function automatic void emit(
      input int unsigned level,
      input string message,
      input string scope,
      input string source_file,
      input int unsigned source_line,
      input string hierarchy,
      input longint unsigned simulation_time_fs
  );
    cpptb_sv_log(level, message, scope, source_file, source_line, hierarchy,
                 simulation_time_fs);
  endfunction
endpackage
