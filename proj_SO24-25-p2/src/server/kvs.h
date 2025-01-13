#ifndef KEY_VALUE_STORE_H
#define KEY_VALUE_STORE_H
#define TABLE_SIZE 26

#include "src/common/constants.h"
#include <pthread.h>
#include <stddef.h>

typedef struct ClientSubscribed {
  struct ClientSubscribed *next;
  char notif_pipe_path[MAX_PIPE_PATH_LENGTH + 1];
} ClientSubscribed;

typedef struct KeyNode {
  char *key;
  char *value;
  struct KeyNode *next;
  struct ClientSubscribed *Head;
  int n_clients;
} KeyNode;

typedef struct HashTable {
  KeyNode *table[TABLE_SIZE];
  pthread_rwlock_t tablelock;
} HashTable;

/// Creates a new KVS hash table.
/// @return Newly created hash table, NULL on failure
struct HashTable *create_hash_table();

int hash(const char *key);

// Writes a key value pair in the hash table.
// @param ht The hash table.
// @param key The key.
// @param value The value.
// @return 0 if successful.
int write_pair(HashTable *ht, const char *key, const char *value);

// Reads the value of a given key.
// @param ht The hash table.
// @param key The key.
// return the value if found, NULL otherwise.
char *read_pair(HashTable *ht, const char *key);

/// Finds a key in the hash table.
/// @param ht Pointer to the hash table where the key should be searched.
/// @param key The key to search for.
/// @return A pointer to the KeyNode containing the key, or NULL if the key is
/// not found.
KeyNode *find_key(HashTable *ht, const char *key);

/// Deletes a pair from the table.
/// @param ht Hash table to read from.
/// @param key Key of the pair to be deleted.
/// @return 0 if the node was deleted successfully, 1 otherwise.
int delete_pair(HashTable *ht, const char *key);

/// Deletes all subscriptions associated with a specific key.
/// @param key_node Pointer to the KeyNode for which all subscriptions should be
/// deleted.
void delete_all_subscriptions_of_key(KeyNode *key_node);

/// Frees the hashtable.
/// @param ht Hash table to be deleted.
void free_table(HashTable *ht);

#endif // KVS_H
