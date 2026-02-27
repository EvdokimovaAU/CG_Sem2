#pragma once
#include <cstdint>
#include "Vertex.h"

struct CubeGeometry
{
    static const Vertex* Vertices();
    static uint32_t        VertexCount();

    static const uint16_t* Indices();
    static uint32_t        IndexCount();
};
