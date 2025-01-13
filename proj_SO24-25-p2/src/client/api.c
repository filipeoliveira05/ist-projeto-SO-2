#include "api.h"
#include "src/common/constants.h"
#include "src/common/protocol.h"
#include "src/server/constants.h"

#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>

void cleanup_fifos(const char *req_pipe_path, const char *resp_pipe_path, const char *notif_pipe_path)
{
  if (unlink(req_pipe_path) != 0 && errno != ENOENT)
  {
    perror("Failed to unlink request FIFO");
    return;
  }
  if (unlink(resp_pipe_path) != 0 && errno != ENOENT)
  {
    perror("Failed to unlink response FIFO");
    return;
  }
  if (unlink(notif_pipe_path) != 0 && errno != ENOENT)
  {
    perror("Failed to unlink notification FIFO");
    return;
  }
  // fprintf(stderr, "Cleaned up FIFOs.\n");
}

int kvs_connect(char const *req_pipe_path, char const *resp_pipe_path, char const *server_pipe_path,
                char const *notif_pipe_path, Client *client)
{

  char buf[MAX_PIPE_BUFFER_SIZE];
  //fprintf(stderr, "Started cleanup of FIFOs\n");

  cleanup_fifos(req_pipe_path, resp_pipe_path, notif_pipe_path);
  // fprintf(stderr, "Started making FIFOs\n");

  if (mkfifo(req_pipe_path, 0640) != 0)
  {
    perror("Failed to create request FIFO");
    return -1;
  }

  if (mkfifo(resp_pipe_path, 0640) != 0)
  {
    perror("Failed to create response FIFO");
    return -1;
  }

  if (mkfifo(notif_pipe_path, 0640) != 0)
  {
    perror("Failed to create notification FIFO");
    return -1;
  }

  // Debug: Confirm FIFOs are created
  // fprintf(stderr, "\n\nCreated FIFOs: \nRequest: %s\nResponse: %s\nNotification: %s\n\n",req_pipe_path, resp_pipe_path, notif_pipe_path);

  // Prepare message to send to the server
  sprintf(buf, "1");
  memcpy(buf + 1, req_pipe_path, MAX_PIPE_PATH_LENGTH);
  memcpy(buf + 1 + MAX_PIPE_PATH_LENGTH, resp_pipe_path, MAX_PIPE_PATH_LENGTH);
  memcpy(buf + 1 + 2 * MAX_PIPE_PATH_LENGTH, notif_pipe_path, MAX_PIPE_PATH_LENGTH);

  // Debug: Attempting to open server pipe
  // fprintf(stderr, "Opening server pipe: %s\n", server_pipe_path);

  client->server_pipe = open(server_pipe_path, O_WRONLY);
  if (client->server_pipe == -1)
  {
    perror("Failed to open server pipe");
    return -1;
  }

  // Write the message to the server pipe
  // fprintf(stderr, "Writing to server pipe\n");
  if (write(client->server_pipe, buf, 1 + MAX_PIPE_PATH_LENGTH * 3) == -1)
  {
    perror("Failed to write to server pipe");
    return -1;
  }

  // Open the response pipe
  // fprintf(stderr, "Opening response pipe: %s\n", resp_pipe_path);
  client->resp_pipe = open(resp_pipe_path, O_RDONLY);
  if (client->resp_pipe == -1)
  {
    perror("Failed to open response pipe");
    return -1;
  }

  // Read response from the server
  // fprintf(stderr, "Reading from response pipe\n");
  if (read(client->resp_pipe, buf, 1) <= 0)
  {
    perror("Failed to read from response pipe");
    return -1;
  }
  // Open the request pipe
  // fprintf(stderr, "Opening request pipe: %s\n", req_pipe_path);
  client->req_pipe = open(req_pipe_path, O_RDWR); // Open for both read and write
  if (client->req_pipe == -1)
  {
    perror("Failed to open request pipe for reading and writing");
    return -1;
  }
  // fprintf(stderr, "Request pipe opened successfully.\n");
  // Parse the response
  int valor = buf[0] - '0';

  // Debug: Received response
  // fprintf(stderr, "Received response from server: %d\n", valor);

  return valor;
}

int kvs_disconnect(Client *client)
{

  char buf[MAX_PIPE_BUFFER_SIZE];

  // fprintf(stderr, "Sending disconnect request...\n");
  if (write(client->req_pipe, "2", 1) == -1)
  {
    perror("Failed to write disconnect request to request pipe");
    return 1;
  }
  // fprintf(stderr, "Disconnect request sent.\n");

  ssize_t bytes_read = 0;
  do
  {
    bytes_read = read(client->resp_pipe, buf, 2); // Read the response (2 bytes expected)
    // fprintf(stderr, "bytes read: %zd Waiting for response...\n", bytes_read);

    if (bytes_read == -1)
    {
      perror("Failed to read response for disconect from response pipe");
      return 1; // Error
    }
  } while (bytes_read == 0); // Keep waiting if no data is read

  // Null-terminate the response to safely handle it as a string
  buf[2] = '\0';

  // fprintf(stderr, "Received response for disconect: %s\n", buf);

  if (buf[0] == '1')
  {
    fprintf(stderr, "Error response received during disconnect.\n");
    return 1; // Handle error
  }

  // fprintf(stderr, "Closing pipes...\n");

  close(client->req_pipe);
  fprintf(stderr, "REQ PATH: %s\n", client->req_pipe_path);
  close(client->resp_pipe);
  close(client->notif_pipe);
  cleanup_fifos(client->req_pipe_path, client->req_pipe_path, client->notif_pipe_path);
  // fprintf(stderr, "Pipes closed and unlinked successfully.\n");

  return 0;
}

int kvs_subscribe(const char *key, Client *client)
{
  char buf[MAX_PIPE_BUFFER_SIZE];

  // Prepare subscribe message
  snprintf(buf, sizeof(buf), "3%s", key);
  // fprintf(stderr, "Attempting to subscribe with key: %s\n", key);

  // Send subscribe message to request pipe
  if (write(client->req_pipe, buf, strlen(buf)) == -1)
  {
    perror("Failed to write subscribe message to request pipe");
    return 1; // Error
  }
  // fprintf(stderr, "Subscribe message sent to request pipe: %s\n", buf);
  // Wait for response from response pipe
  ssize_t bytes_read = 0;
  do
  {
    bytes_read = read(client->resp_pipe, buf, 2); // Read the response (2 bytes expected)
    // fprintf(stderr, "bytes read: %zd Waiting for response...\n", bytes_read);

    if (bytes_read == -1)
    {
      perror("Failed to read response for subscribe from response pipe");
      return 1; // Error
    }
  } while (bytes_read == 0); // Keep waiting if no data is read

  // Null-terminate the response to safely handle it as a string
  buf[2] = '\0';

  // fprintf(stderr, "Received response for subscribe: %s\n", buf);
  // Check response code
  if (buf[1] == '1')
  {
    // fprintf(stderr, "Subscribe failed for key: %s\n", key);
    return 0; // Error
  }

  // fprintf(stderr, "Successfully subscribed to key: %s\n", key);
  return 1; // Success
}

int kvs_unsubscribe(const char *key, Client *client)
{
  char buf[MAX_PIPE_BUFFER_SIZE];

  // Prepare unsubscribe message
  snprintf(buf, sizeof(buf), "4%s", key);
  // fprintf(stderr, "Attempting to unsubscribe with key: %s\n", key);

  // Send unsubscribe message to request pipe
  if (write(client->req_pipe, buf, strlen(buf)) == -1)
  {
    perror("Failed to write unsubscribe message to request pipe");
    return 1; // Error
  }
  // fprintf(stderr, "Unsubscribe message sent to request pipe: %s\n", buf);

  // Wait for response from response pipe
  ssize_t bytes_read = 0;
  do
  {
    bytes_read = read(client->resp_pipe, buf, 2); // Read the response (2 bytes expected)
    // fprintf(stderr, "bytes read: %zd Waiting for response...\n", bytes_read);

    if (bytes_read == -1)
    {
      perror("Failed to read response for unsubscribe from response pipe");
      return 1; // Error
    }
  } while (bytes_read == 0); // Keep waiting if no data is read

  // Null-terminate the response to safely handle it as a string
  buf[2] = '\0';

  // fprintf(stderr, "Received response for unsubscribe: %s\n", buf);
  // Check response code
  if (buf[1] == '1')
  {
    // fprintf(stderr, "Unsubscribe failed for key: %s\n", key);
    return 1; // Error
  }

  // fprintf(stderr, "Successfully unsubscribed from key: %s\n", key);
  return 0; // Success
}
