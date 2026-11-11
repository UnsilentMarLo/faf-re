# The map editor's engine-independent logic as a static library, `faf_editor_logic` (docs/projects/editor/EDITOR_PORT_ANALYSIS.md, work package WP1).
#
# These are the translation units of src/editor/{doc,tools,graph,undo} that include nothing from the engine (moho/, gpg/, lua/, legacy/), Dear ImGui or
# EdDocument: they take strings and vectors and return strings and vectors. Upstream's own tests compile them one by one (tests/CMakeLists.txt); the FAF Studio
# editor (-DFAF_EDITOR=ON, src/moho/editor) links this library and binds it to Lua (the EdUp*.cpp units); the upstream editor (faf_editor, src/editor/sources.cmake)
# keeps compiling its own copy of the same files and does not link it.
#
# Not in the library, because they need the engine or the document: doc/EdDocument, doc/EdMapFile, doc/EdRoundTrip, doc/EdUnitWrecks, tools/EdDecals,
# tools/EdTextureLibrary, graph/EdTextureBake; and doc/EdWaterBodies, which includes moho/sim/WaterBodies.h (the engine's own object, so a second copy would clash
# in the Studio executable): the package that binds it (WP6) adds it. Keep these files unmodified: upstream merges stay cheap (patch through wrappers or
# mark an edit `// STUDIO:`).
#
# Include this file once, from src/CMakeLists.txt (before FAF_EDITOR links the library into faf_engine and before tests/). The target is EXCLUDE_FROM_ALL: it is
# built only when something links it.

set(FAF_EDITOR_LOGIC_SOURCES
  doc/EdAdaptiveModel.cpp
  doc/EdAdaptiveTables.cpp
  doc/EdEmitter.cpp
  doc/EdFafApi.cpp
  doc/EdGroupOptions.cpp
  doc/EdHttp.cpp
  doc/EdImageChunks.cpp
  doc/EdJson.cpp
  doc/EdLayerPreset.cpp
  doc/EdLighting.cpp
  doc/EdLuaSyntax.cpp
  doc/EdLuaText.cpp
  doc/EdMarkerPreset.cpp
  doc/EdNewMap.cpp
  doc/EdPackage.cpp
  doc/EdPropPaint.cpp
  doc/EdRecentMaps.cpp
  doc/EdSaveFile.cpp
  doc/EdSkybox.cpp
  doc/EdUnitFate.cpp
  doc/EdUnitPreset.cpp
  doc/EdUnits.cpp
  doc/EdVault.cpp
  doc/EdViewState.cpp
  doc/EdWaterPreset.cpp
  tools/EdArmySlots.cpp
  tools/EdChecks.cpp
  tools/EdDecalShape.cpp
  tools/EdHeightBrush.cpp
  tools/EdHeightFile.cpp
  tools/EdMarkers.cpp
  tools/EdMaskBrush.cpp
  tools/EdPathing.cpp
  tools/EdShaderSlots.cpp
  tools/EdSymmetryCheck.cpp
  tools/EdTerrainTypes.cpp
  tools/EdViewAids.cpp
  tools/EdWaves.cpp
  graph/EdEffectGraph.cpp
  graph/EdNodeGraph.cpp
  graph/EdTextureGraph.cpp
  undo/EdHeightHistory.cpp
)
list(TRANSFORM FAF_EDITOR_LOGIC_SOURCES PREPEND "${CMAKE_CURRENT_LIST_DIR}/")

add_library(faf_editor_logic STATIC EXCLUDE_FROM_ALL ${FAF_EDITOR_LOGIC_SOURCES})
# The sources include "editor/..." relative to src/.
target_include_directories(faf_editor_logic PUBLIC "${CMAKE_CURRENT_LIST_DIR}/..")
# The zlib the engine links (1.2.3 on x64, wx's 1.1.4 on x86, with 1.2.3's headers: src/CMakeLists.txt); the Windows libraries WIC (height images),
# WinHTTP (vault), DPAPI / CryptoAPI (token file) and ShellExecute need.
if(TARGET zlib123)
  target_link_libraries(faf_editor_logic PUBLIC zlib123)
else()
  target_link_libraries(faf_editor_logic PUBLIC wx_zlib)
  target_include_directories(faf_editor_logic PUBLIC "${FAF_DEPENDENCIES_DIR}/zlib-1.2.3")
endif()
target_link_libraries(faf_editor_logic PUBLIC ole32 windowscodecs winhttp crypt32 shell32)
set_target_properties(faf_editor_logic PROPERTIES FOLDER "Editor")
