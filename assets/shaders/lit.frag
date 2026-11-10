#version 410 core
in vec3 vWorldPosition;
in vec3 vNormal;
in vec2 vUv;
out vec4 FragColor;
uniform sampler2D uAlbedo;
uniform int uHasTexture;
uniform vec4 uBaseColor;
uniform vec3 uSpecularColor;
uniform float uShininess;
uniform vec3 uCameraPosition;
uniform vec3 uDirectionalDirection;
uniform vec3 uDirectionalColor;
uniform float uDirectionalIntensity;
uniform int uPointLightCount;
struct PointLight { vec3 position; vec3 color; float intensity; float range; };
uniform PointLight uPointLights[8];

void main() {
    vec3 albedo = uBaseColor.rgb;
    if (uHasTexture != 0) {
        albedo *= texture(uAlbedo, vUv).rgb;
    }
    vec3 normal = normalize(vNormal);
    vec3 viewDirection = normalize(uCameraPosition - vWorldPosition);
    vec3 color = albedo * 0.16;

    vec3 lightDirection = normalize(-uDirectionalDirection);
    float diffuse = max(dot(normal, lightDirection), 0.0);
    vec3 halfway = normalize(lightDirection + viewDirection);
    float specular = pow(max(dot(normal, halfway), 0.0), max(uShininess, 1.0));
    color += uDirectionalColor * uDirectionalIntensity *
             (albedo * diffuse + uSpecularColor * specular);

    for (int index = 0; index < uPointLightCount; ++index) {
        vec3 delta = uPointLights[index].position - vWorldPosition;
        float distance = length(delta);
        if (distance > uPointLights[index].range || distance < 0.0001) {
            continue;
        }
        vec3 direction = delta / distance;
        float attenuation = 1.0 - clamp(distance / uPointLights[index].range, 0.0, 1.0);
        attenuation *= attenuation;
        float pointDiffuse = max(dot(normal, direction), 0.0);
        vec3 pointHalf = normalize(direction + viewDirection);
        float pointSpecular = pow(max(dot(normal, pointHalf), 0.0), max(uShininess, 1.0));
        color += uPointLights[index].color * uPointLights[index].intensity * attenuation *
                 (albedo * pointDiffuse + uSpecularColor * pointSpecular);
    }

    color = pow(max(color, vec3(0.0)), vec3(1.0 / 2.2));
    FragColor = vec4(color, uBaseColor.a);
}
