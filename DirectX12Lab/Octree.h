#pragma once
// NOMINMAX обязателен: windows.h определяет min/max как макросы,
// которые ломают std::max и инициализаторные списки
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <cmath>        // sqrtf, fabsf
#include <algorithm>    // std::min, std::max, std::remove_if
#include <cfloat>       // FLT_MAX
#include <vector>
#include <memory>
#include <array>
#include <DirectXMath.h>

struct FrustumPlanes
{
    // 6 плоскостей: (nx, ny, nz, d), условие видимости: dot(n,P)+d >= 0
    DirectX::XMFLOAT4 planes[6];
};

inline FrustumPlanes ExtractFrustumPlanes(const DirectX::XMFLOAT4X4& m)
{
    // Метод Gribb–Hartmann (row-vector convention, depth [0..1])
    FrustumPlanes fp;

    auto mkPlane = [](float nx, float ny, float nz, float d) -> DirectX::XMFLOAT4 {
        float len = sqrtf(nx * nx + ny * ny + nz * nz);
        if (len > 1e-6f) { nx /= len; ny /= len; nz /= len; d /= len; }
        return { nx, ny, nz, d };
        };

    fp.planes[0] = mkPlane(m._14 + m._11, m._24 + m._21, m._34 + m._31, m._44 + m._41); // Left
    fp.planes[1] = mkPlane(m._14 - m._11, m._24 - m._21, m._34 - m._31, m._44 - m._41); // Right
    fp.planes[2] = mkPlane(m._14 + m._12, m._24 + m._22, m._34 + m._32, m._44 + m._42); // Bottom
    fp.planes[3] = mkPlane(m._14 - m._12, m._24 - m._22, m._34 - m._32, m._44 - m._42); // Top
    fp.planes[4] = mkPlane(m._13, m._23, m._33, m._43);       // Near
    fp.planes[5] = mkPlane(m._14 - m._13, m._24 - m._23, m._34 - m._33, m._44 - m._43); // Far

    return fp;
}

inline bool IsSphereInFrustum(const FrustumPlanes& fp,
    const DirectX::XMFLOAT3& center, float radius)
{
    for (int i = 0; i < 6; ++i)
    {
        const auto& p = fp.planes[i];
        float dist = p.x * center.x + p.y * center.y + p.z * center.z + p.w;
        if (dist < -radius) return false;
    }
    return true;
}

inline bool IsAABBInFrustum(const FrustumPlanes& fp,
    const DirectX::XMFLOAT3& center, float halfSize)
{
    for (int i = 0; i < 6; ++i)
    {
        const auto& p = fp.planes[i];
        float r = halfSize * (fabsf(p.x) + fabsf(p.y) + fabsf(p.z));
        float dist = p.x * center.x + p.y * center.y + p.z * center.z + p.w;
        if (dist < -r) return false;
    }
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Entry — полная запись (idx + center + radius)
struct OctreeEntry
{
    int idx;
    DirectX::XMFLOAT3 center;
    float radius;
};

// ─────────────────────────────────────────────────────────────────────────────
struct OctreeNode
{
    static constexpr int kMaxPerNode = 8;
    static constexpr int kMaxDepth = 6;

    DirectX::XMFLOAT3 center;
    float             halfSize = 0.f;

    // Храним полные записи, чтобы можно было правильно перераспределять при subdivide
    std::vector<OctreeEntry> entries;
    std::array<std::unique_ptr<OctreeNode>, 8> children{};

    bool IsLeaf() const { return children[0] == nullptr; }

    void Insert(const OctreeEntry& e, int depth = 0)
    {
        if (IsLeaf())
        {
            entries.push_back(e);
            if ((int)entries.size() > kMaxPerNode && depth < kMaxDepth)
                Subdivide(depth);
        }
        else
        {
            PushDown(e, depth);
        }
    }

    void Query(const FrustumPlanes& fp, std::vector<int>& out) const
    {
        if (!IsAABBInFrustum(fp, center, halfSize)) return;

        if (IsLeaf())
        {
            for (const auto& e : entries) out.push_back(e.idx);
        }
        else
        {
            for (const auto& ch : children)
                if (ch) ch->Query(fp, out);
        }
    }

private:
    void Subdivide(int depth)
    {
        float hs = halfSize * 0.5f;
        const float s[2] = { -hs, hs };
        for (int i = 0; i < 8; ++i)
        {
            children[i] = std::make_unique<OctreeNode>();
            children[i]->center = { center.x + s[(i >> 0) & 1],
                                       center.y + s[(i >> 1) & 1],
                                       center.z + s[(i >> 2) & 1] };
            children[i]->halfSize = hs;
        }

        // Перераспределяем существующие записи по потомкам
        for (const auto& e : entries)
        {
            int oct = 0;
            if (e.center.x >= center.x) oct |= 1;
            if (e.center.y >= center.y) oct |= 2;
            if (e.center.z >= center.z) oct |= 4;
            children[oct]->Insert(e, depth + 1);
        }
        // Очищаем родительский список — записи теперь в листьях
        entries.clear();
    }

    void PushDown(const OctreeEntry& e, int depth)
    {
        int oct = 0;
        if (e.center.x >= center.x) oct |= 1;
        if (e.center.y >= center.y) oct |= 2;
        if (e.center.z >= center.z) oct |= 4;
        children[oct]->Insert(e, depth + 1);
    }
};

// ─────────────────────────────────────────────────────────────────────────────
class Octree
{
public:
    // Сохраняем совместимый тип Entry
    using Entry = OctreeEntry;

    void Build(const std::vector<Entry>& entries)
    {
        m_root.reset();
        if (entries.empty()) return;

        // AABB всех центров (учитываем радиусы)
        float mnx = FLT_MAX, mny = FLT_MAX, mnz = FLT_MAX;
        float mxx = -FLT_MAX, mxy = -FLT_MAX, mxz = -FLT_MAX;
        for (const auto& e : entries)
        {
            mnx = (std::min)(mnx, e.center.x - e.radius);
            mny = (std::min)(mny, e.center.y - e.radius);
            mnz = (std::min)(mnz, e.center.z - e.radius);
            mxx = (std::max)(mxx, e.center.x + e.radius);
            mxy = (std::max)(mxy, e.center.y + e.radius);
            mxz = (std::max)(mxz, e.center.z + e.radius);
        }

        // std::max вызывается попарно — не как макрос
        float dx = mxx - mnx, dy = mxy - mny, dz = mxz - mnz;
        float biggest = (std::max)((std::max)(dx, dy), dz);
        float halfSize = biggest * 0.5f + 1.f;

        m_root = std::make_unique<OctreeNode>();
        m_root->center = { (mnx + mxx) * 0.5f, (mny + mxy) * 0.5f, (mnz + mxz) * 0.5f };
        m_root->halfSize = halfSize;

        for (const auto& e : entries)
            m_root->Insert(e);
    }

    void Query(const FrustumPlanes& fp, std::vector<int>& out) const
    {
        out.clear();
        if (m_root) m_root->Query(fp, out);
    }

    bool IsBuilt() const { return m_root != nullptr; }

private:
    std::unique_ptr<OctreeNode> m_root;
};