// ============================================================================
//  Panels.cpp — окно настроек (в т.ч. «Сеть и расширения») и «О программе».
// ============================================================================
#include "ui/Panels.hpp"

#include "audio/AudioEngine.hpp"
#include "github/ToolchainManager.hpp"
#include "themes/ThemeManager.hpp"
#include "ui/KeybindingManager.hpp"
#include "ui/Localization.hpp"

#include <imgui.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <fstream>
#include <memory>

#ifndef IDE_VERSION
#define IDE_VERSION "dev"
#endif

namespace ide {

namespace fs = std::filesystem;

namespace {
const ImVec4 kOk(0.35f, 0.9f, 0.45f, 1.f), kErr(1.f, 0.42f, 0.35f, 1.f), kWarn(1.f, 0.8f, 0.25f, 1.f);

// Права PAT, которые IDE не нужны и опасны при утечке токена.
bool isDangerousScope(const std::string& s) {
    return s == "delete_repo" || s == "admin:org" || s == "admin:enterprise" || s == "admin:public_key" ||
           s == "admin:gpg_key" || s == "admin:org_hook" || s == "admin:repo_hook" || s == "delete:packages" ||
           s == "site_admin" || s == "admin:ssh_signing_key";
}

// Каталог для расширения: имя репозитория без '/'.
fs::path extensionDir(const std::string& fullName) {
    std::string n = fullName;
    std::replace(n.begin(), n.end(), '/', '_');
    return Workspace::userConfigDir() / "extensions" / n;
}

// Защита от path traversal в манифесте расширения ("../../.bashrc").
bool safeRelativePath(const std::string& p) {
    if (p.empty() || p[0] == '/' || p[0] == '\\' || p.find(':') != std::string::npos) return false;
    fs::path rel(p);
    for (const auto& part : rel) if (part == "..") return false;
    return true;
}
} // namespace

// ===========================================================================
void SettingsPanel::render(bool* open, PanelContext& ctx) {
    auto& L = Localization::instance();
    ImGui::SetNextWindowSize(ImVec2(720, 560), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(L.tr("settings.title"), open)) { ImGui::End(); return; }
    if (ImGui::BeginTabBar("##settingsTabs")) {
        auto tab = [&](SettingsTab t, const char* label) {
            ImGuiTabItemFlags f = (hasRequest_ && requested_ == t) ? ImGuiTabItemFlags_SetSelected : 0;
            return ImGui::BeginTabItem(label, nullptr, f);
        };
        if (tab(SettingsTab::General, L.tr("settings.tab_general"))) { tabGeneral(ctx); ImGui::EndTabItem(); }
        if (tab(SettingsTab::Appearance, L.tr("settings.tab_appearance"))) {
            if (ctx.themes) ctx.themes->renderSettings();
            ImGui::EndTabItem();
        }
        if (tab(SettingsTab::Keyboard, L.tr("settings.tab_keyboard"))) { tabKeyboard(ctx); ImGui::EndTabItem(); }
        if (tab(SettingsTab::Network, L.tr("settings.tab_network"))) { tabNetwork(ctx); ImGui::EndTabItem(); }
        hasRequest_ = false;
        ImGui::EndTabBar();
    }
    ImGui::End();
}

// ---------------------------------------------------------------------------
void SettingsPanel::tabGeneral(PanelContext& ctx) {
    auto& L = Localization::instance();
    auto& s = *ctx.settings;
    bool ch = false;

    ImGui::SeparatorText(L.tr("settings.language"));
    for (auto& lang : L.availableLanguages()) {
        if (ImGui::RadioButton(Localization::languageDisplayName(lang), s.language == lang)) {
            s.language = lang;
            L.setLanguage(lang);          // горячая смена — интерфейс перерисуется в этом же кадре
            ch = true;
        }
        ImGui::SameLine();
    }
    ImGui::NewLine();

    ImGui::SeparatorText(L.tr("settings.interface"));
    if (ImGui::SliderFloat(L.tr("settings.font_size"), &s.fontSize, 10.f, 32.f, "%.0f px")) ch = true;
    if (ImGui::IsItemDeactivatedAfterEdit() && ctx.rebuildFonts) ctx.rebuildFonts();
    if (ImGui::SliderFloat(L.tr("settings.ui_scale"), &s.uiScale, 0.75f, 2.5f, "%.2f")) ch = true;
    if (ImGui::IsItemDeactivatedAfterEdit() && ctx.rebuildFonts) ctx.rebuildFonts();
    ch |= ImGui::Checkbox(L.tr("settings.vsync"), &s.vsync);

    ImGui::SeparatorText(L.tr("settings.recent"));
    if (s.recentFolders.empty()) ImGui::TextDisabled("%s", L.tr("props.none"));
    for (auto& r : s.recentFolders) ImGui::BulletText("%s", r.c_str());
    if (!s.recentFolders.empty() && ImGui::SmallButton(L.tr("settings.clear_recent"))) { s.recentFolders.clear(); ch = true; }

    if (ch && ctx.saveSettings) ctx.saveSettings();
}

// ---------------------------------------------------------------------------
void SettingsPanel::tabKeyboard(PanelContext& ctx) {
    auto& L = Localization::instance();
    auto& s = *ctx.settings;
    ImGui::TextWrapped("%s", L.tr("settings.keymap_desc"));
    for (auto& p : KeybindingManager::presetNames()) {
        const char* label = p == "visualstudio" ? "Visual Studio" : "VS Code";
        if (ImGui::RadioButton(label, s.keymap == p)) {
            s.keymap = p;
            if (ctx.keys) ctx.keys->loadPreset(p, ctx.resourceDir);
            if (ctx.saveSettings) ctx.saveSettings();
        }
        ImGui::SameLine();
    }
    ImGui::NewLine();
    if (ImGui::Button(L.tr("settings.open_keybindings")) && ctx.openKeybindings) ctx.openKeybindings();
    ImGui::Spacing();
    if (ctx.keys && ImGui::BeginTable("##keyPreview", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV)) {
        for (const char* id : {"file.save", "view.commandPalette", "build.build", "debug.start", "debug.stepOver",
                               "editor.toggleComment", "editor.addNextOccurrence", "editor.goToDefinition"}) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(L.tr(std::string("cmd.") + id));
            ImGui::TableNextColumn(); ImGui::TextDisabled("%s", ctx.keys->shortcutText(id).c_str());
        }
        ImGui::EndTable();
    }
}

// ---------------------------------------------------------------------------
void SettingsPanel::tabNetwork(PanelContext& ctx) {
    if (!ctx.github) return;
    sectionToken(ctx);
    sectionExtensions(ctx);
    sectionToolchains(ctx);
    sectionBackup(ctx);
}

void SettingsPanel::sectionToken(PanelContext& ctx) {
    auto& L = Localization::instance();
    auto& gh = *ctx.github;
    ImGui::SeparatorText(L.tr("net.token"));
    ImGui::TextWrapped("%s", L.tr("net.token_desc"));

    if (!tokenLoaded_) {                       // при первом показе подхватываем сохранённый токен
        tokenLoaded_ = true;
        if (!gh.hasToken()) if (auto t = Workspace::loadSecret("github_token")) gh.setToken(*t);
    }
    ImGui::SetNextItemWidth(-260);
    ImGui::InputTextWithHint("##pat", gh.hasToken() ? L.tr("net.token_saved") : "ghp_... / github_pat_...", &tokenInput_,
                             ImGuiInputTextFlags_Password);
    ImGui::SameLine();
    ImGui::BeginDisabled(tokenBusy_ || (tokenInput_.empty() && !gh.hasToken()));
    if (ImGui::Button(L.tr("net.validate"))) {
        if (!tokenInput_.empty()) {
            gh.setToken(tokenInput_);
            if (!Workspace::saveSecret("github_token", tokenInput_)) tokenStatus_ = L.tr("net.secret_failed");
            // Затираем буфер с токеном сразу после сохранения.
            std::fill(tokenInput_.begin(), tokenInput_.end(), '\0');
            tokenInput_.clear();
        }
        tokenBusy_ = true;
        tokenStatus_ = L.tr("net.checking");
        gh.validateToken([this](bool ok, std::string err) {
            tokenBusy_ = false;
            tokenStatus_ = ok ? std::string{} : err;
        });
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!gh.hasToken());
    if (ImGui::Button(L.tr("net.forget"))) {
        Workspace::deleteSecret("github_token");
        gh.setToken({});
        tokenStatus_.clear();
    }
    ImGui::EndDisabled();

    if (gh.authenticated()) {
        const auto& u = gh.user();
        ImGui::TextColored(kOk, "%s %s%s%s%s", L.tr("net.signed_in"), u.login.c_str(), u.name.empty() ? "" : " (",
                           u.name.c_str(), u.name.empty() ? "" : ")");
        if (gh.rateLimitRemaining() >= 0) { ImGui::SameLine(); ImGui::TextDisabled("API: %d", gh.rateLimitRemaining()); }
        std::string scopes, danger;
        for (auto& sc : u.scopes) {
            scopes += (scopes.empty() ? "" : ", ") + sc;
            if (isDangerousScope(sc)) danger += (danger.empty() ? "" : ", ") + sc;
        }
        ImGui::TextDisabled("%s: %s", L.tr("net.scopes"), scopes.empty() ? "fine-grained" : scopes.c_str());
        if (!danger.empty()) {
            ImGui::PushTextWrapPos(0);
            ImGui::TextColored(kWarn, "%s %s", L.tr("net.scopes_warning"), danger.c_str());
            ImGui::PopTextWrapPos();
        }
    } else if (!tokenStatus_.empty()) {
        ImGui::TextColored(tokenBusy_ ? kWarn : kErr, "%s", tokenStatus_.c_str());
    }
}

void SettingsPanel::sectionExtensions(PanelContext& ctx) {
    auto& L = Localization::instance();
    auto& gh = *ctx.github;
    ImGui::SeparatorText(L.tr("net.extensions"));
    ImGui::SetNextItemWidth(-120);
    bool enter = ImGui::InputTextWithHint("##extq", L.tr("net.ext_search_hint"), &extQuery_, ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    ImGui::BeginDisabled(extBusy_);
    if (ImGui::Button(L.tr("net.search")) || enter) {
        extBusy_ = true;
        extStatus_ = L.tr("net.searching");
        gh.searchExtensions(extQuery_, [this](std::vector<GitHubRepo> r, std::string err) {
            extBusy_ = false;
            extResults_ = std::move(r);
            extStatus_ = err.empty() ? std::to_string(extResults_.size()) + " " + Localization::instance().tr("net.found") : err;
        });
    }
    ImGui::EndDisabled();
    if (!extStatus_.empty()) ImGui::TextDisabled("%s", extStatus_.c_str());

    // Дополнительные источники (owner/repo), которые показываются всегда.
    auto& srcs = ctx.settings->extensionSources;
    if (ImGui::TreeNode(L.tr("net.ext_sources"))) {
        for (std::size_t i = 0; i < srcs.size(); ++i) {
            ImGui::PushID((int)i);
            ImGui::BulletText("%s", srcs[i].c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton("x")) { srcs.erase(srcs.begin() + (long)i); if (ctx.saveSettings) ctx.saveSettings(); ImGui::PopID(); break; }
            ImGui::PopID();
        }
        ImGui::SetNextItemWidth(220);
        ImGui::InputTextWithHint("##newsrc", "owner/repo", &newExtSource_);
        ImGui::SameLine();
        if (ImGui::SmallButton(L.tr("keys.add")) && newExtSource_.find('/') != std::string::npos) {
            srcs.push_back(newExtSource_); newExtSource_.clear();
            if (ctx.saveSettings) ctx.saveSettings();
        }
        for (auto& s : srcs) {
            ImGui::PushID(s.c_str());
            if (ImGui::SmallButton(L.tr("net.install"))) { GitHubRepo r; r.fullName = s; installExtension(ctx, r); }
            ImGui::SameLine(); ImGui::TextUnformatted(s.c_str());
            if (auto it = extInstallState_.find(s); it != extInstallState_.end()) { ImGui::SameLine(); ImGui::TextDisabled("%s", it->second.c_str()); }
            ImGui::PopID();
        }
        ImGui::TreePop();
    }

    if (!extResults_.empty() && ImGui::BeginTable("##ext", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                                                              ImGuiTableFlags_BordersInnerV, ImVec2(0, 200))) {
        ImGui::TableSetupColumn(L.tr("net.ext_name"), ImGuiTableColumnFlags_WidthStretch, 1.2f);
        ImGui::TableSetupColumn(L.tr("net.ext_desc"), ImGuiTableColumnFlags_WidthStretch, 2.f);
        ImGui::TableSetupColumn("★", ImGuiTableColumnFlags_WidthFixed, 40);
        ImGui::TableSetupColumn("##act", ImGuiTableColumnFlags_WidthFixed, 140);
        ImGui::TableHeadersRow();
        for (auto& r : extResults_) {
            ImGui::PushID(r.fullName.c_str());
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(r.fullName.c_str());
            ImGui::TableNextColumn(); ImGui::TextWrapped("%s", r.description.c_str());
            ImGui::TableNextColumn(); ImGui::Text("%d", r.stars);
            ImGui::TableNextColumn();
            auto it = extInstallState_.find(r.fullName);
            if (it == extInstallState_.end()) { if (ImGui::SmallButton(L.tr("net.install"))) installExtension(ctx, r); }
            else ImGui::TextDisabled("%s", it->second.c_str());
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
}

// Установка: манифест hybridide-extension.json -> файлы из "files" -> каталог
// расширения. Исполняемый код расширений НЕ запускается автоматически —
// только декларативные ресурсы (темы, раскладки, описания тулчейнов).
void SettingsPanel::installExtension(PanelContext& ctx, const GitHubRepo& repo) {
    auto& gh = *ctx.github;
    const std::string name = repo.fullName;
    extInstallState_[name] = Localization::instance().tr("net.installing");
    gh.fetchManifest(name, [this, &ctx, &gh, name](std::optional<ExtensionManifest> m, std::string err) {
        auto& L = Localization::instance();
        if (!m) { extInstallState_[name] = std::string(L.tr("net.failed")) + ": " + err; return; }
        fs::path dir = extensionDir(name);
        std::error_code ec;
        fs::create_directories(dir, ec);
        std::ofstream(dir / "hybridide-extension.json") << m->raw.dump(2);

        std::vector<std::string> files;
        for (auto& f : m->raw.value("files", nlohmann::json::array()))
            if (f.is_string() && safeRelativePath(f.get<std::string>())) files.push_back(f.get<std::string>());
        if (files.empty()) {
            extInstallState_[name] = L.tr("net.installed");
            if (ctx.onExtensionInstalled) ctx.onExtensionInstalled(*m, dir);
            return;
        }
        // Счётчик оставшихся файлов; последний завершившийся вызывает onExtensionInstalled.
        auto remaining = std::make_shared<int>((int)files.size());
        auto failed = std::make_shared<bool>(false);
        auto manifest = std::make_shared<ExtensionManifest>(*m);
        for (auto& f : files) {
            gh.getFile(name, f, [this, &ctx, name, dir, f, remaining, failed, manifest](std::optional<std::string> c, std::string e) {
                if (c) {
                    std::error_code ec2;
                    fs::create_directories((dir / f).parent_path(), ec2);
                    std::ofstream(dir / f, std::ios::binary) << *c;
                } else {
                    *failed = true;
                    if (ctx.log) ctx.log("[extensions] " + name + "/" + f + ": " + e);
                }
                if (--*remaining == 0) {
                    auto& L2 = Localization::instance();
                    extInstallState_[name] = *failed ? L2.tr("net.partial") : L2.tr("net.installed");
                    if (ctx.onExtensionInstalled) ctx.onExtensionInstalled(*manifest, dir);
                }
            });
        }
    });
}

void SettingsPanel::sectionToolchains(PanelContext& ctx) {
    auto& L = Localization::instance();
    if (!ctx.toolchains) return;
    auto& tc = *ctx.toolchains;
    ImGui::SeparatorText(L.tr("net.toolchains"));
    ImGui::TextDisabled("%s %s", L.tr("net.install_root"), tc.installRoot().string().c_str());
    if (ImGui::BeginTable("##tc", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableSetupColumn(L.tr("net.ext_name"), ImGuiTableColumnFlags_WidthStretch, 1.4f);
        ImGui::TableSetupColumn(L.tr("net.status"), ImGuiTableColumnFlags_WidthStretch, 2.f);
        ImGui::TableSetupColumn("##a", ImGuiTableColumnFlags_WidthFixed, 150);
        ImGui::TableHeadersRow();
        for (auto& spec : tc.catalog()) {
            ImGui::PushID(spec.id.c_str());
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(spec.displayName.c_str());
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("github.com/%s", spec.repo.c_str());
            ImGui::TableNextColumn();
            auto inst = std::find_if(tc.installed().begin(), tc.installed().end(), [&](auto& t) { return t.id == spec.id; });
            auto job = tc.jobs().find(spec.id);
            bool busy = false;
            if (job != tc.jobs().end() && job->second.stage != InstallStage::Done && job->second.stage != InstallStage::Failed &&
                job->second.stage != InstallStage::Idle) {
                busy = true;
                ImGui::ProgressBar(job->second.progress, ImVec2(-1, 0), job->second.message.c_str());
            } else if (job != tc.jobs().end() && job->second.stage == InstallStage::Failed) {
                ImGui::TextColored(kErr, "%s", job->second.message.c_str());
            } else if (inst != tc.installed().end()) {
                ImGui::TextColored(kOk, "%s %s", L.tr("net.installed"), inst->version.c_str());
            } else {
                ImGui::TextDisabled("—");
            }
            ImGui::TableNextColumn();
            ImGui::BeginDisabled(busy);
            if (inst == tc.installed().end()) {
                if (ImGui::SmallButton(L.tr("net.install"))) tc.install(spec.id);
            } else {
                if (ImGui::SmallButton(L.tr("net.update"))) tc.install(spec.id);
                ImGui::SameLine();
                if (ImGui::SmallButton(L.tr("explorer.delete"))) tc.uninstall(spec.id);
            }
            ImGui::EndDisabled();
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
}

void SettingsPanel::sectionBackup(PanelContext& ctx) {
    auto& L = Localization::instance();
    auto& gh = *ctx.github;
    auto& s = *ctx.settings;
    ImGui::SeparatorText(L.tr("net.backup"));
    ImGui::TextWrapped("%s", L.tr("net.backup_desc"));
    ImGui::SetNextItemWidth(260);
    if (ImGui::InputText(L.tr("net.backup_repo"), &s.githubBackupRepo) && ctx.saveSettings) ctx.saveSettings();
    ImGui::BeginDisabled(!gh.authenticated() || backupBusy_ || s.githubBackupRepo.empty());
    if (ImGui::Button(L.tr("net.backup_now"))) {
        backupBusy_ = true;
        backupStatus_ = L.tr("net.uploading");
        std::vector<std::pair<std::string, std::string>> files;
        if (ctx.collectBackupFiles) files = ctx.collectBackupFiles();
        gh.backupWorkspace(s.githubBackupRepo, files, [this](bool ok, std::string err) {
            backupBusy_ = false;
            backupStatus_ = ok ? Localization::instance().tr("net.backup_done") : err;
        });
    }
    ImGui::SameLine();
    if (ImGui::Button(L.tr("net.restore"))) {
        backupBusy_ = true;
        backupStatus_ = L.tr("net.downloading");
        const std::string full = gh.user().login + "/" + s.githubBackupRepo;
        // Восстанавливаем глобальные настройки и клавиши; workspace-файлы проекта не трогаем.
        auto left = std::make_shared<int>(2);
        auto finish = [this, &ctx, left](const std::string& msg) {
            if (!msg.empty()) backupStatus_ = msg;
            if (--*left == 0) {
                backupBusy_ = false;
                if (backupStatus_ == Localization::instance().tr("net.downloading"))
                    backupStatus_ = Localization::instance().tr("net.restore_done");
                if (ctx.reloadSettings) ctx.reloadSettings();
            }
        };
        for (const char* f : {"settings.json", "keybindings.json"}) {
            std::string file = f;
            gh.getFile(full, file, [file, finish](std::optional<std::string> c, std::string err) {
                if (c) {
                    std::ofstream(Workspace::userConfigDir() / file, std::ios::binary) << *c;
                    finish({});
                } else {
                    finish(file + ": " + err);
                }
            });
        }
    }
    ImGui::EndDisabled();
    if (!gh.authenticated()) { ImGui::SameLine(); ImGui::TextDisabled("%s", L.tr("net.need_token")); }
    if (!backupStatus_.empty()) ImGui::TextDisabled("%s", backupStatus_.c_str());
}

// ---------------------------------------------------------------------------
void renderAbout(bool* open) {
    auto& L = Localization::instance();
    if (!*open) return;
    ImGui::SetNextWindowSize(ImVec2(520, 0), ImGuiCond_Appearing);
    if (ImGui::Begin(L.tr("about.title"), open, ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("HybridIDE %s", IDE_VERSION);
        ImGui::TextWrapped("%s", L.tr("about.desc"));
        ImGui::Separator();
        ImGui::Text("Dear ImGui %s (docking)", IMGUI_VERSION);
        ImGui::TextUnformatted("ImPlot, GLFW, nlohmann/json, miniaudio, stb_image, libcurl");
        ImGui::TextDisabled("%s", L.tr("about.license"));
    }
    ImGui::End();
}

} // namespace ide
