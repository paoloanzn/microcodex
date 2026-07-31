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
  length = Integer(header_map.fetch("content-length"), 10)
  raise "request body is too large" if length > MAX_REQUEST_BYTES

  body = socket.read(length)
  raise "request body ended early" unless body&.bytesize == length

  [request_line, header_map, body, JSON.parse(body)]
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
    assert(payload["model"] == "test-model", "CLI model option was not sent")
    assert(input_text(payload) == "Say hello from two arguments",
           "CLI prompt arguments were not joined and sent")
  when "http-error"
    nil
  when "tool-write"
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
  else
    raise "unknown mock scenario #{scenario.inspect}"
  end
end

def sse(*events)
  events.map { |event| "data: #{JSON.generate(event)}\n\n" }.join
end

def completed
  {type: "response.completed", response: {}}
end

def text_response
  sse(
    {type: "response.output_text.delta", delta: "Hello"},
    {type: "response.output_text.delta", delta: ", world!"},
    {type: "response.output_item.done",
     item: {type: "message", role: "assistant",
            content: [{type: "output_text", text: "Hello, world!"}]}},
    completed
  )
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
  end
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
expected_requests = scenario == "tool-write" ? 2 : 1
server = TCPServer.new("127.0.0.1", 0)
File.write(port_file, "#{server.addr[1]}\n")

expected_requests.times do |request_number|
  socket = nil
  begin
    Timeout.timeout(REQUEST_TIMEOUT) do
      socket = server.accept
      request_line, headers, body, payload = read_request(socket)
      FileUtils.mkdir_p(request_directory)
      File.binwrite(File.join(request_directory, "request-#{request_number + 1}.txt"),
                    "#{request_line}\r\n#{headers.inspect}\r\n\r\n#{body}")
      validate_common!(request_line, headers, payload)
      validate_scenario!(scenario, request_number, payload)
      send_response(socket, *response_for(scenario, request_number))
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
