/*
  ------------------------------------------------------------------------------
    CoreGeometryTests.cpp

    Polygon helpers in FxmeTools/acoustics, with no JUCE and no mesh in sight:

      1. simplifyPolygon keeps the corners of a densely sampled polygon
         exactly, and reproduces a straight-edged outline with zero error.
      2. It preserves the shape of a curve to a stated tolerance.
      3. Being closed rather than open, it keeps a feature anywhere on the
         ring, including inside the chain that wraps past the end.
      4. simplifyPolygonTo honours its budget, leaves a polygon already inside
         it untouched, and never drops below a triangle.

    The wrap-around case in 3 is the one worth having: a closed-polygon
    Douglas-Peucker has no endpoints to anchor on, so it anchors on vertex 0
    and the vertex furthest from it and simplifies two chains. Getting the
    second chain's indices wrong loses detail on one side of the outline only,
    which looks like a plausible simplification rather than like a bug.

    Exit code 0 when everything passes.

    Author: Olivier Doaré, github.com/odoare
    SPDX-License-Identifier: LGPL-3.0-or-later
  ------------------------------------------------------------------------------
*/

#include <FxmeTools/acoustics/FemMesh.h>

#include <cmath>
#include <cstdio>
#include <vector>

using namespace fxme::acoustics;

static int failures = 0;
static void check (bool ok, const char* what)
{
    std::printf ("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (! ok)
        ++failures;
}

static bool hasVertex (const std::vector<Point2>& p, Point2 v)
{
    for (const auto& q : p)
        if (std::hypot (q.x - v.x, q.y - v.y) < 1.0e-9)
            return true;
    return false;
}

/** Largest distance from any original vertex to the reduced outline's edges. */
static double deviation (const std::vector<Point2>& orig, const std::vector<Point2>& red)
{
    double worst = 0.0;
    for (const auto& o : orig)
    {
        double best = 1.0e30;
        for (size_t i = 0; i < red.size(); ++i)
        {
            const auto& a = red[i];
            const auto& b = red[(i + 1) % red.size()];
            const double dx = b.x - a.x, dy = b.y - a.y;
            const double l2 = dx * dx + dy * dy;
            double u = l2 > 0.0 ? ((o.x - a.x) * dx + (o.y - a.y) * dy) / l2 : 0.0;
            u = u < 0.0 ? 0.0 : (u > 1.0 ? 1.0 : u);
            const double ex = a.x + u * dx - o.x, ey = a.y + u * dy - o.y;
            const double d = std::sqrt (ex * ex + ey * ey);
            if (d < best) best = d;
        }
        if (best > worst) worst = best;
    }
    return worst;
}

int main()
{
    // --- 1. Corners survive; straight edges are exact ----------------------
    std::printf ("\n== Corners ==\n");
    {
        const Point2 corner[4] = { { 0.1, 0.1 }, { 0.9, 0.1 }, { 0.9, 0.9 }, { 0.1, 0.9 } };
        std::vector<Point2> dense;
        for (int s = 0; s < 4; ++s)
            for (int k = 0; k < 30; ++k)
            {
                const double u = k / 30.0;
                const auto& a = corner[s];
                const auto& b = corner[(s + 1) % 4];
                dense.push_back ({ a.x + u * (b.x - a.x), a.y + u * (b.y - a.y) });
            }

        const auto r = simplifyPolygonTo (dense, 20);
        std::printf ("  square 120 -> %d vertices, deviation %.3g\n",
                     (int) r.size(), deviation (dense, r));

        int found = 0;
        for (const auto& c : corner)
            found += hasVertex (r, c) ? 1 : 0;
        check (found == 4, "all four corners survive");
        check (r.size() == 4, "and nothing else does");
        check (deviation (dense, r) < 1.0e-12,
               "a polygon of straight edges is reproduced exactly");
    }

    // --- 2. A curve is preserved to tolerance ------------------------------
    std::printf ("\n== Curves ==\n");
    {
        std::vector<Point2> ellipse;
        for (int i = 0; i < 96; ++i)
        {
            const double a = 2.0 * M_PI * i / 96.0;
            ellipse.push_back ({ 0.5 + 0.42 * std::cos (a), 0.5 + 0.35 * std::sin (a) });
        }

        const auto tight = simplifyPolygon (ellipse, 0.002);
        const auto loose = simplifyPolygon (ellipse, 0.02);
        std::printf ("  ellipse 96 -> %d at tol 0.002, %d at tol 0.02\n",
                     (int) tight.size(), (int) loose.size());
        check (tight.size() > loose.size(), "a looser tolerance keeps fewer vertices");
        check (deviation (ellipse, tight) <= 0.002 + 1.0e-9,
               "and the tolerance is actually honoured");
        check (deviation (ellipse, loose) <= 0.02 + 1.0e-9,
               "at both settings");

        const auto budget = simplifyPolygonTo (ellipse, 20);
        const double areaIn = std::abs (polygonArea (ellipse));
        const double areaOut = std::abs (polygonArea (budget));
        std::printf ("  to 20: %d vertices, area %.5f -> %.5f\n",
                     (int) budget.size(), areaIn, areaOut);
        check (std::abs (areaOut / areaIn - 1.0) < 0.05, "area preserved within 5%");
    }

    // --- 3. The wrapped chain keeps its detail -----------------------------
    std::printf ("\n== Wrap-around ==\n");
    {
        // One spiked vertex, placed three quarters of the way round so it
        // falls inside the chain that wraps past the end of the array.
        std::vector<Point2> ring;
        for (int i = 0; i < 100; ++i)
        {
            const double a = 2.0 * M_PI * i / 100.0;
            const double rad = (i == 75) ? 0.48 : 0.35;
            ring.push_back ({ 0.5 + rad * std::cos (a), 0.5 + rad * std::sin (a) });
        }

        const auto r = simplifyPolygonTo (ring, 20);
        std::printf ("  ring+spike 100 -> %d vertices\n", (int) r.size());
        check (hasVertex (r, ring[75]),
               "a feature in the wrapped-around chain is not lost");

        // And the same spike at the very start, the other edge case.
        std::vector<Point2> ring2;
        for (int i = 0; i < 100; ++i)
        {
            const double a = 2.0 * M_PI * i / 100.0;
            const double rad = (i == 1) ? 0.48 : 0.35;
            ring2.push_back ({ 0.5 + rad * std::cos (a), 0.5 + rad * std::sin (a) });
        }
        check (hasVertex (simplifyPolygonTo (ring2, 20), ring2[1]),
               "and neither is one just past vertex 0");
    }

    // --- 4. The budget, and the floor --------------------------------------
    std::printf ("\n== Budget ==\n");
    {
        std::vector<Point2> blob;
        for (int i = 0; i < 128; ++i)
        {
            const double a = 2.0 * M_PI * i / 128.0;
            const double rad = 0.34 + 0.06 * std::sin (3 * a) + 0.03 * std::cos (5 * a);
            blob.push_back ({ 0.5 + rad * std::cos (a), 0.5 + rad * std::sin (a) });
        }

        for (const int budget : { 8, 12, 20, 40 })
        {
            const auto r = simplifyPolygonTo (blob, budget);
            std::printf ("  budget %2d -> %d vertices\n", budget, (int) r.size());
            check ((int) r.size() <= budget && r.size() >= 3,
                   "the budget is honoured and a triangle is the floor");
        }

        const std::vector<Point2> quad = { { 0.1, 0.1 }, { 0.9, 0.1 },
                                           { 0.9, 0.9 }, { 0.1, 0.9 } };
        check (simplifyPolygonTo (quad, 20).size() == 4,
               "a polygon already inside its budget is untouched");
        check (simplifyPolygon (quad, 10.0).size() == 4,
               "and an absurd tolerance still cannot go below a triangle");

        const std::vector<Point2> tri = { { 0.0, 0.0 }, { 1.0, 0.0 }, { 0.0, 1.0 } };
        check (simplifyPolygonTo (tri, 3).size() == 3, "a triangle survives its own budget");
    }

    std::printf ("\n%s (%d failures)\n",
                 failures ? "TESTS FAILED" : "ALL TESTS PASSED", failures);
    return failures ? 1 : 0;
}
