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
  assert((%w[read write bash] - tool_names).empty?,
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
  when "http-error"
    validate_coding_tools!(payload)
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

def response_for(scenario, request_number)
  case scenario
  when "text" then [200, "OK", "text/event-stream", text_response]
  when "http-error"
    [429, "Too Many Requests", "application/json",
     JSON.generate(error: {message: "rate limited"})]
  when "tool-write"
    [200, "OK", "text/event-stream",
     request_number.zero? ? tool_call_response : tool_final_response]
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
                    elsif %w[tool-write compaction-resume].include?(scenario)
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
      send_response(socket, *response_for(scenario, request_number))
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
