# Пользовательские шейдеры

Встроенные шейдеры тем находятся в `src/graphics/Shaders.hpp`. Чтобы заменить их
без пересборки, положите сюда файлы (GLSL 330 core, фрагментные):

| Файл             | Тема       | Основные uniform-ы |
|------------------|------------|--------------------|
| `aquarium.frag`  | Аквариум   | `uTime, uRes, uWave, uMedia, uHasMedia, uIntensity, uRays, uDeepColor, uShallowColor` |
| `steampunk.frag` | Стимпанк   | `uTime, uRes, uGearAngles, uIntensity, uMedia, uHasMedia` |
| `matrix.frag`    | Хакер      | `uTime, uRes, uIntensity, uTint` |
| `cyberpunk.frag` | Киберпанк  | `uTime, uRes, uIntensity, uColorA, uColorB, uMedia, uHasMedia` |
| `post.frag`      | пост-эффект| `uScene, uRes, uTime, uMode, uCurvature, uScanlines, uGlow, uTint, uMono, uAberration, uGlitch, uSepia` |

Вход фрагментного шейдера — `in vec2 vUV;`, выход — `out vec4 FragColor;`.
При ошибке компиляции IDE остаётся на встроенном шейдере и пишет лог в stderr.
