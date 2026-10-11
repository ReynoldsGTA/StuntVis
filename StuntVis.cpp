// StuntVis: stand-alone stunt visualisation for GTA SA and GTA VC (plugin-sdk).
//   K - toggle the predicted flight path (drawn while no wheel touches the ground);
//       hold K and press + / - to change its line thickness (1-10)
//   U - toggle collision spheres of tracked vehicles;
//       hold U and press + / - to change the opacity of all collision drawing (10-100%),
//       U+S to switch spheres/boxes between wireframe and solid,
//       U+D to switch the depth test of collision drawing on/off
//   N - toggle collision boxes + collision mesh (triangles) of tracked vehicles
//   M - toggle collision spheres, boxes and mesh of the vehicle you are driving (independent of U/N)
//   J - track / untrack a vehicle: the one you are in, or else the nearest one (20 m).
//   G - select / deselect the world objects your vehicle (or you, on foot) is standing on
//       or touching, and show their collision (spheres, boxes, triangle mesh). Press G
//       while touching nothing to clear the selection.
//   H - check handling data of a few vehicles against the default handling.cfg
//       (also runs once on the first game frame; shows only numeric codes, 0000 = all OK;
//       code table in the handling section)
// The trajectory (K) is drawn for the vehicle you are driving and every tracked vehicle.
// A tracked vehicle you are driving follows U/N like any other tracked vehicle.
// No Python, no hard-coded game addresses.

#include "plugin.h"
#include "CVehicle.h"
#include "CPed.h"
#include "CWorld.h"
#include "CHud.h"
#include "CModelInfo.h"
#include "CColModel.h"
#include "common.h"
#include "CTimer.h"
#include "CPools.h"
#include "CBuilding.h"
#include "CObject.h"
#include "cHandlingDataMgr.h"
#include "CCamera.h"
#ifdef GTASA
#include "CCollisionData.h"
#else
#include "CAutomobile.h"
#endif

#include <vector>
#include <algorithm>
#include <string>
#include <cmath>
#include <cstdint>
#include <cstdio>
#ifndef GTASA
#include "CTreadable.h"
#endif

using namespace plugin;

namespace {

// ---- SA / VC compatibility ---------------------------------------------------

// World axes of any entity, as plain vectors (copying a CMatrix could copy an attached
// RenderWare matrix it owns). Field names follow plugin-sdk: 'up' = forward, 'at' = up.
// SA static entities (most buildings) often have no full matrix, only position +
// heading (m_placement); the axes are built from that.
struct Frame { CVector right, up, at, pos; };

Frame entityMatrix(CEntity* e)
{
#ifdef GTASA
    if (e->m_matrix)
        return { e->m_matrix->right, e->m_matrix->up, e->m_matrix->at, e->m_matrix->pos };
    const float h = e->m_placement.m_fHeading;
    return { CVector(cosf(h), sinf(h), 0.0f), CVector(-sinf(h), cosf(h), 0.0f),
             CVector(0.0f, 0.0f, 1.0f), e->m_placement.m_vPosn };
#else
    const CMatrix& m = e->m_placement;
    return { m.right, m.up, m.at, m.pos };
#endif
}

CVehicle* playerVehicle()
{
#ifdef GTASA
    return FindPlayerVehicle(0, false);
#else
    return FindPlayerVehicle();
#endif
}

void showHelp(const char* message)
{
#ifdef GTASA
    CHud::SetHelpMessage(message, true, false, false);
#else
    CHud::SetHelpMessage(message, false, false);
#endif
}

CColModel* getColModel(CEntity* e)
{
#ifdef GTASA
    return e->GetColModel();
#else
    CBaseModelInfo* mi = CModelInfo::ms_modelInfoPtrs[e->m_nModelIndex];
    return mi ? mi->m_pColModel : nullptr;
#endif
}

// Sets plain, opaque line rendering for the duration of a scope and restores the
// game's render states afterwards. Without this the lines inherit whatever the game
// drew last (a texture, vertex alpha, fog), which can make them fade or vanish.
struct DebugRenderState {
    RwUInt32 raster = 0, vertexAlpha = 0, zWrite = 0, zTest = 0, fog = 0, srcBlend = 0, dstBlend = 0;
    RwUInt32 lighting = 0, cullMode = 0;
    static constexpr RwUInt32 D3DRS_LIGHTING_ = 137; // same value in D3D8 and D3D9

    DebugRenderState() {
        RwRenderStateGet(rwRENDERSTATETEXTURERASTER, &raster);
        RwRenderStateGet(rwRENDERSTATEVERTEXALPHAENABLE, &vertexAlpha);
        RwRenderStateGet(rwRENDERSTATEZWRITEENABLE, &zWrite);
        RwRenderStateGet(rwRENDERSTATEZTESTENABLE, &zTest);
        RwRenderStateGet(rwRENDERSTATEFOGENABLE, &fog);
        RwRenderStateGet(rwRENDERSTATESRCBLEND, &srcBlend);
        RwRenderStateGet(rwRENDERSTATEDESTBLEND, &dstBlend);
        RwRenderStateGet(rwRENDERSTATECULLMODE, &cullMode);

        RwRenderStateSet(rwRENDERSTATETEXTURERASTER, nullptr);
        RwRenderStateSet(rwRENDERSTATEVERTEXALPHAENABLE, (void*)TRUE); // collision opacity setting
        // No depth test/write: the debug lines are always drawn in front of the world.
        RwRenderStateSet(rwRENDERSTATEZWRITEENABLE, (void*)FALSE);
        RwRenderStateSet(rwRENDERSTATEZTESTENABLE, (void*)FALSE);
        RwRenderStateSet(rwRENDERSTATEFOGENABLE, (void*)FALSE);
        RwRenderStateSet(rwRENDERSTATESRCBLEND, (void*)rwBLENDSRCALPHA);
        RwRenderStateSet(rwRENDERSTATEDESTBLEND, (void*)rwBLENDINVSRCALPHA);
        RwRenderStateSet(rwRENDERSTATECULLMODE, (void*)rwCULLMODECULLNONE); // thick lines are two-sided quads

        // Models are drawn with D3D lighting on; if it is still on, the line colour comes
        // from the scene lighting instead of the vertex colour (light/dark shifts).
#ifdef GTASA
        RwD3D9GetRenderState(D3DRS_LIGHTING_, &lighting);
        RwD3D9SetRenderState(D3DRS_LIGHTING_, FALSE);
#else
        RwD3D8GetRenderState(D3DRS_LIGHTING_, &lighting);
        RwD3D8SetRenderState(D3DRS_LIGHTING_, FALSE);
#endif
    }

    ~DebugRenderState() {
#ifdef GTASA
        RwD3D9SetRenderState(D3DRS_LIGHTING_, lighting);
#else
        RwD3D8SetRenderState(D3DRS_LIGHTING_, lighting);
#endif
        RwRenderStateSet(rwRENDERSTATETEXTURERASTER, (void*)(uintptr_t)raster);
        RwRenderStateSet(rwRENDERSTATEVERTEXALPHAENABLE, (void*)(uintptr_t)vertexAlpha);
        RwRenderStateSet(rwRENDERSTATEZWRITEENABLE, (void*)(uintptr_t)zWrite);
        RwRenderStateSet(rwRENDERSTATEZTESTENABLE, (void*)(uintptr_t)zTest);
        RwRenderStateSet(rwRENDERSTATEFOGENABLE, (void*)(uintptr_t)fog);
        RwRenderStateSet(rwRENDERSTATESRCBLEND, (void*)(uintptr_t)srcBlend);
        RwRenderStateSet(rwRENDERSTATEDESTBLEND, (void*)(uintptr_t)dstBlend);
        RwRenderStateSet(rwRENDERSTATECULLMODE, (void*)(uintptr_t)cullMode);
    }
};

void renderIm3D(RwIm3DVertex* vertices, int count, RwPrimitiveType type)
{
    if (count <= 0)
        return;

    if (RwIm3DTransform(vertices, static_cast<RwUInt32>(count), nullptr, rwIM3D_VERTEXXYZ | rwIM3D_VERTEXRGBA)) {
#ifdef GTASA
        RwIm3DRenderPrimitive(type);
#else
        // The VC SDK only wraps the indexed variant.
        static std::vector<RwImVertexIndex> indices;
        while (indices.size() < static_cast<size_t>(count))
            indices.push_back(static_cast<RwImVertexIndex>(indices.size()));
        RwIm3DRenderIndexedPrimitive(type, indices.data(), count);
#endif
        RwIm3DEnd();
    }
}

bool isAirborne(CVehicle* v)
{
#ifdef GTASA
    return v->GetNumContactWheels() == 0;
#else
    if (v->m_nVehicleClass == VEHICLE_AUTOMOBILE)
        return ((CAutomobile*)v)->nWheelsOnGround == 0;

    // Bikes etc.: no named wheel-contact field, so probe just below the collision box.
    float below = 1.0f;
    if (CColModel* col = getColModel(v))
        below = -col->m_boundBox.m_vecMin.z;

    CVector from = v->GetPosition();
    CVector to = from - CVector(0.0f, 0.0f, below + 0.3f);
    CColPoint colPoint;
    CEntity* hit = nullptr;
    return !CWorld::ProcessLineOfSight(from, to, colPoint, hit, true, false, false, true, false, false, false, false);
#endif
}

// ---- Collision spheres -------------------------------------------------------

struct Sphere { CVector pos; float radius; };

// The old VC SDK declares CColSphere as 16 bytes, but the game stores 20 per sphere
// (centre, radius, surface, piece + padding), so walk the array by the game's stride.
#ifdef GTASA
constexpr size_t COL_SPHERE_STRIDE = sizeof(CColSphere);
#else
constexpr size_t COL_SPHERE_STRIDE = 0x14;
#endif

void collectColSpheres(CEntity* v, std::vector<Sphere>& out)
{
    out.clear();
    CColModel* col = getColModel(v);
    if (!col)
        return;

#ifdef GTASA
    CCollisionData* data = col->m_pColData;
    if (!data)
        return;
    const int count = data->m_nNumSpheres;
    const char* base = reinterpret_cast<const char*>(data->m_pSpheres);
#else
    const int count = col->m_nNumSpheres;
    const char* base = reinterpret_cast<const char*>(col->m_pSpheres);
#endif
    if (!base)
        return;

    const Frame m = entityMatrix(v);
    for (int i = 0; i < count; ++i) {
        const CSphere* s = reinterpret_cast<const CSphere*>(base + i * COL_SPHERE_STRIDE);
        const CVector& c = s->m_vecCenter;
        // local x -> right, local y -> forward ('up'), local z -> up ('at')
        CVector world = m.pos + m.right * c.x + m.up * c.y + m.at * c.z;
        out.push_back({ world, s->m_fRadius });
    }
}

// Unit-sphere wireframe (latitude rings + longitude lines) as line-list endpoints,
// computed once; every sphere is this template scaled and moved.
const std::vector<CVector>& unitSphereLines()
{
    static std::vector<CVector> lines;
    if (lines.empty()) {
        constexpr int SEGMENTS = 8, RINGS = 8;
        const float PI = 3.14159265358979323846f;
        auto point = [PI, RINGS, SEGMENTS](int ring, int seg) {
            const float phi = PI * ring / RINGS, theta = 2.0f * PI * seg / SEGMENTS;
            return CVector(sinf(phi) * cosf(theta), sinf(phi) * sinf(theta), cosf(phi));
        };
        for (int i = 1; i < RINGS; ++i)
            for (int j = 0; j < SEGMENTS; ++j) {
                lines.push_back(point(i, j));
                lines.push_back(point(i, j + 1));
            }
        for (int j = 0; j < SEGMENTS; ++j)
            for (int i = 0; i < RINGS; ++i) {
                lines.push_back(point(i, j));
                lines.push_back(point(i + 1, j));
            }
    }
    return lines;
}

// Appends a wireframe sphere to a line-list batch.
void appendSphere(std::vector<RwIm3DVertex>& out, const CVector& center, float radius)
{
    for (const CVector& u : unitSphereLines()) {
        RwIm3DVertex v{};
        RwIm3DVertexSetPos(&v, center.x + radius * u.x, center.y + radius * u.y, center.z + radius * u.z);
        RwIm3DVertexSetRGBA(&v, 0, 255, 0, 255);
        out.push_back(v);
    }
}

// Collision drawing style (U+S / U+D).
bool g_collisionSolid = false;      // solid shaded spheres/boxes instead of wireframes
bool g_collisionDepthTest = false;  // hide collision behind world geometry

// Solid shapes are wound counter-clockwise when seen from outside (in GTA's right-handed,
// z-up world), and the GPU culls the back faces. If a game/driver ends up showing only
// the inside (back) halves, switch this to rwCULLMODECULLFRONT.
const RwCullMode kSolidCullMode = rwCULLMODECULLBACK;

inline CVector crossProduct(const CVector& u, const CVector& v)
{
    return CVector(u.y * v.z - u.z * v.y, u.z * v.x - u.x * v.z, u.x * v.y - u.y * v.x);
}

// Fixed light for the solid shading (normalised (0.3, 0.4, 0.866)).
const CVector kLightDir(0.3f, 0.4f, 0.866f);

float shadeFor(const CVector& n)
{
    const float d = n.x * kLightDir.x + n.y * kLightDir.y + n.z * kLightDir.z;
    return 0.45f + 0.55f * (d > 0.0f ? d : 0.0f);
}

void pushVertex(std::vector<RwIm3DVertex>& out, const CVector& p, int r, int g, int b, float shade)
{
    RwIm3DVertex v{};
    RwIm3DVertexSetPos(&v, p.x, p.y, p.z);
    RwIm3DVertexSetRGBA(&v, static_cast<RwUInt8>(r * shade), static_cast<RwUInt8>(g * shade),
                        static_cast<RwUInt8>(b * shade), 255);
    out.push_back(v);
}

// Unit-sphere triangle list (8 rings x 12 segments), computed once. On a unit sphere the
// position is also the normal, so the shading comes straight from it.
const std::vector<CVector>& unitSphereTriangles()
{
    static std::vector<CVector> tris;
    if (tris.empty()) {
        constexpr int SEGMENTS = 12, RINGS = 8;
        const float PI = 3.14159265358979323846f;
        auto point = [PI, SEGMENTS, RINGS](int ring, int seg) {
            const float phi = PI * ring / RINGS, theta = 2.0f * PI * seg / SEGMENTS;
            return CVector(sinf(phi) * cosf(theta), sinf(phi) * sinf(theta), cosf(phi));
        };
        // Emit a triangle wound counter-clockwise seen from outside (normal along the
        // centroid direction); degenerate triangles at the poles are skipped.
        auto emit = [](const CVector& a, const CVector& b, const CVector& c) {
            const CVector n = crossProduct(b - a, c - a);
            const CVector centroid = a + b + c;
            const float facing = n.x * centroid.x + n.y * centroid.y + n.z * centroid.z;
            if (facing == 0.0f)
                return;
            tris.push_back(a);
            if (facing > 0.0f) { tris.push_back(b); tris.push_back(c); }
            else               { tris.push_back(c); tris.push_back(b); }
        };
        for (int i = 0; i < RINGS; ++i)
            for (int j = 0; j < SEGMENTS; ++j) {
                const CVector a = point(i, j), b = point(i + 1, j), c = point(i + 1, j + 1), d = point(i, j + 1);
                emit(a, b, c);
                emit(a, c, d);
            }
    }
    return tris;
}

// Appends a solid, shaded sphere to a triangle-list batch (back faces are culled on the GPU).
void appendSolidSphere(std::vector<RwIm3DVertex>& out, const CVector& center, float radius)
{
    for (const CVector& u : unitSphereTriangles())
        pushVertex(out, center + u * radius, 0, 255, 0, shadeFor(u));
}

// ---- Collision boxes ---------------------------------------------------------

struct Box { CVector corners[8]; };

// Same story as the spheres: the old VC SDK declares CColBox as a bare 24-byte CBox,
// but the game stores min, max, surface, piece + padding = 28 bytes per box (as in SA).
#ifdef GTASA
constexpr size_t COL_BOX_STRIDE = sizeof(CColBox);
#else
constexpr size_t COL_BOX_STRIDE = 0x1C;
#endif

void collectColBoxes(CEntity* v, std::vector<Box>& out)
{
    out.clear();
    CColModel* col = getColModel(v);
    if (!col)
        return;

#ifdef GTASA
    CCollisionData* data = col->m_pColData;
    if (!data)
        return;
    const int count = data->m_nNumBoxes;
    const char* base = reinterpret_cast<const char*>(data->m_pBoxes);
#else
    const int count = col->m_nNumBoxes;
    const char* base = reinterpret_cast<const char*>(col->m_pBoxes);
#endif
    if (!base)
        return;

    const Frame m = entityMatrix(v);
    auto toWorld = [&m](float x, float y, float z) {
        return m.pos + m.right * x + m.up * y + m.at * z;
    };

    for (int i = 0; i < count; ++i) {
        const CBox* b = reinterpret_cast<const CBox*>(base + i * COL_BOX_STRIDE);
        const CVector& lo = b->m_vecMin;
        const CVector& hi = b->m_vecMax;

        // Corner index bits: 1 = max x, 2 = max y, 4 = max z.
        Box box;
        for (int c = 0; c < 8; ++c)
            box.corners[c] = toWorld((c & 1) ? hi.x : lo.x, (c & 2) ? hi.y : lo.y, (c & 4) ? hi.z : lo.z);
        out.push_back(box);
    }
}

// Appends a box's 12 edges to a line-list batch.
void appendBox(std::vector<RwIm3DVertex>& out, const Box& box)
{
    // The 12 edges: corners differing in exactly one bit.
    static const int edges[12][2] = {
        {0,1},{2,3},{4,5},{6,7},   // along x
        {0,2},{1,3},{4,6},{5,7},   // along y
        {0,4},{1,5},{2,6},{3,7},   // along z
    };

    for (int e = 0; e < 12; ++e) {
        for (int k = 0; k < 2; ++k) {
            const CVector& p = box.corners[edges[e][k]];
            RwIm3DVertex v{};
            RwIm3DVertexSetPos(&v, p.x, p.y, p.z);
            RwIm3DVertexSetRGBA(&v, 0, 160, 255, 255);
            out.push_back(v);
        }
    }
}

// Appends a solid, shaded box (6 faces, 2 triangles each) to a triangle-list batch, each
// face wound counter-clockwise seen from outside (back faces are culled on the GPU).
void appendSolidBox(std::vector<RwIm3DVertex>& out, const Box& box)
{
    // Faces as corner indices (bit 1 = max x, 2 = max y, 4 = max z), wound consistently.
    static const int faces[6][4] = {
        {0,2,6,4}, {1,5,7,3},   // -x, +x
        {0,4,5,1}, {2,3,7,6},   // -y, +y
        {0,1,3,2}, {4,6,7,5},   // -z, +z
    };
    const CVector centre = (box.corners[0] + box.corners[7]) * 0.5f;
    for (const auto& f : faces) {
        const CVector& a = box.corners[f[0]];
        const CVector& c = box.corners[f[2]];
        const CVector* b = &box.corners[f[1]];
        const CVector* d = &box.corners[f[3]];
        CVector n = crossProduct(*b - a, *d - a);
        const float len = sqrtf(n.x * n.x + n.y * n.y + n.z * n.z);
        if (len > 1e-6f) n = n * (1.0f / len);
        // Reverse the face if it is wound inwards (depends on the entity's matrix), so it is
        // always counter-clockwise seen from outside.
        const CVector fc = (a + c) * 0.5f - centre;
        if (n.x * fc.x + n.y * fc.y + n.z * fc.z < 0.0f) {
            std::swap(b, d);
            n = n * -1.0f;
        }
        const float s = shadeFor(n);
        pushVertex(out, a, 0, 160, 255, s); pushVertex(out, *b, 0, 160, 255, s); pushVertex(out, c, 0, 160, 255, s);
        pushVertex(out, a, 0, 160, 255, s); pushVertex(out, c, 0, 160, 255, s); pushVertex(out, *d, 0, 160, 255, s);
    }
}

// ---- Collision triangle meshes -----------------------------------------------

// Appends the edges of an entity's collision triangles as a line list.
// SA stores vertices compressed (int16, 1/128 m units); VC (PC) stores plain floats.
// Both use 8-byte triangles (3 x uint16 vertex index + surface data).
void collectColTriangles(CEntity* e, std::vector<RwIm3DVertex>& out, size_t maxTriangles)
{
    CColModel* col = getColModel(e);
    if (!col)
        return;

#ifdef GTASA
    CCollisionData* data = col->m_pColData;
    if (!data || !data->m_pVertices || !data->m_pTriangles)
        return;
    const int count = data->m_nNumTriangles;
    auto vertex = [data](unsigned i) {
        const CompressedVector& c = data->m_pVertices[i];
        return CVector(c.x / 128.0f, c.y / 128.0f, c.z / 128.0f);
    };
    auto tri = [data](int i, unsigned k) {
        const CColTriangle& t = data->m_pTriangles[i];
        return k == 0 ? t.m_nVertA : (k == 1 ? t.m_nVertB : t.m_nVertC);
    };
#else
    if (!col->m_pVertices || !col->m_pTriangles)
        return;
    const int count = col->m_nNumTriangles;
    const CVector* verts = static_cast<const CVector*>(col->m_pVertices);
    const unsigned short* tris = static_cast<const unsigned short*>(col->m_pTriangles); // 4 x uint16 each
    auto vertex = [verts](unsigned i) { return verts[i]; };
    auto tri = [tris](int i, unsigned k) { return tris[i * 4 + k]; };
#endif

    const Frame m = entityMatrix(e);
    auto toWorld = [&m](const CVector& c) { return m.pos + m.right * c.x + m.up * c.y + m.at * c.z; };
    auto add = [&out](const CVector& p) {
        RwIm3DVertex v{};
        RwIm3DVertexSetPos(&v, p.x, p.y, p.z);
        RwIm3DVertexSetRGBA(&v, 255, 140, 0, 255);
        out.push_back(v);
    };

    const size_t limit = maxTriangles < static_cast<size_t>(count) ? maxTriangles : static_cast<size_t>(count);
    for (size_t i = 0; i < limit; ++i) {
        const CVector a = toWorld(vertex(tri(static_cast<int>(i), 0)));
        const CVector b = toWorld(vertex(tri(static_cast<int>(i), 1)));
        const CVector c = toWorld(vertex(tri(static_cast<int>(i), 2)));
        add(a); add(b);
        add(b); add(c);
        add(c); add(a);
    }
}

// Opacity of collision wireframes in steps of 10% (1..10).
int g_collisionOpacity = 10;
constexpr int MAX_COLLISION_OPACITY = 10;

// Sets the alpha of every vertex (D3D colour is ARGB: alpha in the top byte).
void applyAlpha(std::vector<RwIm3DVertex>& verts, int opacityStep)
{
    if (opacityStep >= MAX_COLLISION_OPACITY)
        return;  // vertices are created fully opaque
    const RwUInt32 a = static_cast<RwUInt32>(opacityStep * 255 / MAX_COLLISION_OPACITY) << 24;
    for (RwIm3DVertex& v : verts)
        v.color = (v.color & 0x00FFFFFF) | a;
}

// Draws a long triangle list in chunks (30000 is a multiple of 3).
void drawTriList(std::vector<RwIm3DVertex>& verts)
{
    constexpr size_t CHUNK = 30000;
    for (size_t i = 0; i < verts.size(); i += CHUNK) {
        const size_t n = (verts.size() - i) < CHUNK ? (verts.size() - i) : CHUNK;
        renderIm3D(verts.data() + i, static_cast<int>(n), rwPRIMTYPETRILIST);
    }
}

// Draws a long line list in chunks (RenderWare's immediate mode takes at most 65535 vertices).
void drawLineList(std::vector<RwIm3DVertex>& verts)
{
    constexpr size_t CHUNK = 30000; // even, so lines are never split; well below the 65535 limit
    for (size_t i = 0; i < verts.size(); i += CHUNK) {
        const size_t n = (verts.size() - i) < CHUNK ? (verts.size() - i) : CHUNK;
        renderIm3D(verts.data() + i, static_cast<int>(n), rwPRIMTYPELINELIST);
    }
}

// ---- Object selection --------------------------------------------------------

struct SelectedObject {
    CEntity* entity;
    int modelIndex;    // guards against the pool slot being reused by another entity
    bool isBuilding;   // buildings never move: their geometry is cached once

    // World-space collision geometry, cached so it stays visible when the game streams the
    // collision out (or deletes and later recreates an object) as you move away.
    bool cached = false;
    std::vector<RwIm3DVertex> lines;
    std::vector<Sphere> spheres;
    std::vector<Box> boxes;
};

// True if the pointer is still a live building/object of the same model.
bool isSelectionValid(const SelectedObject& s)
{
    CEntity* e = s.entity;
    bool alive = CPools::ms_pBuildingPool->IsObjectValid(reinterpret_cast<CBuilding*>(e))
              || CPools::ms_pObjectPool->IsObjectValid(reinterpret_cast<CObject*>(e));
#ifndef GTASA
    alive = alive || CPools::ms_pTreadablePool->IsObjectValid(reinterpret_cast<CTreadable*>(e));
#endif
    return alive && e->m_nModelIndex == s.modelIndex;
}

// Buildings and objects the given physical entity is on or touching.
void touchedEntities(CPhysical* self, CEntity* standingOn, std::vector<CEntity*>& out)
{
    out.clear();
    auto add = [&out, self](CEntity* e) {
        if (!e || e == self)
            return;
        if (e->m_nType != ENTITY_TYPE_BUILDING && e->m_nType != ENTITY_TYPE_OBJECT)
            return;
        if (std::find(out.begin(), out.end(), e) == out.end())
            out.push_back(e);
    };

    add(standingOn);

    // Collision records from the last physics step.
#ifdef GTASA
    for (int i = 0; i < self->m_nNumEntitiesCollided && i < 6; ++i)
        add(self->m_apCollidedEntities[i]);
#else
    for (int i = 0; i < self->m_nNumCollisionRecords && i < 6; ++i)
        add(self->m_apCollisionRecords[i]);
#endif

    // Whatever is directly below (covers wheels resting on a roof, which are not collisions).
    CVector from = self->GetPosition();
    CVector to = from - CVector(0.0f, 0.0f, 5.0f);
    CColPoint colPoint;
    CEntity* hit = nullptr;
    if (CWorld::ProcessLineOfSight(from, to, colPoint, hit, true, false, false, true, false, false, false, false))
        add(hit);
}

// ---- Trajectory --------------------------------------------------------------

// Same rules the game applies to an airborne vehicle: gravity 0.008 per 1/50 s step
// plus CPhysical-style air resistance. One polyline vertex per 5 steps (0.1 s).
void computeTrajectory(CVehicle* v, std::vector<CVector>& points, bool& hitsGround)
{
    constexpr float GRAVITY = 0.008f;
    constexpr int   MAX_STEPS = 200;      // 4 s look-ahead
    constexpr int   STEPS_PER_POINT = 5;  // max 40 segments

    points.clear();
    hitsGround = false;

    CVector pos = v->GetPosition();
    CVector vel = v->m_vecMoveSpeed;
    const float airResistance = v->m_fAirResistance;

    points.push_back(pos);
    for (int step = 1; step <= MAX_STEPS; ++step) {
        vel.z -= GRAVITY;
        const float drag = 1.0f - vel.Magnitude() * airResistance;
        if (drag > 0.0f && drag < 1.0f)
            vel *= drag;
        pos += vel;

        if (step % STEPS_PER_POINT != 0 && step != MAX_STEPS)
            continue;

        CColPoint colPoint;
        CEntity* hit = nullptr;
        if (CWorld::ProcessLineOfSight(points.back(), pos, colPoint, hit, true, false, false, true, false, false, false, false)) {
            points.push_back(colPoint.m_vecPoint);
            hitsGround = true;
            return;
        }
        points.push_back(pos);
    }
}

// Trajectory line thickness in (approximate) pixels; 1 = plain 1-pixel lines.
int g_trajectoryWidth = 1;
constexpr int MAX_TRAJECTORY_WIDTH = 10;

CVector cameraPosition()
{
#ifdef GTASA
    return TheCamera.GetPosition();
#else
    return TheCamera.pos;
#endif
}

// Appends segment a-b as a camera-facing quad (two triangles) of about 'px' pixels wide.
// The width grows with distance to the camera so it stays roughly constant on screen
// (0.0013 ~ 2*tan(35 deg)/1080: one pixel at 1 m for a typical FOV and 1080p).
void addThickSegment(std::vector<RwIm3DVertex>& out, const CVector& a, const CVector& b,
                     const CVector& cam, float px, RwUInt8 r, RwUInt8 g, RwUInt8 bl)
{
    auto cross = [](const CVector& u, const CVector& v) {
        return CVector(u.y * v.z - u.z * v.y, u.z * v.x - u.x * v.z, u.x * v.y - u.y * v.x);
    };
    auto length = [](const CVector& v) { return sqrtf(v.x * v.x + v.y * v.y + v.z * v.z); };

    const CVector dir = b - a;
    CVector side = cross(dir, cam - (a + b) * 0.5f);
    const float len = length(side);
    if (len < 1e-6f)
        return;  // segment points straight at the camera
    side = side * (1.0f / len);

    const float k = 0.5f * px * 0.0013f;
    const CVector oa = side * (k * length(cam - a));
    const CVector ob = side * (k * length(cam - b));
    const CVector corners[6] = { a + oa, a - oa, b + ob, b + ob, a - oa, b - ob };
    for (const CVector& p : corners) {
        RwIm3DVertex v{};
        RwIm3DVertexSetPos(&v, p.x, p.y, p.z);
        RwIm3DVertexSetRGBA(&v, r, g, bl, 255);
        out.push_back(v);
    }
}

void drawTrajectory(const std::vector<CVector>& points, bool hitsGround)
{
    if (points.size() < 2)
        return;

    CVector cross[4];
    if (hitsGround) {
        const CVector& l = points.back();
        const float s = 1.0f;
        cross[0] = CVector(l.x - s, l.y, l.z + 0.1f);
        cross[1] = CVector(l.x + s, l.y, l.z + 0.1f);
        cross[2] = CVector(l.x, l.y - s, l.z + 0.1f);
        cross[3] = CVector(l.x, l.y + s, l.z + 0.1f);
    }

    if (g_trajectoryWidth <= 1) {
        std::vector<RwIm3DVertex> v(points.size());
        for (size_t i = 0; i < points.size(); ++i) {
            RwIm3DVertexSetPos(&v[i], points[i].x, points[i].y, points[i].z);
            RwIm3DVertexSetRGBA(&v[i], 255, 200, 0, 255);
        }
        renderIm3D(v.data(), static_cast<int>(v.size()), rwPRIMTYPEPOLYLINE);

        if (hitsGround) {
            RwIm3DVertex c[4]{};
            for (int i = 0; i < 4; ++i) {
                RwIm3DVertexSetPos(&c[i], cross[i].x, cross[i].y, cross[i].z);
                RwIm3DVertexSetRGBA(&c[i], 255, 0, 0, 255);
            }
            renderIm3D(c, 4, rwPRIMTYPELINELIST);
        }
        return;
    }

    const CVector cam = cameraPosition();
    const float px = static_cast<float>(g_trajectoryWidth);
    std::vector<RwIm3DVertex> tris;
    tris.reserve(points.size() * 6 + 12);
    for (size_t i = 1; i < points.size(); ++i)
        addThickSegment(tris, points[i - 1], points[i], cam, px, 255, 200, 0);
    if (hitsGround) {
        addThickSegment(tris, cross[0], cross[1], cam, px, 255, 0, 0);
        addThickSegment(tris, cross[2], cross[3], cam, px, 255, 0, 0);
    }
    renderIm3D(tris.data(), static_cast<int>(tris.size()), rwPRIMTYPETRILIST);
}

// ---- Handling check ----------------------------------------------------------
//
// Compares the handling data the game has in memory with the default handling.cfg.
// Only fields the game keeps exactly as read from the file are checked; values it
// rescales on load (engine acceleration, max velocity, brake deceleration, and in VC
// the turn mass, which VC computes) are skipped.
//
// SA reference: the original handling.cfg of an SA install (file dated 2004-09-30);
//   identical to MTA:SA's built-in copy (mtasa-blue CHandlingManager.cpp).
// VC reference: handling.cfg of a VC install; INFERNUS/PACKER also match the copy
//   published on Grand Theft Wiki.

// Results are reported only as numeric codes so the output does not tell anyone which
// values the check looks at. Code = vehicle index * 100 + field index (both 1-based,
// in the order of kHandlingRefs and of the compare calls in checkVehicle()).
// Code xx00 = the vehicle's handling entry was not found.
struct HandlingReport {
    int vehiclesChecked = 0;
    int mismatches = 0;
    int vehicle = 0;  // 1-based index of the vehicle being checked
    int field = 0;    // 1-based index of the field being checked
    std::vector<int> codes;
#ifdef _DEBUG
    std::string debug;  // actual vs expected values; debug builds only
#endif
};

void fail(HandlingReport& r)
{
    ++r.mismatches;
    r.codes.push_back(r.vehicle * 100 + r.field);
}

void compareF(HandlingReport& r, float actual, float expected)
{
    ++r.field;
    const float tol = 1e-4f * (fabsf(expected) > 1.0f ? fabsf(expected) : 1.0f);
    if (fabsf(actual - expected) > tol) {
        fail(r);
#ifdef _DEBUG
        char s[64];
        snprintf(s, sizeof(s), " [%04d: %g != %g]", r.codes.back(), actual, expected);
        r.debug += s;
#endif
    }
}

void compareU(HandlingReport& r, unsigned actual, unsigned expected)
{
    ++r.field;
    if (actual != expected) {
        fail(r);
#ifdef _DEBUG
        char s[64];
        snprintf(s, sizeof(s), " [%04d: 0x%X != 0x%X]", r.codes.back(), actual, expected);
        r.debug += s;
#endif
    }
}

#ifdef GTASA
// Field indices for the codes (SA):
//    1 mass
//    2 turnMass
//    3 dragMult
//    4 centreOfMass.x
//    5 centreOfMass.y
//    6 centreOfMass.z
//    7 percentSubmerged
//    8 tractionMultiplier
//    9 brakeBias
//   10 ABS
//   11 steeringLock
//   12 tractionLoss
//   13 tractionBias
//   14 suspensionForce
//   15 suspensionDamping
//   16 suspensionHighSpdDamping
//   17 suspensionUpperLimit
//   18 suspensionLowerLimit
//   19 suspensionBias
//   20 suspensionAntiDive
//   21 modelFlags
//   22 handlingFlags
//   23 seatOffset
//   24 monetaryValue
//   25 frontLights
//   26 rearLights
struct HandlingRef {
    const char* name;   // handling.cfg identifier (what GetHandlingId looks up)
    float mass, turnMass, dragMult;
    float comX, comY, comZ;
    unsigned percentSubmerged;
    float tractionMult, brakeBias;
    unsigned abs;
    float steeringLock, tractionLoss, tractionBias;
    float suspForce, suspDamping, suspHighSpdDamping, suspUpper, suspLower, suspBias, suspAntiDive;
    float collisionDamage;
    unsigned modelFlags, handlingFlags;
    float seatOffset;
    unsigned monetary, frontLights, rearLights;
};

const HandlingRef kHandlingRefs[] = {
    { "INFERNUS", 1400.0f, 2725.3f, 1.5f, 0.0f, 0.0f, -0.25f, 70, 0.70f, 0.51f, 0, 30.0f, 0.80f, 0.50f,
      1.20f, 0.19f, 0.0f, 0.25f, -0.10f, 0.50f, 0.40f, 0.72f, 0x40002004, 0x00C04000, 0.37f, 95000, 1, 1 },
    { "PACKER",   8000.0f, 48273.3f, 2.0f, 0.0f, 0.0f, 0.0f, 90, 0.65f, 0.35f, 0, 30.0f, 0.85f, 0.35f,
      1.50f, 0.04f, 0.0f, 0.45f, -0.25f, 0.50f, 0.00f, 0.40f, 0x00004000, 0x00440000, 0.56f, 20000, 0, 1 },
    { "NRG500",   400.0f, 200.0f, 4.0f, 0.0f, 0.08f, -0.09f, 103, 1.80f, 0.50f, 0, 35.0f, 0.90f, 0.48f,
      0.85f, 0.15f, 0.0f, 0.15f, -0.16f, 0.50f, 0.00f, 0.15f, 0x01002000, 0x00000002, 0.00f, 10000, 1, 1 },
};

void checkVehicle(HandlingReport& r, const HandlingRef& e, const tHandlingData& h)
{
    compareF(r, h.m_fMass, e.mass);
    compareF(r, h.m_fTurnMass, e.turnMass);
    compareF(r, h.m_fDragMult, e.dragMult);
    compareF(r, h.m_vecCentreOfMass.x, e.comX);
    compareF(r, h.m_vecCentreOfMass.y, e.comY);
    compareF(r, h.m_vecCentreOfMass.z, e.comZ);
    compareU(r, h.m_nPercentSubmerged, e.percentSubmerged);
    compareF(r, h.m_fTractionMultiplier, e.tractionMult);
    compareF(r, h.m_fBrakeBias, e.brakeBias);
    compareU(r, static_cast<unsigned>(h.m_bABS), e.abs);
    compareF(r, h.m_fSteeringLock, e.steeringLock);
    compareF(r, h.m_fTractionLoss, e.tractionLoss);
    compareF(r, h.m_fTractionBias, e.tractionBias);
    compareF(r, h.m_fSuspensionForceLevel, e.suspForce);
    compareF(r, h.m_fSuspensionDampingLevel, e.suspDamping);
    compareF(r, h.m_fSuspensionHighSpdComDamp, e.suspHighSpdDamping);
    compareF(r, h.m_fSuspensionUpperLimit, e.suspUpper);
    compareF(r, h.m_fSuspensionLowerLimit, e.suspLower);
    compareF(r, h.m_fSuspensionBiasBetweenFrontAndRear, e.suspBias);
    compareF(r, h.m_fSuspensionAntiDiveMultiplier, e.suspAntiDive);
    // collisionDamageMultiplier is not checked: the game rescales it on load (failed on a clean install).
    compareU(r, static_cast<unsigned>(h.m_nModelFlags), e.modelFlags);
    compareU(r, static_cast<unsigned>(h.m_nHandlingFlags), e.handlingFlags);
    compareF(r, h.m_fSeatOffsetDistance, e.seatOffset);
    compareU(r, h.m_nMonetaryValue, e.monetary);
    compareU(r, static_cast<unsigned>(h.m_nFrontLights), e.frontLights);
    compareU(r, static_cast<unsigned>(h.m_nRearLights), e.rearLights);
}
#else
// Field indices for the codes (VC):
//    1 mass
//    2 dimensions.x
//    3 dimensions.y
//    4 dimensions.z
//    5 centreOfMass.x
//    6 centreOfMass.y
//    7 centreOfMass.z
//    8 percentSubmerged
//    9 tractionMultiplier
//   10 tractionLoss
//   11 tractionBias
//   12 brakeBias
//   13 ABS
//   14 steeringLock
//   15 suspensionForce
//   16 suspensionDamping
//   17 seatOffset
//   18 monetaryValue
//   19 suspensionUpperLimit
//   20 suspensionLowerLimit
//   21 suspensionBias
//   22 suspensionAntiDive
//   23 flags
//   24 frontLights
//   25 rearLights
struct HandlingRef {
    const char* name;   // handling.cfg identifier (what GetHandlingId looks up)
    float mass;
    float dimX, dimY, dimZ;
    float comX, comY, comZ;
    unsigned percentSubmerged;
    float tractionMult, tractionLoss, tractionBias, brakeBias;
    unsigned abs;
    float steeringLock, suspForce, suspDamping, seatOffset, collisionDamage;
    unsigned monetary;
    float suspUpper, suspLower, suspBias, suspAntiDive;
    unsigned flags, frontLights, rearLights;
    unsigned flagsAlt;  // second accepted flags value (0 = none), e.g. the LC mod for VC
};

// Flags are hexadecimal in handling.cfg. Values checked against the handling.cfg of a
// VC install: the PCJ-600 uses the handling ID "BIKE" (default.ide).
// Vehicle indices: SA 1 INFERNUS, 2 PACKER, 3 NRG500 / VC 1 INFERNUS, 2 PACKER, 3 PCJ600 (BIKE).
const HandlingRef kHandlingRefs[] = {
    { "INFERNUS", 1600.0f, 2.0f, 4.4f, 1.3f, 0.0f, 0.0f, -0.3f, 70, 0.95f, 0.80f, 0.52f, 0.52f, 0, 30.0f,
      1.8f, 0.19f, 0.37f, 0.72f, 95000, 0.25f, -0.17f, 0.5f, 0.4f, 0x1208182, 1, 1 },
    { "PACKER",   8000.0f, 2.9f, 8.0f, 3.6f, 0.0f, 0.0f, 0.0f, 90, 0.65f, 0.85f, 0.35f, 0.35f, 0, 30.0f,
      1.5f, 0.04f, 0.56f, 0.40f, 20000, 0.45f, -0.25f, 0.5f, 0.0f, 0x8000, 0, 1,
      0x8100 },  // LC mod: PACKER flags 8100 (adds DBL_EXHAUST, visual only)
    { "BIKE",     500.0f, 0.8f, 1.8f, 1.2f, 0.0f, 0.05f, -0.09f, 103, 1.6f, 0.9f, 0.48f, 0.50f, 0, 35.0f,
      0.85f, 0.15f, 0.3f, 0.15f, 10000, 0.15f, -0.16f, 0.5f, 0.0f, 0x10102, 1, 1 },
};

void checkVehicle(HandlingReport& r, const HandlingRef& e, const tHandlingData& h)
{
    compareF(r, h.fMass, e.mass);
    compareF(r, h.m_vDimensions.x, e.dimX);
    compareF(r, h.m_vDimensions.y, e.dimY);
    compareF(r, h.m_vDimensions.z, e.dimZ);
    compareF(r, h.m_vecCentreOfMass.x, e.comX);
    compareF(r, h.m_vecCentreOfMass.y, e.comY);
    compareF(r, h.m_vecCentreOfMass.z, e.comZ);
    compareU(r, static_cast<unsigned>(h.nPercentSubmerged), e.percentSubmerged);
    compareF(r, h.fTractionMultiplier, e.tractionMult);
    compareF(r, h.fTractionLoss, e.tractionLoss);
    compareF(r, h.fTractionBias, e.tractionBias);
    compareF(r, h.fBrakeBias, e.brakeBias);
    compareU(r, static_cast<unsigned>(h.bABS), e.abs);
    compareF(r, h.fSteeringLock, e.steeringLock);
    compareF(r, h.fSuspensionForceLevel, e.suspForce);
    compareF(r, h.fSuspensionDampingLevel, e.suspDamping);
    compareF(r, h.fSeatOffsetDistance, e.seatOffset);
    // collisionDamageMultiplier is not checked: VC rescales it on load (failed on a clean install).
    compareU(r, static_cast<unsigned>(h.nMonetaryValue), e.monetary);
    compareF(r, h.fSuspUpperLimit, e.suspUpper);
    compareF(r, h.fSuspLowerLimit, e.suspLower);
    compareF(r, h.fSuspBias, e.suspBias);
    compareF(r, h.fSuspAntiDiveMultiplier, e.suspAntiDive);
    compareU(r, (e.flagsAlt && h.uFlags == e.flagsAlt) ? e.flags : h.uFlags, e.flags);
    compareU(r, static_cast<unsigned>(h.bFrontLights), e.frontLights);
    compareU(r, static_cast<unsigned>(h.bRearLights), e.rearLights);
}
#endif

// Runs the check; fills 'codes' with "0000" or the failing codes. Returns true if all match.
bool runHandlingCheck(char* codes, size_t size)
{
    HandlingReport r;
    for (const HandlingRef& ref : kHandlingRefs) {
        ++r.vehicle;
        r.field = 0;
        const int id = gHandlingDataMgr.GetHandlingId(const_cast<char*>(ref.name));
        if (id < 0) {
            fail(r);  // xx00: entry missing
            continue;
        }
        checkVehicle(r, ref, gHandlingDataMgr.m_aVehicleHandling[id]);
        ++r.vehiclesChecked;
    }

    snprintf(codes, size, "0000");
    int len = 0;
    for (size_t i = 0; i < r.codes.size() && len < static_cast<int>(size) - 6; ++i)
        len += snprintf(codes + len, size - len, i ? " %04d" : "%04d", r.codes[i]);
#ifdef _DEBUG
    if (!r.debug.empty() && len < static_cast<int>(size) - 1)
        snprintf(codes + len, size - len, "%s", r.debug.c_str());
#endif
    return r.mismatches == 0;
}

// ---- Input -------------------------------------------------------------------

// True once per key press (rising edge), independent of the game's own input.
bool justPressed(int key, bool& wasDown)
{
    const bool down = (GetAsyncKeyState(key) & 0x8000) != 0;
    const bool pressed = down && !wasDown;
    wasDown = down;
    return pressed;
}

} // namespace

class StuntVis {
public:
    StuntVis() {
        Events::gameProcessEvent.Add(process);
        Events::drawingEvent.Add(draw);
    }

private:
    struct VehicleVis {
        std::vector<CVector> trajectory;
        bool hitsGround = false;
    };

    static inline bool s_showTrajectory = false;
    static inline bool s_showCollisions = false;
    static inline bool s_showBoxes = false;
    static inline bool s_nDown = false;
    static inline bool s_ownCollisions = false;
    static inline bool s_mDown = false;
    static inline bool s_kDown = false;
    static inline bool s_kUsedAsModifier = false;  // +/- pressed while K held: no toggle on release
    static inline bool s_uUsedAsModifier = false;  // same for U
    static inline bool s_sDown = false, s_dDown = false;

    // A key that toggles on release, or adjusts 'value' when + / - is pressed while it is held.
    template<class Toggle>
    static void handleModifierKey(int key, bool& wasDown, bool& usedAsModifier, int delta,
                                  int& value, int minValue, int maxValue, const char* format, Toggle toggle) {
        const bool down = (GetAsyncKeyState(key) & 0x8000) != 0;
        if (down) {
            if (!wasDown)
                usedAsModifier = false;
            if (delta != 0) {
                usedAsModifier = true;
                if (visualsAllowed()) {
                    value += delta;
                    if (value < minValue) value = minValue;
                    if (value > maxValue) value = maxValue;
                    char msg[48];
                    snprintf(msg, sizeof(msg), format, value);
                    showHelp(msg);
                }
            }
        }
        else if (wasDown && !usedAsModifier && visualsAllowed()) {
            toggle();
        }
        wasDown = down;
    }
    static inline bool s_plusDown = false, s_minusDown = false, s_addDown = false, s_subDown = false;
    static inline bool s_uDown = false;
    static inline bool s_jDown = false;
    static inline bool s_hDown = false;
    static inline bool s_handlingChecked = false;
    static inline bool s_handlingOk = true;          // visualisations are locked while false
    static inline char s_handlingCodes[200] = "0000";
    static inline unsigned s_lastHandlingCheck = 0;  // CTimer ms
    static constexpr unsigned HANDLING_RECHECK_MS = 5000;

    // Runs the handling check. On failure every visualisation is switched off and stays
    // locked until a later check passes again.
    static void checkHandling(bool announce) {
        const bool wasOk = s_handlingOk;
        s_handlingOk = runHandlingCheck(s_handlingCodes, sizeof(s_handlingCodes));
        s_lastHandlingCheck = CTimer::m_snTimeInMilliseconds;
        if (!s_handlingOk) {
            s_showTrajectory = s_showCollisions = s_showBoxes = s_ownCollisions = false;
            s_tracked.clear();
            s_selected.clear();
        }
        // Show the result when asked (start-up, H) or when the state changes.
        if (announce || wasOk != s_handlingOk)
            showHelp(s_handlingCodes);
    }

    // True if visualisations may be used; otherwise shows the error code.
    static bool visualsAllowed() {
        if (!s_handlingOk)
            showHelp(s_handlingCodes);
        return s_handlingOk;
    }

    // Tracked vehicles as pool handles: GetAtRef() returns null once the vehicle is gone,
    // even if its pool slot has been reused by another vehicle.
    static inline std::vector<int> s_tracked;

    static inline std::vector<VehicleVis> s_vis;
    static inline std::vector<Sphere> s_spheres;
    static inline std::vector<Box> s_boxes;
    static inline std::vector<RwIm3DVertex> s_vehicleMeshLines;  // collision-mesh edges of vehicles

    static inline bool s_gDown = false;
    static inline std::vector<SelectedObject> s_selected;
    static inline std::vector<RwIm3DVertex> s_objectLines;  // triangle edges of selected objects
    static inline std::vector<Sphere> s_objectSpheres;
    static inline std::vector<Box> s_objectBoxes;
    static constexpr size_t MAX_OBJECT_TRIANGLES = 20000;  // per object, keeps big meshes affordable

    static void toggleObjectSelection() {
        CPed* player = FindPlayerPed();
        CVehicle* v = playerVehicle();
        if (!player)
            return;

        std::vector<CEntity*> touched;
        if (v)
            touchedEntities(v, v->m_pEntityWeAreOn, touched);
        else
            touchedEntities(player, nullptr, touched);

        if (touched.empty()) {
            s_selected.clear();
            showHelp("Object selection cleared");
            return;
        }

        // If everything touched is already selected, deselect it; otherwise add the new ones.
        auto isSelected = [](CEntity* e) {
            return std::any_of(s_selected.begin(), s_selected.end(),
                [e](const SelectedObject& s) { return s.entity == e; });
        };
        const bool allSelected = std::all_of(touched.begin(), touched.end(), isSelected);

        int changed = 0;
        for (CEntity* e : touched) {
            if (allSelected) {
                s_selected.erase(std::remove_if(s_selected.begin(), s_selected.end(),
                    [e](const SelectedObject& s) { return s.entity == e; }), s_selected.end());
                ++changed;
            }
            else if (!isSelected(e)) {
                s_selected.push_back({ e, e->m_nModelIndex, e->m_nType == ENTITY_TYPE_BUILDING });
                ++changed;
            }
        }

        char msg[96];
        snprintf(msg, sizeof(msg), "%s %d object(s) (%d selected)", allSelected ? "Deselected" : "Selected",
                 changed, static_cast<int>(s_selected.size()));
        showHelp(msg);
    }

    static void updateSelectedObjects() {
        s_objectLines.clear();
        s_objectSpheres.clear();
        s_objectBoxes.clear();

        std::vector<RwIm3DVertex> lines;
        std::vector<Sphere> spheres;
        std::vector<Box> boxes;
        for (SelectedObject& s : s_selected) {
            // Refresh the cache while the entity exists: buildings once, objects every frame
            // (they can move). The entity is only touched while it is still valid.
            if ((!s.cached || !s.isBuilding) && isSelectionValid(s)) {
                lines.clear();
                collectColTriangles(s.entity, lines, MAX_OBJECT_TRIANGLES);
                collectColSpheres(s.entity, spheres);
                collectColBoxes(s.entity, boxes);
                // Keep the old cache if the collision model is currently streamed out.
                if (!lines.empty() || !spheres.empty() || !boxes.empty()) {
                    s.lines.swap(lines);
                    s.spheres.swap(spheres);
                    s.boxes.swap(boxes);
                    s.cached = true;
                }
            }
            s_objectLines.insert(s_objectLines.end(), s.lines.begin(), s.lines.end());
            s_objectSpheres.insert(s_objectSpheres.end(), s.spheres.begin(), s.spheres.end());
            s_objectBoxes.insert(s_objectBoxes.end(), s.boxes.begin(), s.boxes.end());
        }
    }

    static CVehicle* nearestVehicle(const CVector& from, float maxDistance) {
        CVehicle* best = nullptr;
        float bestDist = maxDistance;
        auto* pool = CPools::ms_pVehiclePool;
        for (int i = 0; i < pool->m_nSize; ++i) {
            CVehicle* v = pool->GetAt(i);
            if (!v)
                continue;
            const float d = (v->GetPosition() - from).Magnitude();
            if (d < bestDist) {
                bestDist = d;
                best = v;
            }
        }
        return best;
    }

    static void toggleTracking() {
        CVehicle* v = playerVehicle();
        if (!v) {
            CPed* player = FindPlayerPed();
            if (player)
                v = nearestVehicle(player->GetPosition(), 20.0f);
        }
        if (!v) {
            showHelp("No vehicle nearby to track");
            return;
        }

        const int ref = CPools::ms_pVehiclePool->GetRef(v);
        auto it = std::find(s_tracked.begin(), s_tracked.end(), ref);
        std::string msg;
        if (it != s_tracked.end()) {
            s_tracked.erase(it);
            msg = "Stopped tracking vehicle";
        }
        else {
            s_tracked.push_back(ref);
            msg = "Tracking vehicle";
        }
        msg += " (" + std::to_string(s_tracked.size()) + " tracked)";
        showHelp(msg.c_str());
    }

    // Vehicles to visualise this frame; drops tracked vehicles that no longer exist.
    static void collectTargets(std::vector<CVehicle*>& targets) {
        targets.clear();
        auto* pool = CPools::ms_pVehiclePool;
        s_tracked.erase(std::remove_if(s_tracked.begin(), s_tracked.end(),
            [pool](int ref) { return pool->GetAtRef(ref) == nullptr; }), s_tracked.end());

        if (CVehicle* v = playerVehicle())
            targets.push_back(v);
        for (int ref : s_tracked) {
            CVehicle* v = pool->GetAtRef(ref);
            if (std::find(targets.begin(), targets.end(), v) == targets.end())
                targets.push_back(v);
        }
    }

    static void process() {
        // handling.cfg is loaded during game start-up, after the plugin itself is loaded,
        // so the first check runs on the first game frame rather than in the constructor.
        // It then re-runs silently every few seconds (and on H) to catch changes made later.
        if (!s_handlingChecked || justPressed('H', s_hDown)) {
            s_handlingChecked = true;
            checkHandling(true);
        }
        else if (CTimer::m_snTimeInMilliseconds - s_lastHandlingCheck > HANDLING_RECHECK_MS) {
            checkHandling(false);
        }

        // K and U toggle when released. Holding one of them and pressing + / - (main
        // keyboard or numpad) changes a setting instead: K = trajectory thickness,
        // U = collision wireframe opacity.
        {
            int delta = 0;
            if (justPressed(VK_OEM_PLUS, s_plusDown) | justPressed(VK_ADD, s_addDown))
                delta = +1;
            if (justPressed(VK_OEM_MINUS, s_minusDown) | justPressed(VK_SUBTRACT, s_subDown))
                delta = -1;

            // U+S / U+D: collision style. Edges are tracked every frame; they only act while U is held.
            const bool uHeld = (GetAsyncKeyState('U') & 0x8000) != 0;
            const bool sPressed = justPressed('S', s_sDown);
            const bool dPressed = justPressed('D', s_dDown);
            if (uHeld && (sPressed || dPressed)) {
                s_uUsedAsModifier = true;  // releasing U must not toggle the spheres
                if (visualsAllowed()) {
                    if (sPressed) {
                        g_collisionSolid = !g_collisionSolid;
                        showHelp(g_collisionSolid ? "Collision Solid" : "Collision Wireframe");
                    }
                    if (dPressed) {
                        g_collisionDepthTest = !g_collisionDepthTest;
                        showHelp(g_collisionDepthTest ? "Collision Depth Test On" : "Collision Depth Test Off");
                    }
                }
            }

            handleModifierKey('K', s_kDown, s_kUsedAsModifier, delta,
                g_trajectoryWidth, 1, MAX_TRAJECTORY_WIDTH, "Trajectory thickness %d",
                [] {
                    s_showTrajectory = !s_showTrajectory;
                    showHelp(s_showTrajectory ? "Trajectory On" : "Trajectory Off");
                });
            handleModifierKey('U', s_uDown, s_uUsedAsModifier, delta,
                g_collisionOpacity, 1, MAX_COLLISION_OPACITY, "Collision opacity %d0%%",
                [] {
                    s_showCollisions = !s_showCollisions;
                    showHelp(s_showCollisions ? "Collision Spheres On" : "Collision Spheres Off");
                });
        }
        if (justPressed('N', s_nDown) && visualsAllowed()) {
            s_showBoxes = !s_showBoxes;
            showHelp(s_showBoxes ? "Collision Boxes/Mesh On" : "Collision Boxes/Mesh Off");
        }
        if (justPressed('M', s_mDown) && visualsAllowed()) {
            s_ownCollisions = !s_ownCollisions;
            showHelp(s_ownCollisions ? "Own Vehicle Collisions On" : "Own Vehicle Collisions Off");
        }
        if (justPressed('J', s_jDown) && visualsAllowed())
            toggleTracking();

        if (justPressed('G', s_gDown) && visualsAllowed())
            toggleObjectSelection();
        updateSelectedObjects();

        s_vis.clear();
        s_spheres.clear();
        s_boxes.clear();
        s_vehicleMeshLines.clear();
        if (!s_showTrajectory && !s_showCollisions && !s_showBoxes && !s_ownCollisions)
            return;

        static std::vector<CVehicle*> targets;
        collectTargets(targets);

        std::vector<Sphere> spheres;
        std::vector<Box> boxes;
        CVehicle* own = playerVehicle();
        auto* pool = CPools::ms_pVehiclePool;
        for (CVehicle* v : targets) {
            // Tracked vehicles follow U/N; the vehicle you drive (if not tracked) follows M.
            const bool tracked = std::find(s_tracked.begin(), s_tracked.end(), pool->GetRef(v)) != s_tracked.end();
            const bool showSpheres = tracked ? s_showCollisions : (v == own && s_ownCollisions);
            const bool showBoxes   = tracked ? s_showBoxes      : (v == own && s_ownCollisions);

            if (s_showTrajectory && isAirborne(v)) {
                s_vis.emplace_back();
                computeTrajectory(v, s_vis.back().trajectory, s_vis.back().hitsGround);
            }
            // Recomputed every frame so the spheres follow the vehicle.
            if (showSpheres) {
                collectColSpheres(v, spheres);
                s_spheres.insert(s_spheres.end(), spheres.begin(), spheres.end());
            }
            if (showBoxes) {
                collectColBoxes(v, boxes);
                s_boxes.insert(s_boxes.end(), boxes.begin(), boxes.end());
                collectColTriangles(v, s_vehicleMeshLines, MAX_OBJECT_TRIANGLES);
            }
        }
    }

    static void draw() {
        if (s_vis.empty() && s_spheres.empty() && s_boxes.empty() && s_vehicleMeshLines.empty()
            && s_objectLines.empty() && s_objectSpheres.empty() && s_objectBoxes.empty())
            return;

        DebugRenderState state; // opaque, untextured lines; restored on return
        for (const VehicleVis& vis : s_vis)
            drawTrajectory(vis.trajectory, vis.hitsGround);

        // All spheres and boxes go into one batch (buffer reused between frames), so they
        // are drawn in a handful of calls instead of one call per sphere/box.
        // Collision style: optional depth test; depth is only written for fully opaque solids
        // (transparent surfaces must not hide what is behind them).
        const bool opaqueSolid = g_collisionSolid && g_collisionOpacity >= MAX_COLLISION_OPACITY;
        RwRenderStateSet(rwRENDERSTATEZTESTENABLE, (void*)(g_collisionDepthTest ? TRUE : FALSE));
        RwRenderStateSet(rwRENDERSTATEZWRITEENABLE, (void*)(g_collisionDepthTest && opaqueSolid ? TRUE : FALSE));

        static std::vector<RwIm3DVertex> batch;
        batch.clear();
        if (g_collisionSolid) {
            for (const Sphere& s : s_spheres)
                appendSolidSphere(batch, s.pos, s.radius);
            for (const Sphere& s : s_objectSpheres)
                appendSolidSphere(batch, s.pos, s.radius);
            for (const Box& b : s_boxes)
                appendSolidBox(batch, b);
            for (const Box& b : s_objectBoxes)
                appendSolidBox(batch, b);
            applyAlpha(batch, g_collisionOpacity);
            RwRenderStateSet(rwRENDERSTATECULLMODE, (void*)kSolidCullMode);
            drawTriList(batch);
            RwRenderStateSet(rwRENDERSTATECULLMODE, (void*)rwCULLMODECULLNONE);
        }
        else {
            for (const Sphere& s : s_spheres)
                appendSphere(batch, s.pos, s.radius);
            for (const Sphere& s : s_objectSpheres)
                appendSphere(batch, s.pos, s.radius);
            for (const Box& b : s_boxes)
                appendBox(batch, b);
            for (const Box& b : s_objectBoxes)
                appendBox(batch, b);
            applyAlpha(batch, g_collisionOpacity);
            drawLineList(batch);
        }

        applyAlpha(s_vehicleMeshLines, g_collisionOpacity);
        applyAlpha(s_objectLines, g_collisionOpacity);
        drawLineList(s_vehicleMeshLines);
        drawLineList(s_objectLines);
    }
} stuntVis;
