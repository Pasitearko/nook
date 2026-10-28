#version 440
// Cursor-following spotlight for a rounded card: a soft radial wash plus a brighter
// rim where the light reaches the edge. Output is premultiplied alpha.
layout(location = 0) in vec2 qt_TexCoord0;
layout(location = 0) out vec4 fragColor;
layout(std140, binding = 0) uniform buf {
    mat4 qt_Matrix;
    float qt_Opacity;
    vec2 center;        // pointer position in item pixels
    vec2 size;          // item size in pixels
    float radius;       // light falloff radius in pixels
    float cornerRadius; // card corner radius in pixels
    float strength;     // fill intensity, 0..1
    vec4 tint;
};

float roundedBox(vec2 p, vec2 halfSize, float r) {
    vec2 q = abs(p) - (halfSize - vec2(r));
    return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;
}

void main() {
    vec2 p = qt_TexCoord0 * size;
    float d = distance(p, center);
    float light = 1.0 - smoothstep(0.0, radius, d);
    light *= light;
    float edge = roundedBox(p - size * 0.5, size * 0.5, cornerRadius);
    float inside = clamp(0.5 - edge, 0.0, 1.0);
    float rim = (1.0 - smoothstep(0.0, 1.6, abs(edge + 0.8))) * (1.0 - smoothstep(0.0, radius * 1.3, d));
    float alpha = clamp(tint.a * (light * strength + rim * 0.85), 0.0, 1.0) * inside * qt_Opacity;
    fragColor = vec4(tint.rgb * alpha, alpha);
}
