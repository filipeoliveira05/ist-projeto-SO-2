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

// Disconnect a client and remove all its subscriptions
int kvs_disconnect_client(const char *notif_pipe_path)
{
  if (kvs_table == NULL)
  {
    fprintf(stderr, "KVS state must be initialized\n");
    return 0;
  }
  pthread_rwlock_wrlock(&kvs_table->tablelock); // Lock for write access
  printf("Disconnecting client with notification pipe path: %s\n", notif_pipe_path);

  for (int i = 0; i < TABLE_SIZE; i++)
  {
    KeyNode *currentKeyNode = kvs_table->table[i];

    while (currentKeyNode != NULL)
    {
      ClientSubscribed *prev = NULL;
      ClientSubscribed *currentClient = currentKeyNode->Head;

      while (currentClient != NULL)
      {
        // Check if this client matches the one to disconnect
        if (strcmp(currentClient->notif_pipe_path, notif_pipe_path) == 0)
        {
          printf("Removing client from key: %s\n", currentKeyNode->key);

          // Remove the client from the list
          if (prev == NULL)
          {
            // The client is at the head of the list
            currentKeyNode->Head = currentClient->next;
          }
          else
          {
            // The client is in the middle or end of the list
            prev->next = currentClient->next;
          }

          // Free the memory for this client
          free(currentClient);

          // Update the count of subscribed clients
          currentKeyNode->n_clients--;

          printf("Client removed. Key: %s, Remaining Clients: %d\n", currentKeyNode->key, currentKeyNode->n_clients);
          break; // Stop searching this list
        }

        // Move to the next client
        prev = currentClient;
        currentClient = currentClient->next;
      }

      currentKeyNode = currentKeyNode->next; // Move to the next key node
    }
  }

  pthread_rwlock_unlock(&kvs_table->tablelock); // Release the lock
  printf("Client successfully disconnected and all subscriptions removed.\n");
  return 1;
}

int kvs_subscribe(char notif_path[MAX_PIPE_PATH_LENGTH], char key[MAX_STRING_SIZE])
{
  pthread_rwlock_wrlock(&kvs_table->tablelock);

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
    pthread_rwlock_unlock(&kvs_table->tablelock);

    return 0;
  }
  pthread_rwlock_unlock(&kvs_table->tablelock);

  return 1;
}
int kvs_unsubscribe(char notif_path[MAX_PIPE_PATH_LENGTH], char key[MAX_STRING_SIZE])
{
  pthread_rwlock_wrlock(&kvs_table->tablelock);

  KeyNode *key_node = find_key(kvs_table, key);

  // If the key does not exist in the key-value store
  if (key_node == NULL)
  {
    pthread_rwlock_unlock(&kvs_table->tablelock); // Unlock before returning failure
    return 0;                                     // Failure: Key not found
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
      pthread_rwlock_unlock(&kvs_table->tablelock); // Unlock before returning success
      return 1;                                     // Success: Unsubscription successful
    }

    client_pre = client;
    client = client->next;
  }
  pthread_rwlock_unlock(&kvs_table->tablelock);
  return 0; // Failure: Client not found
}

// Function to add a client to the session
Client *add_client_to_session(SessionData *session, const char *req_pipe_path, const char *resp_pipe_path, const char *notif_pipe_path, int indexClient)
{
  printf("SESSION ACTIVE CLIENT BEFORE: %d", session->activeClients);
  // Check if the session has room for another client (max of 10 clients)
  if (session->activeClients >= MAX_SESSION_COUNT)
  {
    printf("Error: Session is full. Cannot add more clients.\n");
    return NULL; // Return an error code if no space is available
  }

  // Allocate memory for the new client and the linked list node
  ClientsInSession *new_client_node = (ClientsInSession *)malloc(sizeof(ClientsInSession));
  if (new_client_node == NULL)
  {
    printf("Error: Failed to allocate memory for new client node.\n");
    return NULL; // Memory allocation failure
  }

  new_client_node->Client = (Client *)malloc(sizeof(Client));
  if (new_client_node->Client == NULL)
  {
    printf("Error: Failed to allocate memory for new client structure.\n");
    free(new_client_node); // Clean up partially allocated node
    return NULL;           // Memory allocation failure
  }

  // Assign pipe paths to the new client
  new_client_node->Client->req_pipe_path = strdup(req_pipe_path);     // Copy the string
  new_client_node->Client->resp_pipe_path = strdup(resp_pipe_path);   // Copy the string
  new_client_node->Client->notif_pipe_path = strdup(notif_pipe_path); // Copy the string

  // Check for memory allocation failures for strdup
  if (new_client_node->Client->req_pipe_path == NULL ||
      new_client_node->Client->resp_pipe_path == NULL ||
      new_client_node->Client->notif_pipe_path == NULL)
  {
    printf("Error: Failed to allocate memory for client pipe paths.\n");
    free(new_client_node->Client->req_pipe_path);
    free(new_client_node->Client->resp_pipe_path);
    free(new_client_node->Client->notif_pipe_path);
    free(new_client_node->Client);
    free(new_client_node);
    return NULL;
  }

  // Assign placeholder values for the pipe file descriptors
  new_client_node->Client->client_Index = indexClient + 1;
  new_client_node->Client->req_pipe = -1;
  new_client_node->Client->resp_pipe = -1;
  new_client_node->Client->notif_pipe = -1;
  new_client_node->next = NULL;

  // Add the new client to the linked list
  if (session->head == NULL)
  {
    session->head = new_client_node; // If the list is empty, make this the head
  }
  else
  {
    // Otherwise, traverse the list and add it to the end
    ClientsInSession *current = session->head;
    while (current->next != NULL)
    {
      current = current->next;
    }
    current->next = new_client_node;
  }

  // Increment the activeClients count
  session->activeClients++;

  printf("Client added successfully. Active clients: %d\n", session->activeClients);
  return new_client_node->Client; // Success
}
// Function to remove a client from the session by req_pipe_path
int remove_client_from_session(SessionData *session, const char *req_pipe_path)
{
  // Check if the session is empty
  if (session->head == NULL)
  {
    printf("Error: No clients in the session.\n");
    return -1; // No clients to remove
  }

  ClientsInSession *current = session->head;
  ClientsInSession *prev = NULL;

  // Traverse the linked list to find the client to remove
  while (current != NULL)
  {
    // If the current client's request pipe path matches
    if (strcmp(current->Client->req_pipe_path, req_pipe_path) == 0)
    {
      // Update the linked list
      if (prev == NULL)
      {
        session->head = current->next; // Update head if it's the first client
      }
      else
      {
        prev->next = current->next; // Skip the current client
      }

      // Free the dynamically allocated memory for the client
      free(current->Client->req_pipe_path);
      free(current->Client->resp_pipe_path);
      free(current->Client->notif_pipe_path);
      free(current->Client); // Free the client structure
      free(current);         // Free the linked list node

      // Decrement the activeClients count
      session->activeClients--;

      printf("Client with req_pipe_path '%s' removed successfully. Active clients: %d\n", req_pipe_path, session->activeClients);
      return 0; // Success
    }

    // Move to the next client
    prev = current;
    current = current->next;
  }

  // If no client with the given req_pipe_path was found
  printf("Error: Client with req_pipe_path '%s' not found.\n", req_pipe_path);
  return -1; // Client not found
}

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
