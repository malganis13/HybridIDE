// =============================================================================
//  JsonRpc.cpp
// =============================================================================
#include "lsp/JsonRpc.hpp"

#include <cctype>
#include <cstdlib>

namespace ide {

std::vector<std::string> MessageFramer::feed(std::string_view chunk) {
    std::vector<std::string> out;
    buf_.append(chunk);
    for (;;) {
        auto hdrEnd = buf_.find("\r\n\r\n");
        if (hdrEnd == std::string::npos) break;
        // Ищем Content-Length без учёта регистра
        std::size_t len = 0;
        bool found = false;
        std::size_t pos = 0;
        while (pos < hdrEnd) {
            auto eol = buf_.find("\r\n", pos);
            if (eol == std::string::npos || eol > hdrEnd) eol = hdrEnd;
            std::string line = buf_.substr(pos, eol - pos);
            std::string lower = line;
            for (auto& c : lower) c = (char)std::tolower((unsigned char)c);
            if (lower.rfind("content-length:", 0) == 0) { len = (std::size_t)std::strtoull(line.c_str() + 15, nullptr, 10); found = true; }
            pos = eol + 2;
        }
        if (!found) { buf_.erase(0, hdrEnd + 4); continue; }   // мусорный заголовок — пропускаем
        std::size_t bodyStart = hdrEnd + 4;
        if (buf_.size() < bodyStart + len) break;               // тело ещё не пришло полностью
        out.push_back(buf_.substr(bodyStart, len));
        buf_.erase(0, bodyStart + len);
    }
    return out;
}

std::string MessageFramer::frame(const std::string& body) {
    return "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
}

bool JsonChannel::start(const ProcessOptions& opts, MessageHandler onMessage, std::function<void(std::string_view)> onStderr,
                        std::function<void(int)> onExit, std::string* err) {
    proc_ = std::make_unique<Process>();
    bool ok = proc_->start(opts,
        [this, onMessage](std::string_view d) {
            std::vector<std::string> msgs;
            {
                std::lock_guard lk(framerMtx_);
                msgs = framer_.feed(d);
            }
            for (auto& m : msgs) {
                try { onMessage(json::parse(m)); }
                catch (const std::exception&) { /* некорректный JSON от сервера игнорируем */ }
            }
        },
        [onStderr](std::string_view d) { if (onStderr) onStderr(d); },
        [onExit](int c) { if (onExit) onExit(c); });
    if (!ok && err) *err = proc_->lastError();
    return ok;
}

bool JsonChannel::send(const json& msg) {
    if (!proc_) return false;
    return proc_->write(MessageFramer::frame(msg.dump(-1, ' ', false, json::error_handler_t::replace)));
}

void JsonChannel::stop() {
    if (proc_) { proc_->terminate(); proc_.reset(); }
}

} // namespace ide
