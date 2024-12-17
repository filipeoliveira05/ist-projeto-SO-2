#include "parser.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "constants.h"
#include "io.h"

// Reads a string and indicates the position from where it was
// extracted, based on the KVS specification.
// @param fd File to read from.
// @param buffer To write the string in.
// @param max Maximum string size.
static int read_string(int fd, char *buffer, size_t max) {
  ssize_t bytes_read;
  char ch;
  size_t i = 0;
  int value = -1;

  while (i < max) {
    bytes_read = read(fd, &ch, 1);

    if (bytes_read <= 0) {
        return -1;
    }

    if (ch == ' ') {
      return -1;
    }

    if (ch == ',') {
      value = 0;
      break;
    }
    else if (ch == ')') {
      value = 1;
      break;
    }
    else if (ch == ']') {
      value = 2;
      break;
    }

    buffer[i++] = ch;
  }

  buffer[i] = '\0';

  return value;
}

// Reads a number and stores it in an unsigned integer
// variable.
// @param fd File to read from.
// @param value To store the number in.
// @param next Will point to the character succeding the number.
static int read_uint(int fd, unsigned int *value, char *next) {
  char buf[16];

  int i = 0;
  while (1) {
    if (read(fd, buf + i, 1) == 0) {
      *next = '\0';
      break;
    }

    *next = buf[i];

    if (buf[i] > '9' || buf[i] < '0') {
      buf[i] = '\0';
      break;
    }

    i++;
  }

  unsigned long ul = strtoul(buf, NULL, 10);

  if (ul > UINT_MAX) {
    return 1;
  }

  *value = (unsigned int)ul;

  return 0;
}

// Jumps file descriptor to next line.
// @param fd File descriptor.
static void cleanup(int fd) {
  char ch;
  while (read(fd, &ch, 1) == 1 && ch != '\n')
    ;
}

enum Command get_next(int fd) {
  char buf[16];
  if (read(fd, buf, 1) != 1) {
    return EOC;
  }

  switch (buf[0]) {
    case 'W':
      if (read(fd, buf + 1, 4) != 4 || strncmp(buf, "WAIT ", 5) != 0) {
        if (read(fd, buf + 5, 1) != 1 || strncmp(buf, "WRITE ", 6) != 0) {
          cleanup(fd);
          return CMD_INVALID;
        }
        return CMD_WRITE;
      }

      return CMD_WAIT;

    case 'R':
      if (read(fd, buf + 1, 4) != 4 || strncmp(buf, "READ ", 5) != 0) {
        cleanup(fd);
        return CMD_INVALID;
      }

      return CMD_READ;

    case 'D':
      if (read(fd, buf + 1, 6) != 6 || strncmp(buf, "DELETE ", 7) != 0) {
        cleanup(fd);
        return CMD_INVALID;
      }

      return CMD_DELETE;

    case 'S':
      if (read(fd, buf + 1, 3) != 3 || strncmp(buf, "SHOW", 4) != 0) {
        cleanup(fd);
        return CMD_INVALID;
      }

      if (read(fd, buf + 4, 1) != 0 && buf[4] != '\n') {
        cleanup(fd);
        return CMD_INVALID;
      }

      return CMD_SHOW;

    case 'B':
      if (read(fd, buf + 1, 5) != 5 || strncmp(buf, "BACKUP", 6) != 0) {
        cleanup(fd);
        return CMD_INVALID;
      }

      if (read(fd, buf + 6, 1) != 0 && buf[6] != '\n') {
        cleanup(fd);
        return CMD_INVALID;
      }

      return CMD_BACKUP;

    case 'H':
      if (read(fd, buf + 1, 3) != 3 || strncmp(buf, "HELP", 4) != 0) {
        cleanup(fd);
        return CMD_INVALID;
      }

      if (read(fd, buf + 4, 1) != 0 && buf[4] != '\n') {
        cleanup(fd);
        return CMD_INVALID;
      }

      return CMD_HELP;

    case '#':
      cleanup(fd);
      return CMD_EMPTY;

    case '\n':
      return CMD_EMPTY;

    default:
      cleanup(fd);
      return CMD_INVALID;
  }
}

// Parses a key value pair.
// @param fd File decriptor to read from.
// @param key Pointer where the key will be stored
// @param value Pointer where the value will be stored
// @return 1 if successful, 0 otherwise.
int parse_pair(int fd, char *key, char *value) {
  if (read_string(fd, key, MAX_STRING_SIZE) != 0) {
    cleanup(fd);
    return 0;
  }

  if (read_string(fd, value, MAX_STRING_SIZE) != 1) {
    cleanup(fd);
    return 0;
  }

  return 1;
}

size_t parse_write(int fd, char keys[][MAX_STRING_SIZE], char values[][MAX_STRING_SIZE], size_t max_pairs, size_t max_string_size) {
  char ch;

  if (read(fd, &ch, 1) != 1 || ch != '[') {
    cleanup(fd);
    return 0;
  }

  if (read(fd, &ch, 1) != 1 || ch != '(') {
    cleanup(fd);
    return 0;
  }

  size_t num_pairs = 0;
  char key[max_string_size];
  char value[max_string_size];
  while (num_pairs < max_pairs) {
    if(parse_pair(fd, key, value) == 0) {
      cleanup(fd);
      return 0;
    }

    strcpy(keys[num_pairs], key);
    strcpy(values[num_pairs++], value);

    if (read(fd, &ch, 1) != 1 || (ch != '(' && ch != ']')) {
      cleanup(fd);
      return 0;
    }

    if (ch == ']') {
      break;
    }
  }

  if (num_pairs == max_pairs) {
    cleanup(fd);
    return 0;
  }

  if (read(fd, &ch, 1) != 1 || (ch != '\n' && ch != '\0')) {
    cleanup(fd);
    return 0;
  }

  return num_pairs;
}

size_t parse_read_delete(int fd, char keys[][MAX_STRING_SIZE], size_t max_keys, size_t max_string_size) {
  char ch;

  if (read(fd, &ch, 1) != 1 || ch != '[') {
    cleanup(fd);
    return 0;
  }

  size_t num_keys = 0;
  char key[max_string_size];
  while (num_keys < max_keys) {
    int output = read_string(fd, key, max_string_size);
    if(output < 0 || output == 1) {
      cleanup(fd);
      return 0;
    }

    strcpy(keys[num_keys++], key);

    if (output == 2){
      break;
    }
  }

  if (num_keys == max_keys) {
    cleanup(fd);
    return 0;
  }

  if (read(fd, &ch, 1) != 1 || (ch != '\n' && ch != '\0')) {
    cleanup(fd);
    return 0;
  }

  return num_keys;
}

int parse_wait(int fd, unsigned int *delay, unsigned int *thread_id) {
  char ch;

  if (read_uint(fd, delay, &ch) != 0) {
    cleanup(fd);
    return -1;
  }

  if (ch == ' ') {
    if (thread_id == NULL) {
      cleanup(fd);
      return 0;
    }

    if (read_uint(fd, thread_id, &ch) != 0 || (ch != '\n' && ch != '\0')) {
      cleanup(fd);
      return -1;
    }

    return 1;
  } else if (ch == '\n' || ch == '\0') {
    return 0;
  } else {
    cleanup(fd);
    return -1;
  }
}
