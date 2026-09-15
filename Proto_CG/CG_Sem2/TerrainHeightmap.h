#pragma once

#include <filesystem>
#include <string>
#include <vector>
#include <utility>
#include <algorithm>
#include "stb_image.h"

struct TerrainHeightmap
{
    int Width = 0;
    int Height = 0;
    int SourceTileCount = 0;
    std::vector<unsigned short> Heights;
};

// Generator exports are adjacent raster tiles (no duplicated border pixels).
// R1 is the top row and C1 the left column. All samples retain one global scale.
inline bool LoadTerrainHeightmap(const std::filesystem::path& directory,
    TerrainHeightmap& result, std::string& error)
{
    namespace fs = std::filesystem;
    constexpr int side = 4;
    bool compactNames = false;
    for (int row = 1; row <= side; ++row)
        for (int column = 1; column <= side; ++column)
            if (fs::exists(directory / ("R" + std::to_string(row) + "C" + std::to_string(column) + ".png")))
                compactNames = true;
    auto tilePath = [&](int row, int column)
    {
        if (compactNames)
            return directory / ("R" + std::to_string(row + 1) + "C" + std::to_string(column + 1) + ".png");
        return directory / ("Heightmap_R" + std::to_string(row + 1) +
            "_C" + std::to_string(column + 1) + ".png");
    };
    int found = 0;
    for (int row = 0; row < side; ++row)
        for (int column = 0; column < side; ++column)
            if (fs::exists(tilePath(row, column))) ++found;

    // A partial tiled export is an error, never a silent fallback to the old map.
    const int sourceSide = found > 0 ? side : 1;
    const fs::path combinedPath = fs::exists(directory / "MapNG_Batch_Heightmap_Grid.png")
        ? directory / "MapNG_Batch_Heightmap_Grid.png" : directory / "Heightmap.png";
    TerrainHeightmap loaded;
    int tileWidth = 0, tileHeight = 0;
    for (int row = 0; row < sourceSide; ++row)
        for (int column = 0; column < sourceSide; ++column)
        {
            const std::string path = (found > 0 ? tilePath(row, column) : combinedPath).string();
            int width = 0, height = 0, channels = 0;
            if (!stbi_info(path.c_str(), &width, &height, &channels) || width < 2 || height < 2)
            {
                error = "Missing or invalid terrain heightmap: " + path;
                return false;
            }
            if (row == 0 && column == 0)
            {
                tileWidth = width; tileHeight = height;
                if (width > 8192 / sourceSide || height > 8192 / sourceSide)
                {
                    error = "Combined terrain heightmap must not exceed 8192 x 8192.";
                    return false;
                }
                loaded.Width = width * sourceSide;
                loaded.Height = height * sourceSide;
                loaded.Heights.resize(size_t(loaded.Width) * loaded.Height);
            }
            if (width != tileWidth || height != tileHeight)
            {
                error = "Terrain tiles must have identical dimensions: " + path;
                return false;
            }
            stbi_us* pixels = stbi_load_16(path.c_str(), &width, &height, &channels, 1);
            if (!pixels)
            {
                error = "Cannot decode terrain heightmap: " + path;
                return false;
            }
            for (int y = 0; y < height; ++y)
                std::copy_n(pixels + size_t(y) * width, width,
                    loaded.Heights.begin() + size_t(row * height + y) * loaded.Width + column * width);
            stbi_image_free(pixels);
            ++loaded.SourceTileCount;
        }
    result = std::move(loaded);
    error.clear();
    return true;
}
