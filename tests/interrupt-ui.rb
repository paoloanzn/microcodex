#!/usr/bin/env ruby
# frozen_string_literal: true

# Drives the real termbox UI through a PTY. Output is intentionally discarded;
# the scenario-specific mock server validates the next API request in detail.

require "pty"
require "timeout"

abort "usage: interrupt-ui.rb APP PROMPT READY CONTINUED" unless ARGV.length == 4

app, prompt, ready_file, continued_file = ARGV

PTY.spawn(app) do |reader, writer, pid|
  drain = Thread.new do
    loop do
      reader.readpartial(4096)
    end
  rescue EOFError, Errno::EIO
    nil
  end

  sleep 0.2
  writer.write("#{prompt}\r")
  writer.flush
  Timeout.timeout(10) { sleep 0.02 until File.exist?(ready_file) }
  writer.write("\x03")
  writer.flush

  # Keep Enter idempotently retrying while the interrupted worker winds down.
  # The input itself is typed once and remains buffered if the first Enter lands
  # before the future has been collected.
  sleep 0.3
  writer.write("continue")
  writer.flush
  Timeout.timeout(10) do
    until File.exist?(continued_file)
      writer.write("\r")
      writer.flush
      sleep 0.2
    end
  end

  sleep 0.2
  writer.write("\x11")
  writer.flush
  _, status = Process.wait2(pid)
  drain.join
  exit(status.exitstatus || 1)
end
