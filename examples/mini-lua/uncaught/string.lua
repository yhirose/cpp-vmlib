-- A string error: lua reports the message with the error call's own
-- position in front of it, "string.lua:3: boom".
error("boom")
