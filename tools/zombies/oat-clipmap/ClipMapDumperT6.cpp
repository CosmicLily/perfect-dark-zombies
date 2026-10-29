#include "ClipMapDumperT6.h"

#include <cmath>
#include <iomanip>
#include <vector>

using namespace T6;

namespace
{
    float Det3(const float m[3][3])
    {
        return m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0])
               + m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
    }

    // A model collision triangle is kept as its plane and two edge planes:
    // s and t of a point are dot(p, svec) - svec.w and dot(p, tvec) - tvec.w,
    // and its corners are where (s, t) is (0, 0), (1, 0) and (0, 1) on the plane
    bool CornerOf(const XModelCollTri_s& tri, const float s, const float t, float out[3])
    {
        const float m[3][3] = {
            {tri.plane.v[0], tri.plane.v[1], tri.plane.v[2]},
            {tri.svec.v[0],  tri.svec.v[1],  tri.svec.v[2] },
            {tri.tvec.v[0],  tri.tvec.v[1],  tri.tvec.v[2] },
        };
        const float r[3] = {tri.plane.v[3], tri.svec.v[3] + s, tri.tvec.v[3] + t};
        const float det = Det3(m);

        if (std::fabs(det) < 1e-12f)
            return false;

        // Cramer's rule
        for (int k = 0; k < 3; k++)
        {
            float mk[3][3];

            for (int row = 0; row < 3; row++)
                for (int col = 0; col < 3; col++)
                    mk[row][col] = col == k ? r[row] : m[row][col];

            out[k] = Det3(mk) / det;
        }

        return true;
    }

    bool Invert(const vec3_t in[3], float out[3][3])
    {
        const float m[3][3] = {
            {in[0].x, in[0].y, in[0].z},
            {in[1].x, in[1].y, in[1].z},
            {in[2].x, in[2].y, in[2].z},
        };
        const float det = Det3(m);

        if (std::fabs(det) < 1e-12f)
            return false;

        out[0][0] = (m[1][1] * m[2][2] - m[1][2] * m[2][1]) / det;
        out[0][1] = (m[0][2] * m[2][1] - m[0][1] * m[2][2]) / det;
        out[0][2] = (m[0][1] * m[1][2] - m[0][2] * m[1][1]) / det;
        out[1][0] = (m[1][2] * m[2][0] - m[1][0] * m[2][2]) / det;
        out[1][1] = (m[0][0] * m[2][2] - m[0][2] * m[2][0]) / det;
        out[1][2] = (m[0][2] * m[1][0] - m[0][0] * m[1][2]) / det;
        out[2][0] = (m[1][0] * m[2][1] - m[1][1] * m[2][0]) / det;
        out[2][1] = (m[0][1] * m[2][0] - m[0][0] * m[2][1]) / det;
        out[2][2] = (m[0][0] * m[1][1] - m[0][1] * m[1][0]) / det;

        return true;
    }
} // namespace

namespace clip_map
{
    void DumperT6::DumpAsset(AssetDumpingContext& context, const XAssetInfo<AssetClipMapPvs::Type>& asset)
    {
        const auto* cm = asset.Asset();
        const auto assetFile = context.OpenAssetFile(asset.m_name + ".clip.txt");

        if (!assetFile)
            return;

        auto& s = *assetFile;
        const ClipInfo& info = cm->info;

        s << std::fixed << std::setprecision(3);
        s << "clipmap 1 " << cm->name << "\n";

        // m <index> <name> <surfaceFlags> <contentFlags>
        for (unsigned i = 0; i < info.numMaterials; i++)
            s << "m " << i << " " << (info.materials[i].name ? info.materials[i].name : "-") << " " << info.materials[i].surfaceFlags << " "
              << info.materials[i].contentFlags << "\n";

        // b <contents> <mins> <maxs> <numverts> <verts...> <numsides> <side planes nx ny nz d...>
        for (unsigned i = 0; i < info.numBrushes; i++)
        {
            const cbrush_t& b = info.brushes[i];

            s << "b " << b.contents << " " << b.mins.x << " " << b.mins.y << " " << b.mins.z << " " << b.maxs.x << " " << b.maxs.y << " "
              << b.maxs.z << " " << (b.verts ? b.numverts : 0u);

            for (unsigned v = 0; b.verts && v < b.numverts; v++)
                s << " " << b.verts[v].x << " " << b.verts[v].y << " " << b.verts[v].z;

            s << " " << (b.sides ? b.numsides : 0u);

            for (unsigned k = 0; b.sides && k < b.numsides; k++)
            {
                const cplane_s* p = b.sides[k].plane;

                if (p)
                    s << " " << p->normal.x << " " << p->normal.y << " " << p->normal.z << " " << p->dist;
                else
                    s << " 0 0 0 0";
            }

            s << "\n";
        }

        // t <material> <three corners>: the collision triangles, each once, with
        // the material of the aabb tree leaf that holds its partition
        std::vector<bool> done(cm->triCount > 0 ? cm->triCount : 0, false);

        for (int a = 0; a < cm->aabbTreeCount; a++)
        {
            const CollisionAabbTree& tree = cm->aabbTrees[a];

            if (tree.childCount != 0 || tree.u.partitionIndex < 0 || tree.u.partitionIndex >= cm->partitionCount)
                continue;

            const CollisionPartition& part = cm->partitions[tree.u.partitionIndex];
            const int last = part.firstTri + static_cast<unsigned char>(part.triCount);

            for (int t = part.firstTri; t < last && t < cm->triCount; t++)
            {
                if (t < 0 || done[t])
                    continue;

                done[t] = true;
                s << "t " << tree.materialIndex;

                for (int k = 0; k < 3; k++)
                {
                    const vec3_t& v = cm->verts[cm->triIndices[t][k]];
                    s << " " << v.x << " " << v.y << " " << v.z;
                }

                s << "\n";
            }
        }

        // s <contents> <three corners>: each static model's collision in the world
        for (unsigned i = 0; i < cm->numStaticModels; i++)
        {
            const cStaticModel_s& sm = cm->staticModelList[i];
            float axis[3][3];

            if (!sm.xmodel || !sm.xmodel->collSurfs || !Invert(sm.invScaledAxis, axis))
                continue;

            for (int c = 0; c < sm.xmodel->numCollSurfs; c++)
            {
                const XModelCollSurf_s& surf = sm.xmodel->collSurfs[c];
                const float st[3][2] = {
                    {0, 0},
                    {1, 0},
                    {0, 1}
                };

                for (int t = 0; t < surf.numCollTris; t++)
                {
                    float corners[3][3];
                    bool ok = true;

                    for (int k = 0; k < 3 && ok; k++)
                        ok = CornerOf(surf.collTris[t], st[k][0], st[k][1], corners[k]);

                    if (!ok)
                        continue;

                    s << "s " << surf.contents;

                    // model space to the world: origin + the axes times the corner
                    for (int k = 0; k < 3; k++)
                        for (int w = 0; w < 3; w++)
                            s << " " << sm.origin.v[w] + axis[0][w] * corners[k][0] + axis[1][w] * corners[k][1] + axis[2][w] * corners[k][2];

                    s << "\n";
                }
            }
        }
    }
} // namespace clip_map
