#ifndef HTTP_H
#define HTTP_H

#include <cstddef>
#include <string>
#include <string_view>

struct RequestOptions {
    std::string_view query;
    std::string_view contentType;
    std::string_view host;
    std::string_view origin;
    std::size_t length;
};

struct Request {
    int error = 0;
    std::string_view method;
    std::string_view path;
    RequestOptions options{};
};

struct Response {
    std::string_view status;
    std::string_view type;
    std::string body;
    std::string extra = {};
};

#endif
