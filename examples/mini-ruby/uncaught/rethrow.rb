# A trap rescued and raised again is reported where it first failed (line
# 4), not at the `raise e`.
begin
  1 + "a"
rescue => e
  raise e
end
