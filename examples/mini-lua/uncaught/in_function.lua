-- Raised inside a function: the position prepended is the error call's.
local function save()
  error("disk full")
end
save()
