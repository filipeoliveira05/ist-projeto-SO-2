#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "parser.h"
#include "src/client/api.h"
#include "src/common/constants.h"
#include "src/common/io.h"

char req_pipe_path[256] = "/tmp/req";
char resp_pipe_path[256] = "/tmp/resp";
char notif_pipe_path[256] = "/tmp/notif";
int notif_pipe;

void print_stdout(char operation_char, char response_code)
{
  char operation[12];

  switch (operation_char)
  {
  case '1':
    strcpy(operation, "connect");
    break;
  case '2':
    strcpy(operation, "disconnect");
    break;
  case '3':
    strcpy(operation, "subscribe");
    break;
  case '4':
    strcpy(operation, "unsubscribe");
    break;
  default: // TODO erro
    break;
  }
  fprintf(stdout, "Server returned %c for operation: %s\n", response_code, operation);
}

void *process_stdin()
{
  char keys[MAX_NUMBER_SUB][MAX_STRING_SIZE] = {0};
  unsigned int delay_ms;
  size_t num;

  while (1)
  {
    fprintf(stdout, "%s", ">>");
    fflush(stdout);
    switch (get_next(STDIN_FILENO))
    {
    case CMD_DISCONNECT:
      if (kvs_disconnect(req_pipe_path, resp_pipe_path))
      {
        fprintf(stderr, "Failed to disconnect to the server\n");
        print_stdout('2', '1');
        return NULL;
      }
      close(notif_pipe);
      unlink(notif_pipe_path);
      print_stdout('2', '0');
      printf("Disconnected from server\n");
      return NULL;

    case CMD_SUBSCRIBE:
      num = parse_list(STDIN_FILENO, keys, 1, MAX_STRING_SIZE);
      if (num == 0)
      {
        print_stdout('3', '1');
        fprintf(stderr, "Invalid command. See HELP for usage\n");
        continue;
      }

      if (kvs_subscribe(keys[0]))
      {
        print_stdout('3', '1');
        fprintf(stderr, "Command subscribe failed\n");
      }
      print_stdout('3', '0');

      break;

    case CMD_UNSUBSCRIBE:
      num = parse_list(STDIN_FILENO, keys, 1, MAX_STRING_SIZE);
      if (num == 0)
      {
        print_stdout('4', '1');
        fprintf(stderr, "Invalid command. See HELP for usage\n");
        continue;
      }

      if (kvs_unsubscribe(keys[0]))
      {
        print_stdout('4', '1');
        fprintf(stderr, "Command subscribe failed\n");
      }

      print_stdout('4', '0');
      break;

    case CMD_DELAY:
      if (parse_delay(STDIN_FILENO, &delay_ms) == -1)
      {
        fprintf(stderr, "Invalid command. See HELP for usage\n");
        continue;
      }

      if (delay_ms > 0)
      {
        printf("Waiting...\n");
        delay(delay_ms);
      }
      break;

    case CMD_INVALID:
      fprintf(stderr, "Invalid command. See HELP for usage\n");
      break;

    case CMD_EMPTY:
      break;

    case EOC:
      // input should end in a disconnect, or it will loop here forever
      break;
    }
  }
}

void *process_notifications()
{
  char buf[256];

  notif_pipe = open(notif_pipe_path, O_RDONLY);
  while (1)
  {
    read(notif_pipe, buf, sizeof(buf));
    fprintf(stdout, "%s\n", buf);
  }
}

int main(int argc, char *argv[])
{
  if (argc < 3)
  {
    fprintf(stderr, "Usage: %s <client_unique_id> <register_pipe_path>\n",
            argv[0]);
    return 1;
  }

  char keys[MAX_NUMBER_SUB][MAX_STRING_SIZE] = {0};
  unsigned int delay_ms;
  size_t num;

  // Permite ao cliente receber notificações
  int notif_pipe_fd;

  strncat(req_pipe_path, argv[1], strlen(argv[1]) * sizeof(char));
  memset(req_pipe_path + strlen(req_pipe_path), '\0', sizeof(req_pipe_path) - strlen(req_pipe_path));

  strncat(resp_pipe_path, argv[1], strlen(argv[1]) * sizeof(char));
  memset(resp_pipe_path + strlen(resp_pipe_path), '\0', sizeof(resp_pipe_path) - strlen(resp_pipe_path));

  strncat(notif_pipe_path, argv[1], strlen(argv[1]) * sizeof(char));
  memset(notif_pipe_path + strlen(notif_pipe_path), '\0', sizeof(notif_pipe_path) - strlen(notif_pipe_path));

  char server_pipe_path[256];
  strcpy(server_pipe_path, "/tmp/server");
  strcat(server_pipe_path, argv[2]);

  fprintf(stderr, "Conecting...\n");
  // TODO open pipes
  if (kvs_connect(req_pipe_path, resp_pipe_path, server_pipe_path, notif_pipe_path, &notif_pipe) != 0)
  {
    fprintf(stderr, "Failed to connect to the server\n");
    return 1;
  }
  fprintf(stderr, "Conected Successfully\n");

  pthread_t threads[2];

  pthread_create(&threads[0], NULL, process_stdin, NULL);
  pthread_create(&threads[1], NULL, process_notifications, NULL);
  pthread_join(threads[0], NULL);
  pthread_detach(threads[1]);
}
