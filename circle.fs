#version 330
in vec2 fragTexCoord;
in vec4 fragColor;
uniform sampler2D texture0;
uniform vec2 circle_center;
uniform float circle_radius;
uniform float circle_line_width;
uniform int use_circle;
out vec4 finalColor;

void main() {
    vec4 texColor = texture(texture0, fragTexCoord);
    if (use_circle == 1) {
        float d = distance(fragTexCoord, circle_center);
        if (d > circle_radius + circle_line_width) discard;
        if (d >= circle_radius) {
            finalColor = vec4(1.0, 1.0, 1.0, 1.0);
            return;
        }
    }
    finalColor = texColor * fragColor;
}