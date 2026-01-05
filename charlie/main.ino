#include <ESP8266WiFi.h>
typedef short int16_t;
typedef unsigned short uint16_t;
typedef long long int64_t;
typedef unsigned long long uint64_t;
typedef unsigned char uint8_t;

typedef int32_t client_t; // An empty client's fd is <0.

#define SSIZE_MAX 0x7FFFFFFFFFFFFFFF
#define SIZE_MAX 0xFFFFFFFFFFFFFFFF

#define NULL 0

// Write callback. Returns the number of bytes written, -1 on error.
// First arguement is the client fd, second the array of bytes to be written, and third is the number of bytes to write.
// NOTE: Function should error of the number of bytes is > SSIZE_MAX!
typedef ssize_t (*write_cb)(client_t, char*, size_t);

// Read callback. Same as write, except it reads bytes into the array. This function should time-out after some time, to prevent deadlocks.
// NOTE: Function should error of the number of bytes is > SSIZE_MAX!
typedef ssize_t (*read_cb)(client_t, char*, size_t);

// Close callback. Closes the connection to the client. Is assumed to be unable to fail.
typedef void (*close_cb)(client_t);

// Available callback. Returns the number of bytes that can be currently read.
typedef size_t (*available_cb)(client_t);

// Accept callback. Accepts a new client to connect, and returns the client's fd. Returns -1 if there are no new clients.
typedef client_t (*accept_cb)(void);

// Get time callback. Gets the current time (or the time since the MCU has started) in milliseconds. (Equivelent to arduino millis().).
typedef uint64_t (*time_cb)(void);


typedef struct slice_t{
    char* buffer = NULL;
    size_t buffer_size = NULL;
} slice_t;

typedef struct mfs_message_t{
    uint8_t op = NULL;

    slice_t path;
    slice_t data;

} mfs_message_t;


typedef struct client_handle_t{
    client_t client = -1;
    uint64_t timeout_time = NULL;
} client_handle_t;

// File writer function. Returns a negative value with the appropiate MFS error code when it fails. Returns a positive value with the number of bytes written.
// First arguement is the slice to written.
typedef int64_t (*file_writer_cb)(slice_t);

// File reader function. Returns the number bytes read (positive). Errors are negative values which are interperted as MFS error codes.
// First arguement is a slice_t that contains the destiation buffer, and the size of that buffer. If the buffer is too small, the function should fail with the appropiate MFS error code.
typedef int64_t (*file_reader_cb)(slice_t);

typedef struct mfs_file_t{
    slice_t path; // Contains a C string thats the path of the file. **SIZE SHOULD INCLUDE NULL TERMINATOR**
    file_writer_cb writer_fn = NULL; // If the function pointer is NULL, the file is assumed to not support that operation.
    file_reader_cb reader_fn = NULL;

} mfs_file_t;


#define MFS_OP_NOOP 0
#define MFS_OP_READ 1
#define MFS_OP_WRITE 2
#define MFS_OP_LS 3
#define MFS_OP_ERROR 4
#define MFS_RESPONSE_OF(x) ((x) | 0x80)
#define MFS_RESERVED_OP_RANGE

#define UINT32_LIMIT 0xFFFFFFFF

class mfs_server {
    read_cb read_fn;
    write_cb write_fn;

    accept_cb accept_fn;
    close_cb close_fn;

    available_cb avail_fn;
    time_cb gettime_fn;

    slice_t path_buffer;
    slice_t data_buffer;

    client_handle_t* clients_buffer;
    size_t clients_bsize;

    mfs_file_t* files_buffer;
    size_t files_bsize;

    // Reads headers from the slice. Returns 0 on success, 1 on error.
    int32_t read_headers(slice_t slice, mfs_message_t* output) {
        if (slice.buffer_size < 9) return 1;
        output->op = slice.buffer[8];

        output->data.buffer_size = 0;
        output->path.buffer_size = 0;

        output->path.buffer_size |= ((uint32_t)(slice.buffer[0]) << 0);
        output->path.buffer_size |= ((uint32_t)(slice.buffer[1]) << 8);
        output->path.buffer_size |= ((uint32_t)(slice.buffer[2]) << 16);
        output->path.buffer_size |= ((uint32_t)(slice.buffer[3]) << 24);

        output->data.buffer_size |= ((uint32_t)(slice.buffer[4]) << 0);
        output->data.buffer_size |= ((uint32_t)(slice.buffer[5]) << 8);
        output->data.buffer_size |= ((uint32_t)(slice.buffer[6]) << 16);
        output->data.buffer_size |= ((uint32_t)(slice.buffer[7]) << 24);
        return 0;
    }

    // Writes headers into the slice at offset. Returns 1 on error, 0 on success.
    int32_t write_headers(slice_t slice, uint64_t offset, mfs_message_t headers) {
        if ((offset + 9) > slice.buffer_size) return 1;

        slice.buffer[offset + 0] = ((headers.path.buffer_size >> 0) & 0xFF);
        slice.buffer[offset + 1] = ((headers.path.buffer_size >> 8) & 0xFF);
        slice.buffer[offset + 2] = ((headers.path.buffer_size >> 16) & 0xFF);
        slice.buffer[offset + 3] = ((headers.path.buffer_size >> 24) & 0xFF);

        slice.buffer[offset + 4] = ((headers.data.buffer_size >> 0) & 0xFF);
        slice.buffer[offset + 5] = ((headers.data.buffer_size >> 8) & 0xFF);
        slice.buffer[offset + 6] = ((headers.data.buffer_size >> 16) & 0xFF);
        slice.buffer[offset + 7] = ((headers.data.buffer_size >> 24) & 0xFF);

        slice.buffer[offset + 8] = headers.op;
        return 0;
    }

    // Drops client, and removes it from client array
    // Fails silenty if the client does not exist in the array.
    void drop_client(client_t client) {
        for (size_t i = 0; i < this->clients_bsize; i++) {
            if (client == this->clients_buffer[i].client) {
                this->close_fn(client);
                this->clients_buffer[i].client = -1;
                return;
            }
        }
    }

    // Attempts to consume n amount of bytes from the client. If it is unable to, returns 1.
    // Returns 0 on success.
    int32_t consume(client_t client, size_t n) {
        // I know this is slow, but its the simplest implementation.
        char buf[1];
        for (size_t i = 0; i < n; i++) {
            if (this->read_fn(client, buf, 1) != 1) return 1;
        }
        return 0;
    }
    // Checks if two slices are the same contains the same data.
    // Returns 0 if its the same, 1 if its diffirent.
    int32_t slicecmp(slice_t s1, slice_t s2) {
        if (s1.buffer_size != s2.buffer_size) return 1;
        for (size_t i = 0; i < s1.buffer_size; i++) {
            if (s1.buffer[i] != s2.buffer[i]) return 1;
        }
        return 0;
    }

    // Gets the index of file whoose path is the same as the provided slice.
    // Returns -1 when file isn't found, and the returns the index if it is.
    // Returns -2 if theres more files than it can returns (>SSIZE_MAX)
    ssize_t get_file_index(slice_t path) {
        if (this->files_bsize > SSIZE_MAX) return -2;
        for (size_t i = 0; i < files_bsize; i++) {
            if (this->slicecmp(path, {this->files_buffer[i].path.buffer, this->files_buffer[i].path.buffer_size - 1}) == 0) return i;
        }
        return -1;
    }

    // Sends an MFS message. Can drop clients.
    // Returs 0 on success, 1 on error, 2 if it did drop the client.
    int32_t send_mfs_message(client_t client, mfs_message_t msg) {
        // Checks
        if (msg.data.buffer_size > UINT32_LIMIT || msg.path.buffer_size > UINT32_LIMIT) return 1;

        //Sending actual data
        char header_buffer[9];
        if (this->write_headers({header_buffer, 9}, 0, msg)) return 1;

        int64_t bytes_written = this->write_fn(client, header_buffer, 9);
        if (bytes_written == -1) return 1; // Not de-synchronised.
        if (bytes_written < 9) {
            // Partial write. We are desynchronised. We can't reallly send a de-sycnhronised error message if we can't even write headers.
            this->drop_client(client);
            return 2;
        }

        bytes_written = this->write_fn(client, msg.path.buffer, msg.path.buffer_size);
        if (bytes_written == -1 || bytes_written < msg.path.buffer_size) {
            // De-synchronised from client.
            this->drop_client(client);
            return 2;
        }

        bytes_written = this->write_fn(client, msg.data.buffer, msg.data.buffer_size);
        if (bytes_written == -1 || bytes_written < msg.data.buffer_size) {
            // De-synchronised from client.
            this->drop_client(client);
            return 2;
        }
        return 0;
    }

    // Sends the equivelent MFS error of the given message.
    // Inherits dropping cliets from send_mfs_message. Returs 0 on success, 1 on error, 2 if it did drop the client.
    int32_t send_mfs_error(client_t client, mfs_message_t msg, uint16_t errcode) {
        Serial.print("Sending mfs errorcode: ");
        Serial.println(errcode);
        msg.op = MFS_RESPONSE_OF(MFS_OP_ERROR);
        char buffer[2];
        buffer[0] = (errcode) & 0xFF;
        buffer[1] = (errcode >> 8) & 0xFF;

        msg.data = {buffer, 2};
        return this->send_mfs_message(client, msg);
    }

    // reads MFS message from client to dest. Returns 1 on error, 0 on success. 2 if it drops the client.
    int32_t read_mfs_message(client_t client, mfs_message_t* dest) {
        mfs_message_t empty_msg = {
            .op = 0,
            .path = {NULL,0},
            .data = {NULL, 0},
        };

        // Set initital variables so the rest can go properly
        dest->data.buffer = this->data_buffer.buffer;
        dest->path.buffer = this->path_buffer.buffer;
        // First read headers
        char buffer[9];
        if (this->read_fn(client, buffer, 9) != 9) {
            // cant read headers, send a failed to parse headers error and drop client.
            int32_t result = this->send_mfs_error(client, empty_msg, 3);

            switch (result) {
                case 1:
                    // So, we errored out trying to error out? Drop client.
                    this->drop_client(client);
                    return 2;
                    break;

                case 2:
                    // client was dropped, so we just returns the same 2.
                    return 2;
            }
        }

        if (this->read_headers({buffer, 9}, dest)) {
            // cant read headers, send a failed to parse headers error and drop client.
            int32_t result = this->send_mfs_error(client, empty_msg, 3);

            switch (result) {
                case 1:
                    // So, we errored out trying to error out? Drop client.
                    this->drop_client(client);
                    return 2;
                    break;

                case 2:
                    // client was dropped, so we just returns the same 2.
                    return 2;
                    break;
            };
        }
        if (dest->data.buffer_size > this->hard_limit_bytes || dest->path.buffer_size > this->hard_limit_bytes) {
            // Client exceeded hard limit. Drop it.
            this->drop_client(client);
            return 2;
        }

        if (dest->data.buffer_size > this->data_buffer.buffer_size || dest->path.buffer_size > this->path_buffer.buffer_size) {
            int32_t result = this->send_mfs_error(client, empty_msg, 2);

            switch (result) {
                case 1:
                    // So, we errored out trying to error out? Drop client.
                    this->drop_client(client);
                    return 2;
                    break;

                case 2:
                    // client was dropped, so we just returns the same 2.
                    return 2;
                    break;
            };

            if (this->consume(client, (dest->path.buffer_size))) {
                // can't consume the faulty bytes. We are de-synchronised.
                this->send_mfs_error(client, empty_msg, 2000); // We don't care if this fails or drops clients here, because we are dropping it anyway.
                this->drop_client(client); // safe because it fails silenty if it does not exist.
                return 2;
            }

            if (this->consume(client, (dest->data.buffer_size))) {
                // can't consume the faulty bytes. We are de-synchronised.
                this->send_mfs_error(client, empty_msg, 2000); // We don't care if this fails or drops clients here, because we are dropping it anyway.
                this->drop_client(client); // safe because it fails silenty if it does not exist.
                return 2;
            }


        }

        // Here, we are perfectly certain the data can fit in our buffers.
        if (this->read_fn(client, dest->path.buffer, dest->path.buffer_size) != dest->path.buffer_size) {
            // Unable to read required data from client. Send an error, and then drop it.
            this->send_mfs_error(client, empty_msg, 1);
            this->drop_client(client); // No error checking needed because we are already dropping it.
            return 2;
        }
        if (this->read_fn(client, dest->data.buffer, dest->data.buffer_size) != dest->data.buffer_size) {
            this->send_mfs_error(client, empty_msg, 1);
            this->drop_client(client); // No error checking needed because we are already dropping it.
            return 2;
        }
        // After all this, we are done.
        return 0;
    }

    // list files. Sends the files this server hosts to the client. Returns 0 on success, 1 on error and 2 if it drops the client.
    int32_t list_files(client_t client) {
        // We first have to calculate how many bytes we have to send
        size_t total_bytes = 0;
        for (size_t i = 0; i < this->files_bsize; i++) {
            mfs_file_t* current_file = &this->files_buffer[i];
            total_bytes += current_file->path.buffer_size;
        }
        // First, send the headers (just copy paste some code from send_mfs_message.)
        mfs_message_t headers;
        headers.data.buffer_size = total_bytes;
        headers.op = MFS_RESPONSE_OF(MFS_OP_LS);
        headers.path.buffer_size = 0;

        char header_buffer[9];
        if (this->write_headers({header_buffer, 9}, 0, headers)) return 1;

        int64_t bytes_written = this->write_fn(client, header_buffer, 9);
        if (bytes_written == -1) return 1; // Not de-synchronised.
        if (bytes_written < 9) {
            // Partial write. We are desynchronised. We can't reallly send a de-sycnhronised error message if we can't even write headers.
            this->drop_client(client);
            return 2;
        }



        // Now send out the entire C-strings. since they contain the terminator already, we just have to loop to send em'.
        for (size_t i = 0; i < this->files_bsize; i++) {
            mfs_file_t* current_file = &this->files_buffer[i];
            if (this->write_fn(client, current_file->path.buffer, current_file->path.buffer_size) != current_file->path.buffer_size) {
                // De-syncrhonised. Cant' send error message because sending failed.
                this->drop_client(client);
                return 2;
            }
        }
        return 0;
    }

    void handle_read(client_handle_t* current_client, mfs_message_t client_request) {
        // Requested read operation. Find the file, then call the designated callbacks.,
        ssize_t file_index = this->get_file_index(client_request.path);
        switch (file_index) {
            case -1:
                // File not found.
                if (this->send_mfs_error(current_client->client, client_request, 1000) != 0) {
                    // Error sending error. Drop client and move on.
                    this->drop_client(current_client->client);
                }
                return;
                break;

            case -2:
                // Internal error. This server cannot serve clients properly if we are here. We send an operation failed unexpectadly error code and drop client.
                this->send_mfs_error(current_client->client, client_request, 1);
                this->drop_client(current_client->client);
                return;
                break;
        }

        // Check if the read callback is available
        if (this->files_buffer[file_index].reader_fn == NULL) {
            // Read operation not supported by this file.
            if (this->send_mfs_error(current_client->client, client_request, 1005) != 0) {
                // Error sending error. Drop client and move on.
                this->drop_client(current_client->client);
            }
            return;
        }
        // All the stars are now aligned. Call the callback.
        int64_t result = this->files_buffer[file_index].reader_fn(this->data_buffer); // Make it read into the data buffer
        if (result < 0) {
            // callback returned error, send it to client.
            if (this->send_mfs_error(current_client->client, client_request, (result * -1)) != 0) {
                // Errored sending error.
                this->drop_client(current_client->client);
            }
            return;
        }
        // Send response back to the client.
        client_request.op = MFS_RESPONSE_OF(MFS_OP_READ);
        client_request.data = {this->data_buffer.buffer, (size_t)result};
        if (this->send_mfs_message(current_client->client, client_request) != 0) {
            // Error sending response. send_mfs_message already drops client if its de-synchronised, so we just continue.
            return;
        }
    }

    void handle_write(client_handle_t* current_client, mfs_message_t client_request) {
        ssize_t file_index = this->get_file_index(client_request.path);
        switch (file_index) {
            case -1:
                // File not found.
                if (this->send_mfs_error(current_client->client, client_request, 1000) != 0) {
                    // Error sending error. Drop client and move on.
                    this->drop_client(current_client->client);
                }
                return;
                break;

            case -2:
                // Internal error. This server cannot serve clients properly if we are here. We send an operation failed unexpectadly error code and drop client.
                this->send_mfs_error(current_client->client, client_request, 1);
                this->drop_client(current_client->client);
                return;
                break;
        }
        if (this->files_buffer[file_index].writer_fn == NULL) {
            // write operation not supported by this file.
            if (this->send_mfs_error(current_client->client, client_request, 1004) != 0) {
                // Error sending error. Drop client and move on.
                this->drop_client(current_client->client);
            }
            return;
        }

        int64_t result = this->files_buffer[file_index].writer_fn(client_request.data);
        if (result < 0) {
            if (this->send_mfs_error(current_client->client, client_request, (result * -1)) != 0) {
                // Errored sending error.
                this->drop_client(current_client->client);
            }
            return;
        }
        // Send response back to the client.
        this->data_buffer.buffer[0] = (result >> 0) & 0xFF;
        this->data_buffer.buffer[1] = (result >> 8) & 0xFF;
        this->data_buffer.buffer[2] = (result >> 16) & 0xFF;
        this->data_buffer.buffer[3] = (result >> 24) & 0xFF;
        client_request.op = MFS_RESPONSE_OF(MFS_OP_WRITE);
        client_request.data = {this->data_buffer.buffer, 4};
        if (this->send_mfs_message(current_client->client, client_request) != 0) {
            // Error sending response. send_mfs_message already drops client if its de-synchronised, so we just continue.
            return;
        }
    }

public:
    uint64_t client_wait_ms = 2000; // Amount of milliseconds to wait for each client. If no valid MFS message is sent during this, the client is dropped with a timeout error.
    size_t hard_limit_bytes = 2048; // The maximum amount of bytes to consume if the client's request is too big. If its any higher tha this, the client is dropped immidiently.

    // TODO
    /*
        serve_clients() fn +
        accept_clients() fn +
        register_file() fn + DONT FORGET TO VERIFY FILE PATH CONTENTS! It should not contain a null terminator in the path, only at the end of the C-string.
        deregister_file() fn +
    */

    // THE function that serves clients.
    void serve_clients(void) {
        mfs_message_t empty_msg = {
            .op = 0,
            .path = {NULL,0},
            .data = {NULL, 0},
        };

        for (size_t i = 0; i < this->clients_bsize; i++) {
            client_handle_t* current_client = &this->clients_buffer[i];

            // Check if client is empty
            if (current_client->client < 0) continue;

            // Check if timer has expired.
            if (current_client->timeout_time <= this->gettime_fn()) {
                this->send_mfs_error(current_client->client, empty_msg, 3000);
                this->drop_client(current_client->client); // No error checking needed because we are already dropping it.
                continue;
            }

            // at this point, the client hasn't expired, and is a valid client.
            if (this->avail_fn(current_client->client) >= 9) {
                mfs_message_t client_request;
                if (this->read_mfs_message(current_client->client, &client_request) != 0) continue; // if the client was dropped, the fd is set to empty by drop_client anyway.
                // Also update the client's timeout before i forget it. I miss Zig's `defer` a lot.
                current_client->timeout_time = this->gettime_fn() + this->client_wait_ms;

                switch (client_request.op) {
                    case MFS_OP_NOOP:
                        // Just send what the client sent at us back.
                        client_request.op = MFS_RESPONSE_OF(MFS_OP_NOOP);
                        if (this->send_mfs_message(current_client->client, client_request) != 0) {
                            // sending message failed, if it fails we can't exactly send an error to client. Force drop.
                            this->drop_client(current_client->client);
                            continue;
                        }
                        break;

                    case MFS_OP_ERROR:
                        // A normally functioning client shouldn't send this. give them an illegal operation error and drop them.
                        this->send_mfs_error(current_client->client, empty_msg, 3003);
                        this->drop_client(current_client->client);
                        break;

                    case MFS_OP_LS:
                        // Client requested an LS.
                        if (this->list_files(current_client->client) != 0) {
                            this->drop_client(current_client->client);
                            continue;
                        }
                        break;

                    case MFS_OP_READ:
                            this->handle_read(current_client, client_request); // This function does EVERYTHING for us, so we just call it.
                        break;

                    case MFS_OP_WRITE:
                            //Serial.println("Handling write.");
                            this->handle_write(current_client, client_request);
                        break;
                }

            }
        }
    }

    void accept_clients(void) {
        for (size_t i = 0; i < this->clients_bsize; i++) {
            client_handle_t* current_client = &this->clients_buffer[i];
            if (current_client->client < 0) {
                current_client->client = this->accept_fn();
                current_client->timeout_time = this->gettime_fn() + this->client_wait_ms;
            }
        }
    }

    // Register file. Returns 0 on success.
    // Returns 1 if there are no empty slots left.
    // Returns 2 if the path is already taken by another file.
    // Returns 3 if there are too much files (>SSIZE_MAX)
    // returns 4 if the path is invalid.
    int32_t register_file(mfs_file_t new_file) {
        // First, check if the path is valid.
        if (new_file.path.buffer[new_file.path.buffer_size - 1] != '\0') return 4;
        // -1 to exclude last byte.
        for (size_t i = 0; i < (new_file.path.buffer_size - 1); i++) {
            if (new_file.path.buffer[i] == '\0') return 4;
        }

        ssize_t index = this->get_file_index(new_file.path);
        if (index == -2) return 3;
        if (index >= 0) return 2;

        ssize_t empty_slot = -1;
        for (size_t i = 0; i < this->files_bsize; i++) {
            if (i > SSIZE_MAX) return 3;

            mfs_file_t* cur_file = &this->files_buffer[i];
            if (cur_file->path.buffer_size == NULL && cur_file->path.buffer == NULL) {
                empty_slot = i;
                break;
            }
        }
        if (empty_slot == -1) return 1;
        mfs_file_t* cur_file = &this->files_buffer[empty_slot];
        cur_file->path.buffer = new_file.path.buffer;
        cur_file->path.buffer_size = new_file.path.buffer_size;
        cur_file->reader_fn = new_file.reader_fn;
        cur_file->writer_fn = new_file.writer_fn;
        return 0;
    }

    // Deregister file. Removes the file from the MFS server whoose path matches. Returns 0 on success
    // returns 1 if the file is not found
    // Returns 2 if the maximum file amount (>SSIZE_MAX) is exceeded.
    int32_t deregister_file(slice_t path) {
        ssize_t index = this->get_file_index(path);
        if (index >= 0) {
            mfs_file_t* cur_file = &this->files_buffer[index];
            cur_file->path.buffer = NULL;
            cur_file->path.buffer_size = NULL;
        } else {
            return index * -1; // This is fine because its just making get_file_index errors positive.
        }
        return 0;
    }

    // Finally, the constructor.
    mfs_server(read_cb readf, write_cb writef, close_cb closef, available_cb availf, accept_cb acceptf, time_cb timef, slice_t pathbuf, slice_t databuf, client_handle_t* clientbuf, size_t clientbuf_size, mfs_file_t* filebuf, size_t filebuf_size) {
        this->read_fn = readf;
        this->write_fn = writef;
        this->close_fn = closef;
        this->avail_fn = availf;
        this->accept_fn = acceptf;
        this->gettime_fn = timef;
        this->path_buffer = {pathbuf.buffer, pathbuf.buffer_size};
        this->data_buffer = {databuf.buffer, databuf.buffer_size};
        this->clients_buffer = clientbuf;
        this->clients_bsize = clientbuf_size;
        this->files_buffer = filebuf;
        this->files_bsize = filebuf_size;
    }

};


WiFiClient wclients[2];
WiFiServer server(1212);

ssize_t client_writer(client_t client, char* src, size_t n) {
    if (n > SSIZE_MAX || client < 0) return -1;
    return wclients[client].write(src, n);
}

ssize_t client_reader(client_t client, char* dest, size_t n) {
    //Serial.println("client_reader called!");
    if (n > SSIZE_MAX || client < 0) return -1;
    wclients[client].setNoDelay(true);
    wclients[client].setTimeout(2000);

    return wclients[client].readBytes(dest, n);
}

void client_closer(client_t client) {
    if (client < 0) return;
    wclients[client].stop();
}

size_t client_avail(client_t client) {
    if (client < 0) return 0;
    return wclients[client].available();
}

client_t client_accept(void) {
        for (size_t i = 0; i < 2; i++) {
        if (!wclients[i].connected()) {
            WiFiClient client = server.accept();
            if (client) {
                wclients[i] = client;
                Serial.println("Accepted a client.");
                return i;
            }
        }
    }
    return -1;
}

uint64_t timecb(void) {
    return millis();
}

int64_t uart_writer(slice_t src) {
    //Serial.println("Uart writer called.");
    return Serial.write(src.buffer, src.buffer_size);
}

int64_t uart_reader(slice_t dest) {
    //Serial.println("Uart Reader called.");
    size_t bytes_to_read = dest.buffer_size;
    size_t avail = Serial.available();

    if (avail < bytes_to_read) bytes_to_read = avail;
    return Serial.readBytes(dest.buffer, bytes_to_read);
}

char* uart_path = "/etc/uart0\0";
size_t uart_psize = 11;

slice_t uart_slice = {uart_path, uart_psize};

mfs_file_t uart_file = {uart_slice, &uart_writer, &uart_reader};

char pbuf[32];
char dbuf[4096];
slice_t pathbuf = {pbuf, 32};
slice_t databuf = {dbuf, 4096};

client_handle_t clientsbuf[2];
mfs_file_t filesbuf[1];

mfs_server mymfs(&client_reader, &client_writer, &client_closer, &client_avail, &client_accept, &timecb, pathbuf, databuf, clientsbuf, 2, filesbuf, 1);


const char* wssid = "";
const char* wpasswd = "";

void setup() {
    pinMode(2, OUTPUT);
    WiFi.begin(wssid, wpasswd);
    mymfs.register_file(uart_file);
    Serial.begin(115200);
    server.begin();
}

void loop() {
    while (WiFi.status() != WL_CONNECTED) {
        digitalWrite(2, 1);
        delay(500);
        digitalWrite(2, 0);
        delay(250);
    }
    mymfs.accept_clients();
    mymfs.serve_clients();

    delay(1);
}
