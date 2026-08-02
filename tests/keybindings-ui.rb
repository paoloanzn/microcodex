#!/usr/bin/env ruby
# frozen_string_literal: true

# Exercises both common Option+Arrow encodings through the real terminal UI.
# The mock server verifies the final prompt, which makes cursor movement
# observable without depending on rendered screen escape sequences.

require "pty"
require "timeout"

abort "usage: keybindings-ui.rb APP" unless ARGV.length == 1

ENV["TERM"] = "xterm-256color" if ENV["TERM"].to_s.empty?

PTY.spawn(ARGV.fetch(0)) do |reader, writer, pid|
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

  Timeout.timeout(5) do
    sleep 0.02 until mutex.synchronize { output.include?("\x1b[?2004h") }
  end

  writer.write("alpha gamma")
  writer.write("\x1bbX")       # Option+Left as Meta-b, then insert X.
  writer.write("\x1bf!")       # Option+Right as Meta-f, then insert !.
  writer.write("\x1b[1;3DY")   # Option+Left as a modified arrow, then insert Y.
  writer.write("\x05\x1b\x7f") # End, then Option+Backspace removes YXgamma!.

  writer.write("top\x0amiddle\x0abottom")
  writer.write("\x1b[A2")      # Up to the end of middle, then insert 2.
  writer.write("\x1b[A1")      # Up to the same column on the first line.
  writer.write("\x1b[BD")      # Down to the end of the shorter middle line.
  writer.write("\x1b[B3")      # Down to the end of the shorter bottom line.
  writer.write("\r")
  writer.flush

  sleep 0.5
  writer.write("\x11")
  writer.flush
  _, status = Process.wait2(pid)
  drain.join
  exit(status.exitstatus || 1)
end
