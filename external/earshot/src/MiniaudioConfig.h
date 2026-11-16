#pragma once

// miniaudio's feature switches, shared by the one translation unit that
// compiles the implementation and the one that calls it, so both see the
// same declarations. A capture-only library needs none of the decoding,
// encoding, generation, resource management, node graph or engine; every
// megabyte dropped is dropped from every plugin bundle that vendors earshot.
#define MA_NO_DECODING
#define MA_NO_ENCODING
#define MA_NO_GENERATION
#define MA_NO_RESOURCE_MANAGER
#define MA_NO_NODE_GRAPH
#define MA_NO_ENGINE
