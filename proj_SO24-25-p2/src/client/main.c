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

void initializesClient(char *req_pipe_path, char *resp_pipe_path, char *notif_pipe_path, char *server_pipe_path, char *argv[], Client *client)
{
  // Concatenate the given argv[1] and argv[2] to the respective paths
  snprintf(req_pipe_path, 256, "%s%s", "/tmp/req", argv[1]);
  snprintf(resp_pipe_path, 256, "%s%s", "/tmp/resp", argv[1]);
  snprintf(notif_pipe_path, 256, "%s%s", "/tmp/notif", argv[1]);
  snprintf(server_pipe_path, 256, "%s%s", "/tmp/server", argv[2]);

  // Now assign these paths to the client struct
  // Dynamically allocate memory and copy the string into client fields
  client->req_pipe_path = strdup(req_pipe_path);
  client->resp_pipe_path = strdup(resp_pipe_path);
  client->notif_pipe_path = strdup(notif_pipe_path);
  client->server_pipe_path = strdup(server_pipe_path);
}

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
  default:
    break;
  }
  fprintf(stdout, "Server returned %c for operation: %s\n", response_code, operation);
}

void *process_stdin(void *arg)
{
  Client *client = (Client *)arg; // Cast back to Client *

  char keys[MAX_NUMBER_SUB][MAX_STRING_SIZE] = {0};
  unsigned int delay_ms;
  size_t num;

  while (1)
  {
    // fprintf(stdout, "%s", ">>");
    fflush(stdout);
    switch (get_next(STDIN_FILENO))
    {
    case CMD_DISCONNECT:
      if (kvs_disconnect(client))
      {
        // fprintf(stderr, "Failed to disconnect to the server\n");
        print_stdout('2', '1');
        return NULL;
      }
      print_stdout('2', '0');
      // printf("Disconnected from server\n");
      return NULL;

    case CMD_SUBSCRIBE:
      num = parse_list(STDIN_FILENO, keys, 1, MAX_STRING_SIZE);
      if (num == 0)
      {
        print_stdout('3', '1');
        // fprintf(stderr, "Invalid command. See HELP for usage\n");
        continue;
      }

      if (!kvs_subscribe(keys[0], client))
      {
        print_stdout('3', '1');
        // fprintf(stderr, "Command subscribe failed\n");
        continue;
      }
      print_stdout('3', '0');

      break;

    case CMD_UNSUBSCRIBE:
      num = parse_list(STDIN_FILENO, keys, 1, MAX_STRING_SIZE);
      if (num == 0)
      {
        print_stdout('4', '1');
        // fprintf(stderr, "Invalid command. See HELP for usage\n");
        continue;
      }

      if (kvs_unsubscribe(keys[0], client))
      {
        print_stdout('4', '1');
        // fprintf(stderr, "Command unsubscribe failed\n");
      }

      print_stdout('4', '0');
      break;

    case CMD_DELAY:
      if (parse_delay(STDIN_FILENO, &delay_ms) == -1)
      {
        // fprintf(stderr, "Invalid command. See HELP for usage\n");
        continue;
      }

      if (delay_ms > 0)
      {
        // printf("Waiting...\n");
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

void *process_notifications(void *arg)
{
  Client *client = (Client *)arg; // Cast back to Client *
  char buf[256];

  int notif_pipe = open(client->notif_pipe_path, O_RDONLY);
  while (1)
  {
    // Read data from the notification pipe
    ssize_t bytes_read = read(notif_pipe, buf, sizeof(buf) - 1);

    if (bytes_read > 0)
    {
      // Null-terminate the string and print it
      buf[bytes_read] = '\0';
      fprintf(stdout, "%s\n", buf);

      // Clear the buffer to avoid printing old data in the future
      memset(buf, 0, sizeof(buf));
    }
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
  Client *client = (Client *)malloc(sizeof(Client));
  if (client == NULL)
  {
    perror("Failed to allocate memory for client");
    return 1;
  }

  char req_pipe_path[256] = "/tmp/req";
  char resp_pipe_path[256] = "/tmp/resp";
  char notif_pipe_path[256] = "/tmp/notif";
  char server_pipe_path[256] = "/tmp/server";

  // Permite ao cliente receber notificações
  initializesClient(req_pipe_path, resp_pipe_path, notif_pipe_path, server_pipe_path, argv, client);

    if (kvs_connect(client->req_pipe_path, client->resp_pipe_path, client->server_pipe_path, client->notif_pipe_path, client) != 0)
  {
    fprintf(stderr, "Failed to connect to the server\n");
    return 1;
  }
  // fprintf(stderr, "Conected Successfully\n");

  pthread_t threads[2];
  pthread_create(&threads[0], NULL, process_stdin, (void *)client);
  pthread_create(&threads[1], NULL, process_notifications, (void *)client);
  pthread_join(threads[0], NULL);
  pthread_detach(threads[1]);
  free(client->req_pipe_path);
  free(client->resp_pipe_path);
  free(client->notif_pipe_path);
  free(client->server_pipe_path);
  free(client);
}
