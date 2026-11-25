#ifndef TASKS_H
#define TASKS_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

enum class TaskState { todo, doing, done };
enum class Priority { low, normal, high };
enum class SortOrder { newest, oldest, priority, title };

std::string_view stateName(TaskState state);
std::string_view priorityName(Priority priority);
std::optional<TaskState> parseState(std::string_view value);
std::optional<Priority> parsePriority(std::string_view value);

struct TaskDraft {
    std::string title;
    std::string notes;
    TaskState state = TaskState::todo;
    Priority priority = Priority::normal;
};

struct Task {
    std::uint64_t id;
    TaskDraft draft;
};

struct TaskFilter {
    std::string search;
    std::optional<TaskState> state;
    std::optional<Priority> priority;
    SortOrder order = SortOrder::newest;
    std::size_t offset = 0;
    std::size_t limit = 25;
};

struct TaskPage {
    std::size_t total;
    std::vector<Task> items;
};

struct TaskSummary {
    std::size_t total = 0;
    std::array<std::size_t, 3> byState{};
    std::array<std::size_t, 3> byPriority{};
};

enum class StoreStatus { ok, invalid, missing, full, ioError };

struct StoreResult {
    StoreStatus status;
    std::optional<Task> task;
    std::string message;
};

class TaskStore {
public:
    explicit TaskStore(std::filesystem::path path);

    bool load(std::string& error);
    TaskPage list(const TaskFilter& filter) const;
    std::optional<Task> get(std::uint64_t id) const;
    TaskSummary summary() const;
    StoreResult create(TaskDraft draft);
    StoreResult update(std::uint64_t id, TaskDraft draft);
    StoreResult erase(std::uint64_t id);

private:
    struct Snapshot {
        std::uint64_t nextId = 1;
        std::vector<Task> tasks;
    };

    bool persist(const Snapshot& candidate, std::string& error) const;

    std::filesystem::path path_;
    mutable std::mutex mutex_;
    Snapshot current_;
};

#endif
