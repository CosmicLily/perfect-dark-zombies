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

    // The brushes under a leaf brush node (of a submodel's own clip info, whose
    // brushes are the world's or its own): a node with a count above zero is a
    // leaf and lists that many (as the zone loader reads it), any other has two
    // children at offsets from itself. Owner is by the world's brush index
    void MarkBrushes(const ClipInfo& world, const ClipInfo& info, const unsigned node, const int model, std::vector<int>& owner, std::vector<int>& seen, const int depth)
    {
        if (node >= info.leafbrushNodesCount || depth > 256 || (&info == &world && seen[node] == model))
            return;

        if (&info == &world)
            seen[node] = model;

        const cLeafBrushNode_s& n = info.leafbrushNodes[node];

        if (n.leafBrushCount > 0)
        {
            for (int k = 0; n.data.leaf.brushes && k < n.leafBrushCount; k++)
            {
                const unsigned b = n.data.leaf.brushes[k];

                if (b >= info.numBrushes)
                    continue;

                const ptrdiff_t at = &info.brushes[b] - world.brushes;

                if (at >= 0 && static_cast<size_t>(at) < owner.size() && owner[at] < 0)
                    owner[at] = model;
            }

            return;
        }

        for (int c = 0; c < 2; c++)
            if (n.data.children.childOffset[c])
                MarkBrushes(world, info, node + n.data.children.childOffset[c], model, owner, seen, depth + 1);
    }

    void MarkTrees(const clipMap_t* cm, const int tree, const int count, const int model, std::vector<int>& owner, const int depth)
    {
        for (int a = tree; a < tree + count && a >= 0 && a < cm->aabbTreeCount && depth < 64; a++)
        {
            if (owner[a] < 0)
                owner[a] = model;

            if (cm->aabbTrees[a].childCount != 0)
                MarkTrees(cm, cm->aabbTrees[a].u.firstChildIndex, cm->aabbTrees[a].childCount, model, owner, depth + 1);
        }
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

        // Which brushes and aabb trees are a submodel's (a door's, a vehicle's):
        // those are in the submodel's own space and its entity places them.
        // They are written as B <model> ... and T <model> ...; the world's as b and t
        // (-1: the world's. The world's leaves reach submodels' brushes too, so
        // only the submodels' are marked)
        std::vector<int> brushOwner(info.numBrushes, -1);
        std::vector<int> treeOwner(cm->aabbTreeCount > 0 ? cm->aabbTreeCount : 0, -1);
        std::vector<int> seen(info.leafbrushNodesCount, -2);
        int counts[3] = {0, 0, 0};
        int ownInfo = 0;

        for (unsigned k = 1; cm->cmodels && k < cm->numSubModels; k++)
        {
            const cLeaf_s& leaf = cm->cmodels[k].leaf;

            if (leaf.brushContents)
            {
                int marked = 0;

                MarkBrushes(info, cm->cmodels[k].info ? *cm->cmodels[k].info : info, static_cast<unsigned>(leaf.leafBrushNode), static_cast<int>(k),
                            brushOwner, seen, 0);

                if (cm->cmodels[k].info && cm->cmodels[k].info != &cm->info && cm->cmodels[k].info->brushes != info.brushes)
                    ownInfo++;

                for (const int o : brushOwner)
                    marked += o == static_cast<int>(k);

                // one that reached the world's own tree is not a door: undone
                if (marked > 1000)
                {
                    for (int& o : brushOwner)
                        if (o == static_cast<int>(k))
                            o = -1;

                    counts[2]++;
                }
            }

            MarkTrees(cm, leaf.firstCollAabbIndex, leaf.collAabbCount, static_cast<int>(k), treeOwner, 0);
        }

        for (const int o : brushOwner)
            counts[o > 0 ? 1 : 0]++;

        s << "# brushes: world " << counts[0] << ", submodels " << counts[1] << ", submodels undone " << counts[2] << "; leafs " << cm->numLeafs
          << ", submodels " << cm->numSubModels << ", with their own brushes " << ownInfo << "\n";

        // b <contents> <mins> <maxs> <numverts> <verts...> <numsides> <side planes nx ny nz d...>
        for (unsigned i = 0; i < info.numBrushes; i++)
        {
            const cbrush_t& b = info.brushes[i];

            if (brushOwner[i] > 0)
                s << "B " << brushOwner[i] << " ";
            else
                s << "b ";

            s << b.contents << " " << b.mins.x << " " << b.mins.y << " " << b.mins.z << " " << b.maxs.x << " " << b.maxs.y << " "
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

                if (treeOwner[a] > 0)
                    s << "T " << treeOwner[a] << " ";
                else
                    s << "t ";

                s << tree.materialIndex;

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
