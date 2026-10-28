#version 440
// Recolours a monochrome icon: keeps the SVG's antialiased coverage (alpha) and
// replaces its colour, so one artwork serves every theme and button state.
layout(location = 0) in vec2 qt_TexCoord0;
layout(location = 0) out vec4 fragColor;
layout(std140, binding = 0) uniform buf {
    mat4 qt_Matrix;
    float qt_Opacity;
    vec4 tint;
};
layout(binding = 1) uniform sampler2D source;

void main() {
    float coverage = texture(source, qt_TexCoord0).a * tint.a * qt_Opacity;
    fragColor = vec4(tint.rgb * coverage, coverage);
}
