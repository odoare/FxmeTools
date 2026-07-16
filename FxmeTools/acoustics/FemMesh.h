/*
  ------------------------------------------------------------------------------
    FemMesh.h

    Unstructured triangular mesh generation inside an arbitrary closed 2D
    polygon, for the finite-element plate solver (PlateModes.h) and the
    contour display (FemViewComponent.h).

    Pure C++17, no JUCE dependency — usable from console tests and other
    projects. All types live in namespace fxme::acoustics.

    Typical use:

        std::vector<Point2> outline = ...;      // closed polygon, any units,
                                                //  last point != first point
        FemMesh mesh = generateMesh (outline, 0.07);   // target edge length

        double bary[3];
        int tri = findTriangle (mesh, x, y, bary);     // point location
        float v = evalNodalField (mesh, nodalValues, x, y);

    The mesh generator:
      * resamples the outline at ~targetH spacing, preserving sharp corners,
      * fills the interior with a slightly jittered hexagonal point lattice,
      * triangulates everything with Bowyer-Watson Delaunay,
      * keeps the triangles whose centroid lies inside the polygon.

    Every boundary vertex/edge carries the arc-length parameter t in [0,1)
    of the *input polygon* (0 = first outline point, walking the outline).
    This is how callers attach per-segment boundary conditions: see
    BoundarySpec in PlateModes.h.

    Author: Olivier Doaré, github.com/odoare
    SPDX-License-Identifier: LGPL-3.0-or-later
  ------------------------------------------------------------------------------
*/

#pragma once

#include <array>
#include <vector>

namespace fxme::acoustics
{

struct Point2
{
    double x = 0.0, y = 0.0;
};

/** Triangular mesh with boundary parameterisation. Triangles are CCW.
    An edge is a boundary edge iff it has a single adjacent triangle
    (tri1 < 0); boundary vertices/edges carry the arc-length parameter of
    the generating outline, interior ones carry -1. */
struct FemMesh
{
    struct Edge
    {
        int v0 = -1, v1 = -1;      // v0 < v1 (global orientation convention)
        int tri0 = -1, tri1 = -1;  // adjacent triangles (tri1 < 0 on boundary)
    };

    std::vector<Point2> vertices;
    std::vector<std::array<int, 3>> triangles;   // vertex indices, CCW
    std::vector<std::array<int, 3>> triEdges;    // edge indices, side i = (vi, vi+1)
    std::vector<Edge> edges;

    std::vector<double> vertexParam;   // per vertex: boundary arc param, -1 interior
    std::vector<double> edgeParam;     // per edge: midpoint arc param, -1 interior

    bool isBoundaryEdge (int e) const noexcept { return edges[(size_t) e].tri1 < 0; }
    bool empty() const noexcept                { return triangles.empty(); }
    int numVertices() const noexcept           { return (int) vertices.size(); }
    int numTriangles() const noexcept          { return (int) triangles.size(); }
    int numEdges() const noexcept              { return (int) edges.size(); }
};

/** Signed area of a closed polygon (positive when CCW). */
double polygonArea (const std::vector<Point2>& polygon);

/** Point-in-polygon test (even-odd rule). */
bool pointInPolygon (const std::vector<Point2>& polygon, double x, double y);

/** Generates a triangular mesh filling `polygon` (closed, arbitrary
    orientation, coordinates expected O(1), e.g. inside the unit square).
    `targetH` is the desired element edge length in the same units.
    Returns an empty mesh when the polygon is degenerate (fewer than 3
    points or ~zero area). */
FemMesh generateMesh (const std::vector<Point2>& polygon, double targetH);

/** Locates the triangle containing (x, y); fills bary[3] with its barycentric
    coordinates in that triangle (order matches mesh.triangles[tri]). Returns
    -1 when the point is outside the mesh (bary is then untouched). */
int findTriangle (const FemMesh& mesh, double x, double y, double bary[3]);

/** Linear interpolation of a per-vertex field at (x, y); 0 outside the mesh.
    `triHint` (optional, in/out) caches the located triangle so repeated
    evaluations at the same point (e.g. all modes of a bank) cost one point
    location only. */
float evalNodalField (const FemMesh& mesh, const std::vector<float>& vertexValues,
                      double x, double y, int* triHint = nullptr);

} // namespace fxme::acoustics
