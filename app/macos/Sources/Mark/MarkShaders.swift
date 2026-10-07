// Metal shaders for the Valtz mark (MarkRenderer.swift).
//
// Kept as source and compiled when the renderer starts
// (makeLibrary(source:)): the app and the build-time icon tool both
// render the mark, and neither then depends on Xcode's separately
// downloaded offline Metal compiler.
//
// Passes: `scene` draws the ribbon mesh into two HDR targets (the lit
// color, and what glows); `down` + `blur` make a two-level bloom from the
// glow; `composite` tone-maps, lays mark and bloom over the background
// (transparent, or the app icon's charcoal body) and writes 8-bit,
// sRGB-encoded, premultiplied pixels.

enum MarkShaders {
    static let source = #"""
#include <metal_stdlib>
using namespace metal;

// ---- scene ---------------------------------------------------------------

struct SceneUniforms {
    float4x4 mvp;
    float4   anim;      // motion, phase, detail (0..1), output px per design px
    float4   hole;           // glass (0/1), flat white (0/1), corner radius, count
    float4   holes[8];       // centre xy, the strip edge's direction xy
    float4   hole_shapes[8]; // cross direction xy, half size across, along
    float4   sweep;          // a passing point light: xyz (design px, z in front), w strength
    float4   ground;         // x: 1 = the mark sits on a light ground
};

struct VertexIn {
    float4 position [[attribute(0)]];  // design px (the artwork's 1024, y down)
    float4 color    [[attribute(1)]];  // linear rgb
    float4 edge     [[attribute(2)]];  // px to edge A, to edge B, rim A, rim B
    float4 extra    [[attribute(3)]];  // film, t, v
    float4 sheen    [[attribute(4)]];  // across direction xy, curve, facing
    float4 glow     [[attribute(5)]];  // glow reach from edge A, from edge B
};

struct VertexOut {
    float4 position [[position]];
    float4 color;
    float4 edge;
    float4 extra;
    float4 sheen;
    float4 glow;
    float2 design;
};

vertex VertexOut scene_vertex(VertexIn in [[stage_in]],
                              constant SceneUniforms& u [[buffer(1)]]) {
    float2 p = in.position.xy;
    // The waltz: a slow wave travelling through the ribbon.
    const float m = u.anim.x, ph = u.anim.y;
    p.x += m * 7.0 * sin(6.2831853 * (p.y / 460.0 - ph * 0.45));
    p.y += m * 4.0 * sin(6.2831853 * (p.x / 520.0 - ph * 0.35));
    VertexOut o;
    o.position = u.mvp * float4(p, 0.0, 1.0);
    o.color = in.color;
    o.edge = in.edge;
    o.extra = in.extra;
    o.sheen = in.sheen;
    o.glow = in.glow;
    o.design = in.position.xy;
    return o;
}

static float sd_round_box(float2 p, float2 b, float r) {
    float2 q = abs(p) - b + r;
    return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;
}

// The glow inwards from an edge `x` design px away, reaching `reach`:
// warm white -> yellow -> orange -> the sheet's colour.
// `hue` 1 warms it to pink into red (the curl's underside).
static float3 edge_glow(float3 base, float x, float reach, float strength,
                        float hue) {
    if (reach <= 0.0 || strength <= 0.0) { return base; }
    const float s = x / reach;
    const float3 c0 = mix(float3(1.0, 0.86, 0.56), float3(1.0, 0.55, 0.52), hue);
    const float3 c1 = mix(float3(1.0, 0.66, 0.16), float3(0.95, 0.20, 0.30), hue);
    const float3 c2 = mix(float3(1.0, 0.30, 0.05), float3(0.60, 0.03, 0.12), hue);
    float3 g = mix(c0, c1, smoothstep(0.0, 0.30, s));
    g = mix(g, c2, smoothstep(0.25, 0.65, s));
    const float w = strength * (1.0 - smoothstep(0.45, 1.0, s));
    return mix(base, g, w);
}

struct SceneOut {
    float4 color [[color(0)]];
    float4 glow  [[color(1)]];
};

fragment SceneOut scene_fragment(VertexOut in [[stage_in]],
                                 constant SceneUniforms& u [[buffer(1)]]) {
    if (u.hole.y > 0.5) {               // the flat white mark (tiny icon)
        SceneOut f;
        f.color = float4(1.0);
        f.glow = float4(0.0);
        return f;
    }
    // Derivatives first, in uniform control flow.
    const float px_a = max(fwidth(in.edge.x), 1e-4);
    const float px_b = max(fwidth(in.edge.y), 1e-4);

    // Sprocket holes, cut out (alpha-to-coverage keeps them antialiased),
    // each with a thin luminous lip. As in the artwork they are rounded
    // PARALLELOGRAMS: two sides parallel to the strip's edge, the other
    // two along the hole's own cross direction (near horizontal low down,
    // tilting where the strip turns). Each is measured in its skewed
    // frame q = a*e + b*d.
    float d = 1e6;
    for (int i = 0; i < int(u.hole.w); ++i) {
        const float2 dir = u.holes[i].zw;
        const float2 e = u.hole_shapes[i].xy;
        const float2 q = in.design - u.holes[i].xy;
        const float det = e.x * dir.y - e.y * dir.x;
        const float a = (q.x * dir.y - q.y * dir.x) / det;
        const float b = (e.x * q.y - e.y * q.x) / det;
        d = min(d, sd_round_box(float2(a, b), u.hole_shapes[i].zw, u.hole.z));
    }
    const float daa = max(fwidth(d), 1e-3);
    const float film = in.extra.x * u.anim.z;
    const float coverage = mix(1.0, smoothstep(-daa, daa, d), film);
    const float lip = film * exp(-max(d, 0.0) / 2.2) * smoothstep(-daa, daa, d);

    // Metal. Each sheet is a strip of polished metal curving across its
    // width: its normal turns about the sheet's length by up to half its
    // curve either side of where it faces the viewer. It reflects a small
    // studio -- a soft key high on the left, a strip light on the
    // diagonal, a dark floor -- so glints run ALONG the ribbon, as on
    // anodised metal, over the artwork's own colour.
    const float theta = in.sheen.z * (in.extra.z - in.sheen.w);
    const float2 across = normalize(float2(in.sheen.x, -in.sheen.y));  // y up
    const float3 N = normalize(float3(across * sin(theta), cos(theta)));
    const float3 R = float3(0.0, 0.0, -1.0) + 2.0 * N.z * N;       // reflect -V
    const float kd = saturate(dot(R, normalize(float3(-0.45, 0.55, 0.70))));
    const float key = pow(kd, 5.0);
    const float glint = pow(kd, 28.0);
    const float strip = exp(-pow((dot(R.xy, float2(0.70, 0.70)) - 0.30) / 0.10, 2.0));
    const float ground = saturate(-R.y * 1.3);
    const float fres = pow(1.0 - saturate(N.z), 3.0);
    // The body keeps the artwork's colour (light averaging ~1, a mild
    // fall-off on faces turned away); the metal is in the reflections --
    // sharp, and tinted warm by the metal's own colour, never plain white
    // (a neutral white over saturated orange reads olive).
    const float3 base = in.color.rgb;
    const float lightness = (0.92 + 0.16 * key) * (1.0 - 0.12 * ground)
                          * (0.94 + 0.06 * N.z);
    const float3 tint = mix(base, float3(1.0, 0.82, 0.70), 0.30);
    const float3 col = base * lightness
                     + tint * (0.55 * glint + 0.50 * strip)
                     + base * fres * 0.18;

    // Edges, as the artwork paints them: never a solid band. A hairline
    // of warm white at the edge itself, and from it a glow falling off
    // through yellow and orange into the sheet's own colour (its reds
    // further in). How far it reaches is per edge (in.glow).
    const float wa = max(1.3, 0.9 * px_a);
    const float wb = max(1.3, 0.9 * px_b);
    const float hair = min(in.edge.z, 1.1) * exp(-in.edge.x / wa) +
                       min(in.edge.w, 1.1) * exp(-in.edge.y / wb);
    const bool glass = u.hole.x > 0.5;
    float3 lit = glass ? base : col;
    lit = edge_glow(lit, in.edge.x, in.glow.x, saturate(in.edge.z), in.glow.z);
    lit = edge_glow(lit, in.edge.y, in.glow.y, saturate(in.edge.w), 0.0);
    const float3 core = float3(1.0, 0.92, 0.74);
    const float3 emit = core * hair + float3(1.0, 0.80, 0.50) * lip * 0.8;

    SceneOut o;
    if (!glass) {
        o.color = float4(lit * (1.0 - 0.5 * saturate(hair)) + emit, coverage);
        // Only the hairlines glow: the artwork has no haze outside its edges.
        o.glow = float4(emit * float3(1.0, 0.70, 0.35), 1.0) * coverage;
        return o;
    }

    // Glass. The sheet's colour is its tint. Light crossing it is absorbed
    // along its path, which is longest towards the sheet's edges (where it
    // curls away) and at grazing angles -- so the glass is dense and
    // saturated there and clear in the middle, where whatever is behind
    // shows through. Its reflections are the studio's, untinted: a tight
    // key glint, a narrow strip light, a soft sky above, and the Fresnel
    // rise at grazing angles.
    const float edge_px = min(in.edge.x, in.edge.y);
    const float thick = exp(-edge_px / 18.0);
    const float graze = pow(1.0 - saturate(N.z), 1.5);
    const float density = saturate(0.25 + 0.55 * thick + 0.80 * graze);
    // Stained glass, not frosted: the colour stays saturated; the middle
    // is lighter and lets more through, the edges are deep.
    const float3 clear_tint = lit * 1.12;
    const float3 deep_tint = pow(max(lit, 0.0), float3(1.35)) * 0.92;
    float3 glass_tint = mix(clear_tint, deep_tint, density);
    float alpha = mix(0.62, 0.97, density);
    // On a light ground the white behind the glass shows through and
    // washes it out. There the glass is denser and its colour richer:
    // more saturated, a little deeper, and it lets less of the ground in.
    const float light_ground = u.ground.x;
    if (light_ground > 0.0) {
        const float l = dot(glass_tint, float3(0.2126, 0.7152, 0.0722));
        const float3 rich = max(l + (glass_tint - l) * 1.45, 0.0) * 0.93;
        glass_tint = mix(glass_tint, rich, light_ground);
        alpha = mix(alpha, mix(0.84, 0.99, density), light_ground);
    }
    // The edge optics that say "glass": just inside each edge a thin dark
    // band, where the edge bends the light away; beyond it a faint bright
    // line, light carried along inside the glass.
    const float band = exp(-pow((edge_px - 5.0) / 2.2, 2.0));
    const float pipe = exp(-pow((edge_px - 11.0) / 3.5, 2.0));
    glass_tint *= 1.0 - 0.38 * band;
    glass_tint = mix(glass_tint, float3(1.0, 0.95, 0.88), 0.22 * pipe);
    alpha = saturate(alpha + 0.25 * band);

    const float gkey = pow(kd, 70.0);
    const float gstrip = exp(-pow((dot(R.xy, float2(0.70, 0.70)) - 0.34) / 0.045, 2.0));
    const float gsky = smoothstep(0.3, 0.95, R.y) * 0.10;
    const float gfres = 0.04 + 0.96 * pow(1.0 - saturate(N.z), 5.0);
    // Soft white reflections (sky, Fresnel) are what wash glass out on a
    // light ground; the sharp highlights stay.
    const float soft = 1.0 - 0.6 * u.ground.x;
    float3 refl = float3(1.0) * (1.35 * gkey + 0.95 * gstrip)
                + float3(0.96, 0.98, 1.0) * (gsky + 0.30 * gfres) * soft;

    // A passing light (the sweep): a bright point light moving through the
    // space in front of the mark (u.sweep.xyz, design px; w its strength).
    // Reflection: where the curved glass mirrors it towards the viewer, a
    // sharp highlight, and nearest it a band of lit glass glowing in its
    // own colour -- both travel with it. Refraction: an edge facing it
    // lights up where the light enters; the light carried across the sheet
    // gathers in a bright line inside the far edge.
    if (u.sweep.w > 0.0) {
        const float sw = u.sweep.w;
        const float3 to_light = float3(u.sweep.x - in.design.x,
                                       in.design.y - u.sweep.y,   // y up
                                       u.sweep.z);
        const float3 Ls = normalize(to_light);
        const float rs = saturate(dot(R, Ls));
        const float near = exp(-pow((in.design.x - u.sweep.x) / 170.0, 2.0));
        const float2 dir = normalize(Ls.xy + float2(1e-4, 0.0));
        const float facing_a = saturate(dot(-across, dir));   // edge A faces it
        const float facing_b = saturate(dot(across, dir));    // edge B faces it
        const float enter = exp(-in.edge.x / 3.5) * facing_a +
                            exp(-in.edge.y / 3.5) * facing_b;
        const float gather = exp(-pow((in.edge.x - 10.0) / 4.0, 2.0)) * facing_b +
                             exp(-pow((in.edge.y - 10.0) / 4.0, 2.0)) * facing_a;
        const float lit_band = sw * near;
        refl += float3(1.0, 0.98, 0.94) * sw * (2.8 * pow(rs, 20.0) + 1.1 * pow(rs, 3.0) * near);
        glass_tint = mix(glass_tint, pow(max(lit, 0.0), float3(1.1)) * 1.7 + 0.12,
                         0.8 * lit_band * (0.4 + 0.6 * rs));
        glass_tint += mix(lit, float3(1.0, 0.92, 0.80), 0.4) * lit_band
                    * (1.8 * enter + 1.1 * gather);
        alpha = saturate(alpha + lit_band * (0.3 + 0.45 * (enter + gather)));
    }
    const float refl_a = saturate(max(max(refl.r, refl.g), refl.b));

    // Premultiplied: the tinted transmission, the reflections over it, and
    // the edges' hairlines (a glass edge catches the light).
    const float a = saturate(alpha + refl_a * (1.0 - alpha) + 0.8 * hair) * coverage;
    const float3 rgb = (glass_tint * alpha * (1.0 - 0.4 * saturate(hair)) + refl + emit) * coverage;
    o.color = float4(rgb, a);
    o.glow = float4(emit * float3(1.0, 0.70, 0.35) + refl * 0.25, 1.0) * coverage;
    return o;
}

// ---- full-screen passes ------------------------------------------------------

struct Quad {
    float4 position [[position]];
    float2 uv;
};

vertex Quad quad_vertex(uint id [[vertex_id]]) {
    // One triangle covering the target.
    float2 p = float2((id << 1) & 2, id & 2);
    Quad q;
    q.position = float4(p * 2.0 - 1.0, 0.0, 1.0);
    q.uv = float2(p.x, 1.0 - p.y);
    return q;
}

fragment float4 down_fragment(Quad in [[stage_in]],
                              texture2d<float> src [[texture(0)]]) {
    constexpr sampler lin(filter::linear, address::clamp_to_edge);
    const float2 d = 0.5 / float2(src.get_width(), src.get_height());
    return 0.25 * (src.sample(lin, in.uv + float2(-d.x, -d.y)) +
                   src.sample(lin, in.uv + float2( d.x, -d.y)) +
                   src.sample(lin, in.uv + float2(-d.x,  d.y)) +
                   src.sample(lin, in.uv + float2( d.x,  d.y)));
}

struct BlurUniforms {
    float2 direction;   // one texel along the blur axis
    float  sigma;       // in texels
    float  pad;
};

fragment float4 blur_fragment(Quad in [[stage_in]],
                              texture2d<float> src [[texture(0)]],
                              constant BlurUniforms& b [[buffer(0)]]) {
    constexpr sampler lin(filter::linear, address::clamp_to_edge);
    float4 acc = src.sample(lin, in.uv);
    float total = 1.0;
    for (int i = 1; i <= 12; ++i) {
        const float x = float(i) * max(b.sigma, 0.5) / 4.0;
        const float g = exp(-(x * x) / (2.0 * b.sigma * b.sigma));
        acc += g * (src.sample(lin, in.uv + b.direction * x) +
                    src.sample(lin, in.uv - b.direction * x));
        total += 2.0 * g;
    }
    return acc / total;
}

struct CompositeUniforms {
    float4 body;       // icon: half size, superellipse exponent, pixels per design unit, style (0 mark, 1 icon)
    float4 bloom;      // near weight, far weight, overall, unused
    float4 shadow;     // strength (0 = none), offset down in uv, -, -
};

static float3 tonemap(float3 c) {
    // The layers' colours are authored as they should look, so: identity
    // up to 0.8, then a soft shoulder on the brightest channel (hue kept)
    // for rims and glow.
    const float3 x = max(c, 0.0);
    const float l = max(max(x.r, x.g), x.b);
    if (l <= 0.8) { return x; }
    const float lt = 0.8 + 0.2 * (1.0 - exp(-(l - 0.8) / 0.2));
    return x * (lt / l);
}

static float3 srgb_encode(float3 c) {
    c = saturate(c);
    return select(1.055 * pow(c, 1.0 / 2.4) - 0.055, 12.92 * c, c <= 0.0031308);
}

// Superellipse |x|^n + |y|^n = a^n: the continuous-curvature squircle of
// a macOS icon body. Returns an approximate signed distance in design px.
static float squircle(float2 p, float a, float n) {
    const float2 q = abs(p) / a;
    const float f = pow(q.x, n) + pow(q.y, n) - 1.0;
    const float2 g = n / a * pow(max(q, 1e-4), n - 1.0);
    return f / max(length(g), 1e-4);
}

fragment float4 composite_fragment(Quad in [[stage_in]],
                                   texture2d<float> scene [[texture(0)]],
                                   texture2d<float> near_bloom [[texture(1)]],
                                   texture2d<float> far_bloom [[texture(2)]],
                                   texture2d<float> shadow_map [[texture(3)]],
                                   constant CompositeUniforms& u [[buffer(0)]]) {
    constexpr sampler lin(filter::linear, address::clamp_to_edge);
    const float4 m = scene.sample(lin, in.uv);             // premultiplied
    const float3 g = u.bloom.z * (u.bloom.x * near_bloom.sample(lin, in.uv).rgb +
                                  u.bloom.y * far_bloom.sample(lin, in.uv).rgb);
    const float3 mark = tonemap(m.rgb / max(m.a, 1e-4));
    const float3 glow = tonemap(g);

    float3 rgb;   // premultiplied, linear
    float a;
    if (u.body.w < 0.5) {
        // The mark alone, on nothing: the glow becomes coverage, over a
        // soft shadow (the mark's own coverage, blurred, fallen below it).
        a = saturate(max(max(glow.r, glow.g), glow.b));
        rgb = glow;
        if (u.shadow.x > 0.0) {
            const float sa = u.shadow.x *
                saturate(shadow_map.sample(lin, in.uv - float2(0.0, u.shadow.y)).a);
            a = a + sa * (1.0 - a);         // black: adds coverage, no colour
        }
    } else {
        // The app icon: charcoal body with a soft drop shadow; the glow
        // lights the body.
        const float px = u.body.z;
        const float2 p = (in.uv - 0.5) * float2(scene.get_width(), scene.get_height()) / px;
        const float half_size = u.body.x;
        const float d = squircle(p, half_size, u.body.y) * px;
        const float body = saturate(0.5 - d);
        const float ds = squircle(p - float2(0.0, 12.0), half_size, u.body.y) * px;
        const float shadow = 0.30 * exp(-pow(max(ds, 0.0) / (22.0 * px), 2.0)) * (1.0 - body);
        const float y = p.y / half_size;   // -1 top .. 1 bottom
        const float r = length(p / half_size);
        float3 bg = mix(float3(0.011, 0.011, 0.012), float3(0.004, 0.004, 0.0045),
                        saturate(0.5 + 0.5 * y));
        bg += float3(0.007, 0.005, 0.005) * saturate(1.0 - r);
        // A hairline of light along the upper rim.
        bg += float3(0.05) * smoothstep(-3.0 * px, 0.0, d) * saturate(-y);
        rgb = (bg + glow) * body;
        a = body + shadow;
    }
    rgb = mark * m.a + rgb * (1.0 - m.a);
    a = m.a + a * (1.0 - m.a);
    const float3 straight = rgb / max(a, 1e-4);
    return float4(srgb_encode(straight) * a, a);
}
"""#
}
