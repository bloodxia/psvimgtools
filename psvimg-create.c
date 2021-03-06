/* Copyright (C) 2017 Yifan Lu
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <time.h>
#include "backup.h"
#include "psvimg.h"
#include "utils.h"

#define MAX_PATH_LEN 1024

static int need_psvinf(const char *title) {
  if (strcmp(title, "app") == 0 ||
      strcmp(title, "patch") == 0 ||
      strcmp(title, "addcont") == 0 ||
      strcmp(title, "savedata") == 0 ||
      strcmp(title, "appmeta") == 0 ||
      strcmp(title, "license") == 0 ||
      strcmp(title, "game") == 0) {
    return 0;
  } else {
    return 1;
  }
}

int main(int argc, const char *argv[]) {
  args_t args1, args2;
  struct stat st;
  int fds[4];
  char path[MAX_PATH_LEN];
  int mfd;
  PsvMd_t md;
  pthread_t t1, t2;

  if (argc < 7) {
    fprintf(stderr, "usage: psvimg-create [-m metadata|-n name] -K key inputdir outputdir\n");
    fprintf(stderr, "  specify either a decrypted metadata file as a template or\n");
    fprintf(stderr, "  a name and other metadata fields will retain default values\n");
    return 1;
  }

  // TODO: support more types
  if (strcmp(argv[1], "-m") == 0) {
    mfd = open(argv[2], O_RDONLY);
    if (mfd < 0) {
      perror("metadata");
      return 1;
    }
    if (read_block(mfd, &md, sizeof(md) - sizeof(md.add_data)) < sizeof(md) - sizeof(md.add_data)) {
      fprintf(stderr, "invalid metadata size\n");
      return 1;
    }
    close(mfd);
    if (md.type != 2) {
      fprintf(stderr, "metadata type not supported\n");
      return 1;
    }
    if (read_block(mfd, &md.add_data, sizeof(md.add_data)) < sizeof(md.add_data)) {
      fprintf(stderr, "invalid metadata size\n");
      return 1;
    }
  } else if (strcmp(argv[1], "-n") == 0) {
    memset(&md, 0, sizeof(md));
    md.magic = PSVMD_VER1_MAGIC;
    md.type = 2;
    md.unk_68 = 2;
    md.add_data = 1;
    srand(time(NULL));
    for (int i = 0; i < sizeof(md.iv); i++) {
      md.iv[i] = rand() % 0xFF;
    }
    strncpy(md.name, argv[2], sizeof(md.name));
  } else {
    fprintf(stderr, "you must specify either -m or -t!\n");
    return 1;
  }

  if (pipe(fds) < 0) {
    perror("pipe 1");
    return 1;
  }

  args1.in = 0;
  args1.prefix = argv[5];
  args1.content_size = 0;
  args1.out = fds[1];

  if (stat(argv[6], &st) < 0) {
    mkdir(argv[6], 0700);
  }

  snprintf(path, sizeof(path), "%s/%s.psvimg", argv[6], md.name);

  args2.out = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (args2.out < 0) {
    perror("psvimg output");
    return 1;
  }
  args2.in = fds[0];

  if (parse_key(argv[4], args2.key) < 0) {
    fprintf(stderr, "invalid key\n");
    return 1;
  }

  memcpy(args2.iv, md.iv, sizeof(args2.iv));

  pthread_create(&t1, NULL, pack_thread, &args1);
  pthread_create(&t2, NULL, encrypt_thread, &args2);

  pthread_join(t1, NULL);
  pthread_join(t2, NULL);

  if (stat(path, &st) < 0) {
    perror("stat");
    return 1;
  }
  fprintf(stderr, "created %s (size: %llx, content size: %zx)\n", path, st.st_size, args1.content_size);
  md.total_size = args1.content_size;
  md.psvimg_size = st.st_size;

  // now create the psvmd
  snprintf(path, sizeof(path), "%s/%s.psvmd", argv[6], md.name);

  if (pipe(fds) < 0) {
    perror("pipe 2");
    return 1;
  }
  if (pipe(&fds[2]) < 0) {
    perror("pipe 3");
    return 1;
  }

  args1.in = fds[0];
  args1.out = fds[3];

  args2.in = fds[2];
  args2.out = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (args2.out < 0) {
    perror("psvmd output");
    return 1;
  }
  for (int i = 0; i < sizeof(md.iv); i++) {
    args2.iv[i] = rand() % 0xFF;
  }

  pthread_create(&t1, NULL, compress_thread, &args1);
  pthread_create(&t2, NULL, encrypt_thread, &args2);
  
  write_block(fds[1], &md, sizeof(md));
  close(fds[1]);

  pthread_join(t1, NULL);
  pthread_join(t2, NULL);
  
  fprintf(stderr, "created %s\n", path);

  // finally create the psvinf
  if (need_psvinf(md.name)) {
    snprintf(path, sizeof(path), "%s/%s.psvinf", argv[6], md.name);
    mfd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    write_block(mfd, md.name, strnlen(md.name, 64) + 1);
    close(mfd);
    fprintf(stderr, "created %s\n", path);
  }

  return 0;
}
