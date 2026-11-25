#include "tasks.h"

#include <algorithm>
#include <fstream>
#include <iomanip>
#include <limits>
#include <utility>

namespace {

constexpr std::size_t taskLimit = 500;
constexpr std::size_t maxDatabaseBytes = 2 * 1024 * 1024;
constexpr std::string_view magic = "TASKS-1";
constexpr std::array<std::string_view, 3> states{"todo", "doing", "done"};
constexpr std::array<std::string_view, 3> priorities{"low", "normal", "high"};

bool validText(std::string_view text, bool multiline) {
    std::size_t pos = 0;
    while (pos < text.size()) {
        const auto first = static_cast<unsigned char>(text[pos++]);
        if (first < 0x80) {
            if (first == 0x7F || (first < 0x20 &&
                    !(multiline && (first == '\n' || first == '\t')))) {
                return false;
            }
            continue;
        }

        unsigned int value = 0;
        unsigned int minimum = 0;
        std::size_t continuation = 0;
        if (first >= 0xC2 && first <= 0xDF) {
            value = first & 0x1F;
            minimum = 0x80;
            continuation = 1;
        } else if (first >= 0xE0 && first <= 0xEF) {
            value = first & 0x0F;
            minimum = 0x800;
            continuation = 2;
        } else if (first >= 0xF0 && first <= 0xF4) {
            value = first & 0x07;
            minimum = 0x10000;
            continuation = 3;
        } else {
            return false;
        }

        if (text.size() - pos < continuation) {
            return false;
        }
        for (std::size_t i = 0; i < continuation; ++i) {
            const auto byte = static_cast<unsigned char>(text[pos++]);
            if ((byte & 0xC0) != 0x80) {
                return false;
            }
            value = (value << 6) | (byte & 0x3F);
        }

        if (value < minimum || value > 0x10FFFF ||
            (value >= 0xD800 && value <= 0xDFFF)) {
            return false;
        }
    }
    return true;
}

std::string validate(TaskDraft& draft) {
    const auto first = draft.title.find_first_not_of(' ');
    if (first == std::string::npos) {
        return "Title is required.";
    }

    const auto last = draft.title.find_last_not_of(' ');
    draft.title = draft.title.substr(first, last - first + 1);
    if (draft.title.size() > 120 || !validText(draft.title, false)) {
        return "Use a title of up to 120 UTF-8 bytes on one line.";
    }

    if (draft.notes.size() > 1200 || !validText(draft.notes, true)) {
        return "Use up to 1200 UTF-8 bytes of notes.";
    }
    return {};
}

std::string lowerAscii(std::string_view value) {
    std::string result(value);
    for (char& c : result) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c + ('a' - 'A'));
        }
    }
    return result;
}

bool matches(const Task& task, const TaskFilter& filter,
             const std::string& search) {
    if (filter.state && task.draft.state != *filter.state) {
        return false;
    }
    if (filter.priority && task.draft.priority != *filter.priority) {
        return false;
    }
    return search.empty() ||
           lowerAscii(task.draft.title).find(search) != std::string::npos ||
           lowerAscii(task.draft.notes).find(search) != std::string::npos;
}

bool comesBefore(const Task& a, const Task& b, SortOrder order) {
    switch (order) {
        case SortOrder::oldest:
            return a.id < b.id;
        case SortOrder::priority:
            if (a.draft.priority != b.draft.priority) {
                return a.draft.priority > b.draft.priority;
            }
            break;
        case SortOrder::title: {
            const auto left = lowerAscii(a.draft.title);
            const auto right = lowerAscii(b.draft.title);
            if (left != right) {
                return left < right;
            }
            break;
        }
        case SortOrder::newest:
            break;
    }
    return a.id > b.id;
}

template <typename Tasks>
auto findTask(Tasks& tasks, std::uint64_t id) {
    return std::find_if(tasks.begin(), tasks.end(), [id](const Task& task) {
        return task.id == id;
    });
}

} // namespace

std::string_view stateName(TaskState state) {
    return states.at(static_cast<std::size_t>(state));
}

std::string_view priorityName(Priority priority) {
    return priorities.at(static_cast<std::size_t>(priority));
}

std::optional<TaskState> parseState(std::string_view value) {
    const auto at = std::find(states.begin(), states.end(), value);
    if (at == states.end()) {
        return std::nullopt;
    }
    return static_cast<TaskState>(at - states.begin());
}

std::optional<Priority> parsePriority(std::string_view value) {
    const auto at = std::find(priorities.begin(), priorities.end(), value);
    if (at == priorities.end()) {
        return std::nullopt;
    }
    return static_cast<Priority>(at - priorities.begin());
}

TaskStore::TaskStore(std::filesystem::path path)
    : path_(std::move(path)) {}

bool TaskStore::load(std::string& error) {
    const std::lock_guard<std::mutex> lock(mutex_);
    std::error_code fsError;
    const bool exists = std::filesystem::exists(path_, fsError);
    if (fsError) {
        error = "Inspect task database: " + fsError.message();
        return false;
    }
    if (!exists) {
        current_ = {};
        return true;
    }

    const auto size = std::filesystem::file_size(path_, fsError);
    if (fsError || size > maxDatabaseBytes) {
        error = "Could not read task database size or database is too large.";
        return false;
    }

    std::ifstream file(path_, std::ios::binary);
    std::string header;
    std::size_t count = 0;
    Snapshot candidate;
    if (!(file >> header >> candidate.nextId >> count) ||
        header != magic || candidate.nextId == 0 || count > taskLimit) {
        error = "Bad task database header.";
        return false;
    }

    candidate.tasks.reserve(count);
    std::uint64_t previous = 0;
    for (std::size_t i = 0; i < count; ++i) {
        Task task{};
        std::string state;
        std::string priority;
        if (!(file >> task.id >> state >> priority >>
              std::quoted(task.draft.title) >> std::quoted(task.draft.notes))) {
            error = "Task database ended in the middle of a record.";
            return false;
        }

        const auto parsedState = parseState(state);
        const auto parsedPriority = parsePriority(priority);
        if (!parsedState || !parsedPriority || task.id <= previous ||
            task.id >= candidate.nextId || !validate(task.draft).empty()) {
            error = "Invalid task database record.";
            return false;
        }
        task.draft.state = *parsedState;
        task.draft.priority = *parsedPriority;
        previous = task.id;
        candidate.tasks.push_back(std::move(task));
    }

    file >> std::ws;
    if (!file.eof() || file.bad()) {
        error = "Unexpected data at the end of the task database.";
        return false;
    }

    current_ = std::move(candidate);
    return true;
}

bool TaskStore::persist(const Snapshot& candidate, std::string& error) const {
    auto temporary = path_;
    temporary += ".tmp";
    std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
    if (!file) {
        error = "Could not open the temporary task database.";
        return false;
    }

    file << magic << ' ' << candidate.nextId << ' '
         << candidate.tasks.size() << '\n';
    for (const auto& task : candidate.tasks) {
        file << task.id << ' ' << stateName(task.draft.state) << ' '
             << priorityName(task.draft.priority) << ' '
             << std::quoted(task.draft.title) << ' '
             << std::quoted(task.draft.notes) << '\n';
    }
    file.close();

    std::error_code fsError;
    if (file) {
        std::filesystem::rename(temporary, path_, fsError);
        if (!fsError) {
            return true;
        }
        error = "Replace task database: " + fsError.message();
    } else {
        error = "Could not write the temporary task database.";
    }

    std::filesystem::remove(temporary, fsError);
    return false;
}

TaskPage TaskStore::list(const TaskFilter& filter) const {
    std::vector<Task> snapshot;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        snapshot = current_.tasks;
    }

    const std::string search = lowerAscii(filter.search);
    snapshot.erase(
        std::remove_if(snapshot.begin(), snapshot.end(), [&](const Task& task) {
            return !matches(task, filter, search);
        }), snapshot.end());

    std::sort(snapshot.begin(), snapshot.end(), [&](const Task& a, const Task& b) {
        return comesBefore(a, b, filter.order);
    });

    const auto total = snapshot.size();
    const auto first = std::min(filter.offset, total);
    const auto count = std::min(filter.limit, total - first);
    using Difference = std::vector<Task>::difference_type;
    const auto start = snapshot.begin() + static_cast<Difference>(first);
    const auto end = start + static_cast<Difference>(count);
    return {total, {start, end}};
}

std::optional<Task> TaskStore::get(std::uint64_t id) const {
    const std::lock_guard<std::mutex> lock(mutex_);
    const auto at = findTask(current_.tasks, id);
    if (at == current_.tasks.end()) {
        return std::nullopt;
    }
    return *at;
}

TaskSummary TaskStore::summary() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    TaskSummary counts;
    counts.total = current_.tasks.size();
    for (const auto& task : current_.tasks) {
        ++counts.byState.at(static_cast<std::size_t>(task.draft.state));
        ++counts.byPriority.at(static_cast<std::size_t>(task.draft.priority));
    }
    return counts;
}

StoreResult TaskStore::create(TaskDraft draft) {
    std::string error = validate(draft);
    if (!error.empty()) {
        return {StoreStatus::invalid, {}, std::move(error)};
    }

    const std::lock_guard<std::mutex> lock(mutex_);
    if (current_.tasks.size() == taskLimit ||
        current_.nextId == std::numeric_limits<std::uint64_t>::max()) {
        return {StoreStatus::full, {}, "The board reached its task or ID limit."};
    }

    Snapshot candidate = current_;
    Task task{candidate.nextId++, std::move(draft)};
    candidate.tasks.push_back(task);
    if (!persist(candidate, error)) {
        return {StoreStatus::ioError, {}, std::move(error)};
    }

    current_ = std::move(candidate);
    return {StoreStatus::ok, std::move(task), {}};
}

StoreResult TaskStore::update(std::uint64_t id, TaskDraft draft) {
    std::string error = validate(draft);
    if (!error.empty()) {
        return {StoreStatus::invalid, {}, std::move(error)};
    }

    const std::lock_guard<std::mutex> lock(mutex_);
    Snapshot candidate = current_;
    const auto at = findTask(candidate.tasks, id);
    if (at == candidate.tasks.end()) {
        return {StoreStatus::missing, {}, "Task does not exist."};
    }

    Task task{id, std::move(draft)};
    *at = task;
    if (!persist(candidate, error)) {
        return {StoreStatus::ioError, {}, std::move(error)};
    }

    current_ = std::move(candidate);
    return {StoreStatus::ok, std::move(task), {}};
}

StoreResult TaskStore::erase(std::uint64_t id) {
    const std::lock_guard<std::mutex> lock(mutex_);
    Snapshot candidate = current_;
    const auto at = findTask(candidate.tasks, id);
    if (at == candidate.tasks.end()) {
        return {StoreStatus::missing, {}, "Task does not exist."};
    }

    candidate.tasks.erase(at);
    std::string error;
    if (!persist(candidate, error)) {
        return {StoreStatus::ioError, {}, std::move(error)};
    }

    current_ = std::move(candidate);
    return {StoreStatus::ok, {}, {}};
}
