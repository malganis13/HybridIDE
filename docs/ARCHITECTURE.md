# Архитектура HybridIDE

## Принципы

1. **Модули не знают друг о друге.** Редактор, LSP, DAP, Git, сборка, темы и
   GitHub общаются только через колбэки (`std::function`), которые связывает
   `Application` — единственный «композиционный корень».
2. **UI-поток никогда не блокируется.** Всё, что ходит в сеть, на диск или в
   дочерние процессы, выполняется в пуле потоков (`ThreadPool`, `runAsync`) или
   в выделенных `std::jthread`. Результаты возвращаются в главный поток через
   lock-free MPSC-очередь `MainThreadDispatcher` и применяются в начале кадра.
3. **Ядро тестируемо без GUI.** `ide_core` — статическая библиотека; тесты
   (`tests/test_core.cpp`) проверяют буфер, подсветку, framing, разбор
   вывода компиляторов, VT100, boids, волны, локализацию, клавиши, base64.

## Кадр

```
glfwPollEvents
MainThreadDispatcher::drain()        ← ответы LSP/DAP, git, HTTP, watcher
ImGui::NewFrame
KeybindingManager::process()         ← горячие клавиши до отрисовки панелей
меню → статус-строка → DockSpace → панели → палитра → диалоги
ThemeManager::update()               ← boids, волны, шестерни, глитч
ShaderEngine::beginScene()           ← фон темы + рыбы в offscreen FBO
ImGui_ImplOpenGL3_RenderDrawData     ← интерфейс поверх фона в тот же FBO
ShaderEngine::endSceneToScreen()     ← CRT / неон / сепия на весь кадр
UpdatePlatformWindows / Render       ← вынесенные окна (multi-viewport)
```

## Ключевые решения

| Решение | Почему |
|---|---|
| Свой JSON-клиент DAP вместо cppdap | DAP — тот же JSON-поверх-Content-Length, что и LSP; общий `JsonChannel` убирает зависимость и упрощает сборку на всех ОС |
| ConPTY вместо winpty | ConPTY встроен в Windows 10 1809+, не требует внешних DLL и корректно поддерживает VT-последовательности |
| Git через CLI (`--porcelain=v1 -z`) вместо libgit2 | полная совместимость с конфигурацией пользователя (credential helpers, SSH, hooks), нулевая сборочная зависимость |
| Свой загрузчик GL (`GLLoader`) | минимальный набор функций GL 3.3 без glad/glew |
| Процедурный звук | нет бинарных ассетов в репозитории; каждый эффект можно заменить файлом |
| Встроенный диалог файлов | одинаково работает на всех ОС и во вынесенных окнах ImGui |
| `###Id` в заголовках окон | смена языка «на лету» не ломает докинг-раскладку |
| `-fno-char8_t` / `/Zc:char8_t-` | `path::u8string()` возвращает `std::string` — единый UTF-8 тип строк |

## Потоки

| Поток | Владелец | Назначение |
|---|---|---|
| главный | Application | ImGui, OpenGL, вся мутация состояния UI |
| пул (N/2 ядер) | `ThreadPool::global()` | HTTP, git, генерация проектов, распаковка |
| чтение stdout/stderr | `Process` | LSP/DAP/сборка |
| PTY reader | `TerminalSession` | вывод терминала → `VtScreen` (под мьютексом) |
| watcher | `FileExplorer` | опрос mtime дерева проекта |
| телеметрия | `TelemetryPanel` | /proc, NVML, GetSystemTimes |
| аудио | miniaudio | микширование, команды через MPSC-очередь |

## Расширение

* **Новый язык** — добавить `LanguageDef` в `LanguageRegistry` (ключевые
  слова, комментарии, команда LSP).
* **Новая тема** — значение в `ThemeId`, стиль/палитра в `ThemeManager`,
  фрагментный шейдер в `Shaders.hpp` (или файл в `resources/shaders`).
* **Новая команда** — `keys_.registerCommand("id", handler)` + строка
  `cmd.id` в словарях + привязки в `resources/keymaps/*.json`.
* **Подсветка tree-sitter** — `SyntaxHighlighter` изолирован за интерфейсом
  `tokens(line)`, его можно заменить без изменения редактора.
