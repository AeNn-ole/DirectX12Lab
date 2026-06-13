#pragma once
#include <string>
#include <vector>
#include <DirectXMath.h>

// ── Материал из .mtl-файла ────────────────────────────────────────────────
struct ObjMaterial
{
    std::string       name;
    DirectX::XMFLOAT3 Ka{ 0.1f, 0.1f, 0.1f };
    DirectX::XMFLOAT3 Kd{ 0.8f, 0.8f, 0.8f };
    DirectX::XMFLOAT3 Ks{ 0.5f, 0.5f, 0.5f };
    float             Ns{ 32.0f };
    std::wstring      map_Kd;       // diffuse texture
    std::wstring      map_Bump;     // normal map  (bump / map_Kn)
    std::wstring      map_Disp;     // displacement map (disp / map_Ks иногда используют как placeholder)
};

// ── Вершина — pos + normal + tangent + uv ────────────────────────────────
struct ObjVertex
{
    DirectX::XMFLOAT3 Pos;
    DirectX::XMFLOAT3 Normal;
    DirectX::XMFLOAT3 Tangent;   // ← НОВОЕ: касательный вектор (TBN-basis)
    DirectX::XMFLOAT2 TexCoord;
};

// ── Порция геометрии с одним материалом ──────────────────────────────────
struct ObjSubMesh
{
    uint32_t indexStart    = 0;
    uint32_t indexCount    = 0;
    int      materialIndex = 0;
};

// ── Итоговая модель ───────────────────────────────────────────────────────
struct ObjModel
{
    std::vector<ObjVertex>   vertices;
    std::vector<uint32_t>    indices;
    std::vector<ObjMaterial> materials;
    std::vector<ObjSubMesh>  subMeshes;
};

// Загрузить .obj (и .mtl если указан).
bool LoadObj(const wchar_t* path, ObjModel& outModel);
