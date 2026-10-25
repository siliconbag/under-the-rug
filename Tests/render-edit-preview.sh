#!/bin/zsh
set -e
cd "$(dirname "$0")/.."
mkdir -p build/tests
xcrun swiftc -O -swift-version 5 -framework AppKit -framework Metal -framework MetalKit \
  Sources/Cloth.swift Sources/HeightField.swift Sources/SpatialHash.swift Sources/WholeRugEditor.swift \
  Sources/RugEditOverlay.swift Sources/Renderer.swift Sources/Shaders.swift Tests/RenderEditPreview.swift \
  -o build/tests/RenderEditPreview
./build/tests/RenderEditPreview
