#pragma once
#include <string>
#include <vector>
#include <DirectXMath.h>

// ── Материал из .mtl-файла ────────────────────────────────────────────────
struct ObjMaterial
{
    std::string       name;
    DirectX::XMFLOAT3 Ka{ 0.1f, 0.1f, 0.1f };   // ambient
    DirectX::XMFLOAT3 Kd{ 0.8f, 0.8f, 0.8f };   // diffuse
    DirectX::XMFLOAT3 Ks{ 0.5f, 0.5f, 0.5f };   // specular
    float             Ns{ 32.0f };               // shininess
    std::wstring      map_Kd;  // полный путь к текстуре (пусто = нет текстуры)
};

// ── Вершина (pos + normal + uv) ───────────────────────────────────────────
struct ObjVertex
{
    DirectX::XMFLOAT3 Pos;
    DirectX::XMFLOAT3 Normal;
    DirectX::XMFLOAT2 TexCoord;
};

// ── Порция геометрии с одним материалом ──────────────────────────────────
struct ObjSubMesh
{
    uint32_t indexStart    = 0;
    uint32_t indexCount    = 0;
    int      materialIndex = 0;   // индекс в ObjModel::materials
};

// ── Итоговая модель ────────────────────────────────────────────────────────
struct ObjModel
{
    std::vector<ObjVertex>   vertices;
    std::vector<uint32_t>    indices;    // 32-bit: OBJ-модели бывают большие
    std::vector<ObjMaterial> materials;
    std::vector<ObjSubMesh>  subMeshes;
};

// Загрузить .obj (и .mtl если указан). Возвращает false если файл не открылся.
bool LoadObj(const wchar_t* path, ObjModel& outModel);
