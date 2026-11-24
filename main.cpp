#include "task_api.h"
#include "server_metrics.h"

#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>

#include <cerrno>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <ctime>
#include <fstream>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
#include <system_error>
#include <thread>
#include <vector>

using Clock = std::chrono::steady_clock;

class Socket {
public:
    explicit Socket(int fd) : fd_(fd) {}

    ~Socket() {
        if (fd_ >= 0) {
            close(fd_);
        }
    }

    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;

    Socket(Socket&& other) noexcept
        : fd_(std::exchange(other.fd_, -1)) {}

    int get() const {
        return fd_;
    }

private:
    int fd_;
};

int readPort(int argc, char* argv[]) {
    if (argc == 1) {
        return 8080;
    }
    if (argc != 2) {
        return -1;
    }

    std::string_view text = argv[1];
    int port = 0;
    const char* end = text.data() + text.size();
    const auto result = std::from_chars(text.data(), end, port);
    if (result.ec != std::errc{} || result.ptr != end) {
        return -1;
    }
    if (port < 1 || port > 65535) {
        return -1;
    }
    return port;
}

Socket openListener(int port) {
    Socket listener(socket(AF_INET, SOCK_STREAM, 0));
    if (listener.get() < 0) {
        std::perror("socket");
        return Socket(-1);
    }

    int reuse = 1;
    if (setsockopt(listener.get(), SOL_SOCKET, SO_REUSEADDR,
                   &reuse, sizeof(reuse)) < 0) {
        std::perror("setsockopt");
        return Socket(-1);
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(static_cast<unsigned short>(port));
    if (bind(listener.get(), reinterpret_cast<sockaddr*>(&address),
             sizeof(address)) < 0) {
        std::perror("bind");
        return Socket(-1);
    }

    if (listen(listener.get(), 32) < 0) {
        std::perror("listen");
        return Socket(-1);
    }
    return listener;
}

bool makeNonblocking(int fd) {
    const int flags = fcntl(fd, F_GETFL);
    return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

int waitForSocket(int fd, short events, Clock::time_point deadline) {
    while (true) {
        const auto remaining = deadline - Clock::now();
        if (remaining <= Clock::duration::zero()) {
            return 0;
        }

        const auto milliseconds =
            std::chrono::ceil<std::chrono::milliseconds>(remaining);
        pollfd entry{fd, events, 0};
        const int result = poll(&entry, 1, static_cast<int>(milliseconds.count()));
        if (result >= 0 || errno != EINTR) {
            return result;
        }
    }
}

void sendAll(int fd, std::string_view bytes) {
    const auto deadline = Clock::now() + std::chrono::seconds(3);
    while (!bytes.empty()) {
        if (waitForSocket(fd, POLLOUT, deadline) <= 0) {
            return;
        }
        const ssize_t sent = send(fd, bytes.data(), bytes.size(), 0);
        if (sent > 0) {
            bytes.remove_prefix(static_cast<std::size_t>(sent));
        } else if (sent == 0 || (errno != EINTR && errno != EAGAIN &&
                                 errno != EWOULDBLOCK)) {
            return;
        }
    }
}

struct HeaderRead {
    int status;
    std::string bytes;
};

HeaderRead readHeaders(int fd) {
    const std::size_t maxHeaders = 8192;
    const auto deadline = Clock::now() + std::chrono::seconds(3);
    std::string bytes;
    char buffer[1024];

    while (bytes.size() < maxHeaders) {
        const int ready = waitForSocket(fd, POLLIN, deadline);
        if (ready <= 0) {
            return {ready == 0 ? 408 : 0, std::move(bytes)};
        }

        const std::size_t remaining = maxHeaders - bytes.size();
        const std::size_t count =
            remaining < sizeof(buffer) ? remaining : sizeof(buffer);
        const ssize_t received = recv(fd, buffer, count, 0);
        if (received == 0) {
            return {bytes.empty() ? 0 : 400, std::move(bytes)};
        }
        if (received < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            return {0, std::move(bytes)};
        }

        bytes.append(buffer, static_cast<std::size_t>(received));
        const auto end = bytes.find("\r\n\r\n");
        if (end != std::string::npos) {
            return {200, std::move(bytes)};
        }
    }
    return {431, std::move(bytes)};
}

int readBody(int fd, std::size_t length, std::string& body) {
    const auto deadline = Clock::now() + std::chrono::seconds(3);
    char buffer[1024];

    while (body.size() < length) {
        const int ready = waitForSocket(fd, POLLIN, deadline);
        if (ready <= 0) {
            return ready == 0 ? 408 : 0;
        }

        const std::size_t remaining = length - body.size();
        const std::size_t count =
            remaining < sizeof(buffer) ? remaining : sizeof(buffer);
        const ssize_t received = recv(fd, buffer, count, 0);
        if (received == 0) {
            return 400;
        }
        if (received < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            return 0;
        }
        body.append(buffer, static_cast<std::size_t>(received));
    }
    return 200;
}

bool isAsciiAlnum(char c) {
    return (c >= '0' && c <= '9') ||
           (c >= 'A' && c <= 'Z') ||
           (c >= 'a' && c <= 'z');
}

bool isToken(std::string_view text) {
    const std::string_view punctuation = "!#$%&'*+-.^`_|~";
    if (text.empty()) {
        return false;
    }
    for (char c : text) {
        if (!isAsciiAlnum(c) && punctuation.find(c) == std::string_view::npos) {
            return false;
        }
    }
    return true;
}

std::string asciiLower(std::string_view text) {
    std::string lower(text);
    for (char& c : lower) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c + ('a' - 'A'));
        }
    }
    return lower;
}

std::string_view trimSpaces(std::string_view text) {
    const auto first = text.find_first_not_of(" \t");
    if (first == std::string_view::npos) {
        return {};
    }
    return text.substr(first, text.find_last_not_of(" \t") - first + 1);
}

bool validHost(std::string_view value) {
    const auto colon = value.find(':');
    const auto name = value.substr(0, colon);
    const auto lower = asciiLower(name);
    if (lower != "localhost" && lower != "127.0.0.1") {
        return false;
    }

    if (colon != std::string_view::npos) {
        const auto portText = value.substr(colon + 1);
        int port = 0;
        const auto end = portText.data() + portText.size();
        const auto result = std::from_chars(portText.data(), end, port);
        return result.ec == std::errc{} && result.ptr == end &&
               port >= 1 && port <= 65535;
    }
    return true;
}

Request parseRequest(std::string_view bytes) {
    const auto lineEnd = bytes.find("\r\n");
    const auto line = bytes.substr(0, lineEnd);
    const auto firstSpace = line.find(' ');
    if (firstSpace == std::string_view::npos) {
        return {400, {}, {}};
    }
    const auto secondSpace = line.find(' ', firstSpace + 1);
    if (secondSpace == std::string_view::npos) {
        return {400, {}, {}};
    }

    const auto method = line.substr(0, firstSpace);
    const auto target = line.substr(firstSpace + 1, secondSpace - firstSpace - 1);
    const auto version = line.substr(secondSpace + 1);
    if (!isToken(method) || target.empty() || target.front() != '/') {
        return {400, {}, {}};
    }
    for (unsigned char c : target) {
        if (c <= 32 || c >= 127 || c == '#') {
            return {400, {}, {}};
        }
    }

    if (version.size() != 8 || version.substr(0, 5) != "HTTP/" ||
        version[5] < '0' || version[5] > '9' || version[6] != '.' ||
        version[7] < '0' || version[7] > '9') {
        return {400, {}, {}};
    }
    if (version != "HTTP/1.1") {
        return {505, {}, {}};
    }

    bool sawHost = false;
    bool sawLength = false;
    bool sawTransfer = false;
    bool sawExpect = false;
    bool sawContentType = false;
    std::string_view contentType;
    std::string_view host;
    std::string_view origin;
    unsigned long long contentLength = 0;

    std::size_t position = lineEnd + 2;
    while (true) {
        const auto end = bytes.find("\r\n", position);
        if (end == position) {
            break;
        }

        const auto field = bytes.substr(position, end - position);
        position = end + 2;
        const auto colon = field.find(':');
        const auto name = field.substr(0, colon);
        if (colon == std::string_view::npos || !isToken(name)) {
            return {400, {}, {}};
        }

        const auto value = trimSpaces(field.substr(colon + 1));
        for (unsigned char c : value) {
            if ((c < 32 && c != '\t') || c == 127) {
                return {400, {}, {}};
            }
        }

        const std::string key = asciiLower(name);
        if (key == "host") {
            if (sawHost || !validHost(value)) {
                return {400, {}, {}};
            }
            sawHost = true;
            host = value;
        } else if (key == "content-length") {
            if (sawLength || value.empty()) {
                return {400, {}, {}};
            }
            const char* endValue = value.data() + value.size();
            const auto result = std::from_chars(value.data(), endValue,
                                                contentLength);
            if (result.ec != std::errc{} || result.ptr != endValue) {
                return {400, {}, {}};
            }
            sawLength = true;
        } else if (key == "transfer-encoding") {
            sawTransfer = true;
        } else if (key == "content-type") {
            if (sawContentType) {
                return {400, {}, {}};
            }
            sawContentType = true;
            contentType = value;
        } else if (key == "origin") {
            if (value.empty() || !origin.empty()) {
                return {400, {}, {}};
            }
            origin = value;
        } else if (key == "expect") {
            sawExpect = true;
        }
    }

    if (!sawHost || (sawLength && sawTransfer)) {
        return {400, {}, {}};
    }
    if (sawTransfer) {
        return {501, {}, {}};
    }
    if (sawExpect) {
        return {417, {}, {}};
    }
    if (contentLength > 16 * 1024) {
        return {413, {}, {}};
    }

    const auto separator = target.find('?');
    const auto query = separator == std::string_view::npos
        ? std::string_view{} : target.substr(separator + 1);
    return {0, method, target.substr(0, separator),
            {query, contentType, host, origin,
             static_cast<std::size_t>(contentLength)}};
}

Response plain(std::string_view status, std::string body) {
    return {status, "text/plain; charset=utf-8", std::move(body)};
}

Response errorResponse(int code) {
    switch (code) {
        case 408:
            return plain("408 Request Timeout", "Request timed out.\n");
        case 413:
            return plain("413 Content Too Large", "Keep the request body within 16 KiB.\n");
        case 417:
            return plain("417 Expectation Failed", "Expect is unsupported.\n");
        case 431:
            return plain("431 Request Header Fields Too Large", "Headers too large.\n");
        case 501:
            return plain("501 Not Implemented", "Transfer coding is unsupported.\n");
        case 505:
            return plain("505 HTTP Version Not Supported", "Use HTTP/1.1.\n");
        default:
            return plain("400 Bad Request", "Malformed HTTP request.\n");
    }
}

void sendResponse(int fd, const Response& response, bool headOnly) {
    std::string wire = "HTTP/1.1 ";
    wire += response.status;
    wire += "\r\n";

    const std::time_t now = std::time(nullptr);
    std::tm utc{};
    char date[40];
    if (gmtime_r(&now, &utc) != nullptr &&
        std::strftime(date, sizeof(date), "%a, %d %b %Y %H:%M:%S GMT", &utc)) {
        wire += "Date: ";
        wire += date;
        wire += "\r\n";
    }

    wire += "Content-Type: ";
    wire += response.type;
    wire += "\r\nContent-Length: ";
    wire += std::to_string(response.body.size());
    wire += "\r\nConnection: close\r\n";
    wire += response.extra;
    wire += "\r\n";
    if (!headOnly) {
        wire += response.body;
    }
    sendAll(fd, wire);
}

Response loadAsset(const char* filename, std::string_view contentType) {
    const std::streamoff maxBytes = 64 * 1024;
    std::ifstream file(filename, std::ios::binary | std::ios::ate);
    if (!file) {
        std::cerr << "Could not open " << filename << '\n';
        return plain("500 Internal Server Error", "Could not open site file.\n");
    }

    const std::streamoff size = file.tellg();
    if (size < 0 || size > maxBytes) {
        std::cerr << "Invalid asset size for " << filename << '\n';
        return plain("500 Internal Server Error", "Could not load site file.\n");
    }

    std::string body(static_cast<std::size_t>(size), '\0');
    file.seekg(0);
    if (!file.read(body.data(), static_cast<std::streamsize>(size))) {
        std::cerr << "Could not read " << filename << '\n';
        return plain("500 Internal Server Error", "Could not read site file.\n");
    }

    return {"200 OK", contentType, std::move(body)};
}

struct ServerState {
    TaskStore tasks{"tasks.db"};
    ServerMetrics metrics;
};

Response route(ServerState& state, const Request& request, std::string body) {
    if (request.path == "/api/tasks" || request.path == "/api/summary" ||
        request.path.substr(0, 11) == "/api/tasks/") {
        return routeTasks(state.tasks, request, body);
    }
    if (request.path == "/echo" && request.method == "POST") {
        return plain("200 OK", std::move(body));
    }
    if (request.method != "GET" && request.method != "HEAD") {
        return {"405 Method Not Allowed", "text/plain; charset=utf-8",
                "Use GET or HEAD.\n", "Allow: GET, HEAD\r\n"};
    }
    if (request.path == "/" || request.path == "/index.html") {
        return loadAsset("public/index.html", "text/html; charset=utf-8");
    }
    if (request.path == "/style.css") {
        return loadAsset("public/style.css", "text/css; charset=utf-8");
    }
    if (request.path == "/tasks") {
        return loadAsset("public/tasks.html", "text/html; charset=utf-8");
    }
    if (request.path == "/board.css") {
        return loadAsset("public/board.css", "text/css; charset=utf-8");
    }
    if (request.path == "/board.js") {
        return loadAsset("public/board.js", "text/javascript; charset=utf-8");
    }
    if (request.path == "/health") {
        return plain("200 OK", "ok\n");
    }
    if (request.path == "/metrics") {
        return state.metrics.response();
    }
    if (request.path == "/slow") {
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
        return plain("200 OK", "slow response\n");
    }
    return plain("404 Not Found", "No route here.\n");
}

int handleClient(int fd, ServerState& state) {
    const HeaderRead read = readHeaders(fd);
    if (read.status == 0) {
        return 0;
    }

    const bool headOnly = read.bytes.compare(0, 5, "HEAD ") == 0;
    if (read.status != 200) {
        sendResponse(fd, errorResponse(read.status), headOnly);
        return read.status;
    }

    const std::string_view bytes = read.bytes;
    const auto headerSize = bytes.find("\r\n\r\n") + 4;
    const Request request = parseRequest(bytes.substr(0, headerSize));
    if (request.error != 0) {
        sendResponse(fd, errorResponse(request.error), headOnly);
        return request.error;
    }

    std::string body(bytes.substr(headerSize, request.options.length));
    const int bodyStatus = readBody(fd, request.options.length, body);
    if (bodyStatus != 200) {
        if (bodyStatus != 0) {
            sendResponse(fd, errorResponse(bodyStatus), headOnly);
        }
        return bodyStatus;
    }

    const Response response = route(state, request, std::move(body));
    sendResponse(fd, response, headOnly);
    return (response.status[0] - '0') * 100 +
           (response.status[1] - '0') * 10 + (response.status[2] - '0');
}

struct ClientQueue {
    std::mutex mutex;
    std::condition_variable changed;
    std::queue<Socket> pending;
    bool stopping = false;
};

std::atomic<bool> stopRequested{false};
static_assert(std::atomic<bool>::is_always_lock_free);

void requestStop(int) {
    stopRequested.store(true, std::memory_order_relaxed);
}

void serveClients(ClientQueue& clients, ServerState& state) {
    while (true) {
        std::unique_lock<std::mutex> lock(clients.mutex);
        clients.changed.wait(lock, [&clients] {
            return clients.stopping || !clients.pending.empty();
        });

        if (clients.stopping) {
            return;
        }

        Socket client(std::move(clients.pending.front()));
        clients.pending.pop();
        state.metrics.queued(clients.pending.size());
        lock.unlock();

        state.metrics.started();
        const auto start = Clock::now();
        const int status = handleClient(client.get(), state);
        const auto duration = std::chrono::duration_cast<std::chrono::microseconds>(
            Clock::now() - start);
        state.metrics.finished(status, duration);
    }
}

int acceptClients(const Socket& listener, ClientQueue& clients,
                  ServerMetrics& metrics) {
    const std::size_t queueLimit = 16;
    const std::string_view busy =
        "HTTP/1.1 503 Service Unavailable\r\n"
        "Content-Length: 0\r\n"
        "Retry-After: 1\r\n"
        "Connection: close\r\n\r\n";

    while (!stopRequested.load(std::memory_order_relaxed)) {
        const auto deadline = Clock::now() + std::chrono::milliseconds(200);
        const int ready = waitForSocket(listener.get(), POLLIN, deadline);
        if (ready < 0) {
            std::perror("poll listener");
            return 1;
        }
        if (ready == 0 || stopRequested.load(std::memory_order_relaxed)) {
            continue;
        }

        Socket client(accept(listener.get(), nullptr, nullptr));
        if (client.get() < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK ||
                errno == ECONNABORTED) {
                continue;
            }
            std::perror("accept");
            return 1;
        }
        metrics.accepted();
        if (!makeNonblocking(client.get())) {
            std::perror("fcntl client");
            continue;
        }

        {
            std::lock_guard<std::mutex> lock(clients.mutex);
            if (clients.pending.size() < queueLimit) {
                clients.pending.push(std::move(client));
                metrics.queued(clients.pending.size());
            }
        }

        if (client.get() < 0) {
            clients.changed.notify_one();
        } else {
            // A full queue must never make the accept thread wait on a client.
            metrics.rejected();
            send(client.get(), busy.data(), busy.size(), 0);
        }
    }
    return 0;
}

int main(int argc, char* argv[]) {
    const int port = readPort(argc, argv);
    if (port < 0) {
        std::cerr << "Usage: ./server [port 1-65535]\n";
        return 1;
    }
    if (std::signal(SIGPIPE, SIG_IGN) == SIG_ERR ||
        std::signal(SIGINT, requestStop) == SIG_ERR ||
        std::signal(SIGTERM, requestStop) == SIG_ERR) {
        std::perror("signal");
        return 1;
    }

    ServerState state;
    std::string databaseError;
    if (!state.tasks.load(databaseError)) {
        std::cerr << databaseError << '\n';
        return 1;
    }

    const int workerCount = 4;
    ClientQueue clients;
    std::vector<std::thread> workers;
    workers.reserve(workerCount);
    int exitCode = 0;

    {
        const Socket listener = openListener(port);
        if (listener.get() < 0) {
            return 1;
        }
        if (!makeNonblocking(listener.get())) {
            std::perror("fcntl listener");
            return 1;
        }

        try {
            for (int i = 0; i < workerCount; ++i) {
                workers.emplace_back(serveClients, std::ref(clients), std::ref(state));
            }
        } catch (const std::system_error& error) {
            std::cerr << "Could not start worker: " << error.what() << '\n';
            exitCode = 1;
        }
        if (exitCode == 0) {
            std::cout << "Listening on http://127.0.0.1:" << port
                      << " with " << workers.size() << " workers" << std::endl;
            exitCode = acceptClients(listener, clients, state.metrics);
        }
    }

    {
        std::lock_guard<std::mutex> lock(clients.mutex);
        clients.stopping = true;
        while (!clients.pending.empty()) {
            clients.pending.pop();
        }
        state.metrics.queued(0);
    }
    clients.changed.notify_all();

    for (auto& worker : workers) {
        worker.join();
    }
    return exitCode;
}
