#include "logging.h"
#include "requests.h"
#include "response.h"
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define PUB_PIPE_PATHNAME 256
#define BOX_NAME 32

int handle_publisher(registration_request_t *);
int handle_manager(registration_request_t *);

int main(int argc, char **argv) {
    if (argc < 3 || strcmp(argv[1], "--help") == 0) {
        fprintf(stderr, "usage: mbroker <pipename> <max-sessions>\n");
        exit(EXIT_FAILURE);
    }

    if (mkfifo(argv[1], S_IRUSR | S_IWUSR | S_IWGRP) == -1 && errno != EEXIST) {
        fprintf(stderr, "mbroker: couldn't create FIFO %s\n", argv[1]);
        exit(EXIT_FAILURE);
    }

    int mbroker_fd = open(argv[1], O_RDONLY);
    if (mbroker_fd == -1) {
        fprintf(stderr, "Error opening pipe %s\n", argv[1]);
        exit(EXIT_FAILURE);
    }

    registration_request_t req;

    while (1) {
        ssize_t ret = read(mbroker_fd, &req, sizeof(req));
        if (ret == 0) {
            // pipe closed somewhere
            continue;
        } else if (ret != sizeof(req)) {
            fprintf(stderr, "Partial read or no read\n");
            exit(EXIT_FAILURE);
        }

        switch (req.op_code) {
        case 1:
            fprintf(stderr, "OP_CODE 1: received\n");
            handle_publisher(&req);
            break;
        case 2:
            fprintf(stderr, "OP_CODE 2: received\n");
            break;
        case 3:
            fprintf(stderr, "OP_CODE 3: received\n");
            handle_manager(&req);
            break;
        case 5:
            fprintf(stderr, "OP_CODE 5: received\n");
            handle_manager(&req);
            break;
        case 7:
            fprintf(stderr, "OP_CODE 7: received\n");
            break;
        default:
            fprintf(stderr, "Ignoring unknown OP_CODE %d\n", req.op_code);
            break;
        }
    }

    return 0;
}

int handle_publisher(registration_request_t *req) {
    int publisher_fd = open(req->pipe_name, O_RDONLY);
    if (publisher_fd == -1) {
        fprintf(stderr, "ERROR Failed opening publisher pipe\n");
        exit(EXIT_FAILURE);
    }

    publisher_request_t pub_r;

    while (1) {
        ssize_t ret = read(publisher_fd, &pub_r, sizeof(pub_r));
        // pipe closed, end
        if (ret == 0) {
            break;
        } else if (ret != sizeof(pub_r)) {
            close(publisher_fd);
            return -1;
        }
        fprintf(stderr, "%s\n", pub_r.message);
    }

    close(publisher_fd);
    return 0;
}

int handle_manager(registration_request_t *req) {
    int manager_fd = open(req->pipe_name, O_WRONLY);
    if (manager_fd == -1) {
        fprintf(stderr, "ERROR Failed opening manager pipe\n");
        exit(EXIT_FAILURE);
    }

    manager_response_t resp;

    if (manager_response_init(&resp, req->op_code + 1, 0, NULL) != 0) {
        fprintf(stderr, "ERROR Failed creating response\n");
        exit(EXIT_FAILURE);
    }

    if (manager_response_send(manager_fd, &resp) != 0) {
        fprintf(stderr, "ERROR Failed sending response\n");
        exit(EXIT_FAILURE);
    }

    close(manager_fd);

    return 0;
}