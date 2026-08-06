#!/usr/bin/env ruby
# frozen_string_literal: true

# SPDX-FileCopyrightText: 2026 Paolo Anzani
# SPDX-License-Identifier: Apache-2.0

require "fileutils"
require "json"
require "socket"
require "timeout"

MAX_REQUEST_BYTES = 1024 * 1024
REQUEST_TIMEOUT = 10
TOOL_ROUND_LIMIT = 128

def assert(condition, message)
  raise message unless condition
end

def read_request(socket)
  headers = socket.gets("\r\n\r\n", MAX_REQUEST_BYTES)
  raise "could not read HTTP headers" unless headers&.end_with?("\r\n\r\n")

  request_line, *header_lines = headers.split("\r\n")
  header_map = header_lines.filter_map do |line|
    name, value = line.split(":", 2)
    [name.downcase, value.strip] if value
  end.to_h
  length = Integer(header_map.fetch("content-length", "0"), 10)
  raise "request body is too large" if length > MAX_REQUEST_BYTES

  body = socket.read(length)
  raise "request body ended early" unless body&.bytesize == length

  payload = body.empty? ? nil : JSON.parse(body)
  [request_line, header_map, body, payload]
end

def validate_models_request!(request_line, headers)
  assert(request_line == "GET /models?client_version=0.146.0 HTTP/1.1",
         "request did not GET the models endpoint")
  assert(headers["authorization"] == "Bearer test-access-token",
         "models request did not send the isolated access token")
  assert(headers["chatgpt-account-id"] == "test-account",
         "models request did not send the account ID")
  assert(headers["accept"] == "application/json",
         "models request did not ask for JSON")
end

def validate_common!(request_line, headers, payload)
  assert(request_line == "POST /responses HTTP/1.1", "request did not POST /responses")
  assert(headers["authorization"] == "Bearer test-access-token",
         "request did not send the isolated access token")
  assert(headers["chatgpt-account-id"] == "test-account",
         "request did not send the account ID")
  assert(headers["accept"] == "text/event-stream",
         "request did not ask for an event stream")
  assert(payload["stream"] == true, "request did not enable streaming")

end

def validate_coding_tools!(payload)
  tool_names = payload.fetch("tools").map { |tool| tool["name"] }
  assert((%w[read write edit bash] - tool_names).empty?,
         "request did not advertise the coding tools")
end

def input_text(payload)
  payload.dig("input", 0, "content", 0, "text")
end

def validate_scenario!(scenario, request_number, payload)
  case scenario
  when "text"
    validate_coding_tools!(payload)
    assert(payload["model"] == "test-model", "CLI model option was not sent")
    assert(input_text(payload) == "Say hello from two arguments",
           "CLI prompt arguments were not joined and sent")
    instructions = payload.fetch("instructions")
    assert(instructions.include?("You are MicroCodex, an agent based on GPT-5"),
           "upgraded coding-agent prompt was not sent")
    assert(!instructions.include?("### Formatting rules"),
           "trimmed formatting-rules section is still present")
    assert(instructions.include?("- test-skill: Test shared Codex skill discovery."),
           "shared Codex skill metadata was not added to the prompt")
    assert(instructions.include?("/skills/test-skill/SKILL.md"),
           "shared Codex skill path was not added to the prompt")
  when "http-error"
    validate_coding_tools!(payload)
  when "paste"
    validate_coding_tools!(payload)
    expected = "before\n#{"x" * 1001}\nafter"
    assert(input_text(payload) == expected,
           "multiline paste was submitted early or changed before sending")
  when "keybindings"
    validate_coding_tools!(payload)
    assert(input_text(payload) == "alpha t1op\nmiddle2D\nbottom3",
           "composer keybindings did not preserve the edited multiline prompt")
  when "tool-write"
    validate_coding_tools!(payload)
    if request_number.zero?
      assert(input_text(payload) == "Create the requested file",
             "tool scenario did not receive the user prompt")
    else
      input = payload.fetch("input")
      assert(input.any? { |item| item["type"] == "function_call" &&
                                item["call_id"] == "call_write" && item["name"] == "write" },
             "second request did not replay the function call")
      assert(input.any? { |item| item == {"type" => "function_call_output",
                                         "call_id" => "call_write",
                                         "output" => "Created result.txt"} },
             "second request did not contain the real tool output")
    end
  when "tool-edit"
    validate_coding_tools!(payload)
    if request_number.zero?
      assert(input_text(payload) == "Edit the requested file",
             "edit scenario did not receive the user prompt")
    else
      input = payload.fetch("input")
      assert(input.any? { |item| item["type"] == "function_call" &&
                                item["call_id"] == "call_edit" && item["name"] == "edit" },
             "second request did not replay the edit call")
      assert(input.any? { |item| item == {"type" => "function_call_output",
                                         "call_id" => "call_edit",
                                         "output" => "Edited edit-target.txt"} },
             "second request did not contain the edit result")
    end
  when "tool-shell-env"
    validate_coding_tools!(payload)
    if request_number.zero?
      assert(input_text(payload) == "Read the shell environment",
             "shell environment scenario did not receive the prompt")
    else
      output = payload.fetch("input").find do |item|
        item["type"] == "function_call_output" && item["call_id"] == "call_shell_env"
      end
      assert(output, "shell environment request did not contain a tool output")
      parsed = JSON.parse(output.fetch("output"))
      assert(parsed == {"stdout" => "loaded-from-bashrc", "stderr" => "", "exit_code" => 0},
             "shell tool did not restore the user's bashrc environment")
    end
  when "tool-bash-denied"
    validate_coding_tools!(payload)
    if request_number.zero?
      assert(input_text(payload) == "Try a denied shell command",
             "denied shell scenario did not receive the prompt")
    else
      output = payload.fetch("input").find do |item|
        item["type"] == "function_call_output" && item["call_id"] == "call_bash_denied"
      end
      assert(output, "denied shell request did not contain a tool output")
      assert(output.fetch("output") ==
               "Error: command denied: forced file removal is blocked",
             "dangerous shell command was not rejected by the denylist")
    end
  when "incomplete-output"
    validate_coding_tools!(payload)
    if request_number.zero?
      assert(input_text(payload) == "Start incomplete response test",
             "incomplete response scenario did not receive its first prompt")
    else
      input = payload.fetch("input")
      assert(input.any? { |item| item["role"] == "assistant" &&
                                item.dig("content", 0, "text") == "Partial limited answer" },
             "continued request lost the limit-interrupted assistant output")
      assert(input.any? { |item| item["role"] == "user" &&
                                item.dig("content", 0, "text")&.include?("max_output_tokens") },
             "continued request did not preserve the response limit error")
      assert(input.last.dig("content", 0, "text") == "continue",
             "continued request did not append the follow-up prompt")
    end
  when "tool-round-limit"
    validate_coding_tools!(payload)
    input = payload.fetch("input")
    if request_number.zero?
      assert(input_text(payload) == "Start tool round limit test",
             "tool round limit scenario did not receive its first prompt")
    elsif request_number <= TOOL_ROUND_LIMIT
      previous_call_id = "call_round_#{request_number - 1}"
      assert(input.any? { |item| item["type"] == "function_call" &&
                                item["call_id"] == previous_call_id },
             "tool round request lost the previous function call")
      assert(input.any? { |item| item["type"] == "function_call_output" &&
                                item["call_id"] == previous_call_id },
             "tool round request lost the previous function output")
    else
      over_limit_call_id = "call_round_#{TOOL_ROUND_LIMIT}"
      assert(input.any? { |item| item["type"] == "function_call" &&
                                item["call_id"] == over_limit_call_id },
             "continued request lost the over-limit function call")
      skipped = input.find do |item|
        item["type"] == "function_call_output" && item["call_id"] == over_limit_call_id
      end
      assert(skipped && skipped.fetch("output").include?("was not executed"),
             "over-limit function call was not paired with a non-execution output")
      assert(input.any? { |item| item["role"] == "user" &&
                                item.dig("content", 0, "text")&.include?("maximum number of tool rounds") },
             "continued request did not explain the tool round limit")
      assert(input.last.dig("content", 0, "text") == "continue",
             "tool round continuation did not append the follow-up prompt")
    end
  when "interrupt-output"
    validate_coding_tools!(payload)
    if request_number.zero?
      assert(input_text(payload) == "Start interrupt test",
             "interruption scenario did not receive its first prompt")
    else
      input = payload.fetch("input")
      assert(input.any? { |item| item["role"] == "assistant" &&
                                item.dig("content", 0, "text") == "Partial answer" },
             "continued request lost partial assistant output")
      assert(input.any? { |item| item["role"] == "user" &&
                                item.dig("content", 0, "text")&.include?("<turn_aborted>") },
             "continued request did not contain the interrupted-turn marker")
      assert(input.last.dig("content", 0, "text") == "continue",
             "continued request did not append the follow-up prompt")
    end
  when "interrupt-tool"
    validate_coding_tools!(payload)
    if request_number.zero?
      assert(input_text(payload) == "Start interrupted tool test",
             "tool interruption scenario did not receive its first prompt")
    else
      input = payload.fetch("input")
      assert(input.any? { |item| item["type"] == "function_call" &&
                                item["call_id"] == "call_sleep" },
             "continued request lost the interrupted function call")
      output = input.find do |item|
        item["type"] == "function_call_output" && item["call_id"] == "call_sleep"
      end
      assert(output && output["output"].include?("interrupted"),
             "continued request did not pair the function call with an interruption output")
      assert(input.any? { |item| item["role"] == "user" &&
                                item.dig("content", 0, "text")&.include?("<turn_aborted>") },
             "tool interruption did not contain the interrupted-turn marker")
      assert(input.last.dig("content", 0, "text") == "continue",
             "tool interruption did not append the follow-up prompt")
    end
  when "conversation-first"
    validate_coding_tools!(payload)
    assert(input_text(payload) == "Remember alpha",
           "saved conversation did not receive its first prompt")
  when "conversation-resume"
    validate_coding_tools!(payload)
    assert(payload["model"] == "test-model", "resume did not restore the saved model")
    input = payload.fetch("input")
    assert(input.any? { |item| item.dig("content", 0, "text") == "Remember alpha" },
           "resumed request did not replay the saved user message")
    assert(input.any? { |item| item.dig("content", 0, "text") == "Alpha stored" },
           "resumed request did not replay the saved assistant message")
    assert(input.last.dig("content", 0, "text") == "Recall beta",
           "resumed request did not append the new user message")
  when "compaction-seed"
    validate_coding_tools!(payload)
    assert(input_text(payload) == "Seed compactable context",
           "compaction seed did not receive its prompt")
  when "context-error-seed"
    validate_coding_tools!(payload)
    assert(input_text(payload) == "Seed context error recovery",
           "context-error seed did not receive its prompt")
  when "compaction-resume"
    assert(payload["model"] == "test-model", "compaction resume did not restore the saved model")
    if request_number.zero?
      assert(payload.fetch("tools").empty?, "summary request unexpectedly advertised tools")
      assert(payload.fetch("instructions").include?("Summarize the conversation"),
             "summary request did not use compaction instructions")
      assert(payload.fetch("input").any? do |item|
               item.dig("content", 0, "text") == "Seed compactable context"
             end, "summary request did not contain the old conversation")
    else
      validate_coding_tools!(payload)
      input = payload.fetch("input")
      summary = input.first.dig("content", 0, "text")
      assert(summary.include?("<conversation_summary>"),
             "post-compaction request did not begin with a summary")
      assert(summary.include?("The seed established compactable state."),
             "post-compaction request did not contain the generated summary")
      assert(input.none? { |item| item.dig("content", 0, "text") == "Seed response" },
             "post-compaction request retained summarized assistant history")
      assert(input.last.dig("content", 0, "text") == "Continue after compaction",
             "post-compaction request did not retain the active user message")
    end
  when "context-error-retry"
    if request_number == 1
      assert(payload.fetch("tools").empty?, "context-error summary advertised tools")
      assert(payload.fetch("instructions").include?("Summarize the conversation"),
             "context-error retry did not request a summary")
    else
      validate_coding_tools!(payload)
      input = payload.fetch("input")
      assert(input.last.dig("content", 0, "text") == "Recover from context error",
             "context-error retry lost the active user message")
      if request_number == 2
        assert(input.first.dig("content", 0, "text").include?("<conversation_summary>"),
               "context-error retry did not install the summary")
      end
    end
  else
    raise "unknown mock scenario #{scenario.inspect}"
  end
end

def sse(*events)
  events.map { |event| "data: #{JSON.generate(event)}\n\n" }.join
end

def completed(input_tokens: 10)
  {type: "response.completed", response: {usage: {input_tokens: input_tokens}}}
end

def message_response(text, input_tokens: 10)
  sse(
    {type: "response.output_text.delta", delta: text},
    {type: "response.output_item.done",
     item: {type: "message", role: "assistant",
            content: [{type: "output_text", text: text}]}},
    completed(input_tokens: input_tokens)
  )
end

def incomplete_response(text)
  sse(
    {type: "response.output_text.delta", delta: text},
    {type: "response.incomplete",
     response: {incomplete_details: {reason: "max_output_tokens"}}}
  )
end

def text_response
  message_response("Hello, world!")
end

def tool_call_response
  arguments = JSON.generate(path: "result.txt", content: "made by tool\n")
  sse(
    {type: "response.output_item.done",
     item: {type: "function_call", call_id: "call_write", name: "write",
            arguments: arguments}},
    completed
  )
end

def tool_final_response
  sse(
    {type: "response.output_item.done",
     item: {type: "message", role: "assistant",
            content: [{type: "output_text", text: "Created result.txt"}]}},
    completed
  )
end

def edit_call_response
  arguments = JSON.generate(path: "edit-target.txt", old_content: "line two",
                            new_content: "line two changed")
  sse(
    {type: "response.output_item.done",
     item: {type: "function_call", call_id: "call_edit", name: "edit",
            arguments: arguments}},
    completed
  )
end

def shell_env_call_response
  sse(
    {type: "response.output_item.done",
     item: {type: "function_call", call_id: "call_shell_env", name: "bash",
            arguments: JSON.generate(command: "printf '%s' \"$MICROCODEX_RC_VALUE\"")}},
    completed
  )
end

def denied_bash_call_response
  sse(
    {type: "response.output_item.done",
     item: {type: "function_call", call_id: "call_bash_denied", name: "bash",
            arguments: JSON.generate(command: "rm -rf denylist-sentinel")}},
    completed
  )
end

def sleep_call_response
  sse(
    {type: "response.output_item.done",
     item: {type: "function_call", call_id: "call_sleep", name: "bash",
            arguments: JSON.generate(command: "sleep 30")}},
    completed
  )
end

def round_limit_call_response(request_number)
  sse(
    {type: "response.output_item.done",
     item: {type: "function_call", call_id: "call_round_#{request_number}",
            name: "missing_tool", arguments: "{}"}},
    completed
  )
end

def response_for(scenario, request_number)
  case scenario
  when "text" then [200, "OK", "text/event-stream", text_response]
  when "paste" then [200, "OK", "text/event-stream", message_response("Paste received")]
  when "keybindings" then [200, "OK", "text/event-stream", message_response("Keys received")]
  when "http-error"
    [429, "Too Many Requests", "application/json",
     JSON.generate(error: {message: "rate limited"})]
  when "tool-write"
    [200, "OK", "text/event-stream",
     request_number.zero? ? tool_call_response : tool_final_response]
  when "tool-edit"
    [200, "OK", "text/event-stream",
     request_number.zero? ? edit_call_response : message_response("Edited edit-target.txt")]
  when "tool-shell-env"
    [200, "OK", "text/event-stream",
     request_number.zero? ? shell_env_call_response : message_response("Shell environment loaded")]
  when "tool-bash-denied"
    [200, "OK", "text/event-stream",
     request_number.zero? ? denied_bash_call_response : message_response("Dangerous command blocked")]
  when "incomplete-output"
    body = request_number.zero? ? incomplete_response("Partial limited answer") :
                                  message_response("Continued limited answer")
    [200, "OK", "text/event-stream", body]
  when "tool-round-limit"
    body = request_number <= TOOL_ROUND_LIMIT ? round_limit_call_response(request_number) :
                                                message_response("Continued after tool round limit")
    [200, "OK", "text/event-stream", body]
  when "interrupt-output"
    [200, "OK", "text/event-stream", message_response("Continued partial answer")]
  when "interrupt-tool"
    [200, "OK", "text/event-stream",
     request_number.zero? ? sleep_call_response : message_response("Continued after tool interruption")]
  when "conversation-first"
    [200, "OK", "text/event-stream", message_response("Alpha stored")]
  when "conversation-resume"
    [200, "OK", "text/event-stream", message_response("Alpha and beta recalled")]
  when "compaction-seed"
    [200, "OK", "text/event-stream", message_response("Seed response")]
  when "context-error-seed"
    [200, "OK", "text/event-stream", message_response("Context error seed response")]
  when "compaction-resume"
    text = request_number.zero? ? "The seed established compactable state." :
                                  "Continued from compacted state"
    [200, "OK", "text/event-stream", message_response(text)]
  when "context-error-retry"
    case request_number
    when 0
      [400, "Bad Request", "application/json",
       JSON.generate(error: {message: "Your input exceeds the context window of this model"})]
    when 1
      [200, "OK", "text/event-stream", message_response("Recovered context summary")]
    else
      [200, "OK", "text/event-stream", message_response("Recovered after retry")]
    end
  end
end

def models_response(scenario)
  test_model_context_window = scenario == "compaction-resume" ? 2 : 272_000
  JSON.generate(models: [
    {
      slug: "gpt-5.6-sol",
      context_window: 272_000,
      max_context_window: 272_000,
      effective_context_window_percent: 95,
      auto_compact_token_limit: nil
    },
    {
      slug: "test-model",
      context_window: test_model_context_window,
      max_context_window: test_model_context_window,
      effective_context_window_percent: 95,
      auto_compact_token_limit: nil
    }
  ])
end

def send_response(socket, status, reason, content_type, body)
  socket.write(
    "HTTP/1.1 #{status} #{reason}\r\n" \
    "Content-Type: #{content_type}\r\n" \
    "Connection: close\r\n" \
    "Content-Length: #{body.bytesize}\r\n\r\n#{body}"
  )
end

abort "usage: mock-server.rb SCENARIO PORT_FILE REQUEST_DIR" unless ARGV.length == 3

scenario, port_file, request_directory = ARGV
expected_requests = if scenario == "context-error-retry"
                      3
                    elsif scenario == "tool-round-limit"
                      TOOL_ROUND_LIMIT + 2
                    elsif %w[tool-write tool-edit tool-shell-env tool-bash-denied compaction-resume incomplete-output interrupt-output interrupt-tool].include?(scenario)
                      2
                    else
                      1
                    end
server = TCPServer.new("127.0.0.1", 0)
File.write(port_file, "#{server.addr[1]}\n")

request_number = 0
models_requested = false
while request_number < expected_requests
  socket = nil
  begin
    Timeout.timeout(REQUEST_TIMEOUT) do
      socket = server.accept
      request_line, headers, body, payload = read_request(socket)
      FileUtils.mkdir_p(request_directory)
      if request_line.start_with?("GET ")
        assert(!models_requested, "models endpoint was queried more than once")
        validate_models_request!(request_line, headers)
        models_requested = true
        File.binwrite(File.join(request_directory, "models-request.txt"),
                      "#{request_line}\r\n#{headers.inspect}\r\n\r\n")
        send_response(socket, 200, "OK", "application/json", models_response(scenario))
        next
      end

      File.binwrite(File.join(request_directory, "request-#{request_number + 1}.txt"),
                    "#{request_line}\r\n#{headers.inspect}\r\n\r\n#{body}")
      validate_common!(request_line, headers, payload)
      validate_scenario!(scenario, request_number, payload)
      if scenario == "interrupt-output" && request_number.zero?
        socket.write(
          "HTTP/1.1 200 OK\r\n" \
          "Content-Type: text/event-stream\r\n" \
          "Connection: close\r\n\r\n" \
          "#{sse(type: "response.output_text.delta", delta: "Partial answer")}"
        )
        sleep 0.1
        File.write(File.join(request_directory, "interrupt-ready"), "ready\n")
        begin
          loop do
            sleep 0.05
            socket.write(": waiting\n\n")
          end
        rescue Errno::EPIPE, Errno::ECONNRESET
          File.write(File.join(request_directory, "interrupt-observed"), "observed\n")
        end
      else
        send_response(socket, *response_for(scenario, request_number))
        if scenario == "interrupt-tool" && request_number.zero?
          sleep 0.1
          File.write(File.join(request_directory, "interrupt-ready"), "ready\n")
        elsif scenario == "incomplete-output" && request_number.zero?
          File.write(File.join(request_directory, "incomplete-ready"), "ready\n")
        elsif scenario == "tool-round-limit" && request_number == TOOL_ROUND_LIMIT
          File.write(File.join(request_directory, "limit-ready"), "ready\n")
        elsif scenario == "tool-round-limit" && request_number == TOOL_ROUND_LIMIT + 1
          File.write(File.join(request_directory, "continued"), "continued\n")
        elsif %w[incomplete-output interrupt-output interrupt-tool].include?(scenario) && request_number == 1
          File.write(File.join(request_directory, "continued"), "continued\n")
        end
      end
      request_number += 1
    end
  rescue StandardError => error
    warn "mock server: #{error.message}"
    send_response(socket, 500, "Invalid Request", "application/json",
                  JSON.generate(error: {message: "mock request validation failed"})) if socket
    exit 1
  ensure
    socket&.close
  end
end
assert(models_requested, "models endpoint was not queried")
