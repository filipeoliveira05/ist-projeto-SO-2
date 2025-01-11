#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <fcntl.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>

#include "api.h"

#include "src/common/constants.h"
#include "src/common/protocol.h"


int REQ_FD_WR;
int RESP_FD_RD;
int NOTIFICATIONS_FD_RD;

char REQ_PIPE_PATH[MAX_PIPE_PATH_LENGTH];
char RESP_PIPE_PATH[MAX_PIPE_PATH_LENGTH];
char NOTIFICATIONS_PIPE_PATH[MAX_PIPE_PATH_LENGTH];


/**
 * função auxiliar para enviar mensagens
 * volta a tentar enviar o que não foi enviado no início
 */
void send_message(int fd, char const *str) {
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


void fill_buffer_with_padding(const char *src, char *dest) {
  size_t len = strlen(src);
  if (len > MAX_PIPE_PATH_LENGTH) len = MAX_PIPE_PATH_LENGTH;
  memcpy(dest, src, len);
  memset(dest + len, '\0', MAX_PIPE_PATH_LENGTH - len);  // Fill the rest with '\0'
}

void build_message(const char *req_pipe_path, const char *resp_pipe_path, const char *notifications_pipe_path, char *message) {
  // Começa com OP_CODE=1
  message[0] = (char) 1;

  // Copia e cola os nomes dos pipes na message
  fill_buffer_with_padding(req_pipe_path, message + 1);
  fill_buffer_with_padding(resp_pipe_path, message + 1 + MAX_PIPE_PATH_LENGTH);
  fill_buffer_with_padding(notifications_pipe_path, message + 1 + 2 * MAX_PIPE_PATH_LENGTH);
}

char read_message(int fd) {
  char buffer[2];
  ssize_t ret = read(fd, buffer, 2);
  if (ret == 0) {
      // ret == 0 indica EOF
      fprintf(stderr, "pipe closed\n");
      return '\0';
  } else if (ret == -1) {
      // ret == -1 indica erro
      fprintf(stderr, "read failed: %s\n", strerror(errno));
      exit(EXIT_FAILURE);
  }

  return buffer[1];
}



int kvs_connect(char const *req_pipe_path, char const *resp_pipe_path,
                char const *server_pipe_path, char const *notif_pipe_path,
                int *notif_pipe) {
  
  int fifo_fd_wr = open(server_pipe_path, O_WRONLY);
  if (fifo_fd_wr == -1) {
    perror("Failed to open FIFO");
    exit(EXIT_FAILURE);
  }

  /* remove os pipes se já existiam */
  if (unlink(req_pipe_path) != 0 && errno != ENOENT) {
    perror("unlink(%s) failed");
    exit(EXIT_FAILURE);
  }
  if (unlink(resp_pipe_path) != 0 && errno != ENOENT) {
    perror("unlink(%s) failed");
    exit(EXIT_FAILURE);
  }
  if (unlink(notif_pipe_path) != 0 && errno != ENOENT) {
    perror("unlink(%s) failed");
    exit(EXIT_FAILURE);
  }

  /* criar pipes */

  if (mkfifo(req_pipe_path, 0640) != 0) {
    perror("mkfifo failed");
    exit(EXIT_FAILURE);
  }
  fprintf(stderr, "Request pipe created: %s\n", req_pipe_path);

  if (mkfifo(resp_pipe_path, 0640) != 0) {
    perror("mkfifo failed");
    exit(EXIT_FAILURE);
  }
  fprintf(stderr, "Response pipe created: %s\n", resp_pipe_path);

  if (mkfifo(notif_pipe_path, 0640) != 0) {
    perror("mkfifo failed");
    exit(EXIT_FAILURE);
  }
  fprintf(stderr, "Notification pipe created: %s\n", notif_pipe_path);

  strcpy(REQ_PIPE_PATH, req_pipe_path);
  strcpy(RESP_PIPE_PATH, resp_pipe_path);
  strcpy(NOTIFICATIONS_PIPE_PATH, notif_pipe_path);


  // envia mensagem de connect para o request pipe e espera pela resposta no response pipe
  char message[1 + 3 * MAX_PIPE_PATH_LENGTH] = {0};

  build_message(req_pipe_path, resp_pipe_path, notif_pipe_path, message);
  
  fprintf(stderr,"VAI MANDAR MSG\n");

  send_message(fifo_fd_wr, message);
  fprintf(stderr,"MANDOU MSG\n");

  RESP_FD_RD = open(resp_pipe_path, O_RDONLY|O_NONBLOCK);
  if (RESP_FD_RD == -1) {
    perror("Failed to open FIFO");
    exit(EXIT_FAILURE);
  }
  fprintf(stderr, "Successfully opened response FIFO: %s with FD: %d\n", resp_pipe_path, RESP_FD_RD);
  
  REQ_FD_WR = open(req_pipe_path, O_WRONLY | O_NONBLOCK);
  if (REQ_FD_WR == -1) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
          fprintf(stderr, "Request FIFO is not ready for writing (non-blocking)\n");
      } else {
          perror("Failed to open request FIFO");
      }
      exit(EXIT_FAILURE);
  }
  fprintf(stderr, "Successfully opened request FIFO: %s with FD: %d\n", req_pipe_path, REQ_FD_WR);

  NOTIFICATIONS_FD_RD = open(notif_pipe_path, O_RDONLY | O_NONBLOCK);
  if (NOTIFICATIONS_FD_RD == -1) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
          fprintf(stderr, "Notifications FIFO is not ready for reading (non-blocking)\n");
      } else {
          perror("Failed to open notifications FIFO");
      }
      exit(EXIT_FAILURE);
  }
  fprintf(stderr, "Successfully opened notifications FIFO: %s with FD: %d\n", notif_pipe_path, NOTIFICATIONS_FD_RD);




  fprintf(stderr,"CHEGA AQUI DPS DE ABRIR O NOT\n");
  *notif_pipe = NOTIFICATIONS_FD_RD;

  char result = read_message(RESP_FD_RD);
  printf("Server returned %c for operation: connect\n", result);
  if (result != '0') {
    close(fifo_fd_wr);
    close(RESP_FD_RD);
    close(REQ_FD_WR);
    close(NOTIFICATIONS_FD_RD);
    return 1;
  }

  close(fifo_fd_wr);

  return 0;
}

int kvs_disconnect(void) {
  send_message(REQ_FD_WR, "2");
  char result = read_message(RESP_FD_RD);
  
  printf("Server returned %c for operation: disconnect\n", result);

  if (result == '0') {
    close(RESP_FD_RD);
    close(REQ_FD_WR);
    close(NOTIFICATIONS_FD_RD);
    unlink(REQ_PIPE_PATH);
    unlink(RESP_PIPE_PATH);
    unlink(NOTIFICATIONS_PIPE_PATH);
    return 0;
  }

  return 1;

}

int kvs_subscribe(const char *key) {
  char message_to_send[42] = {0};
  message_to_send[0] = '0';
  strncpy(message_to_send + 1, key, 41);
  send_message(REQ_FD_WR, message_to_send);

  char result = read_message(RESP_FD_RD);
  printf("Server returned %c for operation: subscribe\n", result);

  if (result != '0') {
    return 1;
  }

  return 0;
}

int kvs_unsubscribe(const char *key) {
  char message_to_send[42] = {0};
  message_to_send[0] = '0';
  strncpy(message_to_send + 1, key, 41);
  send_message(REQ_FD_WR, message_to_send);

  char result = read_message(RESP_FD_RD);
  printf("Server returned %c for operation: unsubscribe\n", result);

  if (result != '0') {
    return 1;
  }

  return 0;
}
