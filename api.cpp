#include "api.h"

#include <charconv>
#include <iostream>
#include <map>
#include <string>
#include <utility>

namespace {

using Fields = std::map<std::string, std::string, std::less<>>;

int hexDigit(char ch) {
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return ch - 'a' + 10;
    }
    if (ch >= 'A' && ch <= 'F') {
        return ch - 'A' + 10;
    }
    return -1;
}

std::optional<std::string> decode(std::string_view encoded) {
    std::string decoded;
    decoded.reserve(encoded.size());
    for (std::size_t i = 0; i < encoded.size(); ++i) {
        const char ch = encoded[i];
        if (ch == '+') {
            decoded += ' ';
        } else if (ch == '%') {
            if (encoded.size() - i < 3) {
                return std::nullopt;
            }
            const int high = hexDigit(encoded[i + 1]);
            const int low = hexDigit(encoded[i + 2]);
            if (high < 0 || low < 0) {
                return std::nullopt;
            }
            decoded += static_cast<char>(high * 16 + low);
            i += 2;
        } else {
            decoded += ch;
        }
    }
    return decoded;
}

std::optional<Fields> readFields(std::string_view input) {
    Fields fields;
    while (!input.empty()) {
        const auto separator = input.find('&');
        const auto field = input.substr(0, separator);
        const auto equals = field.find('=');
        if (equals == std::string_view::npos || fields.size() == 12) {
            return std::nullopt;
        }

        auto key = decode(field.substr(0, equals));
        auto value = decode(field.substr(equals + 1));
        if (!key || key->empty() || !value ||
            !fields.emplace(std::move(*key), std::move(*value)).second) {
            return std::nullopt;
        }

        if (separator == std::string_view::npos) {
            break;
        }
        input.remove_prefix(separator + 1);
    }
    return fields;
}

template <typename Number>
bool readNumber(std::string_view text, Number& value) {
    if (text.empty()) {
        return false;
    }
    const auto end = text.data() + text.size();
    const auto result = std::from_chars(text.data(), end, value);
    return result.ec == std::errc{} && result.ptr == end;
}

std::string readFilter(const Fields& fields, TaskFilter& filter) {
    for (const auto& [key, value] : fields) {
        if (key == "q") {
            if (value.size() > 120) {
                return "Search text is too long.";
            }
            filter.search = value;
        } else if (key == "state") {
            filter.state = parseState(value);
            if (!filter.state) {
                return "State must be todo, doing, or done.";
            }
        } else if (key == "priority") {
            filter.priority = parsePriority(value);
            if (!filter.priority) {
                return "Priority must be low, normal, or high.";
            }
        } else if (key == "sort") {
            if (value == "oldest") {
                filter.order = SortOrder::oldest;
            } else if (value == "priority") {
                filter.order = SortOrder::priority;
            } else if (value == "title") {
                filter.order = SortOrder::title;
            } else if (value != "newest") {
                return "Sort by newest, oldest, priority, or title.";
            }
        } else if (key == "limit") {
            if (!readNumber(value, filter.limit) ||
                filter.limit < 1 || filter.limit > 100) {
                return "Limit must be between 1 and 100.";
            }
        } else if (key == "offset") {
            if (!readNumber(value, filter.offset)) {
                return "Offset must be a nonnegative integer.";
            }
        } else {
            return "Unknown task filter.";
        }
    }
    return {};
}

std::string readDraft(const Fields& fields, TaskDraft& draft) {
    for (const auto& [key, value] : fields) {
        if (key == "title") {
            draft.title = value;
        } else if (key == "notes") {
            draft.notes = value;
        } else if (key == "state") {
            const auto state = parseState(value);
            if (!state) {
                return "State must be todo, doing, or done.";
            }
            draft.state = *state;
        } else if (key == "priority") {
            const auto priority = parsePriority(value);
            if (!priority) {
                return "Priority must be low, normal, or high.";
            }
            draft.priority = *priority;
        } else {
            return "Unknown task field.";
        }
    }
    if (!fields.count("title")) {
        return "Title is required.";
    }
    return {};
}

std::string jsonString(std::string_view value) {
    constexpr std::string_view hex = "0123456789abcdef";
    std::string result = "\"";
    for (const unsigned char ch : value) {
        switch (ch) {
            case '"': result += "\\\""; break;
            case '\\': result += "\\\\"; break;
            case '\n': result += "\\n"; break;
            case '\r': result += "\\r"; break;
            case '\t': result += "\\t"; break;
            default:
                if (ch < 32) {
                    result += "\\u00";
                    result += hex[ch >> 4];
                    result += hex[ch & 15];
                } else {
                    result += static_cast<char>(ch);
                }
        }
    }
    result += '"';
    return result;
}

std::string taskJson(const Task& task) {
    return "{\"id\":\"" + std::to_string(task.id) + "\",\"title\":" +
           jsonString(task.draft.title) + ",\"notes\":" +
           jsonString(task.draft.notes) + ",\"state\":" +
           jsonString(stateName(task.draft.state)) + ",\"priority\":" +
           jsonString(priorityName(task.draft.priority)) + "}";
}

Response json(std::string_view status, std::string body,
              std::string extra = {}) {
    body += '\n';
    return {status, "application/json; charset=utf-8", std::move(body),
            "Cache-Control: no-store\r\n" + extra};
}

Response apiError(std::string_view status, std::string_view reason,
                  std::string extra = {}) {
    return json(status, "{\"error\":" + jsonString(reason) + "}", std::move(extra));
}

Response taskList(TaskStore& store, std::string_view query) {
    const auto fields = readFields(query);
    if (!fields) {
        return apiError("400 Bad Request", "Malformed query parameters.");
    }

    TaskFilter filter;
    const auto error = readFilter(*fields, filter);
    if (!error.empty()) {
        return apiError("400 Bad Request", error);
    }

    const TaskPage result = store.list(filter);
    std::string data = "{\"total\":" + std::to_string(result.total) +
                       ",\"offset\":" + std::to_string(filter.offset) +
                       ",\"limit\":" + std::to_string(filter.limit) +
                       ",\"items\":[";
    for (std::size_t i = 0; i < result.items.size(); ++i) {
        if (i != 0) {
            data += ',';
        }
        data += taskJson(result.items[i]);
    }
    data += "]}";
    return json("200 OK", std::move(data));
}

Response taskCounts(TaskStore& store) {
    const auto counts = store.summary();
    const auto& state = counts.byState;
    const auto& priority = counts.byPriority;
    return json("200 OK", "{\"total\":" + std::to_string(counts.total) +
        ",\"todo\":" + std::to_string(state[0]) +
        ",\"doing\":" + std::to_string(state[1]) +
        ",\"done\":" + std::to_string(state[2]) +
        ",\"low\":" + std::to_string(priority[0]) +
        ",\"normal\":" + std::to_string(priority[1]) +
        ",\"high\":" + std::to_string(priority[2]) + "}");
}

Response storeReply(const StoreResult& result, bool created) {
    switch (result.status) {
        case StoreStatus::ok:
            if (created) {
                return json("201 Created", taskJson(*result.task),
                    "Location: /api/tasks/" + std::to_string(result.task->id) + "\r\n");
            }
            if (result.task) {
                return json("200 OK", taskJson(*result.task));
            }
            return json("200 OK", "{\"deleted\":true}");
        case StoreStatus::invalid:
            return apiError("422 Unprocessable Content", result.message);
        case StoreStatus::missing:
            return apiError("404 Not Found", result.message);
        case StoreStatus::full:
            return apiError("409 Conflict", result.message);
        case StoreStatus::ioError:
            std::cerr << "Task storage error: " << result.message << '\n';
            break;
    }
    return apiError("500 Internal Server Error", "Could not save the task board.");
}

Response writeTask(TaskStore& store, const Request& request, std::string_view body,
                   std::optional<std::uint64_t> id) {
    const std::string_view supported = "application/x-www-form-urlencoded";
    const auto type = request.options.contentType;
    const auto media = type.substr(0, type.find(';'));
    if (media != supported) {
        return apiError("415 Unsupported Media Type", "Send URL-encoded form data.");
    }

    const auto fields = readFields(body);
    if (!fields) {
        return apiError("400 Bad Request", "Malformed form data.");
    }
    TaskDraft draft;
    const auto error = readDraft(*fields, draft);
    if (!error.empty()) {
        return apiError("422 Unprocessable Content", error);
    }

    if (id) {
        return storeReply(store.update(*id, std::move(draft)), false);
    }
    return storeReply(store.create(std::move(draft)), true);
}

} // namespace

Response routeTasks(TaskStore& store, const Request& request, std::string_view body) {
    const auto method = request.method;
    const auto path = request.path;
    const bool reading = method == "GET" || method == "HEAD";

    const auto host = request.options.host;
    const auto origin = request.options.origin;
    if (!reading && !origin.empty() && origin != std::string("http://") +
                                                  std::string(host)) {
        return apiError("403 Forbidden", "Open the board from this server.");
    }

    if (path == "/api/tasks") {
        if (reading) {
            return taskList(store, request.options.query);
        }
        if (method == "POST") {
            return writeTask(store, request, body, std::nullopt);
        }
        return apiError("405 Method Not Allowed", "Use GET, HEAD, or POST.",
                        "Allow: GET, HEAD, POST\r\n");
    }
    if (path == "/api/summary") {
        if (reading) {
            return taskCounts(store);
        }
        return apiError("405 Method Not Allowed", "Use GET or HEAD.",
                        "Allow: GET, HEAD\r\n");
    }

    constexpr std::string_view itemPrefix = "/api/tasks/";
    std::uint64_t id = 0;
    if (!readNumber(path.substr(itemPrefix.size()), id) || id == 0) {
        return apiError("404 Not Found", "Task does not exist.");
    }

    if (reading) {
        const auto record = store.get(id);
        if (!record) {
            return apiError("404 Not Found", "Task does not exist.");
        }
        return json("200 OK", taskJson(*record));
    }
    if (method == "PUT") {
        return writeTask(store, request, body, id);
    }
    if (method == "DELETE") {
        return storeReply(store.erase(id), false);
    }

    return apiError("405 Method Not Allowed", "Use GET, HEAD, PUT, or DELETE.",
                    "Allow: GET, HEAD, PUT, DELETE\r\n");
}
