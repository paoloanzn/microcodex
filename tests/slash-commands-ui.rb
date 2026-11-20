#!/usr/bin/env ruby
# frozen_string_literal: true

# Drives the slash commands through the real terminal UI. The mock server
# checks each request's model, reasoning effort and input, which makes the
# effect of every command observable without parsing rendered screen output.

require "pty"
require "timeout"

abort "usage: slash-commands-ui.rb APP MOCK_DIR" unless ARGV.length == 2

app, mock_dir = ARGV
ENV["TERM"] = "xterm-256color" if ENV["TERM"].to_s.empty?

def request_file(mock_dir, number)
  File.join(mock_dir, "request-#{number}.txt")
end

PTY.spawn(app) do |reader, writer, pid|
  output = +""
  mutex = Mutex.new
  drain = Thread.new do
    loop do
      chunk = reader.readpartial(4096)
      mutex.synchronize { output << chunk }
    end
  rescue EOFError, Errno::EIO
    nil
  end

  type = lambda do |text|
    writer.write(text)
    writer.flush
    sleep 0.1
  end

  Timeout.timeout(5) do
    sleep 0.02 until mutex.synchronize { output.include?("\x1b[?2004h") }
  end

  type.call("/st\r")          # Enter in the menu runs the selected /status.
  type.call("/eff\r")         # Opens the effort picker on the current "medium".
  type.call("\x1b[B\r")       # Down to "high", then choose it.
  type.call("/new extra\r")   # Rejected: /new takes no arguments.
  type.call("/etc/hosts is fine\r") # Not a command, so it is sent.

  # The mock holds the first reply, so the turn is still running here.
  Timeout.timeout(10) { sleep 0.02 until File.exist?(request_file(mock_dir, 1)) }
  type.call("/new\r")         # Rejected while a turn is running.

  # Enter is refused until the first turn has been collected, so retry it.
  type.call("second")
  Timeout.timeout(10) do
    until File.exist?(request_file(mock_dir, 2))
      type.call("\r")
      sleep 0.1
    end
  end

  sleep 0.5
  type.call("/effort low\r")
  type.call("/model nope\r")  # Rejected: not in the models catalog.
  type.call("/mod\r")         # Opens the model picker on the current "gpt-6.2-sol".
  type.call("\x1b[B\r")       # Down to "gpt-6.2-astra", then choose it.
  type.call("/new\r")
  type.call("third\r")
  Timeout.timeout(10) { sleep 0.02 until File.exist?(request_file(mock_dir, 3)) }

  sleep 0.5
  type.call("/quit\r")
  _, status = Timeout.timeout(5) { Process.wait2(pid) }
  drain.join
  exit(status.exitstatus || 1)
end
