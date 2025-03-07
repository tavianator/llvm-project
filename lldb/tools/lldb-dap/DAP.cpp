//===-- DAP.cpp -------------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include <chrono>
#include <cstdarg>
#include <fstream>
#include <mutex>
#include <sstream>

#include "DAP.h"
#include "LLDBUtils.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/FormatVariadic.h"

#if defined(_WIN32)
#define NOMINMAX
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#endif

using namespace lldb_dap;

namespace lldb_dap {

DAP g_dap;

DAP::DAP()
    : broadcaster("lldb-dap"),
      exception_breakpoints(
          {{"cpp_catch", "C++ Catch", lldb::eLanguageTypeC_plus_plus},
           {"cpp_throw", "C++ Throw", lldb::eLanguageTypeC_plus_plus},
           {"objc_catch", "Objective-C Catch", lldb::eLanguageTypeObjC},
           {"objc_throw", "Objective-C Throw", lldb::eLanguageTypeObjC},
           {"swift_catch", "Swift Catch", lldb::eLanguageTypeSwift},
           {"swift_throw", "Swift Throw", lldb::eLanguageTypeSwift}}),
      focus_tid(LLDB_INVALID_THREAD_ID), sent_terminated_event(false),
      stop_at_entry(false), is_attach(false),
      enable_auto_variable_summaries(false),
      enable_synthetic_child_debugging(false),
      restarting_process_id(LLDB_INVALID_PROCESS_ID),
      configuration_done_sent(false), waiting_for_run_in_terminal(false),
      progress_event_reporter(
          [&](const ProgressEvent &event) { SendJSON(event.ToJSON()); }),
      reverse_request_seq(0), repl_mode(ReplMode::Auto),
      auto_repl_mode_collision_warning(false) {
  const char *log_file_path = getenv("LLDBDAP_LOG");
#if defined(_WIN32)
  // Windows opens stdout and stdin in text mode which converts \n to 13,10
  // while the value is just 10 on Darwin/Linux. Setting the file mode to binary
  // fixes this.
  int result = _setmode(fileno(stdout), _O_BINARY);
  assert(result);
  result = _setmode(fileno(stdin), _O_BINARY);
  UNUSED_IF_ASSERT_DISABLED(result);
  assert(result);
#endif
  if (log_file_path)
    log.reset(new std::ofstream(log_file_path));
}

DAP::~DAP() = default;

ExceptionBreakpoint *DAP::GetExceptionBreakpoint(const std::string &filter) {
  for (auto &bp : exception_breakpoints) {
    if (bp.filter == filter)
      return &bp;
  }
  return nullptr;
}

ExceptionBreakpoint *DAP::GetExceptionBreakpoint(const lldb::break_id_t bp_id) {
  for (auto &bp : exception_breakpoints) {
    if (bp.bp.GetID() == bp_id)
      return &bp;
  }
  return nullptr;
}

// Send the JSON in "json_str" to the "out" stream. Correctly send the
// "Content-Length:" field followed by the length, followed by the raw
// JSON bytes.
void DAP::SendJSON(const std::string &json_str) {
  output.write_full("Content-Length: ");
  output.write_full(llvm::utostr(json_str.size()));
  output.write_full("\r\n\r\n");
  output.write_full(json_str);
}

// Serialize the JSON value into a string and send the JSON packet to
// the "out" stream.
void DAP::SendJSON(const llvm::json::Value &json) {
  std::string s;
  llvm::raw_string_ostream strm(s);
  strm << json;
  static std::mutex mutex;
  std::lock_guard<std::mutex> locker(mutex);
  std::string json_str = strm.str();
  SendJSON(json_str);

  if (log) {
    *log << "<-- " << std::endl
         << "Content-Length: " << json_str.size() << "\r\n\r\n"
         << llvm::formatv("{0:2}", json).str() << std::endl;
  }
}

// Read a JSON packet from the "in" stream.
std::string DAP::ReadJSON() {
  std::string length_str;
  std::string json_str;
  int length;

  if (!input.read_expected(log.get(), "Content-Length: "))
    return json_str;

  if (!input.read_line(log.get(), length_str))
    return json_str;

  if (!llvm::to_integer(length_str, length))
    return json_str;

  if (!input.read_expected(log.get(), "\r\n"))
    return json_str;

  if (!input.read_full(log.get(), length, json_str))
    return json_str;

  if (log)
    *log << "--> " << std::endl << "Content-Length: " << length << "\r\n\r\n";

  return json_str;
}

// "OutputEvent": {
//   "allOf": [ { "$ref": "#/definitions/Event" }, {
//     "type": "object",
//     "description": "Event message for 'output' event type. The event
//                     indicates that the target has produced some output.",
//     "properties": {
//       "event": {
//         "type": "string",
//         "enum": [ "output" ]
//       },
//       "body": {
//         "type": "object",
//         "properties": {
//           "category": {
//             "type": "string",
//             "description": "The output category. If not specified,
//                             'console' is assumed.",
//             "_enum": [ "console", "stdout", "stderr", "telemetry" ]
//           },
//           "output": {
//             "type": "string",
//             "description": "The output to report."
//           },
//           "variablesReference": {
//             "type": "number",
//             "description": "If an attribute 'variablesReference' exists
//                             and its value is > 0, the output contains
//                             objects which can be retrieved by passing
//                             variablesReference to the VariablesRequest."
//           },
//           "source": {
//             "$ref": "#/definitions/Source",
//             "description": "An optional source location where the output
//                             was produced."
//           },
//           "line": {
//             "type": "integer",
//             "description": "An optional source location line where the
//                             output was produced."
//           },
//           "column": {
//             "type": "integer",
//             "description": "An optional source location column where the
//                             output was produced."
//           },
//           "data": {
//             "type":["array","boolean","integer","null","number","object",
//                     "string"],
//             "description": "Optional data to report. For the 'telemetry'
//                             category the data will be sent to telemetry, for
//                             the other categories the data is shown in JSON
//                             format."
//           }
//         },
//         "required": ["output"]
//       }
//     },
//     "required": [ "event", "body" ]
//   }]
// }
void DAP::SendOutput(OutputType o, const llvm::StringRef output) {
  if (output.empty())
    return;

  llvm::json::Object event(CreateEventObject("output"));
  llvm::json::Object body;
  const char *category = nullptr;
  switch (o) {
  case OutputType::Console:
    category = "console";
    break;
  case OutputType::Stdout:
    category = "stdout";
    break;
  case OutputType::Stderr:
    category = "stderr";
    break;
  case OutputType::Telemetry:
    category = "telemetry";
    break;
  }
  body.try_emplace("category", category);
  EmplaceSafeString(body, "output", output.str());
  event.try_emplace("body", std::move(body));
  SendJSON(llvm::json::Value(std::move(event)));
}

// interface ProgressStartEvent extends Event {
//   event: 'progressStart';
//
//   body: {
//     /**
//      * An ID that must be used in subsequent 'progressUpdate' and
//      'progressEnd'
//      * events to make them refer to the same progress reporting.
//      * IDs must be unique within a debug session.
//      */
//     progressId: string;
//
//     /**
//      * Mandatory (short) title of the progress reporting. Shown in the UI to
//      * describe the long running operation.
//      */
//     title: string;
//
//     /**
//      * The request ID that this progress report is related to. If specified a
//      * debug adapter is expected to emit
//      * progress events for the long running request until the request has
//      been
//      * either completed or cancelled.
//      * If the request ID is omitted, the progress report is assumed to be
//      * related to some general activity of the debug adapter.
//      */
//     requestId?: number;
//
//     /**
//      * If true, the request that reports progress may be canceled with a
//      * 'cancel' request.
//      * So this property basically controls whether the client should use UX
//      that
//      * supports cancellation.
//      * Clients that don't support cancellation are allowed to ignore the
//      * setting.
//      */
//     cancellable?: boolean;
//
//     /**
//      * Optional, more detailed progress message.
//      */
//     message?: string;
//
//     /**
//      * Optional progress percentage to display (value range: 0 to 100). If
//      * omitted no percentage will be shown.
//      */
//     percentage?: number;
//   };
// }
//
// interface ProgressUpdateEvent extends Event {
//   event: 'progressUpdate';
//
//   body: {
//     /**
//      * The ID that was introduced in the initial 'progressStart' event.
//      */
//     progressId: string;
//
//     /**
//      * Optional, more detailed progress message. If omitted, the previous
//      * message (if any) is used.
//      */
//     message?: string;
//
//     /**
//      * Optional progress percentage to display (value range: 0 to 100). If
//      * omitted no percentage will be shown.
//      */
//     percentage?: number;
//   };
// }
//
// interface ProgressEndEvent extends Event {
//   event: 'progressEnd';
//
//   body: {
//     /**
//      * The ID that was introduced in the initial 'ProgressStartEvent'.
//      */
//     progressId: string;
//
//     /**
//      * Optional, more detailed progress message. If omitted, the previous
//      * message (if any) is used.
//      */
//     message?: string;
//   };
// }

void DAP::SendProgressEvent(uint64_t progress_id, const char *message,
                            uint64_t completed, uint64_t total) {
  progress_event_reporter.Push(progress_id, message, completed, total);
}

void __attribute__((format(printf, 3, 4)))
DAP::SendFormattedOutput(OutputType o, const char *format, ...) {
  char buffer[1024];
  va_list args;
  va_start(args, format);
  int actual_length = vsnprintf(buffer, sizeof(buffer), format, args);
  va_end(args);
  SendOutput(
      o, llvm::StringRef(buffer, std::min<int>(actual_length, sizeof(buffer))));
}

ExceptionBreakpoint *DAP::GetExceptionBPFromStopReason(lldb::SBThread &thread) {
  const auto num = thread.GetStopReasonDataCount();
  // Check to see if have hit an exception breakpoint and change the
  // reason to "exception", but only do so if all breakpoints that were
  // hit are exception breakpoints.
  ExceptionBreakpoint *exc_bp = nullptr;
  for (size_t i = 0; i < num; i += 2) {
    // thread.GetStopReasonDataAtIndex(i) will return the bp ID and
    // thread.GetStopReasonDataAtIndex(i+1) will return the location
    // within that breakpoint. We only care about the bp ID so we can
    // see if this is an exception breakpoint that is getting hit.
    lldb::break_id_t bp_id = thread.GetStopReasonDataAtIndex(i);
    exc_bp = GetExceptionBreakpoint(bp_id);
    // If any breakpoint is not an exception breakpoint, then stop and
    // report this as a normal breakpoint
    if (exc_bp == nullptr)
      return nullptr;
  }
  return exc_bp;
}

lldb::SBThread DAP::GetLLDBThread(const llvm::json::Object &arguments) {
  auto tid = GetSigned(arguments, "threadId", LLDB_INVALID_THREAD_ID);
  return target.GetProcess().GetThreadByID(tid);
}

lldb::SBFrame DAP::GetLLDBFrame(const llvm::json::Object &arguments) {
  const uint64_t frame_id = GetUnsigned(arguments, "frameId", UINT64_MAX);
  lldb::SBProcess process = target.GetProcess();
  // Upper 32 bits is the thread index ID
  lldb::SBThread thread =
      process.GetThreadByIndexID(GetLLDBThreadIndexID(frame_id));
  // Lower 32 bits is the frame index
  return thread.GetFrameAtIndex(GetLLDBFrameID(frame_id));
}

llvm::json::Value DAP::CreateTopLevelScopes() {
  llvm::json::Array scopes;
  scopes.emplace_back(CreateScope("Locals", VARREF_LOCALS,
                                  g_dap.variables.locals.GetSize(), false));
  scopes.emplace_back(CreateScope("Globals", VARREF_GLOBALS,
                                  g_dap.variables.globals.GetSize(), false));
  scopes.emplace_back(CreateScope("Registers", VARREF_REGS,
                                  g_dap.variables.registers.GetSize(), false));
  return llvm::json::Value(std::move(scopes));
}

ExpressionContext DAP::DetectExpressionContext(lldb::SBFrame &frame,
                                               std::string &text) {
  // Include the escape hatch prefix.
  if (!text.empty() &&
      llvm::StringRef(text).starts_with(g_dap.command_escape_prefix)) {
    text = text.substr(g_dap.command_escape_prefix.size());
    return ExpressionContext::Command;
  }

  switch (repl_mode) {
  case ReplMode::Variable:
    return ExpressionContext::Variable;
  case ReplMode::Command:
    return ExpressionContext::Command;
  case ReplMode::Auto:
    // If the frame is invalid then there is no variables to complete, assume
    // this is an lldb command instead.
    if (!frame.IsValid()) {
      return ExpressionContext::Command;
    }

    lldb::SBCommandReturnObject result;
    debugger.GetCommandInterpreter().ResolveCommand(text.data(), result);

    // If this command is a simple expression like `var + 1` check if there is
    // a local variable name that is in the current expression. If so, ensure
    // the expression runs in the variable context.
    lldb::SBValueList variables = frame.GetVariables(true, true, true, true);
    llvm::StringRef input = text;
    for (uint32_t i = 0; i < variables.GetSize(); i++) {
      llvm::StringRef name = variables.GetValueAtIndex(i).GetName();
      // Check both directions in case the input is a partial of a variable
      // (e.g. input = `va` and local variable = `var1`).
      if (input.contains(name) || name.contains(input)) {
        if (!auto_repl_mode_collision_warning) {
          llvm::errs() << "Variable expression '" << text
                       << "' is hiding an lldb command, prefix an expression "
                          "with '"
                       << g_dap.command_escape_prefix
                       << "' to ensure it runs as a lldb command.\n";
          auto_repl_mode_collision_warning = true;
        }
        return ExpressionContext::Variable;
      }
    }

    if (result.Succeeded()) {
      return ExpressionContext::Command;
    }
  }

  return ExpressionContext::Variable;
}

void DAP::RunLLDBCommands(llvm::StringRef prefix,
                          const std::vector<std::string> &commands) {
  SendOutput(OutputType::Console,
             llvm::StringRef(::RunLLDBCommands(prefix, commands)));
}

void DAP::RunInitCommands() {
  RunLLDBCommands("Running initCommands:", init_commands);
}

void DAP::RunPreRunCommands() {
  RunLLDBCommands("Running preRunCommands:", pre_run_commands);
}

void DAP::RunStopCommands() {
  RunLLDBCommands("Running stopCommands:", stop_commands);
}

void DAP::RunExitCommands() {
  RunLLDBCommands("Running exitCommands:", exit_commands);
}

void DAP::RunTerminateCommands() {
  RunLLDBCommands("Running terminateCommands:", terminate_commands);
}

lldb::SBTarget
DAP::CreateTargetFromArguments(const llvm::json::Object &arguments,
                               lldb::SBError &error) {
  // Grab the name of the program we need to debug and create a target using
  // the given program as an argument. Executable file can be a source of target
  // architecture and platform, if they differ from the host. Setting exe path
  // in launch info is useless because Target.Launch() will not change
  // architecture and platform, therefore they should be known at the target
  // creation. We also use target triple and platform from the launch
  // configuration, if given, since in some cases ELF file doesn't contain
  // enough information to determine correct arch and platform (or ELF can be
  // omitted at all), so it is good to leave the user an apportunity to specify
  // those. Any of those three can be left empty.
  llvm::StringRef target_triple = GetString(arguments, "targetTriple");
  llvm::StringRef platform_name = GetString(arguments, "platformName");
  llvm::StringRef program = GetString(arguments, "program");
  auto target = this->debugger.CreateTarget(
      program.data(), target_triple.data(), platform_name.data(),
      true, // Add dependent modules.
      error);

  if (error.Fail()) {
    // Update message if there was an error.
    error.SetErrorStringWithFormat(
        "Could not create a target for a program '%s': %s.", program.data(),
        error.GetCString());
  }

  return target;
}

void DAP::SetTarget(const lldb::SBTarget target) {
  this->target = target;

  if (target.IsValid()) {
    // Configure breakpoint event listeners for the target.
    lldb::SBListener listener = this->debugger.GetListener();
    listener.StartListeningForEvents(
        this->target.GetBroadcaster(),
        lldb::SBTarget::eBroadcastBitBreakpointChanged);
    listener.StartListeningForEvents(this->broadcaster,
                                     eBroadcastBitStopEventThread);
  }
}

PacketStatus DAP::GetNextObject(llvm::json::Object &object) {
  std::string json = ReadJSON();
  if (json.empty())
    return PacketStatus::EndOfFile;

  llvm::StringRef json_sref(json);
  llvm::Expected<llvm::json::Value> json_value = llvm::json::parse(json_sref);
  if (!json_value) {
    auto error = json_value.takeError();
    if (log) {
      std::string error_str;
      llvm::raw_string_ostream strm(error_str);
      strm << error;
      strm.flush();
      *log << "error: failed to parse JSON: " << error_str << std::endl
           << json << std::endl;
    }
    return PacketStatus::JSONMalformed;
  }

  if (log) {
    *log << llvm::formatv("{0:2}", *json_value).str() << std::endl;
  }

  llvm::json::Object *object_ptr = json_value->getAsObject();
  if (!object_ptr) {
    if (log)
      *log << "error: json packet isn't a object" << std::endl;
    return PacketStatus::JSONNotObject;
  }
  object = *object_ptr;
  return PacketStatus::Success;
}

bool DAP::HandleObject(const llvm::json::Object &object) {
  const auto packet_type = GetString(object, "type");
  if (packet_type == "request") {
    const auto command = GetString(object, "command");
    auto handler_pos = request_handlers.find(std::string(command));
    if (handler_pos != request_handlers.end()) {
      handler_pos->second(object);
      return true; // Success
    } else {
      if (log)
        *log << "error: unhandled command \"" << command.data() << "\""
             << std::endl;
      return false; // Fail
    }
  }

  if (packet_type == "response") {
    auto id = GetSigned(object, "request_seq", 0);
    ResponseCallback response_handler = [](llvm::Expected<llvm::json::Value>) {
      llvm::errs() << "Unhandled response\n";
    };

    {
      std::lock_guard<std::mutex> locker(call_mutex);
      auto inflight = inflight_reverse_requests.find(id);
      if (inflight != inflight_reverse_requests.end()) {
        response_handler = std::move(inflight->second);
        inflight_reverse_requests.erase(inflight);
      }
    }

    // Result should be given, use null if not.
    if (GetBoolean(object, "success", false)) {
      llvm::json::Value Result = nullptr;
      if (auto *B = object.get("body")) {
        Result = std::move(*B);
      }
      response_handler(Result);
    } else {
      llvm::StringRef message = GetString(object, "message");
      if (message.empty()) {
        message = "Unknown error, response failed";
      }
      response_handler(llvm::createStringError(
          std::error_code(-1, std::generic_category()), message));
    }

    return true;
  }

  return false;
}

llvm::Error DAP::Loop() {
  while (!sent_terminated_event) {
    llvm::json::Object object;
    lldb_dap::PacketStatus status = GetNextObject(object);

    if (status == lldb_dap::PacketStatus::EndOfFile) {
      break;
    }

    if (status != lldb_dap::PacketStatus::Success) {
      return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                     "failed to send packet");
    }

    if (!HandleObject(object)) {
      return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                     "unhandled packet");
    }
  }

  return llvm::Error::success();
}

void DAP::SendReverseRequest(llvm::StringRef command,
                             llvm::json::Value arguments,
                             ResponseCallback callback) {
  int64_t id;
  {
    std::lock_guard<std::mutex> locker(call_mutex);
    id = ++reverse_request_seq;
    inflight_reverse_requests.emplace(id, std::move(callback));
  }

  SendJSON(llvm::json::Object{
      {"type", "request"},
      {"seq", id},
      {"command", command},
      {"arguments", std::move(arguments)},
  });
}

void DAP::RegisterRequestCallback(std::string request,
                                  RequestCallback callback) {
  request_handlers[request] = callback;
}

lldb::SBError DAP::WaitForProcessToStop(uint32_t seconds) {
  lldb::SBError error;
  lldb::SBProcess process = target.GetProcess();
  if (!process.IsValid()) {
    error.SetErrorString("invalid process");
    return error;
  }
  auto timeout_time =
      std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
  while (std::chrono::steady_clock::now() < timeout_time) {
    const auto state = process.GetState();
    switch (state) {
    case lldb::eStateAttaching:
    case lldb::eStateConnected:
    case lldb::eStateInvalid:
    case lldb::eStateLaunching:
    case lldb::eStateRunning:
    case lldb::eStateStepping:
    case lldb::eStateSuspended:
      break;
    case lldb::eStateDetached:
      error.SetErrorString("process detached during launch or attach");
      return error;
    case lldb::eStateExited:
      error.SetErrorString("process exited during launch or attach");
      return error;
    case lldb::eStateUnloaded:
      error.SetErrorString("process unloaded during launch or attach");
      return error;
    case lldb::eStateCrashed:
    case lldb::eStateStopped:
      return lldb::SBError(); // Success!
    }
    std::this_thread::sleep_for(std::chrono::microseconds(250));
  }
  error.SetErrorStringWithFormat("process failed to stop within %u seconds",
                                 seconds);
  return error;
}

void Variables::Clear() {
  locals.Clear();
  globals.Clear();
  registers.Clear();
  expandable_variables.clear();
}

int64_t Variables::GetNewVariableReference(bool is_permanent) {
  if (is_permanent)
    return next_permanent_var_ref++;
  return next_temporary_var_ref++;
}

bool Variables::IsPermanentVariableReference(int64_t var_ref) {
  return var_ref >= PermanentVariableStartIndex;
}

lldb::SBValue Variables::GetVariable(int64_t var_ref) const {
  if (IsPermanentVariableReference(var_ref)) {
    auto pos = expandable_permanent_variables.find(var_ref);
    if (pos != expandable_permanent_variables.end())
      return pos->second;
  } else {
    auto pos = expandable_variables.find(var_ref);
    if (pos != expandable_variables.end())
      return pos->second;
  }
  return lldb::SBValue();
}

int64_t Variables::InsertExpandableVariable(lldb::SBValue variable,
                                            bool is_permanent) {
  int64_t var_ref = GetNewVariableReference(is_permanent);
  if (is_permanent)
    expandable_permanent_variables.insert(std::make_pair(var_ref, variable));
  else
    expandable_variables.insert(std::make_pair(var_ref, variable));
  return var_ref;
}

bool StartDebuggingRequestHandler::DoExecute(
    lldb::SBDebugger debugger, char **command,
    lldb::SBCommandReturnObject &result) {
  // Command format like: `startDebugging <launch|attach> <configuration>`
  if (!command) {
    result.SetError("Invalid use of startDebugging");
    result.SetStatus(lldb::eReturnStatusFailed);
    return false;
  }

  if (!command[0] || llvm::StringRef(command[0]).empty()) {
    result.SetError("startDebugging request type missing.");
    result.SetStatus(lldb::eReturnStatusFailed);
    return false;
  }

  if (!command[1] || llvm::StringRef(command[1]).empty()) {
    result.SetError("configuration missing.");
    result.SetStatus(lldb::eReturnStatusFailed);
    return false;
  }

  llvm::StringRef request{command[0]};
  std::string raw_configuration{command[1]};

  int i = 2;
  while (command[i]) {
    raw_configuration.append(" ").append(command[i]);
  }

  llvm::Expected<llvm::json::Value> configuration =
      llvm::json::parse(raw_configuration);

  if (!configuration) {
    llvm::Error err = configuration.takeError();
    std::string msg =
        "Failed to parse json configuration: " + llvm::toString(std::move(err));
    result.SetError(msg.c_str());
    result.SetStatus(lldb::eReturnStatusFailed);
    return false;
  }

  g_dap.SendReverseRequest(
      "startDebugging",
      llvm::json::Object{{"request", request},
                         {"configuration", std::move(*configuration)}},
      [](llvm::Expected<llvm::json::Value> value) {
        if (!value) {
          llvm::Error err = value.takeError();
          llvm::errs() << "reverse start debugging request failed: "
                       << llvm::toString(std::move(err)) << "\n";
        }
      });

  result.SetStatus(lldb::eReturnStatusSuccessFinishNoResult);

  return true;
}

bool ReplModeRequestHandler::DoExecute(lldb::SBDebugger debugger,
                                       char **command,
                                       lldb::SBCommandReturnObject &result) {
  // Command format like: `repl-mode <variable|command|auto>?`
  // If a new mode is not specified report the current mode.
  if (!command || llvm::StringRef(command[0]).empty()) {
    std::string mode;
    switch (g_dap.repl_mode) {
    case ReplMode::Variable:
      mode = "variable";
      break;
    case ReplMode::Command:
      mode = "command";
      break;
    case ReplMode::Auto:
      mode = "auto";
      break;
    }

    result.Printf("lldb-dap repl-mode %s.\n", mode.c_str());
    result.SetStatus(lldb::eReturnStatusSuccessFinishResult);

    return true;
  }

  llvm::StringRef new_mode{command[0]};

  if (new_mode == "variable") {
    g_dap.repl_mode = ReplMode::Variable;
  } else if (new_mode == "command") {
    g_dap.repl_mode = ReplMode::Command;
  } else if (new_mode == "auto") {
    g_dap.repl_mode = ReplMode::Auto;
  } else {
    lldb::SBStream error_message;
    error_message.Printf("Invalid repl-mode '%s'. Expected one of 'variable', "
                         "'command' or 'auto'.\n",
                         new_mode.data());
    result.SetError(error_message.GetData());
    return false;
  }

  result.Printf("lldb-dap repl-mode %s set.\n", new_mode.data());
  result.SetStatus(lldb::eReturnStatusSuccessFinishNoResult);
  return true;
}

void DAP::SetFrameFormat(llvm::StringRef format) {
  if (format.empty())
    return;
  lldb::SBError error;
  g_dap.frame_format = lldb::SBFormat(format.data(), error);
  if (error.Fail()) {
    g_dap.SendOutput(
        OutputType::Console,
        llvm::formatv(
            "The provided frame format '{0}' couldn't be parsed: {1}\n", format,
            error.GetCString())
            .str());
  }
}

} // namespace lldb_dap
