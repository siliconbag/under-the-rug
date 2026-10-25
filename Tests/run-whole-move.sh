#!/bin/zsh
set -e
cd "$(dirname "$0")/.."
mkdir -p build/tests
xcrun swiftc -O Sources/Cloth.swift Sources/HeightField.swift Sources/SpatialHash.swift Sources/WholeRugEditor.swift \
  Tests/WholeMoveTests.swift -o build/tests/WholeMoveTests
./build/tests/WholeMoveTests
