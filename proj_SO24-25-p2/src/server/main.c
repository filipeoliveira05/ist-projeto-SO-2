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



pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t n_current_backups_lock = PTHREAD_MUTEX_INITIALIZER;
sem_t SEM_BUFFER_SPACE;
sem_t SEM_BUFFER_CLIENTS;
pthread_mutex_t BUFFER_MUTEX;

char BUFFER[MAX_SESSION_COUNT][1 + 3 * MAX_PIPE_PATH_LENGTH] = {0};
int BUFFER_RD_IND = 0;
int BUFFER_WR_IND = 0;



size_t active_backups = 0; // Number of active backups
size_t max_backups;        // Maximum allowed simultaneous backups
size_t max_threads;        // Maximum allowed simultaneous threads
char *register_FIFO_name = NULL;
char *jobs_directory = NULL;



/**
 * helper function to send messages
 * retries to send whatever was not sent in the beginning
 */
void send_msg(int fd, char const *str) {
    size_t len = strlen(str);
    size_t written = 0;

    while (written < len) {
        ssize_t ret = write(fd, str + written, len - written);
        if (ret < 0) {
            perror("Write failed");
            exit(EXIT_FAILURE);
        }

        written += (size_t)ret;
    }
}


int filter_job_files(const struct dirent *entry) {
  const char *dot = strrchr(entry->d_name, '.');
  if (dot != NULL && strcmp(dot, ".job") == 0) {
    return 1; // Keep this file (it has the .job extension)
  }
  return 0;
}

static int entry_files(const char *dir, struct dirent *entry, char *in_path,
                       char *out_path) {
  const char *dot = strrchr(entry->d_name, '.');
  if (dot == NULL || dot == entry->d_name || strlen(dot) != 4 ||
      strcmp(dot, ".job")) {
    return 1;
  }

  if (strlen(entry->d_name) + strlen(dir) + 2 > MAX_JOB_FILE_NAME_SIZE) {
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

static int run_job(int in_fd, int out_fd, char *filename) {
  size_t file_backups = 0;
  while (1) {
    char keys[MAX_WRITE_SIZE][MAX_STRING_SIZE] = {0};
    char values[MAX_WRITE_SIZE][MAX_STRING_SIZE] = {0};
    unsigned int delay;
    size_t num_pairs;

    switch (get_next(in_fd)) {
    case CMD_WRITE:
      num_pairs =
          parse_write(in_fd, keys, values, MAX_WRITE_SIZE, MAX_STRING_SIZE);
      if (num_pairs == 0) {
        write_str(STDERR_FILENO, "Invalid command. See HELP for usage\n");
        continue;
      }

      if (kvs_write(num_pairs, keys, values)) {
        write_str(STDERR_FILENO, "Failed to write pair\n");
      }
      break;

    case CMD_READ:
      num_pairs =
          parse_read_delete(in_fd, keys, MAX_WRITE_SIZE, MAX_STRING_SIZE);

      if (num_pairs == 0) {
        write_str(STDERR_FILENO, "Invalid command. See HELP for usage\n");
        continue;
      }

      if (kvs_read(num_pairs, keys, out_fd)) {
        write_str(STDERR_FILENO, "Failed to read pair\n");
      }
      break;

    case CMD_DELETE:
      num_pairs =
          parse_read_delete(in_fd, keys, MAX_WRITE_SIZE, MAX_STRING_SIZE);

      if (num_pairs == 0) {
        write_str(STDERR_FILENO, "Invalid command. See HELP for usage\n");
        continue;
      }

      if (kvs_delete(num_pairs, keys, out_fd)) {
        write_str(STDERR_FILENO, "Failed to delete pair\n");
      }
      break;

    case CMD_SHOW:
      kvs_show(out_fd);
      break;

    case CMD_WAIT:
      if (parse_wait(in_fd, &delay, NULL) == -1) {
        write_str(STDERR_FILENO, "Invalid command. See HELP for usage\n");
        continue;
      }

      if (delay > 0) {
        printf("Waiting %d seconds\n", delay / 1000);
        kvs_wait(delay);
      }
      break;

    case CMD_BACKUP:
      pthread_mutex_lock(&n_current_backups_lock);
      if (active_backups >= max_backups) {
        wait(NULL);
      } else {
        active_backups++;
      }
      pthread_mutex_unlock(&n_current_backups_lock);
      int aux = kvs_backup(++file_backups, filename, jobs_directory);

      if (aux < 0) {
        write_str(STDERR_FILENO, "Failed to do backup\n");
      } else if (aux == 1) {
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
static void *get_file(void *arguments) {
  struct SharedData *thread_data = (struct SharedData *)arguments;
  DIR *dir = thread_data->dir;
  char *dir_name = thread_data->dir_name;

  if (pthread_mutex_lock(&thread_data->directory_mutex) != 0) {
    fprintf(stderr, "Thread failed to lock directory_mutex\n");
    return NULL;
  }

  struct dirent *entry;
  char in_path[MAX_JOB_FILE_NAME_SIZE], out_path[MAX_JOB_FILE_NAME_SIZE];
  while ((entry = readdir(dir)) != NULL) {
    if (entry_files(dir_name, entry, in_path, out_path)) {
      continue;
    }

    if (pthread_mutex_unlock(&thread_data->directory_mutex) != 0) {
      fprintf(stderr, "Thread failed to unlock directory_mutex\n");
      return NULL;
    }

    int in_fd = open(in_path, O_RDONLY);
    if (in_fd == -1) {
      write_str(STDERR_FILENO, "Failed to open input file: ");
      write_str(STDERR_FILENO, in_path);
      write_str(STDERR_FILENO, "\n");
      pthread_exit(NULL);
    }

    int out_fd = open(out_path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (out_fd == -1) {
      write_str(STDERR_FILENO, "Failed to open output file: ");
      write_str(STDERR_FILENO, out_path);
      write_str(STDERR_FILENO, "\n");
      pthread_exit(NULL);
    }

    int out = run_job(in_fd, out_fd, entry->d_name);

    close(in_fd);
    close(out_fd);

    if (out) {
      if (closedir(dir) == -1) {
        fprintf(stderr, "Failed to close directory\n");
        return 0;
      }

      exit(0);
    }

    if (pthread_mutex_lock(&thread_data->directory_mutex) != 0) {
      fprintf(stderr, "Thread failed to lock directory_mutex\n");
      return NULL;
    }
  }

  if (pthread_mutex_unlock(&thread_data->directory_mutex) != 0) {
    fprintf(stderr, "Thread failed to unlock directory_mutex\n");
    return NULL;
  }

  pthread_exit(NULL);
}


void *main_FIFO() {

  // Remove o pipe, se existir
  if (unlink(register_FIFO_name) != 0 && errno != ENOENT) {
    perror("unlink failed");
    pthread_exit(NULL); // Exit thread instead of the whole process
  }
  
  fprintf(stderr,"VAI CRIAR REGISTER FIFO\n");
  fflush(stderr);
  // Cria o pipe
  if (mkfifo(register_FIFO_name, 0777) != 0) {
    perror("mkfifo failed");
    pthread_exit(NULL);
  }
  fprintf(stderr,"CRIOU REGISTER FIFO: %s\n",register_FIFO_name);

  int fifo_fd = open(register_FIFO_name, O_RDONLY);
  if (fifo_fd == -1) {
    perror("Failed to open FIFO");
    pthread_exit(NULL);
  }

  fprintf(stderr,"ABRIU REGISTER FIFO: fd%d\n",fifo_fd);




  while (1) {
    sem_wait(&SEM_BUFFER_SPACE);

    ssize_t bytes_read = read(fifo_fd, BUFFER[BUFFER_WR_IND], 1 + 3 * MAX_PIPE_PATH_LENGTH);
    if (bytes_read == 0) {
      // EOF - Break the loop for cleanup
      fprintf(stderr, "pipe closed\n");
      break;
    } else if (bytes_read == -1) {
      fprintf(stderr, "read failed: %s\n", strerror(errno));
      break; // Exit the loop on read failure
    }

    // Update write index and signal clients
    BUFFER_WR_IND = (BUFFER_WR_IND + 1) % MAX_SESSION_COUNT;
    sem_post(&SEM_BUFFER_CLIENTS);
  }




  // Cleanup resources

  close(fifo_fd);
  unlink(register_FIFO_name);
  pthread_exit(NULL); // Ensure proper thread exit
}

void *thread_manage_session() {

  char client_paths[1 + 3 * MAX_PIPE_PATH_LENGTH];
  int x=1;
  while (x)   {
    sem_wait(&SEM_BUFFER_CLIENTS);
    pthread_mutex_lock(&BUFFER_MUTEX);
    
    // Lê do buffer global
    // Lock necessário, visto que todas as threads gestoras podem alterar o índice
    strncpy(client_paths, BUFFER[BUFFER_RD_IND], (1 + 3 * MAX_PIPE_PATH_LENGTH));

    BUFFER_RD_IND = (BUFFER_RD_IND + 1) % MAX_SESSION_COUNT;
    pthread_mutex_unlock(&BUFFER_MUTEX);
    sem_post(&SEM_BUFFER_SPACE);

    // OP_CODE=1 | nome do pipe do cliente (para pedidos) | nome do pipe do cliente (para respostas) | nome do pipe do cliente (para notificações)
    int op_code_1;
    char req_pipe_path[MAX_PIPE_PATH_LENGTH];
    char resp_pipe_path[MAX_PIPE_PATH_LENGTH];
    char notif_pipe_path[MAX_PIPE_PATH_LENGTH];
    
    sscanf(client_paths, "%d%s%s%s", &op_code_1, req_pipe_path, resp_pipe_path, notif_pipe_path);
    
    // Abrir os pipes pela ordem que abre no cliente com a flag contrária
    int resp_fd = open(resp_pipe_path, O_WRONLY);
    if (resp_fd == -1) {
      perror("Failed to open response FIFO");
      exit(EXIT_FAILURE);
    }
    int req_fd = open(req_pipe_path, O_RDONLY);
    if (req_fd == -1) {
      perror("Failed to open request FIFO");
      exit(EXIT_FAILURE);
    }
    int notif_fd = open(notif_pipe_path, O_WRONLY);
    if (notif_fd == -1) {
      perror("Failed to open notification FIFO");
      exit(EXIT_FAILURE);
    }

    fprintf(stderr,"CHEGA AQUI NO SERVER\n");

    send_msg(resp_fd, "10");

    char request[42] = {0};
    while (1) {
      // Ler do pipe de pedidos
      ssize_t bytes_read = read(req_fd, request, sizeof(request));
      if (bytes_read == 0) {
        // bytes_read == 0 indica EOF
        fprintf(stderr, "Pipe closed\n");
        break; // Handle client disconnection (e.g., unsubscribe)
      } else if (bytes_read == -1) {
        // bytes_read == -1 indica erro
        fprintf(stderr, "Read failed: %s\n", strerror(errno));
        exit(EXIT_FAILURE); // FIXME should return instead of exit?
      }
    
      // Parse request
      char op_code = request[0];
      char key[41];

      char response[2];
      response[0] = op_code;

      int result;

      switch (op_code) {
        case 2: // Disconnect (OP_CODE=2)
          // Unsubscribe the client
          // Unsubscribe the client from all keys (or the specific one)
          // Modify this part as per the actual implementation logic
          result = kvs_unsubscribe(key, notif_fd); // You can pass the key for unsubscribing
          response[1] = (char)(result + '0'); // '0' or '1' based on success/failure
          send_msg(notif_fd, response);
          break;

        case 3: // Subscribe (OP_CODE=3)
          // OP_CODE=3 | key
          strncpy(key, request + 1, 41);
          result = kvs_subscribe(key, notif_fd);
          response[1] = (char)(result + '0'); // '0' or '1' based on success/failure
          send_msg(notif_fd, response);
          break;

        case 4: // Unsubscribe (OP_CODE=4)
          // OP_CODE=4 | key
          strncpy(key, request + 1, 41);
          result = kvs_unsubscribe(key, notif_fd);
          response[1] = (char)(result + '0'); // '0' or '1' based on success/failure
          send_msg(notif_fd, response);
          break;

        default:
          fprintf(stderr, "Invalid command, op_code unrecognized\n");
          break;
      }

      // Exit the inner loop if the client disconnects
      if (op_code == 2) {
        break;
      }
    }

    close(resp_fd);
    close(req_fd);
    close(notif_fd);
  }
}





static void dispatch_threads(DIR *dir) {
  pthread_t *threads = malloc(max_threads * sizeof(pthread_t));

  if (threads == NULL) {
    fprintf(stderr, "Failed to allocate memory for threads\n");
    return;
  }

  struct SharedData thread_data = {dir, jobs_directory,
                                   PTHREAD_MUTEX_INITIALIZER};

  for (size_t i = 0; i < max_threads; i++) {
    if (pthread_create(&threads[i], NULL, get_file, (void *)&thread_data) !=
        0) {
      fprintf(stderr, "Failed to create thread %zu\n", i);
      pthread_mutex_destroy(&thread_data.directory_mutex);
      free(threads);
      return;
    }
  }

  // ler do FIFO de registo

  pthread_t FIFO_register_thread;
  pthread_mutex_init(&BUFFER_MUTEX, NULL);
// Start the FIFO register thread
  pthread_create(&FIFO_register_thread, NULL, main_FIFO, NULL);

  // Wait for the FIFO_register_thread to finish before continuing
  pthread_join(FIFO_register_thread, NULL);
  fprintf(stderr, "FIFO register thread finished successfully\n");


  sem_init(&SEM_BUFFER_SPACE, 0, MAX_SESSION_COUNT);
  sem_init(&SEM_BUFFER_CLIENTS, 0, 0);
  pthread_t manager_threads[MAX_SESSION_COUNT];
  for (int i = 0; i < MAX_SESSION_COUNT; i++) {
    pthread_create(&manager_threads[i], NULL, thread_manage_session, &thread_data);
  }


  for (unsigned int i = 0; i < max_threads; i++) {
    if (pthread_join(threads[i], NULL) != 0) {
      fprintf(stderr, "Failed to join thread %u\n", i);
      pthread_mutex_destroy(&thread_data.directory_mutex);
      free(threads);
      return;
    }
  }

  if (pthread_mutex_destroy(&thread_data.directory_mutex) != 0) {
    fprintf(stderr, "Failed to destroy directory_mutex\n");
  }

  //  if (sem_destroy(&SEM_BUFFER_SPACE) != 0) {
  //       perror("Failed to destroy SEM_BUFFER_SPACE");
  //   }

  //   if (sem_destroy(&SEM_BUFFER_CLIENTS) != 0) {
  //       perror("Failed to destroy SEM_BUFFER_CLIENTS");
  //   }


  free(threads);
}

int main(int argc, char **argv) {
  if (argc < 5) {
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

  if (*endptr != '\0') {
    fprintf(stderr, "Invalid max_proc value\n");
    return 1;
  }

  max_threads = strtoul(argv[2], &endptr, 10);
 

  if (*endptr != '\0') {
    fprintf(stderr, "Invalid max_threads value\n");
    return 1;
  }

  if (max_backups <= 0) {
    write_str(STDERR_FILENO, "Invalid number of backups\n");
    return 0;
  }

  if (max_threads <= 0) {
    write_str(STDERR_FILENO, "Invalid number of threads\n");
    return 0;
  }
  
  register_FIFO_name = argv[4];
  fprintf(stderr, "FIFO name: %s\n", register_FIFO_name);

  if (register_FIFO_name == NULL || strlen(register_FIFO_name) == 0) {
    write_str(STDERR_FILENO, "Invalid or empty FIFO name\n");
    return 1;
}

  if (kvs_init()) {
    write_str(STDERR_FILENO, "Failed to initialize KVS\n");
    return 1;
  }

  DIR *dir = opendir(argv[1]);
  if (dir == NULL) {
    fprintf(stderr, "Failed to open directory: %s\n", argv[1]);
    return 0;
  }

  dispatch_threads(dir);

  if (closedir(dir) == -1) {
    fprintf(stderr, "Failed to close directory\n");
    return 0;
  }

  while (active_backups > 0) {
    wait(NULL);
    active_backups--;
  }

  kvs_terminate();

  return 0;
}