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

int kvs_disconnect_client(const char *notif_pipe_path);

int kvs_subscribe(char notif_path[MAX_PIPE_PATH_LENGTH], char key[MAX_STRING_SIZE]);
int kvs_unsubscribe(char notif_path[MAX_PIPE_PATH_LENGTH], char key[MAX_STRING_SIZE]);

Client *add_client_to_session(SessionData *session, const char *req_pipe_path, const char *resp_pipe_path, const char *notif_pipe_path, int indexClient);
int remove_client_from_session(SessionData *session, const char *req_pipe_path);
#endif // KVS_OPERATIONS_H
