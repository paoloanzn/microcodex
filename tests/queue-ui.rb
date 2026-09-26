#!/usr/bin/env ruby
# frozen_string_literal: true

# Drives the real termbox UI through a PTY. Output is intentionally discarded;
# the scenario-specific mock server validates the next API request in detail.

require "pty"
require "timeout"

abort "usage: queue-ui.rb APP PROMPT FIRST_REQUEST QUEUED_DONE [quit]" unless (4..5).include?(ARGV.length)

app, prompt, first_request_file, queued_done_file = ARGV
quit_after_queue = ARGV[4] == "quit"

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
  # The mock server holds the first response until both follow-ups are typed,
  # so both Enter presses below must land while the first turn is executing.
  Timeout.timeout(10) { sleep 0.02 until File.exist?(first_request_file) }
  writer.write("Queued one\r")
  writer.flush
  if quit_after_queue
    # Quit while the turn is still executing: the queued message must be
    # discarded, never sent. PTY input is a FIFO byte stream, so the Enter
    # above is processed (and queued) before the Ctrl+Q below.
    sleep 0.3
    writer.write("\x11")
    writer.flush
    _, status = Process.wait2(pid)
    drain.join
    exit(status.exitstatus || 1)
  end
  sleep 0.1
  writer.write("Queued two\r")
  writer.flush
  Timeout.timeout(15) { sleep 0.05 until File.exist?(queued_done_file) }

  sleep 0.3
  writer.write("\x11")
  writer.flush
  _, status = Process.wait2(pid)
  drain.join
  exit(status.exitstatus || 1)
end
