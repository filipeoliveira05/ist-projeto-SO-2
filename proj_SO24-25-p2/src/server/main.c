#include <dirent.h>
#include <semaphore.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>

#include "constants.h"
#include "../common/constants.h"
#include "io.h"
#include "operations.h"
#include "parser.h"
#include "pthread.h"

SessionData *sessionData;
pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t n_current_backups_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t BUFFER_MUTEX;

size_t active_backups = 0; // Number of active backups
size_t max_backups;        // Maximum allowed simultaneous backups
size_t max_threads;        // Maximum allowed simultaneous threads
char register_FIFO_name[256];
char *jobs_directory = NULL;
int server_pipe_fd;

int indexClientCount = 0;
sem_t client_thread_semaphore;

/**
 * helper function to send messages
 * retries to send whatever was not sent in the beginning
 */
void send_msg(int fd, char const *str)
{
  size_t len = strlen(str);
  size_t written = 0;

  while (written < len)
  {
    ssize_t ret = write(fd, str + written, len - written);
    if (ret < 0)
    {
      perror("Write failed");
      exit(EXIT_FAILURE);
    }

    written += (size_t)ret;
  }
}

int filter_job_files(const struct dirent *entry)
{
  const char *dot = strrchr(entry->d_name, '.');
  if (dot != NULL && strcmp(dot, ".job") == 0)
  {
    return 1; // Keep this file (it has the .job extension)
  }
  return 0;
}

static int entry_files(const char *dir, struct dirent *entry, char *in_path,
                       char *out_path)
{
  const char *dot = strrchr(entry->d_name, '.');
  if (dot == NULL || dot == entry->d_name || strlen(dot) != 4 ||
      strcmp(dot, ".job"))
  {
    return 1;
  }

  if (strlen(entry->d_name) + strlen(dir) + 2 > MAX_JOB_FILE_NAME_SIZE)
  {
    fprintf(stderr, "%s/%s\n", dir, entry->d_name);
    return 1;
  }

  strcpy(in_path, dir);
  strcat(in_path, "/");
  strcat(in_path, entry->d_name);

  strcpy(out_path, in_path);
  strcpy(strrchr(out_path, '.'), ".out");

  return 0;
}

static int run_job(int in_fd, int out_fd, char *filename)
{
  size_t file_backups = 0;
  while (1)
  {
    char keys[MAX_WRITE_SIZE][MAX_STRING_SIZE] = {0};
    char values[MAX_WRITE_SIZE][MAX_STRING_SIZE] = {0};
    unsigned int delay;
    size_t num_pairs;

    switch (get_next(in_fd))
    {
    case CMD_WRITE:
      num_pairs =
          parse_write(in_fd, keys, values, MAX_WRITE_SIZE, MAX_STRING_SIZE);
      if (num_pairs == 0)
      {
        write_str(STDERR_FILENO, "Invalid command. See HELP for usage\n");
        continue;
      }

      if (kvs_write(num_pairs, keys, values))
      {
        write_str(STDERR_FILENO, "Failed to write pair\n");
      }
      break;

    case CMD_READ:
      num_pairs =
          parse_read_delete(in_fd, keys, MAX_WRITE_SIZE, MAX_STRING_SIZE);

      if (num_pairs == 0)
      {
        write_str(STDERR_FILENO, "Invalid command. See HELP for usage\n");
        continue;
      }

      if (kvs_read(num_pairs, keys, out_fd))
      {
        write_str(STDERR_FILENO, "Failed to read pair\n");
      }
      break;

    case CMD_DELETE:
      num_pairs =
          parse_read_delete(in_fd, keys, MAX_WRITE_SIZE, MAX_STRING_SIZE);

      if (num_pairs == 0)
      {
        write_str(STDERR_FILENO, "Invalid command. See HELP for usage\n");
        continue;
      }

      if (kvs_delete(num_pairs, keys, out_fd))
      {
        write_str(STDERR_FILENO, "Failed to delete pair\n");
      }
      break;

    case CMD_SHOW:
      kvs_show(out_fd);
      break;

    case CMD_WAIT:
      if (parse_wait(in_fd, &delay, NULL) == -1)
      {
        write_str(STDERR_FILENO, "Invalid command. See HELP for usage\n");
        continue;
      }

      if (delay > 0)
      {
        printf("Waiting %d seconds\n", delay / 1000);
        kvs_wait(delay);
      }
      break;

    case CMD_BACKUP:
      pthread_mutex_lock(&n_current_backups_lock);
      if (active_backups >= max_backups)
      {
        wait(NULL);
      }
      else
      {
        active_backups++;
      }
      pthread_mutex_unlock(&n_current_backups_lock);
      int aux = kvs_backup(++file_backups, filename, jobs_directory);

      if (aux < 0)
      {
        write_str(STDERR_FILENO, "Failed to do backup\n");
      }
      else if (aux == 1)
      {
        return 1;
      }
      break;

    case CMD_INVALID:
      write_str(STDERR_FILENO, "Invalid command. See HELP for usage\n");
      break;

    case CMD_HELP:
      write_str(STDOUT_FILENO,
                "Available commands:\n"
                "  WRITE [(key,value)(key2,value2),...]\n"
                "  READ [key,key2,...]\n"
                "  DELETE [key,key2,...]\n"
                "  SHOW\n"
                "  WAIT <delay_ms>\n"
                "  BACKUP\n" // Not implemented
                "  HELP\n");

      break;

    case CMD_EMPTY:
      break;

    case EOC:
      printf("EOF\n");
      return 0;
    }
  }
}

// frees arguments
static void *get_file(void *arguments)
{
  struct SharedData *thread_data = (struct SharedData *)arguments;
  DIR *dir = thread_data->dir;
  char *dir_name = thread_data->dir_name;

  if (pthread_mutex_lock(&thread_data->directory_mutex) != 0)
  {
    fprintf(stderr, "Thread failed to lock directory_mutex\n");
    return NULL;
  }

  struct dirent *entry;
  char in_path[MAX_JOB_FILE_NAME_SIZE], out_path[MAX_JOB_FILE_NAME_SIZE];
  while ((entry = readdir(dir)) != NULL)
  {
    if (entry_files(dir_name, entry, in_path, out_path))
    {
      continue;
    }

    if (pthread_mutex_unlock(&thread_data->directory_mutex) != 0)
    {
      fprintf(stderr, "Thread failed to unlock directory_mutex\n");
      return NULL;
    }

    int in_fd = open(in_path, O_RDONLY);
    if (in_fd == -1)
    {
      write_str(STDERR_FILENO, "Failed to open input file: ");
      write_str(STDERR_FILENO, in_path);
      write_str(STDERR_FILENO, "\n");
      pthread_exit(NULL);
    }

    int out_fd = open(out_path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (out_fd == -1)
    {
      write_str(STDERR_FILENO, "Failed to open output file: ");
      write_str(STDERR_FILENO, out_path);
      write_str(STDERR_FILENO, "\n");
      pthread_exit(NULL);
    }

    int out = run_job(in_fd, out_fd, entry->d_name);

    close(in_fd);
    close(out_fd);

    if (out)
    {
      if (closedir(dir) == -1)
      {
        fprintf(stderr, "Failed to close directory\n");
        return 0;
      }

      exit(0);
    }

    if (pthread_mutex_lock(&thread_data->directory_mutex) != 0)
    {
      fprintf(stderr, "Thread failed to lock directory_mutex\n");
      return NULL;
    }
  }

  if (pthread_mutex_unlock(&thread_data->directory_mutex) != 0)
  {
    fprintf(stderr, "Thread failed to unlock directory_mutex\n");
    return NULL;
  }

  pthread_exit(NULL);
}

void createsRegisterFIFO()
{
  if (strlen(register_FIFO_name) == 0)
  {
    write_str(STDERR_FILENO, "Invalid or empty FIFO name\n");
  }

  // Remove o pipe, se existir
  if (unlink(register_FIFO_name) != 0 && errno != ENOENT)
  {
    perror("unlink failed");
    pthread_exit(NULL); // Exit thread instead of the whole process
  }
  if (mkfifo(register_FIFO_name, 0777) != 0)
  {
    perror("mkfifo failed");
    pthread_exit(NULL);
  }
}

void *handle_requests(void *arg)
{
  Client *client = (Client *)arg;

  char buf[MAX_PIPE_BUFFER_SIZE];
  char key[MAX_PIPE_PATH_LENGTH + 1];
  client->req_pipe = open(client->req_pipe_path, O_RDONLY);
  if (client->req_pipe == -1)
  {
    perror("Verify_requests: Failed to open request pipe");
    return NULL;
  }
  fprintf(stderr, "Verify_requests: Waiting for requests.\n");
  int resp_pipe_fd = -1;

  while (1)
  {

    ssize_t bytes_read = read(client->req_pipe, buf, sizeof(buf));

    if (bytes_read == 0)
    {
      fprintf(stderr, "Verify_requests: Disconnect request received.\n");
      int disconect_result = kvs_disconnect_client(client->notif_pipe_path);
      client->resp_pipe = open(client->resp_pipe_path, O_WRONLY);
      if (client->resp_pipe == -1)
      {
        perror("Verify_requests: Failed to open response pipe for writing");
      }
      else
      {
        const char *response_code = (disconect_result == 1) ? "20" : "21"; // "20" for success, "21" for failure
        ssize_t bytes_written = write(client->resp_pipe, response_code, 2);
        if (bytes_written < 2)
        {
          perror("Verify_requests: Failed to write Disconect response to response pipe");
        }
        else
        {
          fprintf(stderr, "Verify_requests: Disconect response successfully sent for key: %s\n", key);
          if (disconect_result)
          {
            fprintf(stderr, "Verify_requests: Disconection Successfull\n");
          }
          else
          {
            fprintf(stderr, "Verify_requests: Disconection Failed\n");
          }
        }
        close(client->resp_pipe);
      }
      fprintf(stderr, "Verify_requests: Disconnect response sent.\n");
      return NULL; // Exit the thread completely
    }

    if (bytes_read > 0)
    {
      buf[bytes_read] = '\0'; // Null-terminate for safe string operations
      fprintf(stderr, "Verify_requests: Read request from pipe: %s\n", buf);
      switch (buf[0])
      {
      case '2':
        fprintf(stderr, "Verify_requests: Disconnect request received.\n");
        int disconect_result = kvs_disconnect_client(client->notif_pipe_path);
        client->resp_pipe = open(client->resp_pipe_path, O_WRONLY);
        if (client->resp_pipe == -1)
        {
          perror("Verify_requests: Failed to open response pipe for writing");
        }
        else
        {
          const char *response_code = (disconect_result == 1) ? "20" : "21"; // "20" for success, "21" for failure
          ssize_t bytes_written = write(client->resp_pipe, response_code, 2);
          if (bytes_written < 2)
          {
            perror("Verify_requests: Failed to write disconect response to response pipe");
          }
          else
          {
            fprintf(stderr, "Verify_requests: Disconect response successfully sent for key: %s\n", key);
            if (disconect_result)
            {
              fprintf(stderr, "Verify_requests: Disconection Successfull\n");
            }
            else
            {
              fprintf(stderr, "Verify_requests: Disconection Failed\n");
            }
          }
          close(client->resp_pipe);
        }
        fprintf(stderr, "Verify_requests: Disconnect response sent.\n");
        return NULL; // Exit the thread completely

      case '3':
        strcpy(key, buf + 1);
        fprintf(stderr, "Verify_requests: Subscribe request received for key: %s\n", key);
        int subscribe_result = kvs_subscribe(client->notif_pipe_path, key); // kvs_subscribe should return 1 on success, 0 on failure
        client->resp_pipe = open(client->resp_pipe_path, O_WRONLY);
        if (client->resp_pipe == -1)
        {
          perror("Verify_requests: Failed to open response pipe for writing");
        }
        else
        {
          const char *response_code = (subscribe_result == 0) ? "30" : "31";
          ssize_t bytes_written = write(client->resp_pipe, response_code, 2);
          if (bytes_written < 2)
          {
            perror("Verify_requests: Failed to write subscribe response to response pipe");
          }
          else
          {
            fprintf(stderr, "Verify_requests: Subscribe response successfully sent for key: %s\n", key);
            if (subscribe_result)
            {
              fprintf(stderr, "Verify_requests: Subscription Successfull\n");
            }
            else
            {
              fprintf(stderr, "Verify_requests: Subscription Failed\n");
            }
          }
          close(client->resp_pipe);
        }

        break;

      case '4':
        strcpy(key, buf + 1);
        fprintf(stderr, "Verify_requests: Unsubscribe request received for key: %s\n", key);
        int unsubscribe_result = kvs_unsubscribe(client->notif_pipe_path, key); // kvs_unsubscribe should return 1 on success, 0 on failure
        client->resp_pipe = open(client->resp_pipe_path, O_WRONLY);
        if (client->resp_pipe == -1)
        {
          perror("Verify_requests: Failed to open response pipe for writing");
        }
        else
        {
          const char *response_code = (unsubscribe_result == 1) ? "40" : "41"; // "40" for success, "41" for failure
          ssize_t bytes_written = write(client->resp_pipe, response_code, 2);
          if (bytes_written < 2)
          {
            perror("Verify_requests: Failed to write unsubscribe response to response pipe");
          }
          else
          {
            fprintf(stderr, "Verify_requests: Unsubscribe response successfully sent for key: %s\n", key);
            if (unsubscribe_result)
            {
              fprintf(stderr, "Verify_requests: Unsubscription Successful\n");
            }
            else
            {
              fprintf(stderr, "Verify_requests: Unsubscription Failed\n");
            }
          }
          close(client->resp_pipe);
        }

        break;

      default:
        break;
      }
    }
  }
}
void *handle_server_pipe(void *arg)
{
  SessionData *sessionData = (SessionData *)arg; // Cast back to Session data *
  char req_pipe_path[MAX_PIPE_PATH_LENGTH];
  char resp_pipe_path[MAX_PIPE_PATH_LENGTH];
  char notif_pipe_path[MAX_PIPE_PATH_LENGTH];
  char buf[MAX_PIPE_BUFFER_SIZE];
  int resp_pipe_fd;
  int req_pipe_fd;
  server_pipe_fd = open(register_FIFO_name, O_RDONLY);
  sem_init(&client_thread_semaphore, 0, MAX_SESSION_COUNT);
  pthread_t client_threads[MAX_SESSION_COUNT];
  int active_thread_count = 0;
  while (1)
  {
    ssize_t bytes_read = read(server_pipe_fd, buf, 1 + (MAX_PIPE_PATH_LENGTH * 3));
    if (bytes_read <= 0)
    {
      continue; // Skip if no data read
    }
    if (buf[0] == '1')
    {
      memcpy(req_pipe_path, buf + 1, MAX_PIPE_PATH_LENGTH);
      printf("CLIENT %c CONECTED\n", req_pipe_path[strlen(req_pipe_path) - 1]); // Print to verify
      printf("req_pipe_path: %s\n", req_pipe_path);                             // Print to verify

      memcpy(resp_pipe_path, buf + 1 + MAX_PIPE_PATH_LENGTH, MAX_PIPE_PATH_LENGTH);
      printf("resp_pipe_path: %s\n", resp_pipe_path); // Print to verify

      memcpy(notif_pipe_path, buf + 1 + 2 * MAX_PIPE_PATH_LENGTH, MAX_PIPE_PATH_LENGTH);
      printf("notif_pipe_path: %s\n", notif_pipe_path); // Print to verify

      // int value;
      // sem_getvalue(&client_thread_semaphore, &value);
      // printf("Slots available before acquiring: %d\n", value);

      // // Wait for a slot to handle the client (using semaphore)
      // sem_wait(&client_thread_semaphore); // Block if no available threads

      // // Print the updated available slots after acquiring the slot
      // sem_getvalue(&client_thread_semaphore, &value);
      // printf("Slots available after acquiring: %d\n", value);
      // Join threads that have completed before spawning new ones
      for (int i = 0; i < active_thread_count; i++)
      {
        if (pthread_join(client_threads[i], NULL) == 0)
        {
          // Shift remaining threads down
          for (int j = i; j < active_thread_count - 1; j++)
          {
            client_threads[j] = client_threads[j + 1];
          }
          active_thread_count--;
          i--; // Recheck the current index
        }
      }
      Client *client = add_client_to_session(sessionData, req_pipe_path, resp_pipe_path, notif_pipe_path, indexClientCount);
      if (client != NULL)
      {
        indexClientCount++;
        printf("Client %d:\n", client->client_Index);
        printf("  Request Pipe Path: %s\n", client->req_pipe_path);
        printf("  Response Pipe Path: %s\n", client->resp_pipe_path);
        printf("  Notification Pipe Path: %s\n", client->notif_pipe_path);
        printf("  Request Pipe FD: %d\n", client->req_pipe);
        printf("  Response Pipe FD: %d\n", client->resp_pipe);
        printf("  Notification Pipe FD: %d\n", client->notif_pipe);
        printf("\n");

        client->resp_pipe = open(client->resp_pipe_path, O_WRONLY);
        write(client->resp_pipe, "0", 1);

        client->req_pipe = open(client->req_pipe_path, O_RDONLY);
        // pthread_t thread_request_pipe;
        pthread_create(&client_threads[active_thread_count++], NULL, handle_requests, client);
        // pthread_join(&client_threads[active_thread_count], NULL);
      }
    }
    else
    {
      strncpy(resp_pipe_path, buf + 1 + MAX_PIPE_PATH_LENGTH, MAX_PIPE_PATH_LENGTH);
      resp_pipe_fd = open(resp_pipe_path, O_WRONLY);
      write(resp_pipe_fd, "1", 1);
      // return NULL;
    }
    close(resp_pipe_fd);
  }
  sem_destroy(&client_thread_semaphore);
}
static void dispatch_threads(DIR *dir)
{
  pthread_t *threads = malloc(max_threads * sizeof(pthread_t));

  if (threads == NULL)
  {
    fprintf(stderr, "Failed to allocate memory for threads\n");
    return;
  }

  struct SharedData thread_data = {dir, jobs_directory,
                                   PTHREAD_MUTEX_INITIALIZER};

  for (size_t i = 0; i < max_threads; i++)
  {
    if (pthread_create(&threads[i], NULL, get_file, (void *)&thread_data) !=
        0)
    {
      fprintf(stderr, "Failed to create thread %zu\n", i);
      pthread_mutex_destroy(&thread_data.directory_mutex);
      free(threads);
      return;
    }
  }

  // ler do FIFO de registo

  pthread_t thread_server_pipe;
  pthread_create(&thread_server_pipe, NULL, handle_server_pipe, (void *)sessionData);

  pthread_join(thread_server_pipe, NULL);

  for (unsigned int i = 0; i < max_threads; i++)
  {
    if (pthread_join(threads[i], NULL) != 0)
    {
      fprintf(stderr, "Failed to join thread %u\n", i);
      pthread_mutex_destroy(&thread_data.directory_mutex);
      free(threads);
      return;
    }
  }

  free(threads);
}

int main(int argc, char **argv)
{
  if (argc < 5)
  {
    write_str(STDERR_FILENO, "Usage: ");
    write_str(STDERR_FILENO, argv[0]);
    write_str(STDERR_FILENO, " <jobs_dir>");
    write_str(STDERR_FILENO, " <max_threads>");
    write_str(STDERR_FILENO, " <max_backups>");
    write_str(STDERR_FILENO, " <register_FIFO_name> \n");
    return 1;
  }

  jobs_directory = argv[1];

  char *endptr;
  max_backups = strtoul(argv[3], &endptr, 10);

  if (*endptr != '\0')
  {
    fprintf(stderr, "Invalid max_proc value\n");
    return 1;
  }

  max_threads = strtoul(argv[2], &endptr, 10);

  if (*endptr != '\0')
  {
    fprintf(stderr, "Invalid max_threads value\n");
    return 1;
  }

  if (max_backups <= 0)
  {
    write_str(STDERR_FILENO, "Invalid number of backups\n");
    return 0;
  }

  if (max_threads <= 0)
  {
    write_str(STDERR_FILENO, "Invalid number of threads\n");
    return 0;
  }

  strcpy(register_FIFO_name, "/tmp/server");
  strcat(register_FIFO_name, argv[4]);

  sessionData = malloc(sizeof(SessionData));
  if (sessionData == NULL)
  {
    perror("malloc failed");
  }
  fprintf(stderr, "FIFO name: %s\n", register_FIFO_name);
  createsRegisterFIFO();
  fprintf(stderr, "CRIOU REGISTER FIFO: %s\n", register_FIFO_name);

  sessionData->server_pipe_path = malloc(strlen(register_FIFO_name) + 1); // Allocate memory
  if (sessionData->server_pipe_path == NULL)
  {
    perror("malloc failed");
  }
  strncpy(sessionData->server_pipe_path, register_FIFO_name, strlen(register_FIFO_name) + 1);

  if (kvs_init())
  {
    write_str(STDERR_FILENO, "Failed to initialize KVS\n");
    return 1;
  }

  DIR *dir = opendir(argv[1]);
  if (dir == NULL)
  {
    fprintf(stderr, "Failed to open directory: %s\n", argv[1]);
    return 0;
  }

  dispatch_threads(dir);

  if (closedir(dir) == -1)
  {
    fprintf(stderr, "Failed to close directory\n");
    return 0;
  }

  while (active_backups > 0)
  {
    wait(NULL);
    active_backups--;
  }

  unlink(register_FIFO_name);
  kvs_terminate();
  free(sessionData->server_pipe_path);
  free(sessionData);

  return 0;
}