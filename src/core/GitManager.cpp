// =============================================================================
//  GitManager.cpp
// =============================================================================
#include "core/GitManager.hpp"

#include "core/Async.hpp"
#include "core/Process.hpp"
#include "ui/Localization.hpp"

#include <imgui_stdlib.h>

#include <algorithm>
#include <fstream>
#include <sstream>

namespace ide {

namespace fs = std::filesystem;

void GitManager::setRepository(const fs::path& root) {
    root_ = root;
    refresh();
}

void GitManager::runGit(std::vector<std::string> args, std::function<void(int, std::string)> done, bool refreshAfter) {
    args.insert(args.begin(), "git");
    ++busy_;
    auto root = root_;
    runAsync(
        [args, root] {
            ProcessOptions o; o.argv = args; o.cwd = root; o.mergeStderr = true;
            o.env["GIT_TERMINAL_PROMPT"] = "0";   // не зависать на запросе пароля
            o.env["LC_ALL"] = "C";
            return Process::run(o);
        },
        [this, done, refreshAfter, args](ProcessResult r) {
            --busy_;
            if (log) {
                std::string cmd; for (auto& a : args) cmd += a + " ";
                log("$ " + cmd + "\n" + r.out + r.err);
            }
            if (done) done(r.exitCode, r.out + r.err);
            if (refreshAfter) refresh();
        });
}

std::vector<GitFileStatus> GitManager::parsePorcelain(const std::string& out) {
    // Формат `git status --porcelain=v1 -z`: "XY path\0" (rename: "XY new\0old\0")
    std::vector<GitFileStatus> files;
    std::size_t i = 0;
    while (i + 3 < out.size()) {
        GitFileStatus f;
        f.index = out[i]; f.worktree = out[i + 1];
        std::size_t end = out.find('\0', i + 3);
        if (end == std::string::npos) end = out.size();
        f.path = out.substr(i + 3, end - i - 3);
        i = end + 1;
        if (f.index == 'R' || f.index == 'C') { auto e2 = out.find('\0', i); i = e2 == std::string::npos ? out.size() : e2 + 1; }
        files.push_back(std::move(f));
    }
    return files;
}

std::vector<DiffLine> GitManager::parseUnifiedDiff(const std::string& diff) {
    std::vector<DiffLine> out;
    std::istringstream ss(diff);
    std::string line;
    int o = 0, n = 0;
    bool inHunk = false;
    while (std::getline(ss, line)) {
        if (line.rfind("@@", 0) == 0) {
            // @@ -a,b +c,d @@ context
            int a = 0, c = 0;
            std::sscanf(line.c_str(), "@@ -%d", &a);
            auto plus = line.find('+');
            if (plus != std::string::npos) c = std::atoi(line.c_str() + plus + 1);
            o = a; n = c; inHunk = true;
            out.push_back({DiffLine::Hunk, -1, -1, line});
        } else if (!inHunk) continue;
        else if (!line.empty() && line[0] == '+') out.push_back({DiffLine::Added, -1, n++, line.substr(1)});
        else if (!line.empty() && line[0] == '-') out.push_back({DiffLine::Removed, o++, -1, line.substr(1)});
        else if (!line.empty() && line[0] == '\\') continue;     // "\ No newline at end of file"
        else out.push_back({DiffLine::Context, o++, n++, line.empty() ? "" : line.substr(1)});
    }
    return out;
}

void GitManager::refresh() {
    if (root_.empty()) return;
    auto root = root_;
    ++busy_;
    struct Snapshot { bool repo = false; std::string branch; std::vector<std::string> branches; std::vector<GitFileStatus> files; std::vector<GitCommit> hist; int ahead = 0, behind = 0; };
    runAsync(
        [root] {
            Snapshot s;
            auto git = [&](std::vector<std::string> a) {
                a.insert(a.begin(), "git");
                ProcessOptions o; o.argv = a; o.cwd = root; o.env["LC_ALL"] = "C"; o.env["GIT_TERMINAL_PROMPT"] = "0";
                return Process::run(o);
            };
            auto r = git({"rev-parse", "--is-inside-work-tree"});
            if (!r.ok()) return s;
            s.repo = true;
            s.branch = git({"rev-parse", "--abbrev-ref", "HEAD"}).out;
            while (!s.branch.empty() && (s.branch.back() == '\n' || s.branch.back() == '\r')) s.branch.pop_back();
            std::istringstream bs(git({"branch", "--format=%(refname:short)"}).out);
            for (std::string b; std::getline(bs, b);) if (!b.empty()) s.branches.push_back(b);
            s.files = parsePorcelain(git({"status", "--porcelain=v1", "-z", "-uall"}).out);
            auto ab = git({"rev-list", "--left-right", "--count", "@{upstream}...HEAD"});
            if (ab.ok()) std::sscanf(ab.out.c_str(), "%d %d", &s.behind, &s.ahead);
            // Граф истории: разделитель \x1f между полями
            auto lg = git({"log", "--graph", "--all", "-n", "300", "--date=short", "--pretty=format:\x1f%h\x1f%an\x1f%ad\x1f%d\x1f%s"});
            std::istringstream ls(lg.out);
            for (std::string l; std::getline(ls, l);) {
                GitCommit c;
                auto p = l.find('\x1f');
                c.graph = l.substr(0, p);
                if (p != std::string::npos) {
                    std::vector<std::string> parts; std::string rest = l.substr(p + 1), cur;
                    for (char ch : rest) { if (ch == '\x1f') { parts.push_back(cur); cur.clear(); } else cur += ch; }
                    parts.push_back(cur);
                    if (parts.size() >= 5) { c.hash = parts[0]; c.author = parts[1]; c.date = parts[2]; c.refs = parts[3]; c.subject = parts[4]; }
                }
                s.hist.push_back(std::move(c));
            }
            return s;
        },
        [this](Snapshot s) {
            --busy_;
            isRepo_ = s.repo; branch_ = std::move(s.branch); branches_ = std::move(s.branches);
            files_ = std::move(s.files); history_ = std::move(s.hist); ahead_ = s.ahead; behind_ = s.behind;
        });
}

void GitManager::stage(const std::string& p)   { runGit({"add", "--", p}, nullptr); }
void GitManager::unstage(const std::string& p) { runGit({"restore", "--staged", "--", p}, nullptr); }
void GitManager::stageAll()                     { runGit({"add", "-A"}, nullptr); }
void GitManager::initRepo()                     { runGit({"init", "-b", "main"}, nullptr); }
void GitManager::push()  { runGit({"push", "--set-upstream", "origin", branch_.empty() ? "HEAD" : branch_}, nullptr); }
void GitManager::pull()  { runGit({"pull", "--ff-only"}, nullptr); }
void GitManager::fetch() { runGit({"fetch", "--all", "--prune"}, nullptr); }

void GitManager::commit(const std::string& message, bool amend) {
    if (message.empty() && !amend) return;
    std::vector<std::string> a{"commit", "-m", message};
    if (amend) a.push_back("--amend");
    runGit(a, [this](int code, std::string) { if (code == 0) commitMsg_.clear(); });
}

void GitManager::checkout(const std::string& b, bool create) {
    if (create) runGit({"switch", "-c", b}, nullptr);
    else runGit({"switch", b}, nullptr);
}

void GitManager::openDiff(const std::string& path, bool staged) {
    std::vector<std::string> a{"diff", "--no-color", "-U3"};
    if (staged) a.push_back("--cached");
    a.push_back("--"); a.push_back(path);
    runGit(a, [this, path, staged](int, std::string out) {
        diff_.path = path; diff_.staged = staged;
        diff_.lines = parseUnifiedDiff(out);
        // Неотслеживаемый файл: показываем как полностью добавленный
        if (diff_.lines.empty()) {
            std::error_code ec;
            if (fs::is_regular_file(root_ / path, ec)) {
                std::ifstream f(root_ / path);
                std::string l; int n = 1;
                diff_.lines.push_back({DiffLine::Hunk, -1, -1, "@@ new file @@"});
                while (std::getline(f, l) && n < 5000) diff_.lines.push_back({DiffLine::Added, -1, n++, l});
            }
        }
        diffOpen_ = true;
    }, false);
}

void GitManager::render(const char* title, bool* open) {
    if (!ImGui::Begin(title, open)) { ImGui::End(); return; }
    auto& L = Localization::instance();
    if (root_.empty()) { ImGui::TextDisabled("%s", L.tr("git.no_folder")); ImGui::End(); return; }
    if (!isRepo_) {
        ImGui::TextWrapped("%s", L.tr("git.not_repo"));
        if (ImGui::Button(L.tr("git.init"))) initRepo();
        ImGui::End();
        return;
    }
    // --- Ветка и синхронизация ---
    ImGui::SetNextItemWidth(180);
    if (ImGui::BeginCombo("##branch", branch_.c_str())) {
        for (auto& b : branches_) if (ImGui::Selectable(b.c_str(), b == branch_)) checkout(b);
        ImGui::Separator();
        ImGui::SetNextItemWidth(140);
        ImGui::InputTextWithHint("##nb", L.tr("git.new_branch"), &newBranch_);
        ImGui::SameLine();
        if (ImGui::Button("+") && !newBranch_.empty()) { checkout(newBranch_, true); newBranch_.clear(); ImGui::CloseCurrentPopup(); }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::Button(L.tr("git.pull"))) pull();
    ImGui::SameLine();
    char pushLbl[64]; std::snprintf(pushLbl, sizeof(pushLbl), "%s (%d)", L.tr("git.push"), ahead_);
    if (ImGui::Button(pushLbl)) push();
    ImGui::SameLine();
    if (ImGui::Button(L.tr("git.refresh"))) refresh();
    if (busy_ > 0) { ImGui::SameLine(); ImGui::TextDisabled("..."); }
    if (behind_ > 0) ImGui::TextColored(ImVec4(1, .7f, .2f, 1), "%s: %d", L.tr("git.behind"), behind_);

    if (ImGui::BeginTabBar("##gittabs")) {
        if (ImGui::BeginTabItem(L.tr("git.changes"))) {
            ImGui::InputTextMultiline("##msg", &commitMsg_, ImVec2(-1, 60));
            if (ImGui::Button(L.tr("git.commit"), ImVec2(120, 0))) commit(commitMsg_, amend_);
            ImGui::SameLine();
            if (ImGui::Button(L.tr("git.stage_all"))) stageAll();
            ImGui::SameLine();
            ImGui::Checkbox("amend", &amend_);
            auto section = [&](const char* name, bool staged) {
                int count = (int)std::count_if(files_.begin(), files_.end(), [&](auto& f) { return staged ? f.staged() : f.unstaged(); });
                if (!ImGui::CollapsingHeader((std::string(name) + " (" + std::to_string(count) + ")###" + name).c_str(), ImGuiTreeNodeFlags_DefaultOpen)) return;
                for (auto& f : files_) {
                    if (staged ? !f.staged() : !f.unstaged()) continue;
                    char st = staged ? f.index : (f.index == '?' ? 'U' : f.worktree);
                    ImVec4 col = st == 'M' ? ImVec4(.9f, .75f, .3f, 1) : st == 'A' || st == 'U' ? ImVec4(.4f, .9f, .4f, 1) : st == 'D' ? ImVec4(.95f, .4f, .4f, 1) : ImVec4(.7f, .7f, .7f, 1);
                    ImGui::PushID((f.path + (staged ? "s" : "u")).c_str());
                    ImGui::TextColored(col, "%c", st);
                    ImGui::SameLine();
                    if (ImGui::Selectable(f.path.c_str(), false, ImGuiSelectableFlags_AllowDoubleClick | ImGuiSelectableFlags_AllowOverlap, ImVec2(ImGui::GetContentRegionAvail().x - 30, 0))) {
                        openDiff(f.path, staged);
                        if (ImGui::IsMouseDoubleClicked(0) && openFile) openFile(root_ / f.path);
                    }
                    ImGui::SameLine();
                    if (ImGui::SmallButton(staged ? "-" : "+")) { staged ? unstage(f.path) : stage(f.path); }
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", staged ? L.tr("git.unstage") : L.tr("git.stage"));
                    ImGui::PopID();
                }
            };
            section(L.tr("git.staged"), true);
            section(L.tr("git.unstaged"), false);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(L.tr("git.history"))) {
            if (ImGui::BeginTable("##hist", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable)) {
                ImGui::TableSetupColumn(L.tr("git.graph"), ImGuiTableColumnFlags_WidthFixed, 90);
                ImGui::TableSetupColumn(L.tr("git.message"));
                ImGui::TableSetupColumn(L.tr("git.author"), ImGuiTableColumnFlags_WidthFixed, 110);
                ImGui::TableSetupColumn(L.tr("git.date"), ImGuiTableColumnFlags_WidthFixed, 80);
                ImGui::TableHeadersRow();
                ImU32 laneCol[] = {IM_COL32(80, 160, 255, 255), IM_COL32(255, 120, 80, 255), IM_COL32(120, 220, 120, 255), IM_COL32(220, 120, 220, 255)};
                for (auto& c : history_) {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    // Рисуем граф цветными «дорожками»: каждая колонка ASCII-графа — свой цвет
                    ImDrawList* dl = ImGui::GetWindowDrawList();
                    ImVec2 p = ImGui::GetCursorScreenPos();
                    float cw = ImGui::CalcTextSize("M").x * 0.8f, lh = ImGui::GetTextLineHeight();
                    for (std::size_t k = 0; k < c.graph.size(); ++k) {
                        char g = c.graph[k];
                        ImU32 col = laneCol[(k / 2) % 4];
                        float x = p.x + (float)k * cw + cw * 0.5f;
                        if (g == '*') dl->AddCircleFilled(ImVec2(x, p.y + lh * 0.5f), 4.f, col);
                        else if (g == '|') dl->AddLine(ImVec2(x, p.y - 2), ImVec2(x, p.y + lh + 2), col, 2.f);
                        else if (g == '/') dl->AddLine(ImVec2(x + cw * 0.5f, p.y), ImVec2(x - cw * 0.5f, p.y + lh), col, 2.f);
                        else if (g == '\\') dl->AddLine(ImVec2(x - cw * 0.5f, p.y), ImVec2(x + cw * 0.5f, p.y + lh), col, 2.f);
                    }
                    ImGui::Dummy(ImVec2((float)c.graph.size() * cw, lh));
                    ImGui::TableNextColumn();
                    if (!c.refs.empty()) { ImGui::TextColored(ImVec4(.95f, .8f, .3f, 1), "%s", c.refs.c_str()); ImGui::SameLine(); }
                    ImGui::TextUnformatted(c.subject.c_str());
                    if (ImGui::IsItemHovered() && !c.hash.empty()) ImGui::SetTooltip("%s", c.hash.c_str());
                    ImGui::TableNextColumn(); ImGui::TextUnformatted(c.author.c_str());
                    ImGui::TableNextColumn(); ImGui::TextUnformatted(c.date.c_str());
                }
                ImGui::EndTable();
            }
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::End();
}

void GitManager::renderDiffWindow(const char* title) {
    if (!diffOpen_) return;
    auto& L = Localization::instance();
    ImGui::SetNextWindowSize(ImVec2(1000, 600), ImGuiCond_FirstUseEver);
    std::string t = std::string(title) + ": " + diff_.path + (diff_.staged ? " (staged)" : "") + "###gitdiff";
    if (!ImGui::Begin(t.c_str(), &diffOpen_)) { ImGui::End(); return; }
    ImGui::Checkbox(L.tr("git.side_by_side"), &sideBySide_);
    ImGui::SameLine();
    if (ImGui::SmallButton(diff_.staged ? L.tr("git.unstage") : L.tr("git.stage"))) {
        diff_.staged ? unstage(diff_.path) : stage(diff_.path);
        diffOpen_ = false;
    }
    const ImVec4 add(0.25f, 0.55f, 0.25f, 0.35f), rem(0.65f, 0.2f, 0.2f, 0.35f);
    ImGui::BeginChild("##diffview", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);
    auto rowBg = [](const ImVec4& c) {
        ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddRectFilled(p, ImVec2(p.x + ImGui::GetContentRegionAvail().x, p.y + ImGui::GetTextLineHeightWithSpacing()), ImGui::GetColorU32(c));
    };
    if (!sideBySide_) {
        for (auto& l : diff_.lines) {
            if (l.kind == DiffLine::Hunk) { ImGui::TextColored(ImVec4(.4f, .7f, 1, 1), "%s", l.text.c_str()); continue; }
            if (l.kind == DiffLine::Added) rowBg(add);
            if (l.kind == DiffLine::Removed) rowBg(rem);
            char o[8] = "", n[8] = "";
            if (l.oldNo > 0) std::snprintf(o, sizeof(o), "%d", l.oldNo);
            if (l.newNo > 0) std::snprintf(n, sizeof(n), "%d", l.newNo);
            ImGui::TextDisabled("%5s %5s", o, n); ImGui::SameLine();
            ImGui::Text("%c %s", l.kind == DiffLine::Added ? '+' : l.kind == DiffLine::Removed ? '-' : ' ', l.text.c_str());
        }
    } else if (ImGui::BeginTable("##sbs", 2, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable)) {
        // Выравнивание: блок удалённых строк сопоставляется с блоком добавленных
        std::size_t i = 0;
        while (i < diff_.lines.size()) {
            auto& l = diff_.lines[i];
            if (l.kind == DiffLine::Hunk || l.kind == DiffLine::Context) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                if (l.kind == DiffLine::Hunk) ImGui::TextColored(ImVec4(.4f, .7f, 1, 1), "%s", l.text.c_str());
                else ImGui::Text("%4d  %s", l.oldNo, l.text.c_str());
                ImGui::TableNextColumn();
                if (l.kind == DiffLine::Context) ImGui::Text("%4d  %s", l.newNo, l.text.c_str());
                ++i; continue;
            }
            std::vector<const DiffLine*> rems, adds;
            while (i < diff_.lines.size() && diff_.lines[i].kind == DiffLine::Removed) rems.push_back(&diff_.lines[i++]);
            while (i < diff_.lines.size() && diff_.lines[i].kind == DiffLine::Added) adds.push_back(&diff_.lines[i++]);
            for (std::size_t k = 0; k < std::max(rems.size(), adds.size()); ++k) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                if (k < rems.size()) { rowBg(rem); ImGui::Text("%4d- %s", rems[k]->oldNo, rems[k]->text.c_str()); }
                ImGui::TableNextColumn();
                if (k < adds.size()) { rowBg(add); ImGui::Text("%4d+ %s", adds[k]->newNo, adds[k]->text.c_str()); }
            }
        }
        ImGui::EndTable();
    }
    ImGui::EndChild();
    ImGui::End();
}

} // namespace ide
