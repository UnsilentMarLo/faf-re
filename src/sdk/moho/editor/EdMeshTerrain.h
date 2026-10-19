#pragma once

#include "moho/editor/EdMeshData.h"

namespace moho
{
  class Shadow;
  class CWaterShaderProperties;
  struct GeomCamera3;

  /**
   * Editor addition: the mesh terrain renderer (docs/projects/editor/MESH_VIEW.md). A mesh terrain map (`TerrainSystem = 'mesh'`, manifest
   * `terrain/terrain.lua`, SCM v5 patches) is drawn in place of the height field's surface. Ported from automation/terrainmesh/meshterrain.cpp, which patches
   * the retail exe; here the draw sits in the HighFidelityTerrain passes themselves, so no camera or shadow state has to be read back from shader constants.
   *
   * Lua loads a manifest (`ed_mesh_load`), picks what the viewport shows (`ed_mesh_mode`: 0 height field, 1 mesh, 2 both) and asks for heights / rays against
   * the mesh (`ed_mesh_height`, `ed_mesh_ray`). Main thread only, like the rest of the editor's rendering.
   */
  namespace edmesh
  {
    enum class Mode
    {
      HeightField = 0,
      Mesh = 1,
      Both = 2,
    };

    /// The loaded mesh map (CPU side). Empty when no manifest is loaded.
    MeshMap& Map();
    /// Bumps whenever Map() was loaded or cleared: the draw code rebuilds its GPU copies.
    unsigned Generation();
    void MarkChanged();

    Mode GetMode();
    void SetMode(Mode mode);

    /// A mesh is loaded and the mode asks for it: HighFidelityTerrain draws it (and, per HeightFieldWanted, not its own surface).
    bool MeshWanted();
    bool HeightFieldWanted();

    /// Draws the mesh into the colour target of the terrain surface pass (called from HighFidelityTerrain::DrawNormals). False when nothing was drawn:
    /// the caller then draws the height field.
    bool DrawSurface(const GeomCamera3& camera, Shadow* shadow, const CWaterShaderProperties* water);

    /// Draws the mesh into the shadow map (called from HighFidelityTerrain::DrawTerrainDepth with the light camera). False when nothing was drawn.
    bool DrawDepth(const GeomCamera3& lightCamera);

    /// Remembers the world camera of this frame (HighFidelityTerrain::UpdateRenderContext, not for the minimap): picking rays are made from it.
    void NoteCamera(const GeomCamera3& camera);
    /// The ray under a pixel of the world view (`px`, `py` from the view's top left, `vw` x `vh` its size): origin and unit direction. False before the first frame.
    bool CameraRay(float px, float py, float vw, float vh, float origin[3], float dir[3]);

    /// Frees the GPU copies (map unload, device reset).
    void ReleaseGpu();

    /// Milliseconds of the last surface draw (CPU side), for ed_mesh_stats.
    double LastDrawMs();
  } // namespace edmesh
} // namespace moho
