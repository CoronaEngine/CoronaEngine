#include <corona/systems/network/lanchat_history_store.h>

#include <algorithm>
#include <cctype>
#include <deque>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <utility>

namespace Corona::Network {
namespace {

std::string sanitize_room_id(const std::string& room_id) {
    std::string result;
    result.reserve(room_id.size());
    for (unsigned char ch : room_id) {
        if (std::isalnum(ch) || ch == '-' || ch == '_' || ch == '.') {
            result.push_back(static_cast<char>(ch));
        } else {
            result.push_back('_');
        }
    }
    return result.empty() ? "default" : result;
}

std::string json_escape(const std::string& input) {
    std::ostringstream out;
    for (char ch : input) {
        switch (ch) {
        case '\\': out << "\\\\"; break;
        case '"': out << "\\\""; break;
        case '\b': out << "\\b"; break;
        case '\f': out << "\\f"; break;
        case '\n': out << "\\n"; break;
        case '\r': out << "\\r"; break;
        case '\t': out << "\\t"; break;
        default:
            if (static_cast<unsigned char>(ch) < 0x20) {
                out << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                    << static_cast<int>(static_cast<unsigned char>(ch));
            } else {
                out << ch;
            }
        }
    }
    return out.str();
}

std::string json_string(const std::string& value) {
    return "\"" + json_escape(value) + "\"";
}

bool read_json_string(const std::string& line, const std::string& key, std::string& out) {
    const std::string needle = "\"" + key + "\":";
    const auto key_pos = line.find(needle);
    if (key_pos == std::string::npos) return false;
    auto pos = key_pos + needle.size();
    while (pos < line.size() && std::isspace(static_cast<unsigned char>(line[pos]))) {
        ++pos;
    }
    if (pos >= line.size() || line[pos] != '"') return false;
    ++pos;

    std::string value;
    while (pos < line.size()) {
        const char ch = line[pos++];
        if (ch == '"') {
            out = std::move(value);
            return true;
        }
        if (ch != '\\') {
            value.push_back(ch);
            continue;
        }
        if (pos >= line.size()) return false;
        const char escaped = line[pos++];
        switch (escaped) {
        case '"': value.push_back('"'); break;
        case '\\': value.push_back('\\'); break;
        case '/': value.push_back('/'); break;
        case 'b': value.push_back('\b'); break;
        case 'f': value.push_back('\f'); break;
        case 'n': value.push_back('\n'); break;
        case 'r': value.push_back('\r'); break;
        case 't': value.push_back('\t'); break;
        case 'u':
            if (pos + 4 > line.size()) return false;
            pos += 4;
            break;
        default:
            value.push_back(escaped);
            break;
        }
    }
    return false;
}

bool read_json_u64(const std::string& line, const std::string& key, uint64_t& out) {
    const std::string needle = "\"" + key + "\":";
    const auto key_pos = line.find(needle);
    if (key_pos == std::string::npos) return false;
    auto pos = key_pos + needle.size();
    while (pos < line.size() && std::isspace(static_cast<unsigned char>(line[pos]))) {
        ++pos;
    }
    const auto start = pos;
    while (pos < line.size() && std::isdigit(static_cast<unsigned char>(line[pos]))) {
        ++pos;
    }
    if (pos == start) return false;
    try {
        out = static_cast<uint64_t>(std::stoull(line.substr(start, pos - start)));
        return true;
    } catch (...) {
        return false;
    }
}

std::string message_to_json(const LanChatMessage& message) {
    std::ostringstream out;
    out << "{"
        << "\"message_id\":" << json_string(message.message_id)
        << ",\"sender_id\":" << json_string(message.sender_id)
        << ",\"sender_name\":" << json_string(message.sender_name)
        << ",\"room_id\":" << json_string(message.room_id)
        << ",\"text\":" << json_string(message.text)
        << ",\"seq\":" << message.seq
        << ",\"timestamp_ms\":" << message.timestamp_ms
        << ",\"sender_type\":" << json_string(message.sender_type)
        << ",\"message_kind\":" << json_string(message.message_kind)
        << ",\"target_agent_id\":" << json_string(message.target_agent_id)
        << ",\"source_user_id\":" << json_string(message.source_user_id)
        << ",\"correlation_id\":" << json_string(message.correlation_id)
        << ",\"metadata_json\":" << json_string(message.metadata_json)
        << "}";
    return out.str();
}

std::string agent_to_json(const LanChatAgent& agent) {
    std::ostringstream out;
    out << "{"
        << "\"agent_id\":" << json_string(agent.agent_id)
        << ",\"name\":" << json_string(agent.name)
        << ",\"persona\":" << json_string(agent.persona)
        << ",\"owner_id\":" << json_string(agent.owner_id)
        << "}";
    return out.str();
}

bool message_from_json(const std::string& line, LanChatMessage& message) {
    if (!read_json_string(line, "message_id", message.message_id) ||
        !read_json_string(line, "sender_id", message.sender_id) ||
        !read_json_string(line, "sender_name", message.sender_name) ||
        !read_json_string(line, "room_id", message.room_id) ||
        !read_json_string(line, "text", message.text) ||
        !read_json_u64(line, "seq", message.seq) ||
        !read_json_u64(line, "timestamp_ms", message.timestamp_ms)) {
        return false;
    }
    read_json_string(line, "sender_type", message.sender_type);
    read_json_string(line, "message_kind", message.message_kind);
    read_json_string(line, "target_agent_id", message.target_agent_id);
    read_json_string(line, "source_user_id", message.source_user_id);
    read_json_string(line, "correlation_id", message.correlation_id);
    read_json_string(line, "metadata_json", message.metadata_json);
    if (message.sender_type.empty()) message.sender_type = "user";
    if (message.message_kind.empty()) message.message_kind = "chat";
    return true;
}

bool agent_from_json(const std::string& line, LanChatAgent& agent) {
    if (!read_json_string(line, "agent_id", agent.agent_id) ||
        !read_json_string(line, "name", agent.name)) {
        return false;
    }
    read_json_string(line, "persona", agent.persona);
    read_json_string(line, "owner_id", agent.owner_id);
    if (agent.owner_id.empty()) {
        read_json_string(line, "owner", agent.owner_id);
    }
    return true;
}

}  // namespace

LanChatHistoryStore::LanChatHistoryStore(std::filesystem::path root)
    : root_(std::move(root)) {}

std::vector<LanChatHistoryRoomSummary> LanChatHistoryStore::list_rooms() const {
    std::vector<LanChatHistoryRoomSummary> rooms;
    std::error_code ec;
    if (!std::filesystem::exists(root_, ec) || ec) return rooms;

    for (const auto& entry : std::filesystem::directory_iterator(root_, ec)) {
        if (ec) break;
        if (!entry.is_regular_file(ec) || ec) {
            ec.clear();
            continue;
        }
        if (entry.path().extension() != ".jsonl") continue;

        std::ifstream file(entry.path(), std::ios::binary);
        if (!file) continue;

        LanChatHistoryRoomSummary summary;
        summary.session_id = entry.path().stem().string();
        summary.room_id = summary.session_id;

        std::string line;
        while (std::getline(file, line)) {
            LanChatMessage message;
            if (!message_from_json(line, message)) continue;
            if (!message.room_id.empty()) summary.room_id = message.room_id;
            summary.message_count += 1;
            summary.last_timestamp_ms = message.timestamp_ms;
            summary.last_sender_name = message.sender_name;
            summary.last_text = message.text;
        }

        if (summary.message_count > 0 && !summary.room_id.empty()) {
            rooms.push_back(std::move(summary));
        }
    }

    std::sort(rooms.begin(), rooms.end(), [](const auto& lhs, const auto& rhs) {
        if (lhs.last_timestamp_ms != rhs.last_timestamp_ms) {
            return lhs.last_timestamp_ms > rhs.last_timestamp_ms;
        }
        return lhs.room_id < rhs.room_id;
    });
    return rooms;
}

std::vector<LanChatMessage> LanChatHistoryStore::load_room(
    const std::string& room_id,
    size_t max_messages) const {
    std::ifstream file(resolve_room_file(room_id), std::ios::binary);
    if (!file) return {};

    std::deque<LanChatMessage> retained;
    std::string line;
    while (std::getline(file, line)) {
        LanChatMessage message;
        if (!message_from_json(line, message)) continue;
        retained.push_back(std::move(message));
        while (max_messages > 0 && retained.size() > max_messages) {
            retained.pop_front();
        }
    }
    return {retained.begin(), retained.end()};
}

void LanChatHistoryStore::append_message(
    const std::string& room_id,
    const LanChatMessage& message) const {
    if (message.message_id.empty()) return;
    std::error_code ec;
    std::filesystem::create_directories(root_, ec);
    if (ec) return;

    std::ofstream file(room_file(room_id), std::ios::binary | std::ios::app);
    if (!file) return;
    file << message_to_json(message) << '\n';
}

void LanChatHistoryStore::save_agents(
    const std::string& room_id,
    const std::vector<LanChatAgent>& agents) const {
    if (room_id.empty()) return;
    std::error_code ec;
    std::filesystem::create_directories(root_, ec);
    if (ec) return;

    std::ofstream file(agents_file(room_id), std::ios::binary | std::ios::trunc);
    if (!file) return;
    for (const auto& agent : agents) {
        if (agent.agent_id.empty() || agent.name.empty()) continue;
        file << agent_to_json(agent) << '\n';
    }
}

std::vector<LanChatAgent> LanChatHistoryStore::load_agents(
    const std::string& room_id) const {
    std::ifstream file(resolve_agents_file(room_id), std::ios::binary);
    if (!file) return {};

    std::vector<LanChatAgent> agents;
    std::string line;
    while (std::getline(file, line)) {
        LanChatAgent agent;
        if (!agent_from_json(line, agent)) continue;
        agents.push_back(std::move(agent));
    }
    return agents;
}

std::filesystem::path LanChatHistoryStore::room_file(const std::string& room_id) const {
    return root_ / (sanitize_room_id(room_id) + ".jsonl");
}

std::filesystem::path LanChatHistoryStore::agents_file(const std::string& room_id) const {
    return root_ / (sanitize_room_id(room_id) + ".agents.jsonl");
}

std::filesystem::path LanChatHistoryStore::resolve_room_file(
    const std::string& room_or_session_id) const {
    const auto exact = room_file(room_or_session_id);
    std::error_code ec;
    if (std::filesystem::exists(exact, ec) && !ec) {
        return exact;
    }

    for (const auto& room : list_rooms()) {
        if (room.room_id == room_or_session_id) {
            return room_file(room.session_id);
        }
    }
    return exact;
}

std::filesystem::path LanChatHistoryStore::resolve_agents_file(
    const std::string& room_or_session_id) const {
    const auto exact = agents_file(room_or_session_id);
    std::error_code ec;
    if (std::filesystem::exists(exact, ec) && !ec) {
        return exact;
    }

    for (const auto& room : list_rooms()) {
        if (room.room_id == room_or_session_id) {
            return agents_file(room.session_id);
        }
    }
    return exact;
}

}  // namespace Corona::Network
