#include "parser.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "src/common/constants.h"

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
    } else if (ch == ')') {
      value = 1;
      break;
    } else if (ch == ']') {
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
    case 'S':
      if (read(fd, buf + 1, 9) != 9 || strncmp(buf, "SUBSCRIBE ", 10) != 0) {
        cleanup(fd);
        return CMD_INVALID;
      }

      return CMD_SUBSCRIBE;

    case 'U':
      if (read(fd, buf + 1, 11) != 11 || strncmp(buf, "UNSUBSCRIBE ", 12) != 0) {
        cleanup(fd);
        return CMD_INVALID;
      }

      return CMD_UNSUBSCRIBE;

    case 'D':
      if (read(fd, buf + 1, 5) != 5 || strncmp(buf, "DELAY ", 6) != 0) {
        if (read(fd, buf + 6, 4) != 4 || strncmp(buf, "DISCONNECT", 10) != 0) {
          cleanup(fd);
          return CMD_INVALID;
        }
        if (read(fd, buf + 10, 1) != 0 && buf[10] != '\n') {
          cleanup(fd);
          return CMD_INVALID;
        }
        return CMD_DISCONNECT;
      }

      return CMD_DELAY;

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

size_t parse_list(int fd, char keys[][MAX_STRING_SIZE], size_t max_keys, size_t max_string_size) {
  char ch;

  if (read(fd, &ch, 1) != 1 || ch != '[') {
    cleanup(fd);
    return 0;
  }

  size_t num_keys = 0;
  int output = 2;
  char key[max_string_size];
  while (num_keys < max_keys) {
    output = read_string(fd, key, max_string_size);
    if (output < 0 || output == 1) {
      cleanup(fd);
      return 0;
    }

    strcpy(keys[num_keys++], key);

    if (output == 2) {
      break;
    }
  }

  if (num_keys == max_keys && output != 2) {
    cleanup(fd);
    return 0;
  }

  if (read(fd, &ch, 1) != 1 || (ch != '\n' && ch != '\0')) {
    cleanup(fd);
    return 0;
  }

  return num_keys;
}

int parse_delay(int fd, unsigned int *delay) {
  char ch;

  if (read_uint(fd, delay, &ch) != 0) {
    cleanup(fd);
    return -1;
  }

  return 0;
}
