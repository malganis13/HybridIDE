// =============================================================================
//  DAPClient.cpp
// =============================================================================
#include "lsp/DAPClient.hpp"

#include "core/EventQueue.hpp"
#include "ui/Localization.hpp"

#include <imgui_stdlib.h>

#include <algorithm>
#include <cstdio>

namespace ide {

namespace fs = std::filesystem;

std::vector<std::uint8_t> DAPClient::base64Decode(std::string_view in) {
    static const std::string tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::vector<std::uint8_t> out;
    unsigned int val = 0; int bits = -8;
    for (char c : in) {
        auto p = tbl.find(c);
        if (p == std::string::npos) continue;   // '=' и переводы строк
        val = (val << 6) | (unsigned)p; bits += 6;
        if (bits >= 0) { out.push_back((std::uint8_t)((val >> bits) & 0xFF)); bits -= 8; }
    }
    return out;
}

bool DAPClient::start(const LaunchConfig& cfg, const fs::path& root, std::string* err) {
    stop();
    cfg_ = cfg; root_ = root;
    frames_.clear(); scopes_.clear(); vars_.clear(); requested_.clear(); threads_.clear(); console_.clear();
    ProcessOptions o; o.argv = splitCommandLine(cfg.adapter); o.cwd = root;
    if (o.argv.empty()) { if (err) *err = "debug adapter not specified"; return false; }
    if (!chan_.start(o, [this](const json& m) { handleMessage(m); },
                     [this](std::string_view s) { std::string t(s); postToMain([this, t] { console_.push_back(t); }); },
                     [this](int) { postToMain([this] { state_ = DebugState::Terminated; if (onTerminated) onTerminated(); }); }, err))
        return false;
    state_ = DebugState::Initializing;

    send("initialize", {{"clientID", "hybridide"}, {"clientName", "HybridIDE"}, {"adapterID", o.argv[0]},
                        {"linesStartAt1", true}, {"columnsStartAt1", true}, {"pathFormat", "path"},
                        {"supportsVariableType", true}, {"supportsMemoryReferences", true},
                        {"supportsRunInTerminalRequest", false}, {"locale", Localization::instance().language()}},
         [this](bool ok, const json& body, const std::string& msg) {
             if (!ok) { console_.push_back("initialize failed: " + msg); return; }
             supportsRestart_    = body.value("supportsRestartRequest", false);
             supportsReadMemory_ = body.value("supportsReadMemoryRequest", false);
             json args = cfg_.extra;
             args["program"]     = cfg_.program;
             args["args"]        = cfg_.args;
             args["cwd"]         = cfg_.cwd.empty() ? root_.u8string() : cfg_.cwd;
             args["stopOnEntry"] = cfg_.stopOnEntry;
             send(cfg_.request, args, [this](bool ok2, const json&, const std::string& m2) {
                 if (!ok2) console_.push_back("launch failed: " + m2);
                 else state_ = DebugState::Running;
             });
         });
    return true;
}

void DAPClient::stop() {
    if (chan_.running()) {
        send("disconnect", {{"terminateDebuggee", true}});
        chan_.stop();
    }
    state_ = DebugState::Idle;
}

int DAPClient::send(const std::string& command, json args, Handler h) {
    int s = seq_++;
    if (h) { std::lock_guard lk(pendMtx_); pending_[s] = std::move(h); }
    chan_.send({{"seq", s}, {"type", "request"}, {"command", command}, {"arguments", std::move(args)}});
    return s;
}

void DAPClient::handleMessage(const json& m) {
    std::string type = m.value("type", "");
    if (type == "response") {
        Handler h;
        {
            std::lock_guard lk(pendMtx_);
            auto it = pending_.find(m.value("request_seq", -1));
            if (it == pending_.end()) return;
            h = std::move(it->second);
            pending_.erase(it);
        }
        bool ok = m.value("success", false);
        json body = m.value("body", json::object());
        std::string msg = m.value("message", "");
        postToMain([h = std::move(h), ok, body = std::move(body), msg] { h(ok, body, msg); });
    } else if (type == "event") {
        std::string ev = m.value("event", "");
        json body = m.value("body", json::object());
        postToMain([this, ev, body = std::move(body)] { onEvent(ev, body); });
    } else if (type == "request") {
        // Обратные запросы адаптера (runInTerminal и др.) — отклоняем корректно
        chan_.send({{"seq", seq_++}, {"type", "response"}, {"request_seq", m.value("seq", 0)},
                    {"command", m.value("command", "")}, {"success", false}, {"message", "not supported"}});
    }
}

void DAPClient::onEvent(const std::string& ev, const json& body) {
    if (ev == "initialized") {
        sendAllBreakpoints();
        send("configurationDone", json::object());
    } else if (ev == "stopped") {
        state_ = DebugState::Stopped;
        stoppedThread_ = body.value("threadId", stoppedThread_);
        send("threads", json::object(), [this](bool ok, const json& b, const std::string&) {
            if (!ok) return;
            threads_.clear();
            for (auto& t : b.value("threads", json::array())) threads_.push_back({t.value("id", 0), t.value("name", "")});
            if (!stoppedThread_ && !threads_.empty()) stoppedThread_ = threads_.front().id;
            fetchStack(stoppedThread_);
        });
        console_.push_back("stopped: " + body.value("reason", std::string("?")) + " " + body.value("description", std::string()));
    } else if (ev == "continued") {
        state_ = DebugState::Running;
        frames_.clear(); scopes_.clear(); vars_.clear(); requested_.clear();
    } else if (ev == "output") {
        std::string out = body.value("output", "");
        console_.push_back(out);
        if (console_.size() > 4000) console_.erase(console_.begin(), console_.begin() + 1000);
        if (onOutput) onOutput(out);
    } else if (ev == "terminated" || ev == "exited") {
        state_ = DebugState::Terminated;
        if (ev == "exited") console_.push_back("process exited with code " + std::to_string(body.value("exitCode", 0)));
        frames_.clear(); scopes_.clear(); vars_.clear();
        if (onTerminated) onTerminated();
    } else if (ev == "breakpoint") {
        // Адаптер подтвердил/переместил точку останова — обновим при следующем запросе
    }
}

void DAPClient::sendBreakpoints(const std::string& path) {
    json lines = json::array();
    for (int l : bps_[path]) lines.push_back({{"line", l + 1}});
    send("setBreakpoints", {{"source", {{"path", path}, {"name", fs::u8path(path).filename().u8string()}}}, {"breakpoints", lines}},
         [this, path](bool ok, const json& b, const std::string&) {
             if (!ok) return;
             auto& v = verified_[path];
             v.clear();
             for (auto& bp : b.value("breakpoints", json::array()))
                 v.push_back({bp.value("id", 0), bp.value("verified", false), bp.value("line", 0), bp.value("message", "")});
         });
}

void DAPClient::sendAllBreakpoints() { for (auto& [p, _] : bps_) sendBreakpoints(p); }

void DAPClient::setBreakpoint(const fs::path& file, int line0, bool on) {
    std::string p = file.u8string();
    if (on) bps_[p].insert(line0); else bps_[p].erase(line0);
    if (state_ == DebugState::Running || state_ == DebugState::Stopped) sendBreakpoints(p);
}

void DAPClient::fetchStack(int threadId) {
    send("stackTrace", {{"threadId", threadId}, {"startFrame", 0}, {"levels", 64}}, [this](bool ok, const json& b, const std::string&) {
        if (!ok) return;
        frames_.clear();
        for (auto& f : b.value("stackFrames", json::array())) {
            dap::StackFrame sf;
            sf.id = f.value("id", 0); sf.name = f.value("name", ""); sf.line = f.value("line", 0); sf.column = f.value("column", 0);
            sf.instructionPointerReference = f.value("instructionPointerReference", "");
            if (f.contains("source")) { sf.source.name = f["source"].value("name", ""); sf.source.path = f["source"].value("path", ""); }
            frames_.push_back(std::move(sf));
        }
        // Первый кадр с исходником — переход редактора к строке остановки
        for (std::size_t i = 0; i < frames_.size(); ++i) {
            if (!frames_[i].source.path.empty()) {
                selectFrame(frames_[i].id);
                if (onStoppedAt) onStoppedAt(fs::u8path(frames_[i].source.path), frames_[i].line - 1);
                break;
            }
        }
        refreshWatches();
    });
}

void DAPClient::selectFrame(int frameId) {
    selectedFrame_ = frameId;
    scopes_.clear(); vars_.clear(); requested_.clear();
    send("scopes", {{"frameId", frameId}}, [this](bool ok, const json& b, const std::string&) {
        if (!ok) return;
        for (auto& s : b.value("scopes", json::array())) {
            dap::Scope sc{s.value("name", ""), s.value("variablesReference", 0), s.value("expensive", false)};
            scopes_.push_back(sc);
            if (!sc.expensive) fetchVariables(sc.variablesReference);   // ленивая загрузка «дорогих» областей (Globals/Registers)
        }
    });
}

void DAPClient::fetchVariables(int ref) {
    if (ref <= 0 || requested_.count(ref)) return;
    requested_.insert(ref);
    send("variables", {{"variablesReference", ref}}, [this, ref](bool ok, const json& b, const std::string&) {
        if (!ok) return;
        auto& out = vars_[ref];
        out.clear();
        for (auto& v : b.value("variables", json::array()))
            out.push_back({v.value("name", ""), v.value("value", ""), v.value("type", ""), v.value("evaluateName", ""),
                           v.value("memoryReference", ""), v.value("variablesReference", 0)});
    });
}

void DAPClient::evaluate(const std::string& expr, const std::string& context, std::function<void(std::string)> cb) {
    if (state_ != DebugState::Stopped) return;
    send("evaluate", {{"expression", expr}, {"frameId", selectedFrame_}, {"context", context}},
         [cb](bool ok, const json& b, const std::string& msg) { cb(ok ? b.value("result", "") : "<" + msg + ">"); });
}

void DAPClient::refreshWatches() {
    for (std::size_t i = 0; i < watches_.size(); ++i)
        evaluate(watches_[i].expr, "watch", [this, i](std::string r) { if (i < watches_.size()) watches_[i].value = std::move(r); });
}

void DAPClient::readMemory(const std::string& ref, int offset, int count) {
    if (!supportsReadMemory_) { memError_ = "adapter does not support readMemory"; return; }
    memRef_ = ref;
    send("readMemory", {{"memoryReference", ref}, {"offset", offset}, {"count", count}}, [this](bool ok, const json& b, const std::string& msg) {
        if (!ok) { memError_ = msg; return; }
        memError_.clear();
        std::string addr = b.value("address", "0");
        memBase_ = std::strtoull(addr.c_str(), nullptr, 0);
        memory_ = base64Decode(b.value("data", ""));
    });
}

void DAPClient::continue_() { send("continue", {{"threadId", stoppedThread_}}); state_ = DebugState::Running; }
void DAPClient::next()      { send("next", {{"threadId", stoppedThread_}}); }
void DAPClient::stepIn()    { send("stepIn", {{"threadId", stoppedThread_}}); }
void DAPClient::stepOut()   { send("stepOut", {{"threadId", stoppedThread_}}); }
void DAPClient::pause()     { send("pause", {{"threadId", stoppedThread_}}); }
void DAPClient::restart() {
    if (supportsRestart_) send("restart", json::object());
    else { auto c = cfg_; auto r = root_; stop(); start(c, r); }
}

// ------------------------------ Визуализаторы --------------------------------
void DAPClient::renderCallStack(const char* title, bool* open) {
    if (!ImGui::Begin(title, open)) { ImGui::End(); return; }
    auto& L = Localization::instance();
    if (frames_.empty()) ImGui::TextDisabled("%s", state_ == DebugState::Running ? L.tr("debug.running") : L.tr("debug.not_stopped"));
    if (!threads_.empty()) {
        ImGui::SetNextItemWidth(-1);
        std::string cur = "Thread " + std::to_string(stoppedThread_);
        if (ImGui::BeginCombo("##thread", cur.c_str())) {
            for (auto& t : threads_) if (ImGui::Selectable((std::to_string(t.id) + ": " + t.name).c_str(), t.id == stoppedThread_)) { stoppedThread_ = t.id; fetchStack(t.id); }
            ImGui::EndCombo();
        }
    }
    for (auto& f : frames_) {
        ImGui::PushID(f.id);
        std::string label = f.name + "  " + f.source.name + ":" + std::to_string(f.line);
        if (ImGui::Selectable(label.c_str(), f.id == selectedFrame_)) {
            selectFrame(f.id);
            if (!f.source.path.empty() && onStoppedAt) onStoppedAt(fs::u8path(f.source.path), f.line - 1);
        }
        ImGui::PopID();
    }
    ImGui::End();
}

void DAPClient::drawVariable(const dap::Variable& v, int depth) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::PushID(&v);
    bool openNode = false;
    if (v.variablesReference > 0) {
        openNode = ImGui::TreeNodeEx(v.name.c_str(), ImGuiTreeNodeFlags_SpanFullWidth);
        if (openNode) fetchVariables(v.variablesReference);
    } else ImGui::TreeNodeEx(v.name.c_str(), ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen | ImGuiTreeNodeFlags_SpanFullWidth);
    if (ImGui::BeginPopupContextItem("##varctx")) {
        if (ImGui::MenuItem("Add to Watch")) addWatch(v.evaluateName.empty() ? v.name : v.evaluateName);
        if (!v.memoryReference.empty() && ImGui::MenuItem("View Memory")) { memAddr_ = v.memoryReference; readMemory(v.memoryReference, 0, 256); }
        if (ImGui::MenuItem("Copy Value")) ImGui::SetClipboardText(v.value.c_str());
        ImGui::EndPopup();
    }
    ImGui::TableNextColumn(); ImGui::TextUnformatted(v.value.c_str());
    ImGui::TableNextColumn(); ImGui::TextDisabled("%s", v.type.c_str());
    if (openNode) {
        if (auto it = vars_.find(v.variablesReference); it != vars_.end() && depth < 32)
            for (auto& c : it->second) drawVariable(c, depth + 1);
        ImGui::TreePop();
    }
    ImGui::PopID();
}

void DAPClient::renderVariables(const char* title, bool* open) {
    if (!ImGui::Begin(title, open)) { ImGui::End(); return; }
    auto& L = Localization::instance();
    if (scopes_.empty()) ImGui::TextDisabled("%s", L.tr("debug.not_stopped"));
    if (ImGui::BeginTabBar("##scopes")) {
        for (auto& sc : scopes_) {
            if (!ImGui::BeginTabItem(sc.name.c_str())) continue;   // Locals / Globals / Registers
            fetchVariables(sc.variablesReference);
            if (ImGui::BeginTable("##vars", 3, ImGuiTableFlags_Resizable | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerV)) {
                ImGui::TableSetupColumn(L.tr("debug.name"));
                ImGui::TableSetupColumn(L.tr("debug.value"));
                ImGui::TableSetupColumn(L.tr("debug.type"), ImGuiTableColumnFlags_WidthFixed, 120);
                ImGui::TableHeadersRow();
                if (auto it = vars_.find(sc.variablesReference); it != vars_.end())
                    for (auto& v : it->second) drawVariable(v, 0);
                ImGui::EndTable();
            }
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::End();
}

void DAPClient::renderWatch(const char* title, bool* open) {
    if (!ImGui::Begin(title, open)) { ImGui::End(); return; }
    auto& L = Localization::instance();
    ImGui::SetNextItemWidth(-60);
    bool add = ImGui::InputTextWithHint("##w", L.tr("debug.add_watch"), &watchInput_, ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if ((ImGui::Button("+") || add) && !watchInput_.empty()) { addWatch(watchInput_); watchInput_.clear(); }
    if (ImGui::BeginTable("##watch", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable)) {
        for (std::size_t i = 0; i < watches_.size(); ++i) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(watches_[i].expr.c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(watches_[i].value.c_str());
            ImGui::TableNextColumn();
            ImGui::PushID((int)i);
            if (ImGui::SmallButton("x")) { watches_.erase(watches_.begin() + (long)i); ImGui::PopID(); break; }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::End();
}

void DAPClient::renderBreakpoints(const char* title, bool* open) {
    if (!ImGui::Begin(title, open)) { ImGui::End(); return; }
    auto& L = Localization::instance();
    if (ImGui::SmallButton(L.tr("debug.remove_all"))) {
        auto paths = bps_;
        for (auto& [p, _] : paths) { bps_[p].clear(); if (state_ == DebugState::Running || state_ == DebugState::Stopped) sendBreakpoints(p); }
    }
    for (auto& [path, lines] : bps_) {
        for (int l : lines) {
            bool ver = false;
            for (auto& vb : verified_[path]) if (vb.line == l + 1 && vb.verified) ver = true;
            ImGui::TextColored(ver ? ImVec4(1, .3f, .3f, 1) : ImVec4(.6f, .4f, .4f, 1), "%s", ver ? "(*)" : "( )");
            ImGui::SameLine();
            ImGui::Text("%s:%d", fs::u8path(path).filename().u8string().c_str(), l + 1);
            if (ImGui::IsItemClicked() && onStoppedAt) {}   // навигация обрабатывается приложением через Problems-подобный клик
        }
    }
    ImGui::End();
}

void DAPClient::renderMemory(const char* title, bool* open) {
    if (!ImGui::Begin(title, open)) { ImGui::End(); return; }
    auto& L = Localization::instance();
    ImGui::SetNextItemWidth(220);
    bool go = ImGui::InputTextWithHint("##addr", L.tr("debug.mem_address"), &memAddr_, ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if ((ImGui::Button(L.tr("debug.read")) || go) && !memAddr_.empty()) readMemory(memAddr_, 0, 512);
    if (!memError_.empty()) ImGui::TextColored(ImVec4(1, .4f, .4f, 1), "%s", memError_.c_str());
    ImGui::Separator();
    // Классический hex-дамп: адрес | 16 байт | ASCII
    ImGui::BeginChild("##hex");
    for (std::size_t row = 0; row < memory_.size(); row += 16) {
        char line[128]; int n = std::snprintf(line, sizeof(line), "%016llx  ", (unsigned long long)(memBase_ + row));
        for (std::size_t i = 0; i < 16; ++i)
            n += row + i < memory_.size() ? std::snprintf(line + n, sizeof(line) - (std::size_t)n, "%02X ", memory_[row + i]) : std::snprintf(line + n, sizeof(line) - (std::size_t)n, "   ");
        n += std::snprintf(line + n, sizeof(line) - (std::size_t)n, " ");
        for (std::size_t i = 0; i < 16 && row + i < memory_.size(); ++i) {
            unsigned char c = memory_[row + i];
            line[n++] = (c >= 32 && c < 127) ? (char)c : '.';
        }
        line[n] = 0;
        ImGui::TextUnformatted(line);
    }
    ImGui::EndChild();
    ImGui::End();
}

void DAPClient::renderConsole(const char* title, bool* open) {
    if (!ImGui::Begin(title, open)) { ImGui::End(); return; }
    auto& L = Localization::instance();
    // Панель управления отладкой
    bool stopped = state_ == DebugState::Stopped;
    { if (ImGui::Button(L.tr("debug.continue")) && stopped) continue_(); } ImGui::SameLine();
    { if (ImGui::Button(L.tr("debug.step_over")) && stopped) next(); } ImGui::SameLine();
    { if (ImGui::Button(L.tr("debug.step_in")) && stopped) stepIn(); } ImGui::SameLine();
    { if (ImGui::Button(L.tr("debug.step_out")) && stopped) stepOut(); } ImGui::SameLine();
    { if (ImGui::Button(L.tr("debug.pause")) && state_ == DebugState::Running) pause(); } ImGui::SameLine();
    if (ImGui::Button(L.tr("debug.stop"))) stop();
    ImGui::Separator();
    float h = ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing();
    ImGui::BeginChild("##dcons", ImVec2(0, h), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);
    for (auto& l : console_) ImGui::TextUnformatted(l.c_str());
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 10) ImGui::SetScrollHereY(1.f);
    ImGui::EndChild();
    ImGui::SetNextItemWidth(-1);
    if (ImGui::InputTextWithHint("##repl", L.tr("debug.evaluate"), &consoleInput_, ImGuiInputTextFlags_EnterReturnsTrue) && !consoleInput_.empty()) {
        std::string e = consoleInput_;
        console_.push_back("> " + e);
        evaluate(e, "repl", [this](std::string r) { console_.push_back(r); });
        consoleInput_.clear();
        ImGui::SetKeyboardFocusHere(-1);
    }
    ImGui::End();
}

} // namespace ide
