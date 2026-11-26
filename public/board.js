"use strict";

const filters = document.querySelector("#filters");
const editor = document.querySelector("#editor");
const list = document.querySelector("#task-list");
const previous = document.querySelector("#previous");
const next = document.querySelector("#next");
const pageLabel = document.querySelector("#page-label");
const saveButton = document.querySelector("#save");
const removeButton = document.querySelector("#remove");
const resetButton = document.querySelector("#cancel-edit");
const editorTitle = document.querySelector("#editor-title");
const listMessage = document.querySelector("#list-message");
const editorMessage = document.querySelector("#editor-message");
const stateLabels = {todo: "To do", doing: "Doing", done: "Done"};
const pageSize = 10;

let selectedId = "";
let offset = 0;
let currentQuery = new URLSearchParams();
let pendingList;
let editingBusy = false;

async function request(url, options) {
    const response = await fetch(url, options);
    const body = await response.json();
    if (!response.ok) {
        throw new Error(body.error || `HTTP ${response.status}`);
    }
    return body;
}

function storeFilters() {
    currentQuery = new URLSearchParams();
    for (const [key, value] of new FormData(filters)) {
        if (typeof value === "string" && value.trim()) {
            currentQuery.set(key, value.trim());
        }
    }
}

function taskButton(task) {
    const row = document.createElement("li");
    const button = document.createElement("button");
    button.className = "task";
    button.type = "button";
    button.dataset.taskId = task.id;
    button.setAttribute("aria-pressed", String(task.id === selectedId));

    const title = document.createElement("strong");
    title.className = "task-title";
    title.textContent = task.title;
    const meta = document.createElement("span");
    meta.className = "task-meta";
    meta.textContent = `${stateLabels[task.state]} / ${task.priority} priority`;
    button.append(title, meta);

    if (task.notes) {
        const notes = document.createElement("span");
        notes.className = "task-notes";
        notes.textContent = task.notes;
        button.append(notes);
    }

    button.addEventListener("click", () => {
        if (!editingBusy) {
            editTask(task);
            editor.elements.namedItem("title").focus();
        }
    });
    row.append(button);
    return row;
}

function highlightSelected() {
    for (const button of list.querySelectorAll("[data-task-id]")) {
        button.setAttribute("aria-pressed", String(button.dataset.taskId === selectedId));
    }
}

function editTask(task) {
    selectedId = task.id;
    for (const name of ["title", "notes", "state", "priority"]) {
        editor.elements.namedItem(name).value = task[name];
    }
    editorTitle.textContent = "Edit task";
    saveButton.textContent = "Save changes";
    removeButton.hidden = false;
    resetButton.hidden = false;
    editorMessage.textContent = "";
    highlightSelected();
}

function resetEditor() {
    selectedId = "";
    editor.reset();
    editorTitle.textContent = "New task";
    saveButton.textContent = "Create task";
    removeButton.hidden = true;
    resetButton.hidden = true;
    highlightSelected();
}

async function reload() {
    if (pendingList) {
        pendingList.abort();
    }
    const controller = new AbortController();
    pendingList = controller;
    listMessage.textContent = "Loading tasks…";
    previous.disabled = true;
    next.disabled = true;

    const query = new URLSearchParams(currentQuery);
    query.set("limit", String(pageSize));
    query.set("offset", String(offset));
    try {
        const options = {signal: controller.signal};
        const [page, summary] = await Promise.all([
            request(`/api/tasks?${query}`, options),
            request("/api/summary", options)
        ]);
        if (controller.signal.aborted) {
            return;
        }
        if (offset && page.items.length === 0) {
            offset = page.total ? Math.floor((page.total - 1) / pageSize) * pageSize : 0;
            await reload();
            return;
        }

        list.replaceChildren(...page.items.map(taskButton));
        for (const key of ["total", "todo", "doing", "done"]) {
            document.querySelector(`#${key}-count`).textContent = String(summary[key]);
        }
        listMessage.textContent = page.total
            ? `${page.total} matching ${page.total === 1 ? "task" : "tasks"}`
            : "No tasks match.";
        pageLabel.textContent = page.total
            ? `${offset + 1}–${offset + page.items.length} of ${page.total}`
            : "";
        previous.disabled = offset === 0;
        next.disabled = offset + page.items.length >= page.total;
    } catch (error) {
        if (!controller.signal.aborted) {
            listMessage.textContent = error.message;
        }
    }
}

function setEditingBusy(busy) {
    editingBusy = busy;
    for (const field of editor.elements) {
        field.disabled = busy;
    }
}

filters.addEventListener("submit", (event) => {
    event.preventDefault();
    storeFilters();
    offset = 0;
    void reload();
});

previous.addEventListener("click", () => {
    offset = Math.max(0, offset - pageSize);
    void reload();
});

next.addEventListener("click", () => {
    offset += pageSize;
    void reload();
});

editor.addEventListener("submit", async (event) => {
    event.preventDefault();
    const updating = Boolean(selectedId);
    const url = updating ? `/api/tasks/${selectedId}` : "/api/tasks";
    const body = new URLSearchParams(new FormData(editor));
    setEditingBusy(true);

    try {
        await request(url, {method: updating ? "PUT" : "POST", body});
        resetEditor();
        editorMessage.textContent = updating ? "Task updated." : "Task created.";
        await reload();
    } catch (error) {
        editorMessage.textContent = error.message;
    } finally {
        setEditingBusy(false);
    }
});

removeButton.addEventListener("click", async () => {
    if (!selectedId || !window.confirm("Delete this task?")) {
        return;
    }
    setEditingBusy(true);
    try {
        await request(`/api/tasks/${selectedId}`, {method: "DELETE"});
        resetEditor();
        editorMessage.textContent = "Task deleted.";
        await reload();
    } catch (error) {
        editorMessage.textContent = error.message;
    } finally {
        setEditingBusy(false);
    }
});

resetButton.addEventListener("click", () => {
    resetEditor();
    editorMessage.textContent = "";
    editor.elements.namedItem("title").focus();
});

storeFilters();
void reload();
