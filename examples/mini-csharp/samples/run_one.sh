#!/usr/bin/env bash
# Run one sample under `dotnet`, in a scratch project of its own -- the
# SDK compiles every .cs in a directory, so two samples in one would both
# declare Main.
set -euo pipefail
src=$1
dir=$(mktemp -d)
trap 'rm -rf "$dir"' EXIT
cp "$src" "$dir/Program.cs"
cat > "$dir/s.csproj" <<'EOP'
<Project Sdk="Microsoft.NET.Sdk">
  <PropertyGroup>
    <OutputType>Exe</OutputType>
    <TargetFramework>net8.0</TargetFramework>
    <Nullable>disable</Nullable>
    <AssemblyName>s</AssemblyName>
    <InvariantGlobalization>true</InvariantGlobalization>
  </PropertyGroup>
</Project>
EOP
dotnet run --project "$dir/s.csproj" 2>/dev/null
