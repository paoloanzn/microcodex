#!/usr/bin/env ruby
# frozen_string_literal: true

# Drives a bracketed multiline paste through the real terminal UI. The mock
# server separately verifies that submission receives the full original text.

require "io/console"
require "pty"
require "timeout"

abort "usage: paste-ui.rb APP" unless ARGV.length == 1

ENV["TERM"] = "xterm-256color" if ENV["TERM"].to_s.empty?
paste = "before\n#{"x" * 1001}\nafter"
placeholder = "[Pasted Content #{paste.length} chars]"
output = +""
mutex = Mutex.new

PTY.spawn(ARGV.fetch(0)) do |reader, writer, pid|
  writer.winsize = [24, 80]
  drain = Thread.new do
    loop do
      chunk = reader.readpartial(4096)
      mutex.synchronize { output << chunk }
    end
  rescue EOFError, Errno::EIO
    nil
  end

  Timeout.timeout(5) do
    sleep 0.02 until mutex.synchronize { output.include?("\x1b[?2004h") }
  end
  writer.write("\x1b[200~#{paste}\x1b[201~")
  writer.flush
  Timeout.timeout(5) do
    sleep 0.02 until mutex.synchronize { output.include?(placeholder) }
  end

  writer.write("\r")
  writer.flush
  sleep 0.5
  writer.write("\x11")
  writer.flush
  _, status = Process.wait2(pid)
  drain.join
  exit(status.exitstatus || 1)
end
