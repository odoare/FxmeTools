/*
  ------------------------------------------------------------------------------
    FemMesh.cpp — see FemMesh.h for the API description.

    Author: Olivier Doaré, github.com/odoare
    SPDX-License-Identifier: LGPL-3.0-or-later
  ------------------------------------------------------------------------------
*/

#include "FemMesh.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>

namespace fxme::acoustics
{

double polygonArea (const std::vector<Point2>& polygon)
{
    const size_t n = polygon.size();
    double a = 0.0;
    for (size_t i = 0; i < n; ++i)
    {
        const auto& p = polygon[i];
        const auto& q = polygon[(i + 1) % n];
        a += p.x * q.y - q.x * p.y;
    }
    return 0.5 * a;
}

bool pointInPolygon (const std::vector<Point2>& polygon, double x, double y)
{
    const size_t n = polygon.size();
    bool inside = false;
    for (size_t i = 0, j = n - 1; i < n; j = i++)
    {
        const auto& pi = polygon[i];
        const auto& pj = polygon[j];
        if ((pi.y > y) != (pj.y > y)
             && x < (pj.x - pi.x) * (y - pi.y) / (pj.y - pi.y) + pi.x)
            inside = ! inside;
    }
    return inside;
}

namespace
{

double distToSegment (double px, double py, const Point2& a, const Point2& b)
{
    const double dx = b.x - a.x, dy = b.y - a.y;
    const double len2 = dx * dx + dy * dy;
    double t = len2 > 0.0 ? ((px - a.x) * dx + (py - a.y) * dy) / len2 : 0.0;
    t = std::clamp (t, 0.0, 1.0);
    const double ex = a.x + t * dx - px, ey = a.y + t * dy - py;
    return std::sqrt (ex * ex + ey * ey);
}

double distToPolygon (const std::vector<Point2>& polygon, double x, double y)
{
    const size_t n = polygon.size();
    double d = 1.0e30;
    for (size_t i = 0; i < n; ++i)
        d = std::min (d, distToSegment (x, y, polygon[i], polygon[(i + 1) % n]));
    return d;
}

// Deterministic tiny pseudo-random in [-0.5, 0.5] (splitmix-style hash).
double hashJitter (uint64_t k)
{
    k += 0x9e3779b97f4a7c15ull;
    k = (k ^ (k >> 30)) * 0xbf58476d1ce4e5b9ull;
    k = (k ^ (k >> 27)) * 0x94d049bb133111ebull;
    k ^= k >> 31;
    return (double) (k & 0xffffffffull) / 4294967296.0 - 0.5;
}

// A boundary sample: position + arc-length parameter of the input polygon.
struct BoundaryPoint
{
    Point2 p;
    double t = 0.0;
};

/* Resamples the outline at spacing ~h, keeping sharp corners (turn angle
   above ~30 degrees) as exact sample points so rectangles stay rectangles.
   Returned points carry the arc-length parameter t in [0,1). */
std::vector<BoundaryPoint> resampleBoundary (const std::vector<Point2>& poly, double h)
{
    const size_t n = poly.size();

    // Cumulative arc length at each polygon vertex.
    std::vector<double> cum (n + 1, 0.0);
    for (size_t i = 0; i < n; ++i)
    {
        const auto& p = poly[i];
        const auto& q = poly[(i + 1) % n];
        cum[i + 1] = cum[i] + std::hypot (q.x - p.x, q.y - p.y);
    }
    const double perimeter = cum[n];
    if (perimeter <= 0.0)
        return {};

    // Corner detection: keep vertices where the outline turns sharply.
    std::vector<size_t> corners;
    for (size_t i = 0; i < n; ++i)
    {
        const auto& a = poly[(i + n - 1) % n];
        const auto& b = poly[i];
        const auto& c = poly[(i + 1) % n];
        const double ux = b.x - a.x, uy = b.y - a.y;
        const double vx = c.x - b.x, vy = c.y - b.y;
        const double lu = std::hypot (ux, uy), lv = std::hypot (vx, vy);
        if (lu <= 0.0 || lv <= 0.0)
            continue;
        const double cosang = (ux * vx + uy * vy) / (lu * lv);
        if (cosang < 0.866)   // turn angle > 30 degrees
            corners.push_back (i);
    }
    if (corners.empty())
        corners.push_back (0);   // arbitrary anchor on a smooth outline

    // Resample each chain between consecutive corners uniformly.
    std::vector<BoundaryPoint> out;
    const size_t nc = corners.size();
    for (size_t ci = 0; ci < nc; ++ci)
    {
        const size_t i0 = corners[ci];
        const size_t i1 = corners[(ci + 1) % nc];
        const double s0 = cum[i0];
        const double s1 = i1 > i0 ? cum[i1] : cum[i1] + perimeter;   // wraps for the last chain
        const double chainLen = s1 - s0;
        const int nseg = std::max (1, (int) std::lround (chainLen / h));

        for (int k = 0; k < nseg; ++k)
        {
            const double s = std::fmod (s0 + chainLen * k / nseg, perimeter);

            // Locate s on the polygon (linear scan is fine at these sizes).
            size_t seg = 0;
            while (seg + 1 < n && cum[seg + 1] < s)
                ++seg;
            const double segLen = cum[seg + 1] - cum[seg];
            const double u = segLen > 0.0 ? (s - cum[seg]) / segLen : 0.0;
            const auto& p = poly[seg];
            const auto& q = poly[(seg + 1) % n];
            out.push_back ({ { p.x + u * (q.x - p.x), p.y + u * (q.y - p.y) },
                             s / perimeter });
        }
    }

    // Walk order must follow increasing t (chains were emitted in order,
    // but the anchor corner may not be at t=0).
    std::sort (out.begin(), out.end(),
               [] (const BoundaryPoint& a, const BoundaryPoint& b) { return a.t < b.t; });

    // Drop accidental duplicates (degenerate input polygons).
    std::vector<BoundaryPoint> dedup;
    for (const auto& bp : out)
    {
        if (! dedup.empty())
        {
            const auto& last = dedup.back().p;
            if (std::hypot (bp.p.x - last.x, bp.p.y - last.y) < 1.0e-9)
                continue;
        }
        dedup.push_back (bp);
    }
    return dedup;
}

// ---------------------------------------------------------------------------
// Bowyer-Watson Delaunay triangulation.
// ---------------------------------------------------------------------------

struct DTri
{
    int a, b, c;         // vertex indices
    double cx, cy, r2;   // circumcircle
    bool alive = true;
};

void computeCircumcircle (const std::vector<Point2>& pts, DTri& t)
{
    const auto& A = pts[(size_t) t.a];
    const auto& B = pts[(size_t) t.b];
    const auto& C = pts[(size_t) t.c];
    const double d = 2.0 * (A.x * (B.y - C.y) + B.x * (C.y - A.y) + C.x * (A.y - B.y));
    if (std::abs (d) < 1.0e-30)
    {
        t.cx = t.cy = 0.0;
        t.r2 = 1.0e60;   // degenerate: treat as containing everything
        return;
    }
    const double a2 = A.x * A.x + A.y * A.y;
    const double b2 = B.x * B.x + B.y * B.y;
    const double c2 = C.x * C.x + C.y * C.y;
    t.cx = (a2 * (B.y - C.y) + b2 * (C.y - A.y) + c2 * (A.y - B.y)) / d;
    t.cy = (a2 * (C.x - B.x) + b2 * (A.x - C.x) + c2 * (B.x - A.x)) / d;
    const double dx = A.x - t.cx, dy = A.y - t.cy;
    t.r2 = dx * dx + dy * dy;
}

std::vector<std::array<int, 3>> delaunay (const std::vector<Point2>& points)
{
    const int n = (int) points.size();
    if (n < 3)
        return {};

    // Super-triangle enclosing all points with a wide margin.
    double minx = 1e30, miny = 1e30, maxx = -1e30, maxy = -1e30;
    for (const auto& p : points)
    {
        minx = std::min (minx, p.x); maxx = std::max (maxx, p.x);
        miny = std::min (miny, p.y); maxy = std::max (maxy, p.y);
    }
    const double dmax = std::max (maxx - minx, maxy - miny) * 20.0 + 1.0;
    const double midx = 0.5 * (minx + maxx), midy = 0.5 * (miny + maxy);

    std::vector<Point2> pts = points;
    pts.push_back ({ midx - dmax, midy - dmax * 0.5 });
    pts.push_back ({ midx + dmax, midy - dmax * 0.5 });
    pts.push_back ({ midx,        midy + dmax });

    std::vector<DTri> tris;
    tris.reserve ((size_t) n * 3);
    DTri super { n, n + 1, n + 2, 0, 0, 0 };
    computeCircumcircle (pts, super);
    tris.push_back (super);

    std::vector<std::pair<int, int>> cavityEdges;
    for (int ip = 0; ip < n; ++ip)
    {
        const double px = pts[(size_t) ip].x, py = pts[(size_t) ip].y;

        // Collect the boundary edges of the cavity of "bad" triangles.
        cavityEdges.clear();
        for (auto& t : tris)
        {
            if (! t.alive)
                continue;
            const double dx = px - t.cx, dy = py - t.cy;
            if (dx * dx + dy * dy <= t.r2 * (1.0 + 1.0e-12))
            {
                t.alive = false;
                const int e[3][2] = { { t.a, t.b }, { t.b, t.c }, { t.c, t.a } };
                for (const auto& ed : e)
                {
                    // An edge shared by two bad triangles appears twice with
                    // opposite orientation and cancels out of the cavity hull.
                    bool cancelled = false;
                    for (size_t k = 0; k < cavityEdges.size(); ++k)
                        if (cavityEdges[k].first == ed[1] && cavityEdges[k].second == ed[0])
                        {
                            cavityEdges.erase (cavityEdges.begin() + (long) k);
                            cancelled = true;
                            break;
                        }
                    if (! cancelled)
                        cavityEdges.emplace_back (ed[0], ed[1]);
                }
            }
        }

        for (const auto& ed : cavityEdges)
        {
            DTri t { ed.first, ed.second, ip, 0, 0, 0 };
            computeCircumcircle (pts, t);
            tris.push_back (t);
        }

        // Compact occasionally so the dead-triangle scan stays cheap.
        if (tris.size() > (size_t) n * 8)
            tris.erase (std::remove_if (tris.begin(), tris.end(),
                                        [] (const DTri& t) { return ! t.alive; }),
                        tris.end());
    }

    std::vector<std::array<int, 3>> out;
    for (const auto& t : tris)
        if (t.alive && t.a < n && t.b < n && t.c < n)
            out.push_back ({ t.a, t.b, t.c });
    return out;
}

} // namespace

FemMesh generateMesh (const std::vector<Point2>& polygonIn, double targetH)
{
    FemMesh mesh;
    if (polygonIn.size() < 3 || targetH <= 0.0)
        return mesh;

    // Work with a CCW copy so the boundary parameter always walks CCW.
    std::vector<Point2> polygon = polygonIn;
    if (polygonArea (polygon) < 0.0)
        std::reverse (polygon.begin(), polygon.end());
    const double area = polygonArea (polygon);
    if (area < targetH * targetH * 0.5)
        return mesh;

    const double h = targetH;
    const auto boundary = resampleBoundary (polygon, h);
    if (boundary.size() < 3)
        return mesh;

    // Point set: boundary samples first (they keep their arc parameter),
    // then a jittered hexagonal interior lattice kept clear of the boundary.
    std::vector<Point2> pts;
    std::vector<double> params;
    for (const auto& bp : boundary)
    {
        pts.push_back (bp.p);
        params.push_back (bp.t);
    }

    double minx = 1e30, miny = 1e30, maxx = -1e30, maxy = -1e30;
    for (const auto& p : polygon)
    {
        minx = std::min (minx, p.x); maxx = std::max (maxx, p.x);
        miny = std::min (miny, p.y); maxy = std::max (maxy, p.y);
    }

    const double rowH = h * 0.8660254037844386;   // sqrt(3)/2
    uint64_t cell = 0;
    for (double y = miny + 0.5 * rowH; y < maxy; y += rowH)
    {
        const int row = (int) std::lround ((y - miny) / rowH);
        const double x0 = minx + ((row & 1) != 0 ? 0.75 * h : 0.25 * h);
        for (double x = x0; x < maxx; x += h, ++cell)
        {
            const double jx = x + 0.12 * h * hashJitter (cell * 2);
            const double jy = y + 0.12 * h * hashJitter (cell * 2 + 1);
            if (! pointInPolygon (polygon, jx, jy))
                continue;
            if (distToPolygon (polygon, jx, jy) < 0.62 * h)
                continue;
            pts.push_back ({ jx, jy });
            params.push_back (-1.0);
        }
    }

    auto rawTris = delaunay (pts);
    if (rawTris.empty())
        return mesh;

    // Keep triangles whose centroid is inside the polygon; enforce CCW.
    std::vector<std::array<int, 3>> kept;
    for (auto& t : rawTris)
    {
        const auto& A = pts[(size_t) t[0]];
        const auto& B = pts[(size_t) t[1]];
        const auto& C = pts[(size_t) t[2]];
        const double cx = (A.x + B.x + C.x) / 3.0;
        const double cy = (A.y + B.y + C.y) / 3.0;
        if (! pointInPolygon (polygon, cx, cy))
            continue;
        const double cross = (B.x - A.x) * (C.y - A.y) - (C.x - A.x) * (B.y - A.y);
        if (std::abs (cross) < 1.0e-14)
            continue;
        if (cross < 0.0)
            std::swap (t[1], t[2]);
        kept.push_back (t);
    }
    if (kept.empty())
        return mesh;

    // Compact the vertex list to the vertices actually referenced.
    std::vector<int> remap (pts.size(), -1);
    for (const auto& t : kept)
        for (int v : t)
            if (remap[(size_t) v] < 0)
            {
                remap[(size_t) v] = (int) mesh.vertices.size();
                mesh.vertices.push_back (pts[(size_t) v]);
                mesh.vertexParam.push_back (params[(size_t) v]);
            }
    for (auto& t : kept)
        for (auto& v : t)
            v = remap[(size_t) v];
    mesh.triangles = std::move (kept);

    // Edge table.
    std::map<std::pair<int, int>, int> edgeIndex;
    mesh.triEdges.resize (mesh.triangles.size());
    for (size_t ti = 0; ti < mesh.triangles.size(); ++ti)
    {
        const auto& t = mesh.triangles[ti];
        for (int s = 0; s < 3; ++s)
        {
            const int va = t[(size_t) s];
            const int vb = t[(size_t) ((s + 1) % 3)];
            const auto key = std::minmax (va, vb);
            auto it = edgeIndex.find (key);
            if (it == edgeIndex.end())
            {
                FemMesh::Edge e;
                e.v0 = key.first;
                e.v1 = key.second;
                e.tri0 = (int) ti;
                it = edgeIndex.emplace (key, (int) mesh.edges.size()).first;
                mesh.edges.push_back (e);
            }
            else
            {
                mesh.edges[(size_t) it->second].tri1 = (int) ti;
            }
            mesh.triEdges[ti][(size_t) s] = it->second;
        }
    }

    // Boundary-edge midpoint parameters (wrap-aware circular midpoint).
    mesh.edgeParam.assign (mesh.edges.size(), -1.0);
    for (size_t ei = 0; ei < mesh.edges.size(); ++ei)
    {
        const auto& e = mesh.edges[ei];
        if (e.tri1 >= 0)
            continue;
        const double t0 = mesh.vertexParam[(size_t) e.v0];
        const double t1 = mesh.vertexParam[(size_t) e.v1];
        if (t0 < 0.0 || t1 < 0.0)
            continue;   // boundary edge between interior points: shouldn't happen
        double d = t1 - t0;
        if (d > 0.5)  d -= 1.0;
        if (d < -0.5) d += 1.0;
        double tm = t0 + 0.5 * d;
        if (tm < 0.0) tm += 1.0;
        if (tm >= 1.0) tm -= 1.0;
        mesh.edgeParam[ei] = tm;
    }

    return mesh;
}

int findTriangle (const FemMesh& mesh, double x, double y, double bary[3])
{
    for (int ti = 0; ti < mesh.numTriangles(); ++ti)
    {
        const auto& t = mesh.triangles[(size_t) ti];
        const auto& A = mesh.vertices[(size_t) t[0]];
        const auto& B = mesh.vertices[(size_t) t[1]];
        const auto& C = mesh.vertices[(size_t) t[2]];
        const double det = (B.x - A.x) * (C.y - A.y) - (C.x - A.x) * (B.y - A.y);
        if (std::abs (det) < 1.0e-30)
            continue;
        const double l1 = ((x - A.x) * (C.y - A.y) - (C.x - A.x) * (y - A.y)) / det;
        const double l2 = ((B.x - A.x) * (y - A.y) - (x - A.x) * (B.y - A.y)) / det;
        const double l0 = 1.0 - l1 - l2;
        const double eps = -1.0e-9;
        if (l0 >= eps && l1 >= eps && l2 >= eps)
        {
            bary[0] = l0; bary[1] = l1; bary[2] = l2;
            return ti;
        }
    }
    return -1;
}

float evalNodalField (const FemMesh& mesh, const std::vector<float>& vertexValues,
                      double x, double y, int* triHint)
{
    double bary[3];
    int tri = -1;

    // Try the cached triangle first.
    if (triHint != nullptr && *triHint >= 0 && *triHint < mesh.numTriangles())
    {
        const auto& t = mesh.triangles[(size_t) *triHint];
        const auto& A = mesh.vertices[(size_t) t[0]];
        const auto& B = mesh.vertices[(size_t) t[1]];
        const auto& C = mesh.vertices[(size_t) t[2]];
        const double det = (B.x - A.x) * (C.y - A.y) - (C.x - A.x) * (B.y - A.y);
        if (std::abs (det) > 1.0e-30)
        {
            const double l1 = ((x - A.x) * (C.y - A.y) - (C.x - A.x) * (y - A.y)) / det;
            const double l2 = ((B.x - A.x) * (y - A.y) - (x - A.x) * (B.y - A.y)) / det;
            const double l0 = 1.0 - l1 - l2;
            if (l0 >= -1e-9 && l1 >= -1e-9 && l2 >= -1e-9)
            {
                bary[0] = l0; bary[1] = l1; bary[2] = l2;
                tri = *triHint;
            }
        }
    }
    if (tri < 0)
        tri = findTriangle (mesh, x, y, bary);
    if (tri < 0)
        return 0.0f;
    if (triHint != nullptr)
        *triHint = tri;

    const auto& t = mesh.triangles[(size_t) tri];
    double v = 0.0;
    for (int i = 0; i < 3; ++i)
        v += bary[i] * (double) vertexValues[(size_t) t[(size_t) i]];
    return (float) v;
}

} // namespace fxme::acoustics
