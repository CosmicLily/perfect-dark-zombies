#pragma once

#include "Dumping/AbstractAssetDumper.h"
#include "Game/T6/T6.h"

namespace clip_map
{
    // A map's collision as text (<map>.clip.txt): its brushes, its collision
    // triangles and its static models' collision, for porting the map's
    // collision to another engine exactly
    class DumperT6 final : public AbstractAssetDumper<T6::AssetClipMapPvs>
    {
    protected:
        void DumpAsset(AssetDumpingContext& context, const XAssetInfo<T6::AssetClipMapPvs::Type>& asset) override;
    };
} // namespace clip_map
