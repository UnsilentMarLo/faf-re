// Hydraulic erosion, ground update: T' = T - ((1 - spread) * dT + spread * the 3 x 3 binomial average of dT), edges clamped. The sum of the change is kept (what is dissolved in a cell is taken
// from its neighbourhood), but a channel is no longer one cell wide: the cell scale combs of a grid solver (every cell digs for itself, 1 cell grooves along the axes) turn into grooves 2 - 3 cells
// wide, as the brush of the droplets does. A pinned cell keeps its ground.
#include "common.hlsli"
#include "erosion_hydro_params.hlsli"

CBUFFER(ErodeHydroParams, P, 0)
SRV_BUFFER(float, T, 0)
SRV_BUFFER(float, dT, 1)
UAV_BUFFER(float, Tn, 0)

NUMTHREADS(16, 16, 1)
void CSMain(KERNEL_IDS)
{
    if (dtid.x >= P.w || dtid.y >= P.h) return;
    int x = int(dtid.x);
    int z = int(dtid.y);
    int w = int(P.w);
    int h = int(P.h);
    uint i = dtid.y * P.w + dtid.x;
    bool pinned = (P.flags & 8u) != 0u && (x == 0 || z == 0 || x == w - 1 || z == h - 1);
    if (pinned)
    {
        Tn[i] = T[i];
        return;
    }
    float c = dT[i];
    float avg = c;
    if (P.spread > 0.0f)
    {
        float sum = 0.0f;
        for (int dz = -1; dz <= 1; ++dz)
        {
            for (int dx = -1; dx <= 1; ++dx)
            {
                int xx = ClampIndex(x + dx, w);
                int zz = ClampIndex(z + dz, h);
                float wt = (dx == 0 ? 0.5f : 0.25f) * (dz == 0 ? 0.5f : 0.25f);
                sum += wt * dT[uint(zz) * P.w + uint(xx)];
            }
        }
        avg = sum;
    }
    Tn[i] = T[i] - ((1.0f - P.spread) * c + P.spread * avg);
}
