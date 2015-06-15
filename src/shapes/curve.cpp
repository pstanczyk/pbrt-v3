
/*
    pbrt source code is Copyright(c) 1998-2015
                        Matt Pharr, Greg Humphreys, and Wenzel Jakob.

    This file is part of pbrt.

    Redistribution and use in source and binary forms, with or without
    modification, are permitted provided that the following conditions are
    met:

    - Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.

    - Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.

    THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS
    IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
    TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
    PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
    HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
    SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
    LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
    DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
    THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
    (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
    OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

 */

#include "stdafx.h"

// shapes/curve.cpp*
#include "shapes/curve.h"
#include "paramset.h"
#include "stats.h"
STAT_MEMORY_COUNTER("Memory/Curves", curveBytes);
STAT_PERCENT("Intersections/Ray-curve intersection tests", nHits, nTests);

// Curve Utility Functions
static Point3f BlossomBezier(const Point3f p[4], Float u0, Float u1, Float u2) {
    Point3f a[3] = {Lerp(u0, p[0], p[1]), Lerp(u0, p[1], p[2]),
                    Lerp(u0, p[2], p[3])};
    Point3f b[2] = {Lerp(u1, a[0], a[1]), Lerp(u1, a[1], a[2])};
    return Lerp(u2, b[0], b[1]);
}

inline void SubdivideBezier(const Point3f cp[4], Point3f cpSplit[7]) {
    cpSplit[0] = cp[0];
    cpSplit[1] = (cp[0] + cp[1]) / 2;
    cpSplit[2] = (cp[0] + 2 * cp[1] + cp[2]) / 4;
    cpSplit[3] = (cp[0] + 3 * cp[1] + 3 * cp[2] + cp[3]) / 8;
    cpSplit[4] = (cp[1] + 2 * cp[2] + cp[3]) / 4;
    cpSplit[5] = (cp[2] + cp[3]) / 2;
    cpSplit[6] = cp[3];
}

static Point3f EvalBezier(const Point3f cp[4], Float t,
                          Vector3f *deriv = nullptr) {
    Point3f cp1[3] = {Lerp(t, cp[0], cp[1]), Lerp(t, cp[1], cp[2]),
                      Lerp(t, cp[2], cp[3])};
    Point3f cp2[2] = {Lerp(t, cp1[0], cp1[1]), Lerp(t, cp1[1], cp1[2])};
    if (deriv) *deriv = (Float)3. * (cp2[1] - cp2[0]);
    return Lerp(t, cp2[0], cp2[1]);
}

// Curve Method Definitions
Curve::Curve(const Transform *ObjectToWorld, const Transform *WorldToObject,
             bool ReverseOrientation,
             const std::shared_ptr<CurveCommon> &common, Float uMin, Float uMax)
    : Shape(ObjectToWorld, WorldToObject, ReverseOrientation),
      common(common),
      uMin(uMin),
      uMax(uMax) {}
CurveCommon::CurveCommon(const Point3f c[4], Float width0, Float width1,
                         CurveType curveType, const Normal3f *norm)
    : curveType(curveType),
      cpObj{c[0], c[1], c[2], c[3]},
      width{width0, width1} {
    if (norm) {
        n[0] = Normalize(norm[0]);
        n[1] = Normalize(norm[1]);
        normalAngle = std::acos(Clamp(Dot(n[0], n[1]), 0, 1));
        invSinNormalAngle = 1 / std::sin(normalAngle);
    }
}

std::vector<std::shared_ptr<Shape>> CreateCurve(
    const Transform *o2w, const Transform *w2o, bool reverseOrientation,
    const Point3f c[4], Float w0, Float w1, CurveType type,
    const Normal3f *norm, int splitDepth) {
    std::vector<std::shared_ptr<Shape>> segments;
    std::shared_ptr<CurveCommon> common =
        std::make_shared<CurveCommon>(c, w0, w1, type, norm);
    const int nSegments = 1 << splitDepth;
    segments.reserve(nSegments);
    for (int i = 0; i < nSegments; ++i) {
        Float uMin = i / (Float)nSegments;
        Float uMax = (i + 1) / (Float)nSegments;
        segments.push_back(std::make_shared<Curve>(o2w, w2o, reverseOrientation,
                                                   common, uMin, uMax));
    }
    curveBytes += sizeof(CurveCommon) + nSegments * sizeof(Curve);
    return segments;
}

Bounds3f Curve::ObjectBound() const {
    // Compute control points for curve segment
    Point3f cpObj[4];
    cpObj[0] = BlossomBezier(common->cpObj, uMin, uMin, uMin);
    cpObj[1] = BlossomBezier(common->cpObj, uMin, uMin, uMax);
    cpObj[2] = BlossomBezier(common->cpObj, uMin, uMax, uMax);
    cpObj[3] = BlossomBezier(common->cpObj, uMax, uMax, uMax);
    Bounds3f b =
        Union(Bounds3f(cpObj[0], cpObj[1]), Bounds3f(cpObj[2], cpObj[3]));
    Float width[2] = {Lerp(uMin, common->width[0], common->width[1]),
                      Lerp(uMax, common->width[0], common->width[1])};
    return Expand(b, std::max(width[0], width[1]) * 0.5f);
}

bool Curve::Intersect(const Ray &r, Float *tHit,
                      SurfaceInteraction *isect) const {
    ++nTests;
    // Transform _Ray_ to object space
    Vector3f oErr, dErr;
    Ray ray = (*WorldToObject)(r, &oErr, &dErr);

    // Compute control points for curve segment
    Point3f cpObj[4];
    cpObj[0] = BlossomBezier(common->cpObj, uMin, uMin, uMin);
    cpObj[1] = BlossomBezier(common->cpObj, uMin, uMin, uMax);
    cpObj[2] = BlossomBezier(common->cpObj, uMin, uMax, uMax);
    cpObj[3] = BlossomBezier(common->cpObj, uMax, uMax, uMax);

    // Project curve control points to plane perpendicular to ray
    Vector3f dx, dy;
    CoordinateSystem(ray.d, &dx, &dy);
    Transform objectToRay = LookAt(ray.o, ray.o + ray.d, dx);
    Point3f cp[4] = {objectToRay(cpObj[0]), objectToRay(cpObj[1]),
                     objectToRay(cpObj[2]), objectToRay(cpObj[3])};

    // Compute refinement depth for curve, _maxDepth_
    Float L0 = 0;
    for (int i = 0; i < 2; ++i)
        L0 = std::max(
            L0, std::max(std::abs(cp[i].x - 2 * cp[i + 1].x + cp[i + 2].x),
                         std::abs(cp[i].y - 2 * cp[i + 1].y + cp[i + 2].y)));
    Float eps =
        std::max(common->width[0], common->width[1]) * .05f;  // width / 20
#define LOG4(x) (std::log(x) * 0.7213475108f)
    Float fr0 = LOG4(1.41421356237f * 12.f * L0 / (8.f * eps));
#undef LOG4
    int r0 = (int)std::round(fr0);
    int maxDepth = Clamp(r0, 0, 10);
    return recursiveIntersect(ray, tHit, isect, cp, Inverse(objectToRay), uMin,
                              uMax, maxDepth);
}

bool Curve::recursiveIntersect(const Ray &ray, Float *tHit,
                               SurfaceInteraction *isect, const Point3f cp[4],
                               const Transform &rayToObject, Float u0, Float u1,
                               int depth) const {
    // Try to cull curve segment versus ray
    Float maxWidth = std::max(Lerp(u0, common->width[0], common->width[1]),
                              Lerp(u1, common->width[0], common->width[1]));
    Bounds3f curveBounds =
        Union(Bounds3f(cp[0], cp[1]), Bounds3f(cp[2], cp[3]));
    Float rayLength = ray.d.Length();
    Float zMax = rayLength * ray.tMax;
    Bounds3f originBounds(Point3f(-0.5f * maxWidth, -0.5f * maxWidth, 0),
                          Point3f(0.5f * maxWidth, 0.5f * maxWidth, zMax));
    if (Overlaps(curveBounds, originBounds) == false) return false;
    if (depth > 0) {
        // Split curve segment into sub-segments and test for intersection
        --depth;
        Float umid = 0.5f * (u0 + u1);
        Point3f cpSplit[7];
        SubdivideBezier(cp, cpSplit);
        return (recursiveIntersect(ray, tHit, isect, &cpSplit[0], rayToObject,
                                   u0, umid, depth) ||
                recursiveIntersect(ray, tHit, isect, &cpSplit[3], rayToObject,
                                   umid, u1, depth));
    } else {
        // Intersect ray with linearized curve segment

        // Test sample point versus linearized curve
        Vector2f segmentDirection = Point2f(cp[3]) - Point2f(cp[0]);

        // Test sample point against tangent perpendicular at curve start
        Vector2f startTangent = Point2f(cp[1]) - Point2f(cp[0]);
        if (Dot(segmentDirection, startTangent) < 0)
            startTangent = -startTangent;
        if (Dot(startTangent, -Vector2f(cp[0])) < 0) return false;

        // Test sample point against tangent perpendicular at curve end
        Vector2f endTangent = Point2f(cp[2]) - Point2f(cp[3]);
        if (Dot(segmentDirection, endTangent) < 0) endTangent = -endTangent;
        if (Dot(endTangent, Vector2f(cp[3])) < 0) return false;

        // Compute line $w$ that gives minimum distance to sample point
        Float w = Dot(segmentDirection, segmentDirection);
        if (w == 0) return false;
        w = -Dot(Vector2f(cp[0]), segmentDirection) / w;

        // Compute $(u,v)$ coordinates of curve intersection point
        Float u = Clamp(Lerp(w, u0, u1), u0, u1);
        Point2f closestPt = Lerp(w, Point2f(cp[0]), Point2f(cp[3]));
        Float ptLineDist =
            std::sqrt(closestPt.x * closestPt.x + closestPt.y * closestPt.y);
        Float edgeFunc =
            segmentDirection.x * -cp[0].y + cp[0].x * segmentDirection.y;
        Float v;

        // Compute effective curve width for candidate intersection
        Float hitWidth = Lerp(u, common->width[0], common->width[1]);
        Normal3f nHit;
        if (common->curveType == CurveType::Ribbon) {
            // Scale curve width based on ribbon orientation
            Float sin0 = std::sin((1 - u) * common->normalAngle) *
                         common->invSinNormalAngle;
            Float sin1 =
                std::sin(u * common->normalAngle) * common->invSinNormalAngle;
            nHit = sin0 * common->n[0] + sin1 * common->n[1];
            hitWidth *= AbsDot(nHit, -ray.d / rayLength);
        }
        if (edgeFunc > 0.)
            v = 0.5f + ptLineDist / hitWidth;
        else
            v = 0.5f - ptLineDist / hitWidth;

        // Test intersection point against curve width
        Point3f p = EvalBezier(cp, Clamp(w, 0, 1));
        if (p.x * p.x + p.y * p.y > hitWidth * hitWidth * .25) return false;
        if (p.z < 0 || p.z > zMax) return false;

        // Compute hit _t_ and differential geometry for curve intersection
        if (tHit != nullptr) {
            *tHit = p.z / rayLength;
            // Compute error bounds for curve intersection
            Vector3f pError(2 * hitWidth, 2 * hitWidth, 2 * hitWidth);

            // Compute $\dpdu$ and $\dpdv$ for curve intersection
            Vector3f dpdu, dpdv;
            EvalBezier(common->cpObj, u, &dpdu);
            if (common->curveType == CurveType::Ribbon)
                dpdv = Normalize(Cross(nHit, dpdu)) * hitWidth;
            else {
                Vector3f dpduPlane = (Inverse(rayToObject))(dpdu);
                Vector3f dpdvPlane =
                    Normalize(Vector3f(-dpduPlane.y, dpduPlane.x, 0)) *
                    hitWidth;
                if (common->curveType == CurveType::Cylinder) {
                    Float theta = Lerp(v, -90., 90.);
                    Transform rot = Rotate(-theta, dpduPlane);
                    dpdvPlane = rot(dpdvPlane);
                }
                dpdv = rayToObject(dpdvPlane);
            }
            *isect = (*ObjectToWorld)(SurfaceInteraction(
                ray(p.z), pError, Point2f(u, v), -ray.d, dpdu, dpdv,
                Normal3f(0, 0, 0), Normal3f(0, 0, 0), ray.time, this));
        }
        ++nHits;
        return true;
    }
}

Float Curve::Area() const {
    // Compute control points for curve segment
    Point3f cpObj[4];
    cpObj[0] = BlossomBezier(common->cpObj, uMin, uMin, uMin);
    cpObj[1] = BlossomBezier(common->cpObj, uMin, uMin, uMax);
    cpObj[2] = BlossomBezier(common->cpObj, uMin, uMax, uMax);
    cpObj[3] = BlossomBezier(common->cpObj, uMax, uMax, uMax);
    Float width0 = Lerp(uMin, common->width[0], common->width[1]);
    Float width1 = Lerp(uMax, common->width[0], common->width[1]);
    Float avgWidth = (width0 + width1) * 0.5f;
    Float approxLength = 0.f;
    for (int i = 0; i < 3; ++i)
        approxLength += Distance(cpObj[i], cpObj[i + 1]);
    return approxLength * avgWidth;
}

bool Curve::Sample(const Point2f &sample, Interaction *it) const {
    Severe("Curve::Sample not implemented.");
    return false;
}

std::vector<std::shared_ptr<Shape>> CreateCurveShape(const Transform *o2w,
                                                     const Transform *w2o,
                                                     bool reverseOrientation,
                                                     const ParamSet &params) {
    Float width = params.FindOneFloat("width", 1.f);
    Float width0 = params.FindOneFloat("width0", width);
    Float width1 = params.FindOneFloat("width1", width);

    int ncp;
    const Point3f *cp = params.FindPoint3f("P", &ncp);
    if (ncp != 4) {
        Error(
            "Must provide 4 control points for \"curve\" primitive. "
            "(Provided %d).",
            ncp);
        return std::vector<std::shared_ptr<Shape>>();
    }

    CurveType type;
    std::string curveType = params.FindOneString("type", "flat");
    if (curveType == "flat")
        type = CurveType::Flat;
    else if (curveType == "ribbon")
        type = CurveType::Ribbon;
    else if (curveType == "cylinder")
        type = CurveType::Cylinder;
    else {
        Error("Unknown curve type \"%s\".  Using \"flat\".", curveType.c_str());
        type = CurveType::Cylinder;
    }
    int nnorm;
    const Normal3f *n = params.FindNormal3f("N", &nnorm);
    if (n != nullptr) {
        if (type != CurveType::Ribbon) {
            Warning("Curve normals are only used with \"ribbon\" type curves.");
            n = nullptr;
        } else if (nnorm != 2) {
            Error(
                "Must provide two normals with \"N\" parameter for ribbon "
                "curves. "
                "(Provided %d).",
                nnorm);
            return std::vector<std::shared_ptr<Shape>>();
        }
    }

    int sd = params.FindOneFloat("splitdepth", 2);

    if (type == CurveType::Ribbon && n == nullptr) {
        Error(
            "Must provide normals \"N\" at curve endpoints with ribbon "
            "curves.");
        return std::vector<std::shared_ptr<Shape>>();
    } else
        return CreateCurve(o2w, w2o, reverseOrientation, cp, width0, width1,
                           type, n, sd);
}