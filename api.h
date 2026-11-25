#ifndef API_H
#define API_H

#include "http.h"
#include "tasks.h"

Response routeTasks(TaskStore& store, const Request& request,
                    std::string_view body);

#endif
