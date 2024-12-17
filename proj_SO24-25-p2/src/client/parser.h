#ifndef KVS_PARSER_H
#define KVS_PARSER_H

#include <stddef.h>

#include "src/common/constants.h"

enum Command {
  CMD_DISCONNECT,
  CMD_SUBSCRIBE,
  CMD_UNSUBSCRIBE,
  CMD_DELAY,
  CMD_EMPTY,
  CMD_INVALID,
  EOC  // End of commands
};

// Parses input from the given file descriptor, according to
// KVS specification.
// @param fd File descriptor of input.
// @return enum Command Command code.
enum Command get_next(int fd);

// Parses a list of strings
// @param fd File descriptor to read from.
// @param keys Array to store the keys
// @param max_pairs Maximum number of pairs it will write.
// @param max_string_size Maximum string size allowed.
// @return 0 if the command was not parsed successfully, otherwise return the
//          of keys parsed
size_t parse_list(int fd, char keys[][MAX_STRING_SIZE], size_t max_keys, size_t max_string_size);

// Parses a DELAY command.
// @param fd File descriptor to read from.
// @param delay Pointer to the variable to store the wait delay in.
// @param thread_id Pointer to the variable to store the thread ID in. May not be set.
// @return 0 if no thread was specified, 1 if a thread was specified, -1 on error.
int parse_delay(int fd, unsigned int *delay);

#endif  // KVS_PARSER_H
