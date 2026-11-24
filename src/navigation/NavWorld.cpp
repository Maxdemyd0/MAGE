// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Maxdemyd0 and MAGE contributors
#include "navigation/NavWorld.h"
#include "core/Log.h"
#include "physics/PhysicsWorld.h"

#include <DetourCommon.h>
#include <DetourCrowd.h>
#include <DetourNavMesh.h>
#include <DetourNavMeshBuilder.h>
#include <DetourNavMeshQuery.h>
#include <Recast.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <thread>

namespace mage {

namespace {

using Clock = std::chrono::steady_clock;
using TileKey = std::pair<int, int>;
constexpr int kMaxTiles = 1024;
constexpr int kMaxPolys = 2048;       // per tile; 10 + 11 bits leave 11 for the salt
constexpr int kMaxAgents = 512;
constexpr float kMaxAgentRadius = 2.5f;
constexpr int kMaxPathPolys = 512;
const float kExtents[3] = {2.0f, 4.0f, 2.0f};

float RandomUnit() { return static_cast<float>(std::rand()) / (static_cast<float>(RAND_MAX) + 1.0f); }
bool Finite(const glm::vec3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

struct BuildJob {
    TileKey key;
    uint64_t version = 0;
    glm::vec2 min{0.0f};
    StaticGeometry geometry;
};

struct BuildResult {
    TileKey key;
    uint64_t version = 0;
    unsigned char* data = nullptr; // null = nothing walkable
    int size = 0;
    double ms = 0.0;
};

struct RecastObjects {
    rcHeightfield* solid = nullptr;
    rcCompactHeightfield* chf = nullptr;
    rcContourSet* contours = nullptr;
    rcPolyMesh* mesh = nullptr;
    rcPolyMeshDetail* detail = nullptr;
    ~RecastObjects() {
        rcFreeHeightField(solid);
        rcFreeCompactHeightfield(chf);
        rcFreeContourSet(contours);
        rcFreePolyMesh(mesh);
        rcFreePolyMeshDetail(detail);
    }
};

// Runs on a worker thread; touches nothing but its arguments.
BuildResult BuildTile(const BuildJob& job, const NavSettings& s) {
    BuildResult out;
    out.key = job.key;
    out.version = job.version;
    const auto start = Clock::now();
    const std::vector<glm::vec3>& tris = job.geometry.triangles;
    const int triCount = static_cast<int>(tris.size() / 3);
    if (triCount == 0) return out;

    rcConfig cfg{};
    cfg.cs = s.cellSize;
    cfg.ch = s.cellHeight;
    cfg.walkableSlopeAngle = s.agentSlopeDegrees;
    cfg.walkableHeight = static_cast<int>(std::ceil(s.agentHeight / cfg.ch));
    cfg.walkableClimb = static_cast<int>(std::floor(s.agentClimb / cfg.ch));
    cfg.walkableRadius = static_cast<int>(std::ceil(s.agentRadius / cfg.cs - 1e-4f));
    cfg.maxEdgeLen = static_cast<int>(12.0f / cfg.cs);
    cfg.maxSimplificationError = 1.3f;
    cfg.minRegionArea = 64;
    cfg.mergeRegionArea = 400;
    cfg.maxVertsPerPoly = DT_VERTS_PER_POLYGON;
    cfg.detailSampleDist = 6.0f * cfg.cs;
    cfg.detailSampleMaxError = cfg.ch;
    cfg.tileSize = static_cast<int>(std::lround(s.tileSize / cfg.cs));
    cfg.borderSize = cfg.walkableRadius + 3;
    cfg.width = cfg.height = cfg.tileSize + cfg.borderSize * 2;
    const float border = static_cast<float>(cfg.borderSize) * cfg.cs;
    const float minX = job.min.x - border, minZ = job.min.y - border;
    const float maxX = job.min.x + s.tileSize + border, maxZ = job.min.y + s.tileSize + border;
    cfg.bmin[0] = minX; cfg.bmin[2] = minZ;
    cfg.bmax[0] = maxX; cfg.bmax[2] = maxZ;
    // Vertical range: a huge triangle (the ground) may reach far outside the tile, but not far in height.
    float lo = tris[0].y, hi = tris[0].y;
    for (const glm::vec3& v : tris) {
        lo = std::min(lo, v.y);
        hi = std::max(hi, v.y);
    }
    cfg.bmin[1] = lo - 1.0f;
    cfg.bmax[1] = std::min(hi + 1.0f, cfg.bmin[1] + cfg.ch * 8000.0f); // span heights are limited to 13 bits

    rcContext ctx(false);
    RecastObjects r;
    r.solid = rcAllocHeightfield();
    if (!r.solid || !rcCreateHeightfield(&ctx, *r.solid, cfg.width, cfg.height, cfg.bmin, cfg.bmax, cfg.cs, cfg.ch)) return out;
    std::vector<unsigned char> areas(static_cast<size_t>(triCount), RC_NULL_AREA);
    std::vector<int> indices(tris.size());
    for (size_t i = 0; i < indices.size(); ++i) indices[i] = static_cast<int>(i);
    rcMarkWalkableTriangles(&ctx, cfg.walkableSlopeAngle, &tris[0].x, static_cast<int>(tris.size()), indices.data(), triCount, areas.data());
    if (!rcRasterizeTriangles(&ctx, &tris[0].x, static_cast<int>(tris.size()), indices.data(), areas.data(), triCount, *r.solid, cfg.walkableClimb)) return out;
    rcFilterLowHangingWalkableObstacles(&ctx, cfg.walkableClimb, *r.solid);
    rcFilterLedgeSpans(&ctx, cfg.walkableHeight, cfg.walkableClimb, *r.solid);
    rcFilterWalkableLowHeightSpans(&ctx, cfg.walkableHeight, *r.solid);

    r.chf = rcAllocCompactHeightfield();
    if (!r.chf || !rcBuildCompactHeightfield(&ctx, cfg.walkableHeight, cfg.walkableClimb, *r.solid, *r.chf)) return out;
    // Boxes are rasterized as hollow shells; nobody can stand inside one.
    for (const StaticGeometry::Box& b : job.geometry.boxes) {
        const float top = b.center.y + b.halfHeight - 0.15f, bottom = b.center.y - b.halfHeight - 0.2f;
        if (top <= bottom) continue;
        float quad[12];
        for (int i = 0; i < 4; ++i) {
            const glm::vec3 p = b.center + b.axisX * (i == 0 || i == 3 ? -1.0f : 1.0f) + b.axisZ * (i < 2 ? -1.0f : 1.0f);
            quad[i * 3] = p.x;
            quad[i * 3 + 1] = p.y;
            quad[i * 3 + 2] = p.z;
        }
        rcMarkConvexPolyArea(&ctx, quad, 4, bottom, top, RC_NULL_AREA, *r.chf);
    }
    if (!rcErodeWalkableArea(&ctx, cfg.walkableRadius, *r.chf)) return out;
    if (!rcBuildDistanceField(&ctx, *r.chf)) return out;
    if (!rcBuildRegions(&ctx, *r.chf, cfg.borderSize, cfg.minRegionArea, cfg.mergeRegionArea)) return out;
    r.contours = rcAllocContourSet();
    if (!r.contours || !rcBuildContours(&ctx, *r.chf, cfg.maxSimplificationError, cfg.maxEdgeLen, *r.contours)) return out;
    r.mesh = rcAllocPolyMesh();
    if (r.contours->nconts == 0 || !r.mesh || !rcBuildPolyMesh(&ctx, *r.contours, cfg.maxVertsPerPoly, *r.mesh) || r.mesh->npolys == 0) return out;
    if (r.mesh->npolys > kMaxPolys || r.mesh->nverts >= 0xffff) return out;
    r.detail = rcAllocPolyMeshDetail();
    if (!r.detail || !rcBuildPolyMeshDetail(&ctx, *r.mesh, *r.chf, cfg.detailSampleDist, cfg.detailSampleMaxError, *r.detail)) return out;
    for (int i = 0; i < r.mesh->npolys; ++i) r.mesh->flags[i] = r.mesh->areas[i] == RC_NULL_AREA ? 0 : 1;

    dtNavMeshCreateParams p{};
    p.verts = r.mesh->verts;
    p.vertCount = r.mesh->nverts;
    p.polys = r.mesh->polys;
    p.polyAreas = r.mesh->areas;
    p.polyFlags = r.mesh->flags;
    p.polyCount = r.mesh->npolys;
    p.nvp = r.mesh->nvp;
    p.detailMeshes = r.detail->meshes;
    p.detailVerts = r.detail->verts;
    p.detailVertsCount = r.detail->nverts;
    p.detailTris = r.detail->tris;
    p.detailTriCount = r.detail->ntris;
    p.walkableHeight = s.agentHeight;
    p.walkableRadius = s.agentRadius;
    p.walkableClimb = s.agentClimb;
    p.tileX = job.key.first;
    p.tileY = job.key.second;
    rcVcopy(p.bmin, r.mesh->bmin);
    rcVcopy(p.bmax, r.mesh->bmax);
    p.cs = cfg.cs;
    p.ch = cfg.ch;
    p.buildBvTree = true;
    if (!dtCreateNavMeshData(&p, &out.data, &out.size)) out.data = nullptr;
    out.ms = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    return out;
}

struct Agent {
    bool used = false;
    bool obstacle = false;
    bool paused = false;
    int crowd = -1; // dtCrowd index, -1 = not on a mesh
    float radius = 0.4f, height = 1.8f;
    glm::vec3 position{0.0f}, velocity{0.0f}, steering{0.0f};
    bool hasTarget = false, requested = false;
    glm::vec3 target{0.0f}, snapped{0.0f};
    float speed = 1.4f, stopDistance = 0.4f;
    NavAgentStatus status = NavAgentStatus::Idle;
    float retry = 0.0f;          // seconds until the next attempt
    glm::vec3 progressFrom{0.0f};
    float progressTime = 0.0f;
    int stuck = 0;
};

} // namespace

struct NavWorld::Impl {
    PhysicsWorld* physics = nullptr;
    NavSettings settings;
    dtNavMesh* mesh = nullptr;
    dtNavMeshQuery* query = nullptr;
    dtCrowd* crowd = nullptr;
    dtQueryFilter filter;

    struct Tile {
        uint64_t version = 0, built = 0; // up to date when equal
        bool queued = false;             // a build for 'version' is under way
    };
    std::map<TileKey, Tile> tiles;       // everything in the active area
    uint64_t nextVersion = 1;

    std::vector<std::thread> threads;
    mutable std::mutex mutex;
    std::condition_variable wake;
    std::deque<BuildJob> jobs;
    std::vector<BuildResult> results;
    int running = 0;
    bool quit = false;

    std::vector<Agent> agents;
    NavStats stats;

    TileKey KeyOf(float x, float z) const {
        return {static_cast<int>(std::floor((x - settings.origin.x) / settings.tileSize)), static_cast<int>(std::floor((z - settings.origin.y) / settings.tileSize))};
    }
    glm::vec2 TileMin(TileKey k) const { return settings.origin + glm::vec2(static_cast<float>(k.first), static_cast<float>(k.second)) * settings.tileSize; }
    float Border() const { return (std::ceil(settings.agentRadius / settings.cellSize) + 3.0f) * settings.cellSize; }

    void Work() {
        std::unique_lock lock(mutex);
        for (;;) {
            wake.wait(lock, [&] { return quit || !jobs.empty(); });
            if (quit) return;
            BuildJob job = std::move(jobs.front());
            jobs.pop_front();
            ++running;
            lock.unlock();
            BuildResult r = BuildTile(job, settings);
            lock.lock();
            --running;
            results.push_back(r);
        }
    }

    void RemoveTileData(TileKey k) {
        if (const dtTileRef ref = mesh->getTileRefAt(k.first, k.second, 0)) mesh->removeTile(ref, nullptr, nullptr);
    }

    bool Snap(const glm::vec3& p, dtPolyRef& ref, float* out) const {
        ref = 0;
        if (!Finite(p)) return false;
        if (dtStatusFailed(query->findNearestPoly(&p.x, kExtents, &filter, &ref, out)) || !ref) return false;
        // Detour only tests polygon bounds against the box, so the result can be far outside it.
        return glm::distance(glm::vec2(p.x, p.z), glm::vec2(out[0], out[2])) <= kExtents[0] + 0.01f && std::abs(p.y - out[1]) <= kExtents[1];
    }

    Agent* Get(NavAgentHandle h) { return h < agents.size() && agents[h].used ? &agents[h] : nullptr; }

    NavAgentHandle Add(const glm::vec3& feet, float radius, float height, bool obstacle) {
        Agent a;
        a.used = true;
        a.obstacle = obstacle;
        a.radius = std::clamp(radius, 0.1f, kMaxAgentRadius);
        a.height = std::max(height, 0.2f);
        a.position = feet;
        for (NavAgentHandle h = 0; h < agents.size(); ++h)
            if (!agents[h].used) {
                agents[h] = a;
                return h;
            }
        agents.push_back(a);
        return static_cast<NavAgentHandle>(agents.size() - 1);
    }

    void LeaveCrowd(Agent& a) {
        if (a.crowd >= 0) crowd->removeAgent(a.crowd);
        a.crowd = -1;
        a.requested = false;
        a.steering = glm::vec3(0.0f);
    }

    dtCrowdAgentParams Params(const Agent& a) const {
        dtCrowdAgentParams p{};
        p.radius = a.radius;
        p.height = a.height;
        p.maxAcceleration = 12.0f;
        p.maxSpeed = a.obstacle ? 60.0f : a.speed;
        p.collisionQueryRange = a.radius * 12.0f;
        p.pathOptimizationRange = a.radius * 30.0f;
        p.separationWeight = 1.0f;
        p.updateFlags = a.obstacle ? 0 : DT_CROWD_ANTICIPATE_TURNS | DT_CROWD_OBSTACLE_AVOIDANCE | DT_CROWD_SEPARATION | DT_CROWD_OPTIMIZE_VIS | DT_CROWD_OPTIMIZE_TOPO;
        p.obstacleAvoidanceType = 3;
        return p;
    }

    bool JoinCrowd(Agent& a) {
        dtPolyRef ref;
        float p[3];
        if (!Snap(a.position, ref, p)) return false;
        const dtCrowdAgentParams params = Params(a);
        a.crowd = crowd->addAgent(p, &params);
        a.requested = false;
        return a.crowd >= 0;
    }

    bool Request(Agent& a) {
        dtPolyRef ref;
        float p[3];
        if (a.crowd < 0 || !Snap(a.target, ref, p)) return false;
        a.snapped = glm::vec3(p[0], p[1], p[2]);
        a.requested = crowd->requestMoveTarget(a.crowd, ref, p);
        a.progressFrom = a.position;
        a.progressTime = 0.0f;
        return a.requested;
    }

    void Finish(Agent& a, NavAgentStatus status) {
        a.hasTarget = false;
        a.requested = false;
        a.status = status;
        a.steering = glm::vec3(0.0f);
        if (a.crowd >= 0) crowd->resetMoveTarget(a.crowd);
    }
};

NavWorld::NavWorld() : m(std::make_unique<Impl>()) {}
NavWorld::~NavWorld() { Shutdown(); }

bool NavWorld::Init(PhysicsWorld& physics, const NavSettings& settings) {
    Shutdown();
    m->physics = &physics;
    m->settings = settings;
    m->mesh = dtAllocNavMesh();
    m->query = dtAllocNavMeshQuery();
    m->crowd = dtAllocCrowd();
    dtNavMeshParams p{};
    p.orig[0] = settings.origin.x;
    p.orig[2] = settings.origin.y;
    p.tileWidth = p.tileHeight = settings.tileSize;
    p.maxTiles = kMaxTiles;
    p.maxPolys = kMaxPolys;
    if (!m->mesh || !m->query || !m->crowd || dtStatusFailed(m->mesh->init(&p)) || dtStatusFailed(m->query->init(m->mesh, 4096)) ||
        !m->crowd->init(kMaxAgents, kMaxAgentRadius, m->mesh)) {
        Log::Error("Navigation could not be initialized");
        Shutdown();
        return false;
    }
    m->filter.setIncludeFlags(0xffff);
    dtObstacleAvoidanceParams oa = *m->crowd->getObstacleAvoidanceParams(0);
    oa.velBias = 0.5f;
    oa.adaptiveDivs = 7;
    oa.adaptiveRings = 2;
    oa.adaptiveDepth = 3;
    m->crowd->setObstacleAvoidanceParams(3, &oa);
    m->quit = false;
    for (int i = 0; i < std::clamp(settings.workerThreads, 1, 16); ++i) m->threads.emplace_back([impl = m.get()] { impl->Work(); });
    return true;
}

void NavWorld::Shutdown() {
    {
        std::lock_guard lock(m->mutex);
        m->quit = true;
        m->jobs.clear();
    }
    m->wake.notify_all();
    for (std::thread& t : m->threads) t.join();
    m->threads.clear();
    for (BuildResult& r : m->results) dtFree(r.data);
    m->results.clear();
    dtFreeCrowd(m->crowd);
    dtFreeNavMeshQuery(m->query);
    dtFreeNavMesh(m->mesh);
    m->crowd = nullptr;
    m->query = nullptr;
    m->mesh = nullptr;
    m->tiles.clear();
    m->agents.clear();
    m->physics = nullptr;
}

void NavWorld::Update(const glm::vec3& focus) {
    if (!m->mesh || !Finite(focus)) return;
    Impl& s = *m;
    const float tile = s.settings.tileSize, radius = s.settings.activeRadius;

    // ---- Active area (with hysteresis, so that walking along a border does not thrash) -------------
    auto distance = [&](TileKey k) {
        const glm::vec2 mn = s.TileMin(k);
        return glm::distance(glm::clamp(glm::vec2(focus.x, focus.z), mn, mn + tile), glm::vec2(focus.x, focus.z));
    };
    const TileKey a = s.KeyOf(focus.x - radius, focus.z - radius), b = s.KeyOf(focus.x + radius, focus.z + radius);
    for (int x = a.first; x <= b.first; ++x)
        for (int z = a.second; z <= b.second; ++z)
            if (distance({x, z}) <= radius && !s.tiles.contains({x, z})) s.tiles[{x, z}].version = s.nextVersion++;
    for (auto it = s.tiles.begin(); it != s.tiles.end();) {
        if (distance(it->first) > radius + tile) {
            s.RemoveTileData(it->first);
            it = s.tiles.erase(it);
        } else ++it;
    }

    // ---- Changed collision ------------------------------------------------------------------------
    const float border = s.Border();
    for (const StaticChange& c : s.physics->TakeStaticChanges())
        for (auto& [key, t] : s.tiles) {
            const glm::vec2 mn = s.TileMin(key) - border, mx = s.TileMin(key) + tile + border;
            if (c.max.x < mn.x || c.min.x > mx.x || c.max.z < mn.y || c.min.z > mx.y) continue;
            t.version = s.nextVersion++;
            t.queued = false;
        }

    // ---- Finished builds ----------------------------------------------------------------------------
    std::vector<BuildResult> done;
    int inFlight = 0;
    {
        std::lock_guard lock(s.mutex);
        done.swap(s.results);
        inFlight = static_cast<int>(s.jobs.size()) + s.running;
    }
    for (BuildResult& r : done) {
        ++s.stats.builds;
        if (r.ms > 0.0) s.stats.lastBuildMs = r.ms;
        const auto it = s.tiles.find(r.key);
        if (it == s.tiles.end() || it->second.version != r.version) { // left the area or changed again meanwhile
            dtFree(r.data);
            continue;
        }
        s.RemoveTileData(r.key);
        if (r.data && dtStatusFailed(s.mesh->addTile(r.data, r.size, DT_TILE_FREE_DATA, 0, nullptr))) dtFree(r.data);
        it->second.built = r.version;
        it->second.queued = false;
    }

    // ---- New builds, nearest first ------------------------------------------------------------------
    std::vector<std::pair<float, TileKey>> todo;
    s.stats.pending = 0;
    for (auto& [key, t] : s.tiles) {
        if (t.built == t.version) continue;
        ++s.stats.pending;
        if (!t.queued) todo.emplace_back(distance(key), key);
    }
    std::sort(todo.begin(), todo.end());
    const int capacity = static_cast<int>(s.threads.size()) * 2 - inFlight;
    for (int i = 0; i < std::min(capacity, static_cast<int>(todo.size())); ++i) {
        BuildJob job;
        job.key = todo[static_cast<size_t>(i)].second;
        Impl::Tile& t = s.tiles[job.key];
        job.version = t.version;
        job.min = s.TileMin(job.key);
        s.physics->CollectStatic({job.min.x - border, -1000.0f, job.min.y - border}, {job.min.x + tile + border, 2000.0f, job.min.y + tile + border}, job.geometry);
        t.queued = true;
        std::lock_guard lock(s.mutex);
        s.jobs.push_back(std::move(job));
    }
    if (capacity > 0 && !todo.empty()) s.wake.notify_all();
    s.stats.tiles = static_cast<int>(s.tiles.size());
}

void NavWorld::WaitForBuilds(const glm::vec3& focus) {
    if (!m->mesh) return;
    do {
        Update(focus);
        if (m->stats.pending) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } while (m->stats.pending);
}

bool NavWorld::IsReady(const glm::vec3& p) const {
    const auto it = m->tiles.find(m->KeyOf(p.x, p.z));
    return it != m->tiles.end() && it->second.built == it->second.version;
}

// ============================================================================ Queries
NavPathResult NavWorld::FindPath(const glm::vec3& from, const glm::vec3& to, std::vector<glm::vec3>& out) const {
    out.clear();
    dtPolyRef a, b;
    float pa[3], pb[3];
    if (!m->mesh || !m->Snap(from, a, pa) || !m->Snap(to, b, pb)) return NavPathResult::None;
    std::vector<dtPolyRef> polys(kMaxPathPolys);
    int count = 0;
    const dtStatus st = m->query->findPath(a, b, pa, pb, &m->filter, polys.data(), &count, kMaxPathPolys);
    if (dtStatusFailed(st) || count == 0) return NavPathResult::None;
    const bool complete = polys[static_cast<size_t>(count - 1)] == b;
    float end[3] = {pb[0], pb[1], pb[2]};
    if (!complete) m->query->closestPointOnPoly(polys[static_cast<size_t>(count - 1)], pb, end, nullptr);
    std::vector<glm::vec3> points(256);
    int n = 0;
    if (dtStatusFailed(m->query->findStraightPath(pa, end, polys.data(), count, &points[0].x, nullptr, nullptr, &n, 256))) return NavPathResult::None;
    out.assign(points.begin(), points.begin() + n);
    return complete ? NavPathResult::Complete : NavPathResult::Partial;
}

bool NavWorld::NearestPoint(const glm::vec3& p, glm::vec3& out) const {
    dtPolyRef ref;
    float q[3];
    if (!m->mesh || !m->Snap(p, ref, q)) return false;
    out = glm::vec3(q[0], q[1], q[2]);
    return true;
}

bool NavWorld::RandomPoint(const glm::vec3& center, float radius, glm::vec3& out) const {
    dtPolyRef ref, found = 0;
    float p[3], q[3];
    if (!m->mesh || !m->Snap(center, ref, p) || !(radius > 0.0f)) return false;
    // Detour picks a polygon touching the circle; large ones reach beyond it, so try a few times.
    for (int i = 0; i < 12; ++i) {
        if (dtStatusFailed(m->query->findRandomPointAroundCircle(ref, p, radius, &m->filter, RandomUnit, &found, q)) || !found) return false;
        out = glm::vec3(q[0], q[1], q[2]);
        if (glm::distance(glm::vec2(out.x, out.z), glm::vec2(p[0], p[2])) <= radius) return true;
    }
    // Pull the last one onto the circle, along the mesh.
    const glm::vec2 c(p[0], p[2]), d = glm::vec2(out.x, out.z) - c;
    const glm::vec2 t = c + d * (radius / glm::length(d));
    const float target[3] = {t.x, out.y, t.y};
    dtPolyRef visited[64];
    int count = 0;
    if (dtStatusFailed(m->query->moveAlongSurface(ref, p, target, &m->filter, q, visited, &count, 64)) || count == 0) return false;
    float h = q[1];
    m->query->getPolyHeight(visited[count - 1], q, &h);
    out = glm::vec3(q[0], h, q[2]);
    return true;
}

bool NavWorld::Raycast(const glm::vec3& from, const glm::vec3& to, glm::vec3& hit) const {
    hit = to;
    dtPolyRef ref;
    float p[3];
    if (!m->mesh || !m->Snap(from, ref, p) || !Finite(to)) return false;
    float t = 0.0f, normal[3];
    dtPolyRef path[128];
    int count = 0;
    if (dtStatusFailed(m->query->raycast(ref, p, &to.x, &m->filter, &t, normal, path, &count, 128)) || t >= 1.0f) return false;
    hit = glm::mix(glm::vec3(p[0], p[1], p[2]), to, t);
    return true;
}

// ============================================================================ Agents
NavAgentHandle NavWorld::AddAgent(const glm::vec3& feet, float radius, float height) { return m->Add(feet, radius, height, false); }
NavAgentHandle NavWorld::AddObstacle(const glm::vec3& feet, float radius, float height) { return m->Add(feet, radius, height, true); }

void NavWorld::RemoveAgent(NavAgentHandle h) {
    if (Agent* a = m->Get(h)) {
        if (m->crowd) m->LeaveCrowd(*a);
        *a = Agent{};
    }
}

void NavWorld::SetAgentBody(NavAgentHandle h, const glm::vec3& feet, const glm::vec3& velocity) {
    if (Agent* a = m->Get(h); a && Finite(feet) && Finite(velocity)) {
        a->position = feet;
        a->velocity = glm::vec3(velocity.x, 0.0f, velocity.z);
    }
}

bool NavWorld::MoveAgent(NavAgentHandle h, const glm::vec3& target, float speed, float stopDistance) {
    Agent* a = m->Get(h);
    if (!a || a->obstacle || !Finite(target) || !std::isfinite(speed) || !std::isfinite(stopDistance)) return false;
    a->target = target;
    a->speed = std::clamp(speed, 0.1f, 20.0f);
    a->stopDistance = std::max(stopDistance, 0.15f);
    a->hasTarget = true;
    a->requested = false;
    a->stuck = 0;
    a->retry = 0.0f;
    a->status = NavAgentStatus::Moving;
    return true;
}

void NavWorld::StopAgent(NavAgentHandle h) {
    if (Agent* a = m->Get(h); a && m->crowd) m->Finish(*a, NavAgentStatus::Idle);
}

void NavWorld::PauseAgent(NavAgentHandle h, bool paused) {
    if (Agent* a = m->Get(h)) a->paused = paused;
}

NavAgentStatus NavWorld::AgentStatus(NavAgentHandle h) const {
    const Agent* a = m->Get(h);
    return a ? a->status : NavAgentStatus::Idle;
}

glm::vec3 NavWorld::AgentVelocity(NavAgentHandle h) const {
    const Agent* a = m->Get(h);
    return a ? a->steering : glm::vec3(0.0f);
}

void NavWorld::UpdateAgents(float dt) {
    if (!m->crowd || dt <= 0.0f) return;
    Impl& s = *m;
    s.stats.agents = 0;
    for (Agent& a : s.agents) {
        if (!a.used) continue;
        ++s.stats.agents;
        a.retry -= dt;

        // ---- Keep the crowd's copy where the body really is ---------------------------------------
        if (a.crowd >= 0) {
            dtCrowdAgent* ag = s.crowd->getEditableAgent(a.crowd);
            const bool lost = ag->state != DT_CROWDAGENT_STATE_WALKING || !s.query->isValidPolyRef(ag->corridor.getFirstPoly(), &s.filter);
            bool rejoin = lost;
            if (!lost) {
                const float p[3] = {a.position.x, a.position.y, a.position.z};
                ag->corridor.movePosition(p, s.query, &s.filter);
                dtVcopy(ag->npos, ag->corridor.getPos());
                // Height over the polygon's plane differs from the body's; only horizontal error counts.
                rejoin = dtVdist2D(ag->npos, p) > 1.0f;
            }
            if (rejoin) {
                s.LeaveCrowd(a);
                a.retry = 0.0f;
            }
        }
        if (a.crowd < 0 && a.retry <= 0.0f && !s.JoinCrowd(a)) {
            a.retry = 0.5f;
            if (a.hasTarget) a.status = NavAgentStatus::Waiting;
        }
        if (a.crowd < 0) {
            a.steering = glm::vec3(0.0f);
            continue;
        }
        dtCrowdAgent* ag = s.crowd->getEditableAgent(a.crowd);
        if (a.obstacle) {
            dtVcopy(ag->vel, &a.velocity.x);
            dtVcopy(ag->dvel, &a.velocity.x);
            dtVcopy(ag->nvel, &a.velocity.x);
            continue;
        }

        // ---- Target --------------------------------------------------------------------------------
        if (!a.hasTarget) continue;
        if (!a.requested && a.retry <= 0.0f) {
            dtCrowdAgentParams p = s.Params(a);
            s.crowd->updateAgentParameters(a.crowd, &p);
            if (s.Request(a)) a.status = NavAgentStatus::Moving;
            else {
                a.status = NavAgentStatus::Waiting; // the target's tile may still be streaming in
                a.retry = 0.5f;
                a.steering = glm::vec3(0.0f);
            }
        }
        if (!a.requested || a.paused) continue;
        const float left = glm::distance(glm::vec2(a.position.x, a.position.z), glm::vec2(a.snapped.x, a.snapped.z));
        if (left <= a.stopDistance && std::abs(a.position.y - a.snapped.y) < 1.5f) {
            s.Finish(a, NavAgentStatus::Arrived);
            continue;
        }
        if (ag->targetState == DT_CROWDAGENT_TARGET_FAILED) {
            s.Finish(a, NavAgentStatus::Blocked);
            continue;
        }
        // No progress: plan again, and give up after a while.
        a.progressTime += dt;
        if (a.progressTime > 2.0f) {
            const bool moved = glm::distance(a.progressFrom, a.position) > 0.4f;
            a.progressFrom = a.position;
            a.progressTime = 0.0f;
            if (!moved) {
                const bool end = ag->partial && ag->targetState == DT_CROWDAGENT_TARGET_VALID;
                if (end || ++a.stuck >= 3) {
                    s.Finish(a, NavAgentStatus::Blocked);
                    continue;
                }
                a.requested = false;
            } else a.stuck = 0;
        }
    }

    s.crowd->update(dt, nullptr);

    for (Agent& a : s.agents) {
        if (!a.used || a.obstacle || a.crowd < 0) continue;
        const dtCrowdAgent* ag = s.crowd->getAgent(a.crowd);
        a.steering = a.hasTarget && a.requested && !a.paused ? glm::vec3(ag->vel[0], 0.0f, ag->vel[2]) : glm::vec3(0.0f);
        if (!Finite(a.steering)) a.steering = glm::vec3(0.0f);
    }
}

// ============================================================================ Debug
void NavWorld::DebugEdges(const glm::vec3& center, float radius, std::vector<glm::vec3>& out) const {
    if (!m->mesh) return;
    const dtNavMesh& mesh = *m->mesh;
    for (int t = 0; t < mesh.getMaxTiles(); ++t) {
        const dtMeshTile* tile = mesh.getTile(t);
        if (!tile->header) continue;
        for (int i = 0; i < tile->header->polyCount; ++i) {
            const dtPoly& poly = tile->polys[i];
            for (int j = 0; j < poly.vertCount; ++j) {
                const float* a = &tile->verts[poly.verts[j] * 3];
                const float* b = &tile->verts[poly.verts[(j + 1) % poly.vertCount] * 3];
                const glm::vec3 pa(a[0], a[1], a[2]), pb(b[0], b[1], b[2]);
                if (glm::distance((pa + pb) * 0.5f, center) > radius + glm::distance(pa, pb) * 0.5f) continue;
                out.push_back(pa);
                out.push_back(pb);
            }
        }
    }
}

NavStats NavWorld::Stats() const { return m->stats; }
const NavSettings& NavWorld::Settings() const { return m->settings; }

} // namespace mage
