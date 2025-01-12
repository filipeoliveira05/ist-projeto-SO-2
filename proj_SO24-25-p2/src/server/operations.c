#include "operations.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "constants.h"
#include "io.h"
#include "kvs.h"

static struct HashTable *kvs_table = NULL;

/// Calculates a timespec from a delay in milliseconds.
/// @param delay_ms Delay in milliseconds.
/// @return Timespec with the given delay.
static struct timespec delay_to_timespec(unsigned int delay_ms)
{
  return (struct timespec){delay_ms / 1000, (delay_ms % 1000) * 1000000};
}

int kvs_init()
{
  if (kvs_table != NULL)
  {
    fprintf(stderr, "KVS state has already been initialized\n");
    return 1;
  }

  kvs_table = create_hash_table();
  return kvs_table == NULL;
}

int kvs_terminate()
{
  if (kvs_table == NULL)
  {
    fprintf(stderr, "KVS state must be initialized\n");
    return 1;
  }

  free_table(kvs_table);
  kvs_table = NULL;
  return 0;
}

int kvs_write(size_t num_pairs, char keys[][MAX_STRING_SIZE],
              char values[][MAX_STRING_SIZE])
{
  if (kvs_table == NULL)
  {
    fprintf(stderr, "KVS state must be initialized\n");
    return 1;
  }

  pthread_rwlock_wrlock(&kvs_table->tablelock);

  for (size_t i = 0; i < num_pairs; i++)
  {
    KeyNode *key_node = find_key(kvs_table, keys[i]);
    int is_new_key = 0;

    if (key_node == NULL)
    {
      // doesn't notify
      is_new_key = 1;
    }

    if (write_pair(kvs_table, keys[i], values[i]) != 0)
    {
      fprintf(stderr, "Failed to write key pair (%s,%s)\n", keys[i], values[i]);
    }

    if (!is_new_key)
    {
      // Notify all subscribed clients
      ClientSubscribed *client = key_node->Head;
      while (client != NULL)
      {

        int notif_fd = open(client->notif_pipe_path, O_WRONLY | O_NONBLOCK);
        if (notif_fd == -1)
        {
          perror("Failed to open notification pipe");
        }
        else
        {
          // Cria a mensagem de notificação no formato esperado
          char notification[82] = {0}; // Maximum length if both key and value use up to 40 chars each
          snprintf(notification, sizeof(notification), "(%s,%s)", keys[i], values[i]);
          // TODO padding
          if (write(notif_fd, notification, strlen(notification)) == -1)
          {
            perror("Failed to write notification");
          }
          else
          {
            fprintf(stderr, "Notification sent to client: %s\n", client->notif_pipe_path);
          }
          close(notif_fd);
        }
        client = client->next; // Move to the next client
      }
    }
  }

  pthread_rwlock_unlock(&kvs_table->tablelock);
  return 0;
}

int kvs_read(size_t num_pairs, char keys[][MAX_STRING_SIZE], int fd)
{
  if (kvs_table == NULL)
  {
    fprintf(stderr, "KVS state must be initialized\n");
    return 1;
  }

  pthread_rwlock_rdlock(&kvs_table->tablelock);

  write_str(fd, "[");
  for (size_t i = 0; i < num_pairs; i++)
  {
    char *result = read_pair(kvs_table, keys[i]);
    char aux[MAX_STRING_SIZE];
    if (result == NULL)
    {
      snprintf(aux, MAX_STRING_SIZE, "(%s,KVSERROR)", keys[i]);
    }
    else
    {
      snprintf(aux, MAX_STRING_SIZE, "(%s,%s)", keys[i], result);
    }
    write_str(fd, aux);
    free(result);
  }
  write_str(fd, "]\n");

  pthread_rwlock_unlock(&kvs_table->tablelock);
  return 0;
}

int kvs_delete(size_t num_pairs, char keys[][MAX_STRING_SIZE], int fd)
{
  if (kvs_table == NULL)
  {
    fprintf(stderr, "KVS state must be initialized\n");
    return 1;
  }

  pthread_rwlock_wrlock(&kvs_table->tablelock);

  int aux = 0;
  for (size_t i = 0; i < num_pairs; i++)
  {
    KeyNode *key_node = find_key(kvs_table, keys[i]);

    if (key_node != NULL)
    {
      ClientSubscribed *client = key_node->Head;

      while (client != NULL)
      {

        int notif_fd = open(client->notif_pipe_path, O_WRONLY | O_NONBLOCK);
        if (notif_fd == -1)
        {
          perror("Failed to open notification pipe");
        }
        else
        {
          // Cria a mensagem de notificação no formato esperado
          char notification[82] = {0}; // Maximum length if both key and value use up to 40 chars each
          snprintf(notification, sizeof(notification), "(%.40s,DELETED)", keys[i]);
          // TODO padding
          if (write(notif_fd, notification, strlen(notification)) == -1)
          {
            perror("Failed to write notification");
          }
          else
          {
            fprintf(stderr, "Notification sent to client: %s\n", client->notif_pipe_path);
          }
          close(notif_fd);
        }
        client = client->next;
      }
    }

    if (delete_pair(kvs_table, keys[i]) != 0)
    {
      if (!aux)
      {
        write_str(fd, "[");
        aux = 1;
      }
      char str[MAX_STRING_SIZE];
      snprintf(str, MAX_STRING_SIZE, "(%s,KVSMISSING)", keys[i]);
      write_str(fd, str);
    }
  }
  if (aux)
  {
    write_str(fd, "]\n");
  }

  pthread_rwlock_unlock(&kvs_table->tablelock);
  return 0;
}

void kvs_show(int fd)
{
  if (kvs_table == NULL)
  {
    fprintf(stderr, "KVS state must be initialized\n");
    return;
  }

  pthread_rwlock_rdlock(&kvs_table->tablelock);
  char aux[MAX_STRING_SIZE];

  for (int i = 0; i < TABLE_SIZE; i++)
  {
    KeyNode *keyNode = kvs_table->table[i]; // Get the next list head
    while (keyNode != NULL)
    {
      snprintf(aux, MAX_STRING_SIZE, "(%s, %s)\n", keyNode->key,
               keyNode->value);
      write_str(fd, aux);
      keyNode = keyNode->next; // Move to the next node of the list
    }
  }

  pthread_rwlock_unlock(&kvs_table->tablelock);
}

int kvs_backup(size_t num_backup, char *job_filename, char *directory)
{
  pid_t pid;
  char bck_name[50];
  snprintf(bck_name, sizeof(bck_name), "%s/%s-%ld.bck", directory,
           strtok(job_filename, "."), num_backup);

  pthread_rwlock_rdlock(&kvs_table->tablelock);
  pid = fork();
  pthread_rwlock_unlock(&kvs_table->tablelock);
  if (pid == 0)
  {
    // functions used here have to be async signal safe, since this
    // fork happens in a multi thread context (see man fork)
    int fd = open(bck_name, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    for (int i = 0; i < TABLE_SIZE; i++)
    {
      KeyNode *keyNode = kvs_table->table[i]; // Get the next list head
      while (keyNode != NULL)
      {
        char aux[MAX_STRING_SIZE];
        aux[0] = '(';
        size_t num_bytes_copied = 1; // the "("
        // the - 1 are all to leave space for the '/0'
        num_bytes_copied += strn_memcpy(aux + num_bytes_copied, keyNode->key,
                                        MAX_STRING_SIZE - num_bytes_copied - 1);
        num_bytes_copied += strn_memcpy(aux + num_bytes_copied, ", ",
                                        MAX_STRING_SIZE - num_bytes_copied - 1);
        num_bytes_copied += strn_memcpy(aux + num_bytes_copied, keyNode->value,
                                        MAX_STRING_SIZE - num_bytes_copied - 1);
        num_bytes_copied += strn_memcpy(aux + num_bytes_copied, ")\n",
                                        MAX_STRING_SIZE - num_bytes_copied - 1);
        aux[num_bytes_copied] = '\0';
        write_str(fd, aux);
        keyNode = keyNode->next; // Move to the next node of the list
      }
    }
    exit(1);
  }
  else if (pid < 0)
  {
    return -1;
  }
  return 0;
}

void kvs_wait(unsigned int delay_ms)
{
  struct timespec delay = delay_to_timespec(delay_ms);
  nanosleep(&delay, NULL);
}

int kvs_subscribe(char notif_path[MAX_PIPE_PATH_LENGTH], char key[MAX_STRING_SIZE])
{
  KeyNode *key_node = find_key(kvs_table, key);
  if (key_node != NULL)
  {
    ClientSubscribed *client = key_node->Head;
    ClientSubscribed *client_new = malloc(sizeof(ClientSubscribed));

    client_new->next = NULL;
    strcpy(client_new->notif_pipe_path, notif_path);

    if (client != NULL)
    {
      while (client->next != NULL)
      {
        client = client->next;
      }
      client->next = client_new;
    }
    else
      key_node->Head = client_new;

    key_node->n_clients++;
  }
  else
  {
    return 0;
  }
  return 1;
}
int kvs_unsubscribe(char notif_path[MAX_PIPE_PATH_LENGTH], char key[MAX_STRING_SIZE])
{
  KeyNode *key_node = find_key(kvs_table, key);

  // If the key does not exist in the key-value store
  if (key_node == NULL)
  {
    return 0; // Failure: Key not found
  }

  ClientSubscribed *client = key_node->Head;
  ClientSubscribed *client_pre = NULL;

  // Traverse the linked list of clients for the given key
  while (client != NULL)
  {
    if (strcmp(client->notif_pipe_path, notif_path) == 0)
    {
      // Remove the client from the list
      if (client_pre == NULL)
      {
        key_node->Head = client->next;
      }
      else
      {
        client_pre->next = client->next;
      }

      free(client);
      key_node->n_clients--;
      return 1; // Success: Unsubscription successful
    }

    client_pre = client;
    client = client->next;
  }
  return 0; // Failure: Client not found
}

// void add_client(ClientsInSession **head, const char *req_pipe_path, const char *resp_pipe_path, const char *notif_pipe_path)
// {
//   ClientsInSession *new_client = (ClientsInSession *)malloc(sizeof(ClientsInSession));
//   if (!new_client)
//   {
//     perror("Failed to allocate memory for new client");
//     return;
//   }
//   // Initialize the new client
//   strncpy(new_client->req_pipe_path, req_pipe_path, MAX_PIPE_PATH_LENGTH);
//   strncpy(new_client->resp_pipe_path, resp_pipe_path, MAX_PIPE_PATH_LENGTH);
//   strncpy(new_client->notif_pipe_path, notif_pipe_path, MAX_PIPE_PATH_LENGTH);
//   new_client->req_pipe = open(req_pipe_path, O_RDWR);
//   new_client->resp_pipe = open(resp_pipe_path, O_RDONLY);
//   new_client->notif_pipe = open(notif_pipe_path, O_RDONLY);
//   new_client->next = *head; // Add to the front of the list

//   *head = new_client;
// }

// void remove_client(ClientsInSession **head, const char *notif_pipe_path)
// {
//   ClientsInSession *temp = *head;
//   ClientsInSession *prev = NULL;

//   // Search for the client with the given notif_pipe_path
//   while (temp != NULL && strcmp(temp->notif_pipe_path, notif_pipe_path) != 0)
//   {
//     prev = temp;
//     temp = temp->next;
//   }

//   // Client not found
//   if (temp == NULL)
//   {
//     return;
//   }

//   // Remove the client from the list
//   if (prev == NULL)
//   {
//     *head = temp->next;
//   }
//   else
//   {
//     prev->next = temp->next;
//   }
//   // Close the pipes and free the memory
//   close(temp->req_pipe);
//   close(temp->resp_pipe);
//   close(temp->notif_pipe);
//   cleanup_fifos(temp->req_pipe_path, temp->req_pipe_path, temp->notif_pipe_path);
//   free(temp);
// }

// ClientsInSession *find_client(ClientsInSession *head, const char *resp_pipe_path)
// {
//   ClientsInSession *temp = head;
//   while (temp != NULL)
//   {
//     if (strcmp(temp->resp_pipe_path, resp_pipe_path) == 0)
//     {
//       return temp;
//     }
//     temp = temp->next;
//   }
//   return NULL; // Client not found
// }
