#!/usr/bin/env ruby
# frozen_string_literal: true

# Drives a follow-up turn after the server terminates the first response. Output
# is discarded because the mock server validates the continued request itself.

require "pty"
require "timeout"

abort "usage: continue-ui.rb APP PROMPT READY CONTINUED" unless ARGV.length == 4

app, prompt, ready_file, continued_file = ARGV

# GitHub Actions does not set TERM for non-interactive steps. The application
# still runs inside a real PTY here, so provide a matching terminal type when
# the parent environment has none.
ENV["TERM"] = "xterm-256color" if ENV["TERM"].to_s.empty?

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

  # The response can finish just before the UI collects its worker future. Keep
  # Enter idempotently retrying while the single buffered prompt remains intact.
  sleep 0.2
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
