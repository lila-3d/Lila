//! 2D signed distances (negative inside) and the operations that combine them. Distances are in uv units.

float2 sdfRotate(float2 p, float degrees)
{
    float a = radians(degrees);
    float c = cos(a);
    float s = sin(a);
    return float2(c * p.x + s * p.y, -s * p.x + c * p.y);
}

float sdfCircle(float2 p, float r) { return length(p) - r; }

float sdfBox(float2 p, float2 halfSize, float corner)
{
    float2 d = abs(p) - halfSize + corner;
    return length(max(d, float2(0.0, 0.0))) + min(max(d.x, d.y), 0.0) - corner;
}

float sdfSegment(float2 p, float2 a, float2 b)
{
    float2 pa = p - a;
    float2 ba = b - a;
    float h = saturate(dot(pa, ba) / max(dot(ba, ba), 1e-12));
    return length(pa - ba * h);
}

/** p folded into the half sector next to +y of an n-fold symmetric shape: x >= 0, angle from +y at most pi / n. */
float2 sdfFold(float2 p, int n)
{
    float sector = 6.28318530718 / float(max(n, 1));
    float a = atan2(p.x, p.y);
    float k = floor(a / sector + 0.5);
    float2 q = sdfRotate(p, -degrees(k * sector));
    return float2(abs(q.x), q.y);
}

/** Regular polygon with n sides and inradius r, a flat side facing +y. */
float sdfPolygon(float2 p, float r, int n)
{
    int sides = max(n, 3);
    float2 q = sdfFold(p, sides);
    float halfEdge = r * tan(3.14159265359 / float(sides));
    if (q.y < r)
    {
        if (q.x <= halfEdge)
            return q.y - r;
    }
    return length(q - float2(min(q.x, halfEdge), r));
}

/** Star with n points at radius r, its valleys at r * inner, a point facing +y. */
float sdfStar(float2 p, float r, float inner, int n)
{
    int points = max(n, 2);
    float2 q = sdfFold(p, points);
    float h = 3.14159265359 / float(points);
    float2 tip = float2(0.0, r);
    float2 valley = float2(sin(h), cos(h)) * (r * inner);
    float d = sdfSegment(q, tip, valley);
    float2 e = valley - tip;
    float2 w = q - tip;
    return (e.x * w.y - e.y * w.x) < 0.0 ? -d : d;
}

float sdfSmoothUnion(float a, float b, float k)
{
    if (k <= 0.0)
        return min(a, b);
    float h = saturate(0.5 + 0.5 * (b - a) / k);
    return lerp(b, a, h) - k * h * (1.0 - h);
}

float sdfSmoothIntersect(float a, float b, float k) { return -sdfSmoothUnion(-a, -b, k); }

/** 0 union, 1 subtract (a minus b), 2 intersect, 3 exclusive or; k rounds the seams. */
float sdfBoolean(int op, float a, float b, float k)
{
    if (op == 1)
        return sdfSmoothIntersect(a, -b, k);
    if (op == 2)
        return sdfSmoothIntersect(a, b, k);
    if (op == 3)
        return max(min(a, b), -max(a, b));
    return sdfSmoothUnion(a, b, k);
}

/** 1 inside, 0 outside, with an antialiased edge `soft` wide (uv). */
float sdfFill(float d, float soft) { return saturate(0.5 - d / max(soft, 1e-6)); }
