#ifndef KVS_OPERATIONS_H
#define KVS_OPERATIONS_H

#include <stddef.h>
#include <dirent.h>
#include "pthread.h"

#include "constants.h"
#include "../common/constants.h"

typedef struct SharedData
{
  DIR *dir;
  char *dir_name;
  pthread_mutex_t directory_mutex;
} SharedData;

typedef struct Client
{
  int client_Index;
  int thread_slot;
  char *req_pipe_path;
  char *resp_pipe_path;
  char *notif_pipe_path;
  int req_pipe;
  int resp_pipe;
  int notif_pipe;
} Client;

typedef struct ClientsInSession
{
  Client *Client;
  struct ClientsInSession *next;

} ClientsInSession;

typedef struct SessionData
{
  int activeClients;
  char *server_pipe_path;
  ClientsInSession *head;
} SessionData;

/// Initializes the KVS state.
/// @return 0 if the KVS state was initialized successfully, 1 otherwise.
int kvs_init();

/// Destroys the KVS state.
/// @return 0 if the KVS state was terminated successfully, 1 otherwise.
int kvs_terminate();

/// Writes a key value pair to the KVS. If key already exists it is updated.
/// @param num_pairs Number of pairs being written.
/// @param keys Array of keys' strings.
/// @param values Array of values' strings.
/// @return 0 if the pairs were written successfully, 1 otherwise.
int kvs_write(size_t num_pairs, char keys[][MAX_STRING_SIZE],
              char values[][MAX_STRING_SIZE]);

/// Reads values from the KVS.
/// @param num_pairs Number of pairs to read.
/// @param keys Array of keys' strings.
/// @param fd File descriptor to write the (successful) output.
/// @return 0 if the key reading, 1 otherwise.
int kvs_read(size_t num_pairs, char keys[][MAX_STRING_SIZE], int fd);

/// Deletes key value pairs from the KVS.
/// @param num_pairs Number of pairs to read.
/// @param keys Array of keys' strings.
/// @return 0 if the pairs were deleted successfully, 1 otherwise.
int kvs_delete(size_t num_pairs, char keys[][MAX_STRING_SIZE], int fd);

/// Writes the state of the KVS.
/// @param fd File descriptor to write the output.
void kvs_show(int fd);

/// Creates a backup of the KVS state and stores it in the correspondent
/// backup file
/// @return 0 if the backup was successful, 1 otherwise.
int kvs_backup(size_t num_backup, char *job_filename, char *directory);

/// Waits for the last backup to be called.
void kvs_wait_backup();

/// Waits for a given amount of time.
/// @param delay_us Delay in milliseconds.
void kvs_wait(unsigned int delay_ms);

// Setter for max_backups
// @param _max_backups
void set_max_backups(int _max_backups);

// Setter for n_current_backups
// @param _n_current_backups
void set_n_current_backups(int _n_current_backups);

// Getter for n_current_backups
// @return n_current_backups
int get_n_current_backups();

/// Disconnects a client by its notification pipe path.
/// @param notif_pipe_path The path to the notification pipe associated with the client.
/// @return 1 if the client was successfully disconnected, 0 otherwise.
int kvs_disconnect_client(const char *notif_pipe_path);

/// Subscribes a client to notifications for a specific key.
/// @param notif_path The path to the notification pipe for the client.
/// @param key The key to subscribe to.
/// @return 1 if the subscription was successful, 0 otherwise.
int kvs_subscribe(char notif_path[MAX_PIPE_PATH_LENGTH], char key[MAX_STRING_SIZE]);

/// Unsubscribes a client from notifications for a specific key.
/// @param notif_path The path to the notification pipe for the client.
/// @param key The key to unsubscribe from.
/// @return 1 if the unsubscription was successful, 0 otherwise.
int kvs_unsubscribe(char notif_path[MAX_PIPE_PATH_LENGTH], char key[MAX_STRING_SIZE]);

/// Adds a client to the current session.
/// @param session Pointer to the session data.
/// @param req_pipe_path Path to the request pipe of the client.
/// @param resp_pipe_path Path to the response pipe of the client.
/// @param notif_pipe_path Path to the notification pipe of the client.
/// @param indexClient Index of the client in the session.
/// @param thread_slot The thread slot assigned to the client.
/// @return A pointer to the newly added client, or NULL if the client could not be added.
Client *add_client_to_session(SessionData *session, const char *req_pipe_path, const char *resp_pipe_path, const char *notif_pipe_path, int indexClient, int thread_slot);

/// Removes a client from the current session using the request pipe path.
/// @param session Pointer to the session data.
/// @param req_pipe_path The request pipe path of the client to remove.
/// @return 0 if the client was successfully removed, -1 otherwise.
int remove_client_from_session(SessionData *session, const char *req_pipe_path);

/// Removes all clients from the current session.
/// @param session Pointer to the session data.
/// @return 0 if the remove was succesfull, -1 otherwise
int remove_all_clients(SessionData *session);

/// Deletes all active subscriptions for keys in the KVS.
/// This function is used to clean up subscriptions when necessary.
void delete_all_subscriptions();

#endif // KVS_OPERATIONS_H
