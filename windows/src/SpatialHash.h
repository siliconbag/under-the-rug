#pragma once
#include <cstdint>
#include <cmath>
#include <vector>
#include "vec.h"

// Spatial hash for cloth self-collisions (port of Hash from Ten Minute Physics #11/#15).
class SpatialHash {
public:
    SpatialHash(float spacing, int maxObjects) : spacing(spacing), tableSize(2 * maxObjects), maxObjects(maxObjects) {
        cellStart.resize(tableSize + 1);
        cellEntries.resize(maxObjects);
        queryIds.resize(maxObjects);
        firstAdj.assign(maxObjects + 1, 0);
        adj.reserve(16 * maxObjects);
    }

    void create(const float3* pos, int n) {
        std::fill(cellStart.begin(), cellStart.end(), 0);
        for (int i = 0; i < n; i++) cellStart[hashPos(pos[i])]++;
        int32_t start = 0;
        for (int i = 0; i < tableSize; i++) {
            start += cellStart[i];
            cellStart[i] = start;
        }
        cellStart[tableSize] = start;
        for (int i = 0; i < n; i++) {
            int h = hashPos(pos[i]);
            cellStart[h]--;
            cellEntries[cellStart[h]] = i;
        }
    }

    // For every particle, its neighbours closer than maxDist (only j < i, so each pair once).
    void queryAll(const float3* pos, int n, float maxDist) {
        adj.clear();
        float md2 = maxDist * maxDist;
        for (int i = 0; i < n; i++) {
            firstAdj[i] = (int32_t)adj.size();
            query(pos[i], maxDist);
            for (int q = 0; q < querySize; q++) {
                int j = queryIds[q];
                if (j >= i) continue;
                if (length2(pos[j] - pos[i]) > md2) continue;
                adj.push_back(j);
            }
        }
        firstAdj[n] = (int32_t)adj.size();
    }

    std::vector<int32_t> firstAdj;
    std::vector<int32_t> adj;

private:
    float spacing;
    int tableSize;
    int maxObjects;
    std::vector<int32_t> cellStart, cellEntries, queryIds;
    int querySize = 0;

    int32_t coord(float c) const { return (int32_t)std::floor(c / spacing); }

    int hashCoords(int32_t x, int32_t y, int32_t z) const {
        // wrapping multiply like Swift's &*, then |h| % tableSize
        int32_t h = (int32_t)(((uint32_t)x * 92837111u) ^ ((uint32_t)y * 689287499u) ^ ((uint32_t)z * 283923481u));
        uint32_t mag = h < 0 ? (uint32_t)(-(int64_t)h) : (uint32_t)h;
        return (int)(mag % (uint32_t)tableSize);
    }

    int hashPos(float3 p) const { return hashCoords(coord(p.x), coord(p.y), coord(p.z)); }

    void query(float3 p, float maxDist) {
        int32_t x0 = coord(p.x - maxDist), y0 = coord(p.y - maxDist), z0 = coord(p.z - maxDist);
        int32_t x1 = coord(p.x + maxDist), y1 = coord(p.y + maxDist), z1 = coord(p.z + maxDist);
        querySize = 0;
        for (int32_t xi = x0; xi <= x1; xi++) {
            for (int32_t yi = y0; yi <= y1; yi++) {
                for (int32_t zi = z0; zi <= z1; zi++) {
                    int h = hashCoords(xi, yi, zi);
                    int s = cellStart[h], e = cellStart[h + 1];
                    for (int i = s; i < e && querySize < maxObjects; i++) queryIds[querySize++] = cellEntries[i];
                }
            }
        }
    }
};
