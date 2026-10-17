# Raised inside a method: ruby reports "disk full (RuntimeError)", at the
# raise's line.
def save
  raise "disk full"
end

save
