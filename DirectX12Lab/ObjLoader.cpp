#include "ObjLoader.h"
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <cmath>
#include <windows.h>    // WideCharToMultiByte / MultiByteToWideChar
using namespace DirectX;

// ─────────────────────────────────────────────────────────────────────────────
// Утилиты для строк
// ─────────────────────────────────────────────────────────────────────────────
static std::wstring StrToWStr(const std::string& s)
{
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring ws(n - 1, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, ws.data(), n);
    return ws;
}

static std::wstring GetDirectory(const std::wstring& path)
{
    size_t pos = path.find_last_of(L"/\\");
    return (pos == std::wstring::npos) ? L"" : path.substr(0, pos + 1);
}

// ─────────────────────────────────────────────────────────────────────────────
// Разбор одного токена грани: "1"  "1/2"  "1/2/3"  "1//3"
// Возвращает 1-based индексы (pos, uv, normal).  Отрицательные индексы OBJ
// (относительные) корректно преобразуются в абсолютные.
// ─────────────────────────────────────────────────────────────────────────────
struct FaceIdx { int p = 0, t = 0, n = 0; };

static FaceIdx ParseFaceToken(const std::string& tok,
                               int posCount, int uvCount, int normCount)
{
    FaceIdx fi{};
    auto a = tok.find('/');
    if (a == std::string::npos)
    {
        fi.p = std::stoi(tok);
    }
    else
    {
        fi.p = std::stoi(tok.substr(0, a));
        auto b = tok.find('/', a + 1);
        if (b == std::string::npos)
        {
            if (a + 1 < tok.size()) fi.t = std::stoi(tok.substr(a + 1));
        }
        else
        {
            if (b > a + 1)       fi.t = std::stoi(tok.substr(a + 1, b - a - 1));
            if (b + 1 < tok.size()) fi.n = std::stoi(tok.substr(b + 1));
        }
    }
    // Отрицательные индексы OBJ: -1 = последний элемент
    if (fi.p < 0) fi.p += posCount + 1;
    if (fi.t < 0) fi.t += uvCount  + 1;
    if (fi.n < 0) fi.n += normCount + 1;
    return fi;
}

// ─────────────────────────────────────────────────────────────────────────────
// Загрузка .mtl
// ─────────────────────────────────────────────────────────────────────────────
static void LoadMtl(const std::wstring& mtlPath, const std::wstring& dir,
                    std::vector<ObjMaterial>& mats)
{
    std::ifstream f(mtlPath);
    if (!f.is_open()) return;

    ObjMaterial* cur = nullptr;
    std::string line;
    while (std::getline(f, line))
    {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;

        std::istringstream ss(line);
        std::string tok;
        ss >> tok;

        if (tok == "newmtl")
        {
            std::string name; ss >> name;
            mats.push_back({});
            mats.back().name = name;
            cur = &mats.back();
        }
        else if (cur)
        {
            if      (tok == "Ka") { ss >> cur->Ka.x >> cur->Ka.y >> cur->Ka.z; }
            else if (tok == "Kd") { ss >> cur->Kd.x >> cur->Kd.y >> cur->Kd.z; }
            else if (tok == "Ks") { ss >> cur->Ks.x >> cur->Ks.y >> cur->Ks.z; }
            else if (tok == "Ns") { ss >> cur->Ns; }
            else if (tok == "map_Kd")
            {
                // Собираем остаток строки (путь может содержать пробелы)
                std::string rest;
                std::getline(ss >> std::ws, rest);
                if (!rest.empty() && rest.back() == '\r') rest.pop_back();
                cur->map_Kd = dir + StrToWStr(rest);
            }
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Главная функция загрузки .obj
// ─────────────────────────────────────────────────────────────────────────────
bool LoadObj(const wchar_t* path, ObjModel& out)
{
    std::wstring wpath(path);
    std::wstring dir = GetDirectory(wpath);

    std::ifstream f(wpath);
    if (!f.is_open()) return false;

    std::vector<XMFLOAT3> positions;
    std::vector<XMFLOAT3> normals;
    std::vector<XMFLOAT2> uvs;

    // Сырые грани сгруппированы по материалу
    struct RawFace  { FaceIdx v[3]; };
    struct RawGroup { int matIndex; std::vector<RawFace> faces; };
    std::vector<RawGroup> groups;
    int curMat = -1;    // -1 → ещё не установлен

    std::string line;
    while (std::getline(f, line))
    {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;

        std::istringstream ss(line);
        std::string tok;
        ss >> tok;

        if (tok == "v")
        {
            XMFLOAT3 p; ss >> p.x >> p.y >> p.z;
            positions.push_back(p);
        }
        else if (tok == "vn")
        {
            XMFLOAT3 n; ss >> n.x >> n.y >> n.z;
            normals.push_back(n);
        }
        else if (tok == "vt")
        {
            XMFLOAT2 uv; ss >> uv.x >> uv.y;
            uv.y = 1.0f - uv.y;    // OBJ — OpenGL-конвенция (Y вверх), D3D — Y вниз
            uvs.push_back(uv);
        }
        else if (tok == "mtllib")
        {
            std::string name; ss >> name;
            LoadMtl(dir + StrToWStr(name), dir, out.materials);
        }
        else if (tok == "usemtl")
        {
            std::string matName; ss >> matName;

            // Найти материал по имени
            int mi = -1;
            for (int i = 0; i < (int)out.materials.size(); ++i)
                if (out.materials[i].name == matName) { mi = i; break; }

            // Если не нашли — создать заглушку
            if (mi < 0)
            {
                ObjMaterial def; def.name = matName;
                out.materials.push_back(def);
                mi = (int)out.materials.size() - 1;
            }
            curMat = mi;
            groups.push_back({ mi, {} });
        }
        else if (tok == "f")
        {
            // Убедимся что есть хотя бы одна группа
            if (groups.empty())
            {
                if (out.materials.empty())
                    out.materials.push_back({ "__default__" });
                groups.push_back({ (curMat >= 0 ? curMat : 0), {} });
            }

            // Читаем произвольное число вершин грани (треугольники, квады и т.д.)
            std::vector<FaceIdx> fv;
            std::string vtok;
            while (ss >> vtok)
                fv.push_back(ParseFaceToken(vtok,
                    (int)positions.size(), (int)uvs.size(), (int)normals.size()));

            // Fan-триангуляция: (0,i,i+1) для i=1..n-2
            for (int i = 1; i + 1 < (int)fv.size(); ++i)
                groups.back().faces.push_back({ fv[0], fv[i], fv[i + 1] });
        }
        // Прочие команды (o, g, s, l, …) — игнорируем
    }

    // Если вообще не было материалов
    if (out.materials.empty())
        out.materials.push_back({ "__default__" });

    // ── Собираем плоские буферы вершин/индексов, дедуплицируя вершины ────
    struct VKey
    {
        int p, t, n;
        bool operator==(const VKey& o) const { return p==o.p && t==o.t && n==o.n; }
    };
    struct VKeyHash
    {
        size_t operator()(const VKey& k) const
        {
            size_t h = std::hash<int>{}(k.p);
            h ^= std::hash<int>{}(k.t) + 0x9e3779b9u + (h << 6) + (h >> 2);
            h ^= std::hash<int>{}(k.n) + 0x9e3779b9u + (h << 6) + (h >> 2);
            return h;
        }
    };
    std::unordered_map<VKey, uint32_t, VKeyHash> vertMap;

    auto getOrAdd = [&](const FaceIdx& fi) -> uint32_t
    {
        VKey key{ fi.p, fi.t, fi.n };
        auto it = vertMap.find(key);
        if (it != vertMap.end()) return it->second;

        ObjVertex ov{};
        if (fi.p >= 1 && fi.p <= (int)positions.size()) ov.Pos      = positions[fi.p - 1];
        if (fi.t >= 1 && fi.t <= (int)uvs.size())       ov.TexCoord = uvs[fi.t - 1];
        if (fi.n >= 1 && fi.n <= (int)normals.size())   ov.Normal   = normals[fi.n - 1];

        uint32_t idx = (uint32_t)out.vertices.size();
        out.vertices.push_back(ov);
        vertMap[key] = idx;
        return idx;
    };

    for (auto& grp : groups)
    {
        if (grp.faces.empty()) continue;

        ObjSubMesh sub;
        sub.indexStart    = (uint32_t)out.indices.size();
        sub.materialIndex = grp.matIndex;

        for (auto& face : grp.faces)
        {
            out.indices.push_back(getOrAdd(face.v[0]));
            out.indices.push_back(getOrAdd(face.v[1]));
            out.indices.push_back(getOrAdd(face.v[2]));
        }
        sub.indexCount = (uint32_t)out.indices.size() - sub.indexStart;

        // Слить с предыдущим сабмешем если тот же материал (группы с одним mat
        // встречаются в OBJ при разбивке на объекты)
        if (!out.subMeshes.empty() &&
            out.subMeshes.back().materialIndex == grp.matIndex)
            out.subMeshes.back().indexCount += sub.indexCount;
        else
            out.subMeshes.push_back(sub);
    }

    // ── Генерируем флэт-нормали для вершин у которых нормали отсутствуют ─
    auto isZero = [](const XMFLOAT3& v) {
        return fabsf(v.x) < 1e-6f && fabsf(v.y) < 1e-6f && fabsf(v.z) < 1e-6f;
    };
    for (size_t i = 0; i + 2 < out.indices.size(); i += 3)
    {
        auto& v0 = out.vertices[out.indices[i]];
        auto& v1 = out.vertices[out.indices[i + 1]];
        auto& v2 = out.vertices[out.indices[i + 2]];

        if (isZero(v0.Normal) || isZero(v1.Normal) || isZero(v2.Normal))
        {
            XMVECTOR p0 = XMLoadFloat3(&v0.Pos);
            XMVECTOR p1 = XMLoadFloat3(&v1.Pos);
            XMVECTOR p2 = XMLoadFloat3(&v2.Pos);
            XMVECTOR n  = XMVector3Normalize(XMVector3Cross(p1 - p0, p2 - p0));
            XMFLOAT3 nf; XMStoreFloat3(&nf, n);
            if (isZero(v0.Normal)) v0.Normal = nf;
            if (isZero(v1.Normal)) v1.Normal = nf;
            if (isZero(v2.Normal)) v2.Normal = nf;
        }
    }

    return !out.vertices.empty();
}
