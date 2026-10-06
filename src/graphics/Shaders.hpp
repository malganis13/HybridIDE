// =============================================================================
//  Shaders.hpp — исходники GLSL 330 core для фоновых и пост-эффектов тем.
//  Встроены в бинарник (raw string literals); при наличии файлов в
//  resources/shaders/*.frag они загружаются вместо встроенных (горячая
//  перезагрузка через меню View -> Reload Shaders).
// =============================================================================
#pragma once

namespace ide::shaders {

// Полноэкранный треугольник без VBO (gl_VertexID)
inline constexpr const char* kFullscreenVS = R"(#version 330 core
out vec2 vUV;
void main() {
    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
    vUV = p;
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";

// ---------------------------------------------------------------------------
//  Тема 1: Аквариум — каустика, лучи света, рябь (поле высот волнового уравнения)
// ---------------------------------------------------------------------------
inline constexpr const char* kAquariumFS = R"(#version 330 core
in vec2 vUV;
out vec4 FragColor;
uniform float uTime;
uniform vec2  uRes;
uniform sampler2D uWave;      // R32F: высоты ряби
uniform sampler2D uMedia;     // пользовательский 4K-фон (опционально)
uniform int   uHasMedia;
uniform float uIntensity;     // яркость каустики
uniform float uRays;          // интенсивность лучей
uniform vec3  uDeepColor;
uniform vec3  uShallowColor;

// Каустика: итеративная суперпозиция синусоид (вариация классического
// tileable water caustic), даёт характерную сеть световых «жилок».
float caustic(vec2 uv, float t) {
    vec2 p = mod(uv * 6.2831853, 6.2831853) - 250.0;
    vec2 i = p;
    float c = 1.0;
    const float inten = 0.005;
    for (int n = 0; n < 5; n++) {
        float tt = t * (1.0 - (3.5 / float(n + 1)));
        i = p + vec2(cos(tt - i.x) + sin(tt + i.y), sin(tt - i.y) + cos(tt + i.x));
        c += 1.0 / length(vec2(p.x / (sin(i.x + tt) / inten), p.y / (cos(i.y + tt) / inten)));
    }
    c /= 5.0;
    c = 1.17 - pow(c, 1.4);
    return pow(abs(c), 8.0);
}

void main() {
    vec2 uv = vUV;
    // Нормаль ряби из градиента поля высот -> преломление UV
    vec2 texel = 1.0 / vec2(textureSize(uWave, 0));
    float hL = texture(uWave, uv - vec2(texel.x, 0)).r, hR = texture(uWave, uv + vec2(texel.x, 0)).r;
    float hD = texture(uWave, uv - vec2(0, texel.y)).r, hU = texture(uWave, uv + vec2(0, texel.y)).r;
    vec2 grad = vec2(hR - hL, hU - hD);
    vec2 ruv = uv + grad * 0.06;

    float aspect = uRes.x / max(uRes.y, 1.0);
    vec2 cuv = vec2(ruv.x * aspect, ruv.y) * 0.9;
    float t = uTime * 0.35;
    float c = caustic(cuv, t) * 0.6 + caustic(cuv * 1.7 + 3.1, t * 1.3) * 0.4;

    // Градиент глубины: светлее у поверхности (верх экрана — uv.y = 1)
    vec3 base = mix(uDeepColor, uShallowColor, smoothstep(0.0, 1.0, ruv.y));
    if (uHasMedia == 1) base = mix(base, texture(uMedia, vec2(ruv.x, 1.0 - ruv.y)).rgb, 0.85);

    // Объёмные лучи света, расходящиеся от поверхности
    float rays = 0.0;
    for (int k = 0; k < 4; k++) {
        float fk = float(k);
        float x = ruv.x + (1.0 - ruv.y) * (0.15 + 0.05 * fk) * sin(uTime * 0.1 + fk);
        rays += pow(max(0.0, sin(x * (9.0 + fk * 3.7) + uTime * (0.2 + fk * 0.07))), 18.0);
    }
    rays *= smoothstep(0.0, 1.0, ruv.y) * uRays * 0.25;

    vec3 col = base + vec3(0.55, 0.85, 1.0) * c * uIntensity * (0.3 + 0.7 * ruv.y) + vec3(0.6, 0.9, 1.0) * rays;
    // Блик на гребнях волн
    vec3 n = normalize(vec3(-grad * 18.0, 1.0));
    float spec = pow(max(dot(n, normalize(vec3(0.3, 0.5, 1.0))), 0.0), 60.0);
    col += vec3(spec) * 0.6 * step(0.0005, length(grad));
    // Частицы планктона
    vec2 g = floor(uv * vec2(120.0, 70.0));
    float h = fract(sin(dot(g, vec2(12.9898, 78.233))) * 43758.5453);
    float part = step(0.997, h) * (0.5 + 0.5 * sin(uTime * 2.0 + h * 50.0));
    col += vec3(0.6, 0.8, 1.0) * part * 0.35;
    // Виньетка
    col *= 1.0 - 0.35 * length(uv - 0.5);
    FragColor = vec4(col, 1.0);
}
)";

// ---------------------------------------------------------------------------
//  Тема 2: Steampunk — процедурные шестерни (SDF), латунь, дымка
// ---------------------------------------------------------------------------
inline constexpr const char* kSteampunkFS = R"(#version 330 core
in vec2 vUV;
out vec4 FragColor;
uniform float uTime;
uniform vec2  uRes;
uniform vec4  uGearAngles;    // текущие углы 4 шестерён (интегрируются на CPU по загрузке CPU)
uniform float uIntensity;
uniform sampler2D uMedia;
uniform int   uHasMedia;

vec3 brass(float shade) { return mix(vec3(0.35, 0.22, 0.08), vec3(0.95, 0.72, 0.35), shade); }

void main() {
    vec2 p = (vUV - 0.5) * vec2(uRes.x / uRes.y, 1.0);
    vec3 col = mix(vec3(0.07, 0.045, 0.025), vec3(0.16, 0.10, 0.05), vUV.y);
    if (uHasMedia == 1) col = mix(col, texture(uMedia, vec2(vUV.x, 1.0 - vUV.y)).rgb * 0.5, 0.8);
    // Текстура «медного листа» — шумовые разводы
    float n = fract(sin(dot(floor(vUV * uRes / 3.0), vec2(12.99, 78.23))) * 43758.5);
    col += (n - 0.5) * 0.015;

    // Зубчатая передача: соседние шестерни вращаются в противоположные стороны
    vec2 c[4]; float r[4]; float tth[4];
    c[0] = vec2(-0.55, -0.25); r[0] = 0.32; tth[0] = 18.0;
    c[1] = vec2(-0.13, 0.05);  r[1] = 0.20; tth[1] = 12.0;
    c[2] = vec2(0.62, 0.28);   r[2] = 0.40; tth[2] = 24.0;
    c[3] = vec2(0.25, -0.33);  r[3] = 0.16; tth[3] = 10.0;
    for (int i = 0; i < 4; i++) {
        vec2 q = p - c[i];
        float ang = uGearAngles[i];
        float a = atan(q.y, q.x) + ang;
        float d = length(q);
        float tooth = smoothstep(-0.2, 0.2, cos(a * tth[i])) * r[i] * 0.13;
        float body = d - (r[i] + tooth);
        // SDF шестерни: тело с зубцами минус «окна» между спицами
        float rim = abs(d - r[i] * 0.75) - r[i] * 0.07;
        float spoke = abs(sin(a * 3.0)) * d - r[i] * 0.06;
        float windows = max(max(d - r[i] * 0.62, r[i] * 0.22 - d), -spoke);
        float shape = max(body, -windows);
        float aa = 1.5 / uRes.y;
        float m = 1.0 - smoothstep(0.0, aa, shape);
        float shade = 0.5 + 0.5 * dot(normalize(q + 1e-4), normalize(vec2(-0.6, 0.8)));
        vec3 g = brass(shade) * (0.55 + 0.45 * uIntensity);
        g *= 0.8 + 0.2 * (1.0 - smoothstep(0.0, aa * 3.0, abs(rim)));
        col = mix(col, g * 0.6, m * 0.85);
    }
    // Тёплое свечение газовых ламп
    col += vec3(1.0, 0.6, 0.2) * 0.08 * (1.0 - length(vUV - vec2(0.5, 1.1)));
    col *= 1.0 - 0.45 * length(vUV - 0.5);
    FragColor = vec4(col, 1.0);
}
)";

// ---------------------------------------------------------------------------
//  Тема 3: Hacker — «матричный дождь» (фон)
// ---------------------------------------------------------------------------
inline constexpr const char* kMatrixFS = R"(#version 330 core
in vec2 vUV;
out vec4 FragColor;
uniform float uTime;
uniform vec2  uRes;
uniform float uIntensity;
uniform vec3  uTint;        // фосфор: зелёный или янтарный

float hash(vec2 p) { return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453); }

// Псевдоглиф 5x7: случайная битовая маска, меняется со временем
float glyph(vec2 cell, vec2 f, float t) {
    vec2 px = floor(f * vec2(5.0, 7.0));
    float seed = hash(cell + floor(t * (2.0 + hash(cell) * 6.0)));
    float bit = step(0.5, hash(px + seed * 37.0));
    vec2 m = step(vec2(0.1), f) * step(f, vec2(0.9));
    return bit * m.x * m.y;
}

void main() {
    vec2 cellSize = vec2(14.0, 18.0);
    vec2 frag = vUV * uRes;
    vec2 cell = floor(frag / cellSize);
    vec2 f = fract(frag / cellSize);
    float col = cell.x;
    float speed = 6.0 + hash(vec2(col, 1.0)) * 14.0;
    float offset = hash(vec2(col, 7.0)) * 100.0;
    float rows = uRes.y / cellSize.y;
    // Позиция «головы» капли; ось Y направлена вниз по экрану
    float head = mod(uTime * speed + offset, rows + 30.0);
    float y = rows - cell.y;
    float dist = head - y;
    float trail = 22.0 + hash(vec2(col, 3.0)) * 18.0;
    float b = (dist >= 0.0 && dist < trail) ? pow(1.0 - dist / trail, 1.6) : 0.0;
    float g = glyph(cell, f, uTime);
    vec3 c = uTint * g * b * uIntensity;
    c += vec3(0.85, 1.0, 0.9) * g * step(dist, 1.0) * step(0.0, dist) * uIntensity;   // яркая «голова»
    c += uTint * 0.03;                                                                     // фоновое свечение фосфора
    FragColor = vec4(c * 0.55, 1.0);
}
)";

// ---------------------------------------------------------------------------
//  Тема 4: Cyberpunk — неоновая сетка-горизонт, солнце, звёзды
// ---------------------------------------------------------------------------
inline constexpr const char* kCyberFS = R"(#version 330 core
in vec2 vUV;
out vec4 FragColor;
uniform float uTime;
uniform vec2  uRes;
uniform float uIntensity;
uniform vec3  uColorA;   // magenta
uniform vec3  uColorB;   // cyan
uniform sampler2D uMedia;
uniform int   uHasMedia;

void main() {
    vec2 uv = vUV;
    vec2 p = (uv - vec2(0.5, 0.42)) * vec2(uRes.x / uRes.y, 1.0);
    vec3 col = mix(vec3(0.02, 0.0, 0.06), vec3(0.12, 0.0, 0.18), uv.y);
    if (uHasMedia == 1) col = mix(col, texture(uMedia, vec2(uv.x, 1.0 - uv.y)).rgb * 0.6, 0.8);
    // Звёзды
    vec2 sg = floor(uv * uRes / 3.0);
    float st = fract(sin(dot(sg, vec2(12.9898, 78.233))) * 43758.5453);
    col += vec3(step(0.9985, st)) * (0.5 + 0.5 * sin(uTime * 3.0 + st * 100.0)) * step(0.42, uv.y);
    // Ретро-солнце с полосами
    float sun = length(p - vec2(0.0, 0.18));
    if (p.y > -0.02) {
        float bands = step(0.5, fract((p.y + uTime * 0.02) * 28.0)) + step(0.25, p.y);
        float m = (1.0 - smoothstep(0.24, 0.245, sun)) * clamp(bands, 0.0, 1.0);
        col = mix(col, mix(uColorA, vec3(1.0, 0.8, 0.2), clamp(p.y * 3.0, 0.0, 1.0)), m);
        col += uColorA * 0.25 * exp(-sun * 5.0);
    }
    // Перспективная сетка пола
    if (p.y < -0.02) {
        float z = 0.25 / (-p.y + 0.001);
        vec2 g = vec2(p.x * z, z + uTime * 1.6);
        vec2 gl = abs(fract(g) - 0.5) / fwidth(g);
        float line = 1.0 - min(min(gl.x, gl.y), 1.0);
        float fade = exp(-z * 0.12);
        col = mix(col, vec3(0.03, 0.0, 0.05), 0.7);
        col += mix(uColorB, uColorA, 0.5 + 0.5 * sin(uTime * 0.5)) * line * fade * 1.3;
    }
    col += uColorB * 0.08 * exp(-abs(p.y + 0.02) * 30.0);   // свечение горизонта
    FragColor = vec4(col * uIntensity, 1.0);
}
)";

// ---------------------------------------------------------------------------
//  Пост-обработка всего кадра (фон + UI)
// ---------------------------------------------------------------------------
inline constexpr const char* kPostFS = R"(#version 330 core
in vec2 vUV;
out vec4 FragColor;
uniform sampler2D uScene;
uniform vec2  uRes;
uniform float uTime;
uniform int   uMode;         // 0 — без эффектов, 1 — CRT, 2 — Cyberpunk, 3 — Steampunk (сепия)
uniform float uCurvature;    // кривизна стекла CRT
uniform float uScanlines;    // интенсивность строк развёртки
uniform float uGlow;         // фосфорное свечение
uniform vec3  uTint;         // монохромный фосфор
uniform float uMono;         // 0 — цвет, 1 — полный монохром
uniform float uAberration;   // хроматическая аберрация, пиксели
uniform float uGlitch;       // 0..1 сила глитча (импульс при смене панелей)
uniform float uSepia;

float hash(float n) { return fract(sin(n) * 43758.5453); }

vec2 curve(vec2 uv, float k) {
    uv = uv * 2.0 - 1.0;
    vec2 off = abs(uv.yx) * k;
    uv = uv + uv * off * off;
    return uv * 0.5 + 0.5;
}

void main() {
    vec2 uv = vUV;
    vec3 col;
    if (uMode == 1) {
        uv = curve(uv, uCurvature);
        if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) { FragColor = vec4(0.0, 0.0, 0.0, 1.0); return; }
        col = texture(uScene, uv).rgb;
        // Свечение фосфора: 8 выборок по кругу
        vec3 glow = vec3(0.0);
        vec2 px = 1.0 / uRes;
        for (int i = 0; i < 8; i++) {
            float a = float(i) * 0.785398;
            glow += texture(uScene, uv + vec2(cos(a), sin(a)) * px * 2.5).rgb;
        }
        col += glow / 8.0 * uGlow;
        float lum = dot(col, vec3(0.299, 0.587, 0.114));
        col = mix(col, uTint * lum * 1.4, uMono);
        float scan = 0.5 + 0.5 * sin(uv.y * uRes.y * 3.14159);
        col *= 1.0 - uScanlines * (1.0 - scan);
        col *= 0.97 + 0.03 * sin(uTime * 110.0);                 // мерцание 60 Гц
        // Апертурная решётка (RGB-триады)
        float m = mod(gl_FragCoord.x, 3.0);
        col *= mix(vec3(1.0), vec3(m < 1.0 ? 1.1 : 0.9, m >= 1.0 && m < 2.0 ? 1.1 : 0.9, m >= 2.0 ? 1.1 : 0.9), 0.3);
        vec2 vg = uv * (1.0 - uv.yx);
        col *= pow(vg.x * vg.y * 15.0, 0.25);                     // виньетка кинескопа
    } else if (uMode == 2) {
        // Глитч: смещение горизонтальных полос
        float row = floor(uv.y * 40.0);
        float t = floor(uTime * 20.0);
        float g = step(1.0 - uGlitch * 0.35, hash(row + t)) * uGlitch;
        uv.x += (hash(row * 3.1 + t) - 0.5) * 0.08 * g;
        float ab = (uAberration + g * 12.0) / uRes.x;
        col.r = texture(uScene, uv + vec2(ab, 0.0)).r;
        col.g = texture(uScene, uv).g;
        col.b = texture(uScene, uv - vec2(ab, 0.0)).b;
        // Цифровые артефакты: блоки инверсии
        vec2 blk = floor(uv * vec2(24.0, 14.0));
        if (hash(blk.x * 13.0 + blk.y * 7.0 + t) > 1.0 - g * 0.04) col = 1.0 - col;
        col *= 0.96 + 0.04 * sin(uv.y * uRes.y * 1.5);
    } else if (uMode == 3) {
        col = texture(uScene, uv).rgb;
        vec3 sep = vec3(dot(col, vec3(0.393, 0.769, 0.189)), dot(col, vec3(0.349, 0.686, 0.168)), dot(col, vec3(0.272, 0.534, 0.131)));
        col = mix(col, sep, uSepia);
        col *= 1.0 - 0.25 * length(uv - 0.5);
    } else {
        col = texture(uScene, uv).rgb;
    }
    FragColor = vec4(col, 1.0);
}
)";

// Рыбы: цветные треугольники в пиксельных координатах
inline constexpr const char* kFishVS = R"(#version 330 core
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec4 aColor;
uniform vec2 uRes;
out vec4 vColor;
void main() {
    vColor = aColor;
    vec2 ndc = vec2(aPos.x / uRes.x * 2.0 - 1.0, 1.0 - aPos.y / uRes.y * 2.0);
    gl_Position = vec4(ndc, 0.0, 1.0);
}
)";
inline constexpr const char* kFishFS = R"(#version 330 core
in vec4 vColor;
out vec4 FragColor;
uniform float uDepthFog;   // рыбы «за стеклом» слегка растворены в воде
uniform vec3  uFogColor;
void main() { FragColor = vec4(mix(vColor.rgb, uFogColor, uDepthFog), vColor.a); }
)";

} // namespace ide::shaders
